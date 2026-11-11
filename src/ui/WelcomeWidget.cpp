// SPDX-License-Identifier: MIT
#include "WelcomeWidget.h"

#include "Theme.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace aslgc {

namespace {

class RecentDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override {
        return {200, option.fontMetrics.height() * 2 + 14};
    }

    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QString path = index.data(Qt::UserRole).toString();
        opt.text.clear();
        opt.icon = QIcon();
        const QWidget* w = opt.widget;
        QStyle* style = w ? w->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, p, w);
        const bool selected = opt.state & QStyle::State_Selected;
        const QColor fg = opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
        QColor dim = fg;
        dim.setAlphaF(0.6f);
        const QRect r = opt.rect.adjusted(12, 6, -12, -6);
        p->save();
        QFont bold = opt.font;
        bold.setBold(true);
        p->setFont(bold);
        p->setPen(fg);
        p->drawText(QRect(r.left(), r.top(), r.width(), r.height() / 2), Qt::AlignLeft | Qt::AlignVCenter,
                    QFileInfo(path).fileName());
        p->setFont(opt.font);
        p->setPen(dim);
        QString shown = path;
        const QString home = QDir::homePath();
        if (shown.startsWith(home)) shown = QStringLiteral("~") + shown.mid(home.size());
        p->drawText(QRect(r.left(), r.top() + r.height() / 2, r.width(), r.height() / 2), Qt::AlignLeft | Qt::AlignVCenter,
                    opt.fontMetrics.elidedText(shown, Qt::ElideMiddle, r.width()));
        p->restore();
    }
};

}  // namespace

WelcomeWidget::WelcomeWidget(QWidget* parent) : QWidget(parent) {
    auto* column = new QWidget;
    column->setMaximumWidth(680);
    auto* cl = new QVBoxLayout(column);

    auto* logo = new QLabel;
    logo->setPixmap(theme::appIcon().pixmap(112, 112));
    logo->setAlignment(Qt::AlignCenter);
    auto* title = new QLabel(QStringLiteral("<span style='font-size:26pt; font-weight:600'>aslgc</span>"));
    title->setAlignment(Qt::AlignCenter);
    auto* subtitle = new QLabel(tr("A simple Linux Git client"));
    subtitle->setAlignment(Qt::AlignCenter);
    QPalette sp = subtitle->palette();
    QColor dim = sp.color(QPalette::WindowText);
    dim.setAlphaF(0.65f);
    sp.setColor(QPalette::WindowText, dim);
    subtitle->setPalette(sp);

    auto* buttons = new QHBoxLayout;
    auto* open = new QPushButton(theme::actionIcon(QStringLiteral("folder-open")), tr("Open Repository…"));
    auto* clone = new QPushButton(theme::actionIcon(QStringLiteral("download")), tr("Clone…"));
    auto* init = new QPushButton(theme::actionIcon(QStringLiteral("folder-plus")), tr("New Repository…"));
    for (QPushButton* btn : {open, clone, init}) {
        btn->setMinimumHeight(34);
        buttons->addWidget(btn);
    }

    auto* recentTitle = new QLabel(QStringLiteral("<b>%1</b>").arg(tr("Recent repositories")));
    m_filter = new QLineEdit;
    m_filter->setPlaceholderText(tr("Filter…"));
    m_filter->setClearButtonEnabled(true);
    m_recent = new QListWidget;
    m_recent->setItemDelegate(new RecentDelegate(m_recent));
    m_recent->setContextMenuPolicy(Qt::CustomContextMenu);
    m_recent->setUniformItemSizes(true);

    cl->addSpacing(24);
    cl->addWidget(logo);
    cl->addWidget(title);
    cl->addWidget(subtitle);
    cl->addSpacing(18);
    cl->addLayout(buttons);
    cl->addSpacing(18);
    auto* recentHeader = new QHBoxLayout;
    recentHeader->addWidget(recentTitle);
    recentHeader->addStretch();
    recentHeader->addWidget(m_filter);
    cl->addLayout(recentHeader);
    cl->addWidget(m_recent, 1);

    auto* outer = new QHBoxLayout(this);
    outer->addStretch();
    outer->addWidget(column, 4);
    outer->addStretch();

    connect(open, &QPushButton::clicked, this, &WelcomeWidget::openRequested);
    connect(clone, &QPushButton::clicked, this, &WelcomeWidget::cloneRequested);
    connect(init, &QPushButton::clicked, this, &WelcomeWidget::initRequested);
    connect(m_filter, &QLineEdit::textChanged, this, &WelcomeWidget::applyFilter);
    connect(m_filter, &QLineEdit::returnPressed, this, [this] {
        for (int i = 0; i < m_recent->count(); ++i) {
            if (!m_recent->item(i)->isHidden()) {
                emit openPath(m_recent->item(i)->data(Qt::UserRole).toString());
                return;
            }
        }
    });
    connect(m_recent, &QListWidget::itemActivated, this,
            [this](QListWidgetItem* item) { emit openPath(item->data(Qt::UserRole).toString()); });
    connect(m_recent, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* item = m_recent->itemAt(pos);
        if (!item) return;
        const QString path = item->data(Qt::UserRole).toString();
        QMenu menu(this);
        menu.addAction(tr("Open"), this, [this, path] { emit openPath(path); });
        menu.addAction(tr("Remove from List"), this, [this, path] { emit forgetPath(path); });
        menu.exec(m_recent->viewport()->mapToGlobal(pos));
    });
}

void WelcomeWidget::setRecent(const QStringList& paths) {
    m_recent->clear();
    for (const QString& path : paths) {
        auto* item = new QListWidgetItem(QFileInfo(path).fileName(), m_recent);
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
    }
    applyFilter();
}

void WelcomeWidget::applyFilter() {
    const QString f = m_filter->text().trimmed();
    for (int i = 0; i < m_recent->count(); ++i) {
        QListWidgetItem* item = m_recent->item(i);
        item->setHidden(!f.isEmpty() && !item->data(Qt::UserRole).toString().contains(f, Qt::CaseInsensitive));
    }
}

}  // namespace aslgc
