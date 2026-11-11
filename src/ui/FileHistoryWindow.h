// SPDX-License-Identifier: MIT
#pragma once

#include "git/Diff.h"

#include <QDateTime>
#include <QWidget>
#include <optional>

class QAction;
class QButtonGroup;
class QComboBox;
class QFrame;
class QLabel;
class QMenu;
class QSplitter;
class QStackedWidget;
class QTextBrowser;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace aslgc {

class BlameView;
class DiffView;
class FileList;
class RepoView;

// A window with the history of one file (following renames) or folder, and the blame of a file.
// The path is shown as a breadcrumb: clicking a folder in it switches to that folder's history.
// The history follows the current branch, another branch or all of them; on the current branch,
// changes that are not committed yet come first.
class FileHistoryWindow : public QWidget {
    Q_OBJECT
public:
    FileHistoryWindow(RepoView* owner, const QString& path, bool folder);
    ~FileHistoryWindow() override;

    const QString& path() const { return m_path; }
    bool isFolder() const { return m_folder; }

    void showHistoryPage();
    // Blames `blamePath` (default: this window's file) at `rev`; an empty `rev` means the working tree.
    void showBlame(const QString& rev, const QString& blamePath = {});
    // Narrows the history to the commits that changed lines `first`..`last` (1-based) of `path`
    // as of `rev` (empty: the working tree file), each shown with its diff of just those lines.
    void showLineHistory(const QString& path, int first, int last, const QString& rev);
    void reload();

protected:
    void changeEvent(QEvent* event) override;

private:
    struct BlameTarget {
        QString rev;  // empty = working tree
        QString path;
        int line = -1;  // 0-based line to show, -1 = top
        bool center = false;
    };
    struct DetailsData {
        CommitInfo info;
        QVector<FileChange> files;
    };
    struct LineRange {
        QString path;
        int first = 0;
        int last = 0;
        QString rev;  // empty = the working tree file
    };

    void setTarget(const QString& path, bool folder);
    void updateTitle();
    // Refills the branch choice; returns true when the chosen branch is gone (back to the current one).
    bool updateBranchChoices();
    void chooseBranch(int index);
    bool isAllBranches() const;
    QString historyTip() const;  // changes whenever the history being shown could have changed
    void loadHistory(const QString& selectSha);
    void populateList(const QString& selectSha);
    QTreeWidgetItem* entryItem(int index) const;  // the list row of m_entries[index]
    int entryIndex(const QString& sha) const;
    void selectEntry(const QString& sha);
    // Shows `sha` on the history page; commits outside this history are shown on their own.
    void showCommit(const QString& sha, const QString& pathAtCommit);
    void onCurrentEntryChanged();
    void showDetails(const QString& sha, const QVector<FileChange>& fileChanges);
    void loadFileDiff();
    void showListMenu(const QPoint& pos);
    void onHeaderLink(const QUrl& url);
    int currentEntry() const;  // index into m_entries, -1 when none is selected
    bool showsUncommitted() const;
    QTreeWidgetItem* uncommittedItem() const;
    void fillUncommittedItem(QTreeWidgetItem* item) const;
    void loadUncommitted();
    void applyUncommitted(const QVector<FileChange>& changes);
    void showUncommitted();
    void requestLineHistory(int first, int last, bool oldSide);
    void clearLineHistory();
    void updateLineBanner();

    void showDefaultBlame();
    BlameOptions blameOptions() const;
    void blameOptionsChanged();
    void loadBlame(const BlameTarget& target, bool remember, bool quiet = false);
    void blameBack();
    void updateBlameBar();
    void fillBlameMenu(QMenu& menu, int line);
    void showCommitOfLine(int line);
    QString describeRev(const QString& rev) const;

    RepoView* m_owner;
    QString m_path;
    bool m_folder;
    QVector<FileLogEntry> m_entries;
    std::optional<LineRange> m_lines;  // set while showing the history of a range of lines
    QVector<FileDiff> m_lineDiffs;     // per entry, while m_lines is set
    QString m_lineError;
    QStringList m_revs;  // what the history walks (git log revisions); empty: HEAD
    QString m_loadedTip;
    QVector<FileChange> m_uncommitted;  // shown first while the history follows HEAD
    bool m_uncommittedSelected = false;
    QByteArray m_uncommittedDiff;  // last diff shown for the uncommitted row, to skip unchanged reloads

    QLabel* m_breadcrumb;
    QLabel* m_count;
    QComboBox* m_branch;
    QButtonGroup* m_pages;
    QToolButton* m_historyButton;
    QToolButton* m_blameButton;
    QStackedWidget* m_stack;

    QWidget* m_historyPage;
    QFrame* m_lineBanner;
    QLabel* m_lineLabel;
    QTreeWidget* m_list;
    QTextBrowser* m_header;
    FileList* m_files;
    DiffView* m_diff;
    QSplitter* m_vertical;
    QSplitter* m_bottom;
    QSplitter* m_detailsSplit;
    QString m_detailsSha;
    QStringList m_detailsParents;

    QWidget* m_blamePage;
    QToolButton* m_back;
    QLabel* m_blameLabel;
    QToolButton* m_blameOptionsButton;
    QAction* m_blameWhitespace;
    QAction* m_blameMoves;
    QAction* m_blameCopies;
    BlameView* m_blame;
    BlameTarget m_blameTarget;
    QVector<BlameTarget> m_blameHistory;
    bool m_blameLoaded = false;
    QDateTime m_blameFileTime;  // working tree file's modification time when blamed

    int m_historyGen = 0;
    int m_uncommittedGen = 0;
    int m_detailsGen = 0;
    int m_diffGen = 0;
    int m_blameGen = 0;
};

}  // namespace aslgc
