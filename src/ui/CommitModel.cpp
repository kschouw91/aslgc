// SPDX-License-Identifier: MIT
#include "CommitModel.h"

#include <QApplication>
#include <QDateTime>
#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace aslgc {

namespace {

constexpr int kLaneWidth = 14;

void drawConnector(QPainter* p, const QPointF& a, const QPointF& b) {
    if (qFuzzyCompare(a.x(), b.x())) {
        p->drawLine(a, b);
        return;
    }
    QPainterPath path(a);
    const qreal my = (a.y() + b.y()) / 2.0;
    path.cubicTo(QPointF(a.x(), my), QPointF(b.x(), my), b);
    p->drawPath(path);
}

}  // namespace

QString formatTime(qint64 secs) {
    const QDateTime dt = QDateTime::fromSecsSinceEpoch(secs);
    const QDate today = QDate::currentDate();
    if (dt.date() == today) return QObject::tr("Today %1").arg(dt.toString(QStringLiteral("HH:mm")));
    if (dt.date() == today.addDays(-1)) return QObject::tr("Yesterday %1").arg(dt.toString(QStringLiteral("HH:mm")));
    if (dt.date().year() == today.year()) return dt.toString(QStringLiteral("d MMM, HH:mm"));
    return dt.toString(QStringLiteral("d MMM yyyy, HH:mm"));
}

QString commitHeaderHtml(const CommitInfo& info, const QVector<RefBadge>& badges, const QPalette& pal) {
    const Commit& c = info.commit;
    QColor dim = pal.color(QPalette::Text);
    dim.setAlphaF(0.6f);
    const QString dimCss = dim.name(QColor::HexArgb);
    const QString subject = info.message.section(QLatin1Char('\n'), 0, 0);
    const QString body = info.message.section(QLatin1Char('\n'), 1).trimmed();

    QString html = QStringLiteral("<div style='font-size:large; font-weight:600'>%1</div>").arg(subject.toHtmlEscaped());
    if (!body.isEmpty())
        html += QStringLiteral("<div style='white-space:pre-wrap; margin-top:6px'>%1</div>").arg(body.toHtmlEscaped());
    html += QStringLiteral("<table cellspacing='0' cellpadding='2' style='margin-top:10px'>");
    auto row = [&](const QString& label, const QString& value) {
        html += QStringLiteral("<tr><td style='color:%1; padding-right:10px'>%2</td><td>%3</td></tr>").arg(dimCss, label, value);
    };
    const QString when = QDateTime::fromSecsSinceEpoch(c.authorTime).toString(QStringLiteral("ddd d MMM yyyy, HH:mm"));
    row(QObject::tr("Author"),
        QStringLiteral("%1 &lt;%2&gt; · %3").arg(c.authorName.toHtmlEscaped(), c.authorEmail.toHtmlEscaped(), when));
    if (c.committerName != c.authorName || c.committerEmail != c.authorEmail || c.commitTime != c.authorTime) {
        const QString cwhen = QDateTime::fromSecsSinceEpoch(c.commitTime).toString(QStringLiteral("ddd d MMM yyyy, HH:mm"));
        row(QObject::tr("Committer"),
            QStringLiteral("%1 &lt;%2&gt; · %3").arg(c.committerName.toHtmlEscaped(), c.committerEmail.toHtmlEscaped(), cwhen));
    }
    row(QObject::tr("SHA"), QStringLiteral("<code>%1</code>").arg(c.sha));
    if (!c.parents.isEmpty()) {
        QStringList links;
        for (const QString& p : c.parents) links << QStringLiteral("<a href='commit:%1'><code>%2</code></a>").arg(p, p.left(8));
        row(c.parents.size() > 1 ? QObject::tr("Parents") : QObject::tr("Parent"), links.join(QStringLiteral(", ")));
    }
    if (!badges.isEmpty()) {
        QStringList refs;
        for (const RefBadge& b : badges) {
            refs << QStringLiteral("<span style='color:%1; font-weight:600'>%2</span>")
                        .arg(theme::badgeColor(b.kind).name(), b.text.toHtmlEscaped());
        }
        row(QObject::tr("Refs"), refs.join(QStringLiteral(", ")));
    }
    html += QStringLiteral("</table>");
    return html;
}

void CommitModel::setCommits(const QVector<Commit>& commits, const QVector<GraphRow>& graph,
                             const QHash<QString, QVector<RefBadge>>& badges, const QString& headSha,
                             const QSet<QString>& reflogOnly) {
    beginResetModel();
    m_commits = commits;
    m_graph = graph;
    m_badges = badges;
    m_headSha = headSha;
    m_reflogOnly = reflogOnly;
    m_rowOf.clear();
    m_rowOf.reserve(commits.size());
    for (int i = 0; i < commits.size(); ++i) m_rowOf.insert(commits[i].sha, i);
    endResetModel();
}

int CommitModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : int(m_commits.size()); }

int CommitModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : ColumnCount; }

QVariant CommitModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= m_commits.size()) return {};
    const Commit& c = m_commits[index.row()];
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
            case Description: return c.subject;
            case Author: return c.authorName;
            case Date: return formatTime(c.authorTime);
            case Sha: return c.shortSha();
        }
    } else if (role == Qt::ToolTipRole) {
        switch (index.column()) {
            case Description:
                return m_reflogOnly.contains(c.sha) ? tr("Not on any branch or tag: only the reflog still remembers this commit")
                                                    : QVariant();
            case Author: return QStringLiteral("%1 <%2>").arg(c.authorName, c.authorEmail);
            case Date: return QDateTime::fromSecsSinceEpoch(c.authorTime).toString(Qt::RFC2822Date);
            case Sha: return c.sha;
            default: return {};
        }
    } else if (role == Qt::ForegroundRole && index.column() != Description) {
        QColor col = QApplication::palette().color(QPalette::Text);
        col.setAlphaF(m_reflogOnly.contains(c.sha) ? 0.45f : 0.7f);
        return col;
    } else if (role == Qt::FontRole && index.column() == Sha) {
        QFont f = QApplication::font();
        f.setFamilies({QStringLiteral("monospace")});
        f.setStyleHint(QFont::Monospace);
        return f;
    }
    return {};
}

