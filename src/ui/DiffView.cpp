// SPDX-License-Identifier: MIT
#include "DiffView.h"

#include "Theme.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>

#include <algorithm>
#include <climits>
#include <memory>

namespace aslgc {

class DiffGutter : public QWidget {
public:
    explicit DiffGutter(DiffView* view) : QWidget(view), m_view(view) {}
    QSize sizeHint() const override { return {m_view->gutterWidth(), 0}; }

protected:
    void paintEvent(QPaintEvent* event) override { m_view->paintGutter(event); }

private:
    DiffView* m_view;
};

namespace {
constexpr int kMaxRows = 60000;
}

DiffView::DiffView(QWidget* parent) : QPlainTextEdit(parent), m_gutter(new DiffGutter(this)) {
    setReadOnly(true);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    setFrameShape(QFrame::NoFrame);
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect& rect, int dy) {
        if (dy)
            m_gutter->scroll(0, dy);
        else
            m_gutter->update(0, rect.y(), m_gutter->width(), rect.height());
    });
    connect(this, &QPlainTextEdit::selectionChanged, viewport(), qOverload<>(&QWidget::update));
    layoutGutter();
}

void DiffView::setDiff(const FileDiff& diff, Mode mode, bool keepScroll) {
    m_diff = diff;
    m_mode = mode;
    if (diff.binary) {
        showMessage(tr("Binary file — no text diff to show."));
        return;
    }
    if (diff.hunks.isEmpty()) {
        showMessage(diff.header.size() > 1 ? tr("No content changes (file mode or metadata only).") : tr("No changes."));
        return;
    }
    QString text;
    QVector<Row> rows;
    int maxNo = 0;
    bool truncated = false;
    for (int h = 0; h < diff.hunks.size() && !truncated; ++h) {
        const DiffHunk& hunk = diff.hunks[h];
        if (!rows.isEmpty()) text += QLatin1Char('\n');
        text += QString::fromUtf8(hunk.header);
        rows.push_back({Row::Hunk, h, -1, -1, -1});
        for (int i = 0; i < hunk.lines.size(); ++i) {
            if (rows.size() >= kMaxRows) {
                truncated = true;
                break;
            }
            const DiffLine& l = hunk.lines[i];
            Row row{Row::Context, h, i, l.oldLine, l.newLine};
            if (l.type == '+')
                row.kind = Row::Added;
            else if (l.type == '-')
                row.kind = Row::Removed;
            else if (l.type == '\\')
                row.kind = Row::NoNewline;
            text += QLatin1Char('\n');
            text += row.kind == Row::NoNewline ? tr("\\ No newline at end of file") : l.text();
            maxNo = std::max({maxNo, l.oldLine, l.newLine});
            rows.push_back(row);
        }
    }
    if (truncated) {
        text += QLatin1Char('\n') + tr("… diff truncated (too large to display)");
        rows.push_back({});
    }
    m_numberDigits = std::max(3, int(QString::number(maxNo).size()));
    m_messageOnly = false;
    setRows(text, std::move(rows), keepScroll);
}

void DiffView::setConflictText(const QByteArray& content, bool keepScroll) {
    m_diff = {};
    m_mode = Mode::Conflict;
    QString text;
    QVector<Row> rows;
    int state = 0;  // 0 outside, 1 ours, 2 base, 3 theirs
    const QList<QByteArray> lines = content.split('\n');
    int n = int(lines.size());
    if (n > 0 && lines.last().isEmpty()) --n;
    for (int i = 0; i < n && rows.size() < kMaxRows; ++i) {
        QByteArray l = lines[i];
        if (l.endsWith('\r')) l.chop(1);
        Row row;
        row.newNo = i + 1;
        if (l.startsWith("<<<<<<<")) {
            row.kind = Row::Marker;
            state = 1;
        } else if (l.startsWith("|||||||") && state != 0) {
            row.kind = Row::Marker;
            state = 2;
        } else if (l.startsWith("=======") && state != 0) {
            row.kind = Row::Marker;
            state = 3;
        } else if (l.startsWith(">>>>>>>") && state != 0) {
            row.kind = Row::Marker;
            state = 0;
        } else {
            row.kind = state == 1 ? Row::Ours : state == 3 ? Row::Theirs : Row::Context;
        }
        if (i) text += QLatin1Char('\n');
        text += QString::fromUtf8(l);
        rows.push_back(row);
    }
    m_numberDigits = std::max(3, int(QString::number(n).size()));
    m_messageOnly = false;
    setRows(text, std::move(rows), keepScroll);
}

void DiffView::showMessage(const QString& message) {
    m_messageOnly = true;
    setRows(message, {Row{}}, false);
}

