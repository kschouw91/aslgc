// SPDX-License-Identifier: MIT
#include "BlameView.h"

#include "Theme.h"

#include <QContextMenuEvent>
#include <QDateTime>
#include <QFontDatabase>
#include <QMenu>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QToolTip>

#include <algorithm>
#include <limits>
#include <memory>

namespace aslgc {

class BlameGutter : public QWidget {
public:
    explicit BlameGutter(BlameView* view) : QWidget(view), m_view(view) { setMouseTracking(true); }
    QSize sizeHint() const override { return {m_view->gutterWidth(), 0}; }

protected:
    void paintEvent(QPaintEvent* event) override { m_view->paintGutter(event); }
    void mouseMoveEvent(QMouseEvent* event) override { m_view->gutterMouseMove(event); }
    void mousePressEvent(QMouseEvent* event) override { m_view->gutterMousePress(event); }
    void contextMenuEvent(QContextMenuEvent* event) override { m_view->gutterContextMenu(event); }
    void leaveEvent(QEvent* event) override {
        m_view->setHoverCommit(-1);
        QWidget::leaveEvent(event);
    }
    bool event(QEvent* event) override {
        if (event->type() == QEvent::ToolTip) {
            auto* help = static_cast<QHelpEvent*>(event);
            if (!m_view->gutterToolTip(help->pos(), help->globalPos())) {
                QToolTip::hideText();
                event->ignore();
            }
            return true;
        }
        return QWidget::event(event);
    }

private:
    BlameView* m_view;
};

namespace {

constexpr int kBar = 4;  // age bar width
constexpr int kShaChars = 8;
constexpr int kAuthorChars = 18;
constexpr int kDateChars = 10;  // yyyy-MM-dd

struct GutterLayout {
    int sha, author, date, number, width, digit;
};

GutterLayout gutterLayout(const QFontMetrics& fm, int numberDigits) {
    GutterLayout l{};
    l.digit = fm.horizontalAdvance(QLatin1Char('9'));
    l.sha = kBar + 6;
    l.author = l.sha + kShaChars * l.digit + 10;
    l.date = l.author + kAuthorChars * l.digit + 10;
    l.number = l.date + kDateChars * l.digit + 12;
    l.width = l.number + numberDigits * l.digit + 8;
    return l;
}

}  // namespace

BlameView::BlameView(QWidget* parent) : QPlainTextEdit(parent), m_gutter(new BlameGutter(this)) {
    setReadOnly(true);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    setFrameShape(QFrame::NoFrame);
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect& rect, int dy) {
        if (dy)
            m_gutter->scroll(0, dy);
        else
            m_gutter->update(0, rect.y(), m_gutter->width(), rect.height());
    });
    layoutGutter();
}

void BlameView::setBlame(const Blame& blame, const QString& path) {
    m_blame = blame;
    m_path = path;
    m_hoverCommit = -1;
    m_runStart.resize(blame.lines.size());
    m_oldest = std::numeric_limits<qint64>::max();
    m_newest = 0;
    for (const BlameCommit& c : blame.commits) {
        if (c.isUncommitted() || c.authorTime <= 0) continue;
        m_oldest = std::min(m_oldest, c.authorTime);
        m_newest = std::max(m_newest, c.authorTime);
    }
    QString text;
    for (int i = 0; i < blame.lines.size(); ++i) {
        if (i) text += QLatin1Char('\n');
        text += blame.lines[i].text;
        m_runStart[i] = i == 0 || blame.lines[i].commit != blame.lines[i - 1].commit;
    }
    m_numberDigits = std::max(3, int(QString::number(blame.lines.size()).size()));
    m_messageOnly = blame.lines.isEmpty();
    layoutGutter();
    setPlainText(m_messageOnly ? tr("The file is empty.") : text);
    m_gutter->update();
}

void BlameView::showMessage(const QString& message) {
    m_blame = {};
    m_runStart.clear();
    m_hoverCommit = -1;
    m_messageOnly = true;
    layoutGutter();
    setPlainText(message);
    m_gutter->update();
}

int BlameView::lineAtCursor() const { return m_messageOnly ? -1 : textCursor().blockNumber(); }

void BlameView::selectedLineRange(int line, int* first, int* last) const {
    *first = *last = line;
    const QTextCursor c = textCursor();
    if (m_messageOnly || !c.hasSelection()) return;
    const QTextBlock start = document()->findBlock(c.selectionStart());
    const QTextBlock end = document()->findBlock(c.selectionEnd());
    int b0 = start.blockNumber();
    int b1 = end.blockNumber();
    if (b1 > b0 && end.position() == c.selectionEnd()) --b1;  // selection ends at the start of a line
    if (line >= b0 && line <= b1) {
        *first = b0;
        *last = b1;
    }
}

