// SPDX-License-Identifier: MIT
#pragma once

#include "CommitModel.h"
#include "git/Types.h"

#include <QWidget>

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

    void setCommits(const QVector<Commit>& commits, const QVector<GraphRow>& graph,
                    const QHash<QString, QVector<RefBadge>>& badges, const QString& headSha);
    void selectCommit(const QString& sha);
    void showCommit(const QString& sha);  // details even when not in the list
    QString selectedSha() const;
    QVector<RefBadge> badgesFor(const QString& sha) const { return m_model->badgesFor(sha); }
    void focusSearch();
    void saveLayout(QSettings& settings) const;
    void restoreLayout(const QSettings& settings);

private:
    void onCurrentRowChanged();
    void findNext(bool backwards = false);
    void loadFileDiff();
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

    QString m_detailsSha;
    QStringList m_detailsParents;
    int m_detailsGen = 0;
    int m_diffGen = 0;
    bool m_firstLoad = true;
};

}  // namespace aslgc
