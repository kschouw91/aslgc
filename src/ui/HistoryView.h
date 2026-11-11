// SPDX-License-Identifier: MIT
#pragma once

#include "CommitModel.h"
#include "git/Types.h"

#include <QWidget>
#include <optional>

class QLabel;
class QLineEdit;
class QSettings;
class QSplitter;
class QTextBrowser;
class QTreeView;

namespace aslgc {

class DiffView;
class FileList;
class RepoView;

// "All Commits": graph + commit list on top, commit details / files / diff below.
class HistoryView : public QWidget {
    Q_OBJECT
public:
    explicit HistoryView(RepoView* owner);

    // `reflogOnly`: lost commits, listed because a reflog still points to them.
    void setCommits(const QVector<Commit>& commits, const QVector<GraphRow>& graph,
                    const QHash<QString, QVector<RefBadge>>& badges, const QString& headSha,
                    const QSet<QString>& reflogOnly = {});
    bool hasCommit(const QString& sha) const { return m_model->rowOf(sha) >= 0; }
    void selectCommit(const QString& sha);
    void showCommit(const QString& sha);  // details even when not in the list
    QString selectedSha() const;
    // Shows what changed between `sha` and the working tree (staged and unstaged changes).
    void compareWithWorkingTree(const QString& sha);
    // Reloads a working tree comparison, keeping the file and scroll position.
    void refreshWorkingTreeComparison();
    QVector<RefBadge> badgesFor(const QString& sha) const { return m_model->badgesFor(sha); }
    void focusSearch();
    void saveLayout(QSettings& settings) const;
    void restoreLayout(const QSettings& settings);

private:
    // Two or more selected commits, or one commit and the working tree.
    struct Comparison {
        QString oldest;
        QString newest;              // empty: the working tree
        int count = 1;               // selected commits
        bool includeOldest = false;  // range diff: start before the oldest commit, so its own changes show too

        bool sameAs(const Comparison& o) const { return oldest == o.oldest && newest == o.newest && count == o.count; }
    };

    QStringList selectedShas() const;  // newest first
    void onSelectionChanged();
    void showComparison(const Comparison& comparison, bool refresh = false);
    void renderComparisonHeader(int fileCount);
    void saveComparisonPatch();
    void showSelectionMenu(const QStringList& shas, const QPoint& globalPos);
    void findNext(bool backwards = false);
    void loadFileDiff(bool keepScroll = false);
    void renderHeader(const CommitInfo& info);

    RepoView* m_owner;
    CommitModel* m_model;
    QTreeView* m_list;
    QLineEdit* m_search;
    QLabel* m_searchStatus;
    QTextBrowser* m_header;
    FileList* m_files;
    DiffView* m_diff;
    QSplitter* m_vertical;
    QSplitter* m_bottom;
    QSplitter* m_detailsSplit;

    QString m_detailsSha;          // the commit shown, or the newer side of a comparison (empty: working tree)
    QStringList m_detailsParents;  // what the diff is against
    std::optional<Comparison> m_compare;
    QString m_compareFrom;  // resolved older side of m_compare
    int m_detailsGen = 0;
    int m_diffGen = 0;
    bool m_firstLoad = true;
};

}  // namespace aslgc