int BlameView::topLine() const { return m_messageOnly ? 0 : firstVisibleBlock().blockNumber(); }

void BlameView::scrollToLine(int line, bool center) {
    if (m_messageOnly) return;
    line = std::clamp(line, 0, int(m_blame.lines.size()) - 1);
    setTextCursor(QTextCursor(document()->findBlockByNumber(line)));
    if (center)
        centerCursor();
    else
        verticalScrollBar()->setValue(line);
}

const BlameCommit* BlameView::commitOfLine(int line) const {
    if (line < 0 || line >= m_blame.lines.size()) return nullptr;
    const int c = m_blame.lines[line].commit;
    return c >= 0 && c < m_blame.commits.size() ? &m_blame.commits[c] : nullptr;
}

int BlameView::gutterWidth() const {
    if (m_messageOnly) return 0;
    return gutterLayout(fontMetrics(), m_numberDigits).width;
}

void BlameView::layoutGutter() {
    const int w = gutterWidth();
    setViewportMargins(w, 0, 0, 0);
    const QRect cr = contentsRect();
    m_gutter->setGeometry(cr.left(), cr.top(), w, cr.height());
}

QColor BlameView::ageColor(const BlameCommit& c) const {
    const theme::DiffColors colors = theme::diffColors();
    if (c.isUncommitted()) return theme::mix(colors.gutterBg, QColor(0x2da44e), 0.85);
    const qreal t = m_newest > m_oldest ? qreal(c.authorTime - m_oldest) / qreal(m_newest - m_oldest) : 1.0;
    const QColor heat = theme::isDark() ? QColor(0xf0883e) : QColor(0xd4700a);
    return theme::mix(colors.gutterBg, heat, 0.12 + 0.88 * t);
}

int BlameView::lineAtY(int y) const {
    if (m_messageOnly) return -1;
    QTextBlock block = firstVisibleBlock();
    const QPointF off = contentOffset();
    while (block.isValid()) {
        const QRectF r = blockBoundingGeometry(block).translated(off);
        if (r.top() > y) break;
        if (y < r.bottom()) return block.blockNumber() < m_blame.lines.size() ? block.blockNumber() : -1;
        block = block.next();
    }
    return -1;
}

void BlameView::paintGutter(QPaintEvent* event) {
    QPainter p(m_gutter);
    const theme::DiffColors colors = theme::diffColors();
    p.fillRect(event->rect(), colors.gutterBg);
    if (m_messageOnly) return;
    p.setFont(font());
    const QFontMetrics fm(font());
    const GutterLayout l = gutterLayout(fm, m_numberDigits);
    QColor hover = palette().color(QPalette::Highlight);
    hover.setAlphaF(0.18f);
    const QColor separator = theme::mix(colors.gutterBg, colors.gutterFg, 0.3);
    const QColor text = palette().color(QPalette::Text);

    QTextBlock block = firstVisibleBlock();
    const QPointF off = contentOffset();
    while (block.isValid()) {
        const QRectF rf = blockBoundingGeometry(block).translated(off);
        if (rf.top() > event->rect().bottom()) break;
        const int n = block.blockNumber();
        if (n < m_blame.lines.size() && rf.bottom() >= event->rect().top()) {
            const QRect r(0, qRound(rf.top()), m_gutter->width(), qRound(rf.height()));
            const BlameLine& line = m_blame.lines[n];
            const BlameCommit* c = commitOfLine(n);
            if (line.commit == m_hoverCommit) p.fillRect(r, hover);
            if (c) p.fillRect(QRect(0, r.top(), kBar, r.height()), ageColor(*c));
            if (m_runStart[n] && n > 0) {
                p.setPen(separator);
                p.drawLine(kBar, r.top(), r.right(), r.top());
            }
            if (m_runStart[n] && c) {
                if (c->isUncommitted()) {
                    p.setPen(text);
                    p.drawText(QRect(l.sha, r.top(), l.date - l.sha, r.height()), Qt::AlignLeft | Qt::AlignVCenter,
                               fm.elidedText(tr("Not committed yet"), Qt::ElideRight, l.date - l.sha));
                } else {
                    p.setPen(colors.gutterFg);
                    p.drawText(QRect(l.sha, r.top(), kShaChars * l.digit, r.height()), Qt::AlignLeft | Qt::AlignVCenter,
                               c->sha.left(kShaChars));
                    p.setPen(text);
                    const int aw = l.date - l.author - 10;
                    p.drawText(QRect(l.author, r.top(), aw, r.height()), Qt::AlignLeft | Qt::AlignVCenter,
                               fm.elidedText(c->authorName, Qt::ElideRight, aw));
                    p.setPen(colors.gutterFg);
                    p.drawText(QRect(l.date, r.top(), kDateChars * l.digit, r.height()), Qt::AlignLeft | Qt::AlignVCenter,
                               QDateTime::fromSecsSinceEpoch(c->authorTime).toString(QStringLiteral("yyyy-MM-dd")));
                }
            }
            p.setPen(colors.gutterFg);
            p.drawText(QRect(l.number, r.top(), m_numberDigits * l.digit, r.height()), Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(n + 1));
        }
        block = block.next();
    }
}