QVariant CommitModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
        case Description: return tr("Description");
        case Author: return tr("Author");
        case Date: return tr("Date");
        case Sha: return tr("Commit");
    }
    return {};
}

QSize CommitDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    QSize s = QStyledItemDelegate::sizeHint(option, index);
    s.setHeight(qMax(s.height() + 6, option.fontMetrics.height() + 10));
    return s;
}

void CommitDelegate::paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    if (index.column() != CommitModel::Description) {
        QStyledItemDelegate::paint(p, option, index);
        return;
    }
    const auto* model = static_cast<const CommitModel*>(index.model());
    const int row = index.row();
    const GraphRow& g = model->graph(row);
    const Commit& c = model->commit(row);

    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    opt.text.clear();
    const QWidget* w = opt.widget;
    QStyle* style = w ? w->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, p, w);

    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const QRectF r = opt.rect;
    const qreal x0 = r.left() + 6;
    auto laneX = [&](int lane) { return x0 + lane * kLaneWidth + kLaneWidth / 2.0; };
    const qreal top = r.top();
    const qreal bottom = r.bottom() + 1;
    const qreal mid = r.center().y() + 0.5;

    p->setBrush(Qt::NoBrush);
    for (const GraphLine& l : g.lines) {
        QPen pen(theme::laneColor(l.color), 2.0);
        pen.setCapStyle(Qt::FlatCap);
        p->setPen(pen);
        switch (l.kind) {
            case GraphLine::TopToBottom: p->drawLine(QPointF(laneX(l.from), top), QPointF(laneX(l.to), bottom)); break;
            case GraphLine::TopToCenter: drawConnector(p, QPointF(laneX(l.from), top), QPointF(laneX(l.to), mid)); break;
            case GraphLine::CenterToBottom: drawConnector(p, QPointF(laneX(l.from), mid), QPointF(laneX(l.to), bottom)); break;
        }
    }

    // Commits only the reflog keeps are faded: node, subject (in italics) and the other columns.
    const bool reflogOnly = model->isReflogOnly(c.sha);
    const QPointF center(laneX(g.column), mid);
    QColor nodeColor = theme::laneColor(g.color);
    if (reflogOnly) nodeColor = theme::mix(opt.palette.color(QPalette::Base), nodeColor, 0.5);
    constexpr qreal rad = 4.5;
    if (c.isMerge()) {
        p->setPen(QPen(nodeColor, 2.0));
        p->setBrush(opt.palette.base());
        p->drawEllipse(center, rad - 0.5, rad - 0.5);
    } else {
        p->setPen(Qt::NoPen);
        p->setBrush(nodeColor);
        p->drawEllipse(center, rad, rad);
    }
    if (model->isHead(row)) {
        p->setPen(QPen(opt.palette.color(QPalette::Text), 1.5));
        p->setBrush(Qt::NoBrush);
        p->drawEllipse(center, rad + 2.5, rad + 2.5);
    }

    qreal x = x0 + g.width * kLaneWidth + 4;
    const bool selected = opt.state & QStyle::State_Selected;

    // Ref badges
    QFont badgeFont = opt.font;
    badgeFont.setPointSizeF(badgeFont.pointSizeF() * 0.88);
    const QFontMetrics bfm(badgeFont);
    const qreal badgeH = qMin<qreal>(r.height() - 6, bfm.height() + 4);
    for (const RefBadge& b : model->badges(row)) {
        if (x > r.right() - 60) break;
        QFont f = badgeFont;
        f.setBold(b.kind == theme::Badge::CurrentBranch || b.kind == theme::Badge::Head);
        const QFontMetricsF fm(f);
        const QString text = b.kind == theme::Badge::CurrentBranch ? QStringLiteral("✓ ") + b.text : b.text;
        const qreal textW = std::ceil(fm.horizontalAdvance(text)) + 2;
        const qreal bw = qMin<qreal>(textW + 12, 220);
        const QRectF br(x, mid - badgeH / 2.0, bw, badgeH);
        p->setPen(Qt::NoPen);
        p->setBrush(theme::badgeColor(b.kind));
        p->drawRoundedRect(br, 4, 4);
        p->setPen(Qt::white);
        p->setFont(f);
        p->drawText(br.adjusted(6, 0, -6, 0), Qt::AlignCenter,
                    textW + 12 <= 220 ? text : fm.elidedText(text, Qt::ElideMiddle, bw - 12));
        x += bw + 4;
    }

    QFont subjectFont = opt.font;
    subjectFont.setItalic(reflogOnly);
    QColor subjectColor = opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
    if (reflogOnly && !selected) subjectColor.setAlphaF(0.55f);
    p->setFont(subjectFont);
    p->setPen(subjectColor);
    const QRectF textRect(x + 2, r.top(), r.right() - x - 4, r.height());
    p->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                QFontMetrics(subjectFont).elidedText(c.subject, Qt::ElideRight, int(textRect.width())));
    p->restore();
}

}  // namespace aslgc