void DiffView::clearView() {
    m_diff = {};
    m_mode = Mode::ReadOnly;
    m_messageOnly = true;
    setRows({}, {}, false);
}

void DiffView::setRows(const QString& text, QVector<Row> rows, bool keepScroll) {
    const int v = verticalScrollBar()->value();
    const int hz = horizontalScrollBar()->value();
    m_rows = std::move(rows);
    m_hover = -1;
    layoutGutter();
    setPlainText(text);
    if (keepScroll) {
        verticalScrollBar()->setValue(v);
        horizontalScrollBar()->setValue(hz);
    }
    m_gutter->update();
    viewport()->update();
}

QVector<DiffView::Action> DiffView::availableActions() const {
    if (m_messageOnly || m_diff.hunks.isEmpty()) return {};
    switch (m_mode) {
        case Mode::Unstaged: return {Action::Discard, Action::Stage};
        case Mode::Staged: return {Action::Unstage};
        case Mode::Untracked: return {Action::Stage};
        default: return {};
    }
}

QString DiffView::actionLabel(Action action, bool lines) const {
    switch (action) {
        case Action::Stage:
            if (m_mode == Mode::Untracked) return tr("Stage File");
            return lines ? tr("Stage Lines") : tr("Stage Hunk");
        case Action::Unstage: return lines ? tr("Unstage Lines") : tr("Unstage Hunk");
        case Action::Discard: return lines ? tr("Discard Lines") : tr("Discard Hunk");
    }
    return {};
}

QSet<quint64> DiffView::selectedChangeLines(int fallbackBlock) const {
    QSet<quint64> out;
    int b0 = -1;
    int b1 = -1;
    const QTextCursor c = textCursor();
    if (c.hasSelection()) {
        const QTextBlock start = document()->findBlock(c.selectionStart());
        const QTextBlock end = document()->findBlock(c.selectionEnd());
        b0 = start.blockNumber();
        b1 = end.blockNumber();
        if (b1 > b0 && end.position() == c.selectionEnd()) --b1;
    } else if (fallbackBlock >= 0) {
        b0 = b1 = fallbackBlock;
    } else {
        return out;
    }
    for (int n = std::max(0, b0); n <= b1 && n < m_rows.size(); ++n) {
        const Row& r = m_rows[n];
        if (r.kind == Row::Added || r.kind == Row::Removed) out.insert(lineKey(r.hunk, r.line));
    }
    return out;
}

bool DiffView::lineRange(int block, int* first, int* last, bool* oldSide) const {
    if (m_messageOnly || block < 0 || block >= m_rows.size()) return false;
    int b0 = block;
    int b1 = block;
    const QTextCursor c = textCursor();
    if (c.hasSelection()) {
        const QTextBlock start = document()->findBlock(c.selectionStart());
        const QTextBlock end = document()->findBlock(c.selectionEnd());
        int s0 = start.blockNumber();
        int s1 = end.blockNumber();
        if (s1 > s0 && end.position() == c.selectionEnd()) --s1;
        if (block >= s0 && block <= s1) {
            b0 = s0;
            b1 = s1;
        }
    }
    int newFirst = INT_MAX, newLast = -1, oldFirst = INT_MAX, oldLast = -1;
    for (int n = std::max(0, b0); n <= b1 && n < m_rows.size(); ++n) {
        const Row& r = m_rows[n];
        if ((r.kind == Row::Context || r.kind == Row::Added) && r.newNo > 0) {
            newFirst = std::min(newFirst, r.newNo);
            newLast = std::max(newLast, r.newNo);
        } else if (r.kind == Row::Removed && r.oldNo > 0) {
            oldFirst = std::min(oldFirst, r.oldNo);
            oldLast = std::max(oldLast, r.oldNo);
        }
    }
    *oldSide = newLast < 0;
    *first = *oldSide ? oldFirst : newFirst;
    *last = *oldSide ? oldLast : newLast;
    return *last > 0;
}

QVector<DiffView::Button> DiffView::buttons() const {
    QVector<Button> out;
    const QVector<Action> actions = availableActions();
    if (actions.isEmpty()) return out;
    QSet<int> hunksWithSelection;
    if (m_mode != Mode::Untracked) {
        for (quint64 k : selectedChangeLines()) hunksWithSelection.insert(int(k >> 32));
    }
    QFont bf = QApplication::font();
    const QFontMetrics fm(bf);
    const int vw = viewport()->width();
    QTextBlock block = firstVisibleBlock();
    const QPointF off = contentOffset();
    while (block.isValid()) {
        const QRectF r = blockBoundingGeometry(block).translated(off);
        if (r.top() > viewport()->height()) break;
        const int n = block.blockNumber();
        if (n < m_rows.size() && m_rows[n].kind == Row::Hunk) {
            const bool lines = hunksWithSelection.contains(m_rows[n].hunk);
            int x = vw - 8;
            for (int i = int(actions.size()) - 1; i >= 0; --i) {
                const QString label = actionLabel(actions[i], lines);
                const int w = fm.horizontalAdvance(label) + 18;
                const QRect br(x - w, int(r.top()) + 1, w, std::max(int(r.height()) - 2, fm.height()));
                out.push_back({br, m_rows[n].hunk, actions[i], lines, label});
                x -= w + 6;
            }
        }
        block = block.next();
    }
    return out;
}

