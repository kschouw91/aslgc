// SPDX-License-Identifier: MIT
#pragma once

#include "Theme.h"
#include "git/Graph.h"
#include "git/Types.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QStyledItemDelegate>

namespace aslgc {

struct RefBadge {
    theme::Badge kind = theme::Badge::Local;
    QString text;
};

class CommitModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Description, Author, Date, Sha, ColumnCount };

    using QAbstractTableModel::QAbstractTableModel;

    void setCommits(const QVector<Commit>& commits, const QVector<GraphRow>& graph,
                    const QHash<QString, QVector<RefBadge>>& badges, const QString& headSha);

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    const Commit& commit(int row) const { return m_commits[row]; }
    const GraphRow& graph(int row) const { return m_graph[row]; }
    QVector<RefBadge> badges(int row) const { return m_badges.value(m_commits[row].sha); }
    QVector<RefBadge> badgesFor(const QString& sha) const { return m_badges.value(sha); }
    bool isHead(int row) const { return m_commits[row].sha == m_headSha; }
    int rowOf(const QString& sha) const { return m_rowOf.value(sha, -1); }

private:
    QVector<Commit> m_commits;
    QVector<GraphRow> m_graph;
    QHash<QString, QVector<RefBadge>> m_badges;
    QHash<QString, int> m_rowOf;
    QString m_headSha;
};

// Paints the lane graph, ref badges and subject in the Description column.
class CommitDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};

QString formatTime(qint64 secsSinceEpoch);
// Commit details as rich text: message, author, committer, SHA, parent links ("commit:<sha>"), refs.
QString commitHeaderHtml(const CommitInfo& info, const QVector<RefBadge>& badges, const QPalette& palette);

}  // namespace aslgc
