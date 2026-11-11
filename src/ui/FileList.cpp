// SPDX-License-Identifier: MIT
#include "FileList.h"

#include "Theme.h"

#include <QApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QStyledItemDelegate>

namespace aslgc {

namespace {

class FileItemDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QString path = opt.text;
        opt.text.clear();
        const QWidget* w = opt.widget;
        QStyle* style = w ? w->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, p, w);

        const QRect tr = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, w).adjusted(2, 0, -2, 0);
        const int slash = path.lastIndexOf(QLatin1Char('/'));
        const QString name = path.mid(slash + 1);
        const QString dir = slash >= 0 ? path.left(slash) : QString();
        const bool selected = opt.state & QStyle::State_Selected;
        const QColor fg = opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
        QColor dim = fg;
        dim.setAlphaF(0.55f);

        p->save();
        p->setFont(opt.font);
        const QFontMetrics fm(opt.font);
        const QString shownName = fm.elidedText(name, Qt::ElideMiddle, tr.width());
        p->setPen(fg);
        p->drawText(tr, Qt::AlignVCenter | Qt::AlignLeft, shownName);
        const int used = fm.horizontalAdvance(shownName);
        if (!dir.isEmpty() && used + 24 < tr.width()) {
            const QRect dr = tr.adjusted(used + 8, 0, 0, 0);
            p->setPen(dim);
            p->drawText(dr, Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(dir, Qt::ElideLeft, dr.width()));
        }
        p->restore();
    }
};

}  // namespace

FileList::FileList(QWidget* parent) : QListWidget(parent) {
    setItemDelegate(new FileItemDelegate(this));
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setUniformItemSizes(true);
    setIconSize(QSize(16, 16));
    setFrameShape(QFrame::NoFrame);
    connect(this, &QListWidget::itemDoubleClicked, this, [this] { emit fileActivated(); });
}

void FileList::setFiles(const QVector<FileChange>& files) {
    m_files = files;
    clear();
    for (int i = 0; i < files.size(); ++i) {
        const FileChange& f = files[i];
        auto* item = new QListWidgetItem(theme::statusIcon(f.status), f.path, this);
        item->setData(Qt::UserRole, i);
        QString tip = theme::statusName(f.status) + QStringLiteral(": ") + f.path;
        if (!f.oldPath.isEmpty() && f.oldPath != f.path) tip += tr("\nRenamed from %1").arg(f.oldPath);
        if (!f.conflict.isEmpty()) tip += QStringLiteral(" [%1]").arg(f.conflict);
        if (f.submodule) tip += tr(" (submodule)");
        item->setToolTip(tip);
    }
}

int FileList::currentFileIndex() const {
    const QListWidgetItem* item = currentItem();
    return item ? item->data(Qt::UserRole).toInt() : -1;
}

const FileChange* FileList::currentFile() const {
    const int i = currentFileIndex();
    return i >= 0 && i < m_files.size() ? &m_files[i] : nullptr;
}

QVector<FileChange> FileList::selectedFiles() const {
    QVector<FileChange> out;
    for (int row = 0; row < count(); ++row) {
        const QListWidgetItem* it = item(row);
        if (it->isSelected()) out.push_back(m_files[it->data(Qt::UserRole).toInt()]);
    }
    if (out.isEmpty()) {
        if (const FileChange* f = currentFile()) out.push_back(*f);
    }
    return out;
}

bool FileList::selectPath(const QString& path) {
    for (int i = 0; i < m_files.size(); ++i) {
        if (m_files[i].path == path) {
            setCurrentRow(i, QItemSelectionModel::ClearAndSelect);
            return true;
        }
    }
    return false;
}

void FileList::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        emit fileActivated();
        return;
    }
    QListWidget::keyPressEvent(event);
}

}  // namespace aslgc