void DiffView::trigger(const Button& b) {
    if (b.lines)
        emit linesAction(selectedChangeLines(), b.action);
    else
        emit hunkAction(b.hunk, b.action);
}

int DiffView::gutterWidth() const {
    if (m_messageOnly || m_rows.isEmpty()) return 0;
    const int digit = fontMetrics().horizontalAdvance(QLatin1Char('9'));
    const int col = digit * m_numberDigits + 10;
    return m_mode == Mode::Conflict ? col + 6 : col * 2 + digit + 10;
}

void DiffView::layoutGutter() {
    const int w = gutterWidth();
    setViewportMargins(w, 0, 0, 0);
    const QRect cr = contentsRect();
    m_gutter->setGeometry(cr.left(), cr.top(), w, cr.height());
}

void DiffView::paintGutter(QPaintEvent* event) {
    QPainter p(m_gutter);
    const theme::DiffColors colors = theme::diffColors();
    p.fillRect(event->rect(), colors.gutterBg);
    p.setFont(font());
    const int digit = fontMetrics().horizontalAdvance(QLatin1Char('9'));
    const int col = digit * m_numberDigits + 10;
    QTextBlock block = firstVisibleBlock();
    const QPointF off = contentOffset();
    while (block.isValid()) {
        const QRectF rf = blockBoundingGeometry(block).translated(off);
        if (rf.top() > event->rect().bottom()) break;
        const int n = block.blockNumber();
        if (n < m_rows.size() && rf.bottom() >= event->rect().top()) {
            const Row& row = m_rows[n];
            const QRect r(0, qRound(rf.top()), m_gutter->width(), qRound(rf.height()));
            QColor bg;
            switch (row.kind) {
                case Row::Added: bg = colors.addGutter; break;
                case Row::Removed: bg = colors.delGutter; break;
                case Row::Hunk: bg = colors.hunkBg; break;
                case Row::Marker: bg = colors.markerBg; break;
                default: break;
            }
            if (bg.isValid()) p.fillRect(r, bg);
            p.setPen(colors.gutterFg);
            if (m_mode == Mode::Conflict) {
                if (row.newNo > 0)
                    p.drawText(QRect(0, r.top(), col - 2, r.height()), Qt::AlignRight | Qt::AlignVCenter,
                               QString::number(row.newNo));
            } else {
                if (row.oldNo > 0)
                    p.drawText(QRect(0, r.top(), col - 6, r.height()), Qt::AlignRight | Qt::AlignVCenter,
                               QString::number(row.oldNo));
                if (row.newNo > 0)
                    p.drawText(QRect(col, r.top(), col - 6, r.height()), Qt::AlignRight | Qt::AlignVCenter,
                               QString::number(row.newNo));
                if (row.kind == Row::Added || row.kind == Row::Removed) {
                    p.setPen(palette().color(QPalette::Text));
                    p.drawText(QRect(col * 2, r.top(), digit + 8, r.height()), Qt::AlignCenter,
                               row.kind == Row::Added ? QStringLiteral("+") : QStringLiteral("−"));
                }
            }
        }
        block = block.next();
    }
}