void BlameView::paintEvent(QPaintEvent* event) {
    if (m_hoverCommit >= 0 && !m_messageOnly) {
        QPainter p(viewport());
        QColor hover = palette().color(QPalette::Highlight);
        hover.setAlphaF(0.12f);
        QTextBlock block = firstVisibleBlock();
        const QPointF off = contentOffset();
        const int w = viewport()->width();
        while (block.isValid()) {
            const QRectF r = blockBoundingGeometry(block).translated(off);
            if (r.top() > event->rect().bottom()) break;
            const int n = block.blockNumber();
            if (n < m_blame.lines.size() && m_blame.lines[n].commit == m_hoverCommit)
                p.fillRect(QRectF(0, r.top(), w, r.height()), hover);
            block = block.next();
        }
    }
    QPlainTextEdit::paintEvent(event);
}

void BlameView::setHoverCommit(int commit) {
    if (commit == m_hoverCommit) return;
    m_hoverCommit = commit;
    m_gutter->update();
    viewport()->update();
}

void BlameView::gutterMouseMove(QMouseEvent* event) {
    const int line = lineAtY(event->position().toPoint().y());
    const BlameCommit* c = commitOfLine(line);
    setHoverCommit(line >= 0 ? m_blame.lines[line].commit : -1);
    m_gutter->setCursor(c && !c->isUncommitted() ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void BlameView::gutterMousePress(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    const int line = lineAtY(event->position().toPoint().y());
    const BlameCommit* c = commitOfLine(line);
    if (!c) return;
    setTextCursor(QTextCursor(document()->findBlockByNumber(line)));
    if (!c->isUncommitted()) emit commitClicked(line);
}

void BlameView::gutterContextMenu(QContextMenuEvent* event) {
    const int line = lineAtY(event->pos().y());
    if (line < 0 || !m_menuProvider) return;
    int first = line;
    int last = line;
    selectedLineRange(line, &first, &last);
    if (first == last) setTextCursor(QTextCursor(document()->findBlockByNumber(line)));  // keeps a selection around it
    QMenu menu(this);
    m_menuProvider(menu, line);
    if (!menu.isEmpty()) menu.exec(event->globalPos());
}

bool BlameView::gutterToolTip(const QPoint& pos, const QPoint& globalPos) {
    const int line = lineAtY(pos.y());
    const BlameCommit* c = commitOfLine(line);
    if (!c) return false;
    QString html;
    if (c->isUncommitted()) {
        html = tr("This line has changes that are not committed yet.");
    } else {
        const QString when = QDateTime::fromSecsSinceEpoch(c->authorTime).toString(QStringLiteral("ddd d MMM yyyy, HH:mm"));
        html = QStringLiteral("<b>%1</b><br>%2 &lt;%3&gt;<br>%4<br><code>%5</code>")
                   .arg(c->summary.toHtmlEscaped(), c->authorName.toHtmlEscaped(), c->authorEmail.toHtmlEscaped(), when, c->sha);
        const QString path = m_blame.lines[line].path;
        if (!path.isEmpty() && path != m_path) html += QStringLiteral("<br>") + tr("File was named %1").arg(path.toHtmlEscaped());
        html += QStringLiteral("<br><i>%1</i>").arg(tr("Click to show this commit"));
    }
    QToolTip::showText(globalPos, html, m_gutter);
    return true;
}

void BlameView::contextMenuEvent(QContextMenuEvent* event) {
    std::unique_ptr<QMenu> menu(createStandardContextMenu(event->pos()));
    const int line = m_messageOnly ? -1 : cursorForPosition(event->pos()).blockNumber();
    if (line >= 0 && line < m_blame.lines.size() && m_menuProvider) {
        QMenu extra;
        m_menuProvider(extra, line);
        const QList<QAction*> actions = extra.actions();
        if (!actions.isEmpty()) {
            QAction* first = menu->actions().value(0);
            for (QAction* a : actions) a->setParent(menu.get());
            menu->insertActions(first, actions);
            menu->insertSeparator(first);
        }
    }
    menu->exec(event->globalPos());
}

void BlameView::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);
    layoutGutter();
}

void BlameView::changeEvent(QEvent* event) {
    QPlainTextEdit::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
        setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
        layoutGutter();
    } else if (event->type() == QEvent::PaletteChange) {
        m_gutter->update();
    }
}

}  // namespace aslgc