void DiffView::paintEvent(QPaintEvent* event) {
    const theme::DiffColors colors = theme::diffColors();
    {
        QPainter p(viewport());
        QTextBlock block = firstVisibleBlock();
        const QPointF off = contentOffset();
        const int w = viewport()->width();
        while (block.isValid()) {
            const QRectF r = blockBoundingGeometry(block).translated(off);
            if (r.top() > event->rect().bottom()) break;
            const int n = block.blockNumber();
            if (n < m_rows.size()) {
                QColor c;
                switch (m_rows[n].kind) {
                    case Row::Added: c = colors.addBg; break;
                    case Row::Removed: c = colors.delBg; break;
                    case Row::Hunk: c = colors.hunkBg; break;
                    case Row::Marker: c = colors.markerBg; break;
                    case Row::Ours: c = colors.oursBg; break;
                    case Row::Theirs: c = colors.theirsBg; break;
                    default: break;
                }
                if (c.isValid()) p.fillRect(QRectF(0, r.top(), w, r.height()), c);
            }
            block = block.next();
        }
    }
    QPlainTextEdit::paintEvent(event);

    const QVector<Button> btns = buttons();
    if (btns.isEmpty()) return;
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    p.setFont(QApplication::font());
    for (int i = 0; i < btns.size(); ++i) {
        const Button& b = btns[i];
        const bool primary = b.action != Action::Discard;
        QColor bg = primary ? palette().color(QPalette::Highlight) : palette().color(QPalette::Button);
        const QColor fg = primary ? palette().color(QPalette::HighlightedText) : palette().color(QPalette::ButtonText);
        if (i == m_hover) bg = theme::isDark() ? bg.lighter(125) : bg.darker(110);
        p.setPen(QPen(theme::mix(bg, palette().color(QPalette::Shadow), 0.25), 1));
        p.setBrush(bg);
        p.drawRoundedRect(QRectF(b.rect).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
        p.setPen(fg);
        p.drawText(b.rect, Qt::AlignCenter, b.label);
    }
}

void DiffView::mouseMoveEvent(QMouseEvent* event) {
    const QVector<Button> btns = buttons();
    int hover = -1;
    for (int i = 0; i < btns.size(); ++i) {
        if (btns[i].rect.contains(event->position().toPoint())) hover = i;
    }
    if (hover != m_hover) {
        m_hover = hover;
        viewport()->setCursor(hover >= 0 ? Qt::PointingHandCursor : Qt::IBeamCursor);
        viewport()->update();
    }
    if (hover < 0 || event->buttons() != Qt::NoButton) QPlainTextEdit::mouseMoveEvent(event);
}

void DiffView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        for (const Button& b : buttons()) {
            if (b.rect.contains(event->position().toPoint())) {
                event->accept();
                trigger(b);
                return;
            }
        }
    }
    QPlainTextEdit::mousePressEvent(event);
}

bool DiffView::viewportEvent(QEvent* event) {
    if (event->type() == QEvent::Leave && m_hover >= 0) {
        m_hover = -1;
        viewport()->update();
    }
    return QPlainTextEdit::viewportEvent(event);
}

void DiffView::contextMenuEvent(QContextMenuEvent* event) {
    std::unique_ptr<QMenu> menu(createStandardContextMenu(event->pos()));
    int lineFirst = 0;
    int lineLast = 0;
    bool oldSide = false;
    if (m_lineHistory && m_mode == Mode::ReadOnly &&
        lineRange(cursorForPosition(event->pos()).blockNumber(), &lineFirst, &lineLast, &oldSide)) {
        QString label;
        if (oldSide)
            label = lineFirst == lineLast ? tr("History of Line %1 Before This Commit").arg(lineFirst)
                                          : tr("History of Lines %1–%2 Before This Commit").arg(lineFirst).arg(lineLast);
        else
            label = lineFirst == lineLast ? tr("History of Line %1").arg(lineFirst)
                                          : tr("History of Lines %1–%2").arg(lineFirst).arg(lineLast);
        auto* act = new QAction(theme::actionIcon(QStringLiteral("history")), label, menu.get());
        connect(act, &QAction::triggered, this,
                [this, lineFirst, lineLast, oldSide] { emit lineHistoryRequested(lineFirst, lineLast, oldSide); });
        QAction* top = menu->actions().value(0);
        menu->insertAction(top, act);
        menu->insertSeparator(top);
    }
    const QVector<Action> actions = availableActions();
    if (!actions.isEmpty()) {
        const int block = cursorForPosition(event->pos()).blockNumber();
        const int hunk = block >= 0 && block < m_rows.size() ? m_rows[block].hunk : -1;
        const QSet<quint64> lines = selectedChangeLines(block);
        QList<QAction*> added;
        for (int i = int(actions.size()) - 1; i >= 0; --i) {
            const Action a = actions[i];
            if (m_mode != Mode::Untracked && !lines.isEmpty()) {
                auto* act = new QAction(actionLabel(a, true), menu.get());
                connect(act, &QAction::triggered, this, [this, lines, a] { emit linesAction(lines, a); });
                added << act;
            }
            if (hunk >= 0) {
                auto* act = new QAction(actionLabel(a, false), menu.get());
                connect(act, &QAction::triggered, this, [this, hunk, a] { emit hunkAction(hunk, a); });
                added << act;
            }
        }
        if (!added.isEmpty()) {
            QAction* first = menu->actions().value(0);
            menu->insertActions(first, added);
            menu->insertSeparator(first);
        }
    }
    menu->exec(event->globalPos());
}

void DiffView::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);
    layoutGutter();
}

void DiffView::changeEvent(QEvent* event) {
    QPlainTextEdit::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
        setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
        layoutGutter();
    } else if (event->type() == QEvent::PaletteChange) {
        m_gutter->update();
    }
}

}  // namespace aslgc
