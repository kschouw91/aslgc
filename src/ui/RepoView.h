// SPDX-License-Identifier: MIT
#pragma once

#include "Sidebar.h"
#include "git/Rebase.h"
#include "git/Repository.h"
#include "hosting/RemoteUrl.h"

#include <QWidget>
#include <functional>
#include <optional>

class QFileSystemWatcher;
class QFrame;
class QLabel;
class QMenu;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QTimer;

namespace aslgc {

class ChangesView;
class FileHistoryWindow;
class HistoryView;
struct RefBadge;

// One open repository (one tab): sidebar + Local Changes / All Commits views,
// and every git action that can be taken on it.
class RepoView : public QWidget {
    Q_OBJECT
public:
    explicit RepoView(const Repository& repo, QWidget* parent = nullptr);
    ~RepoView() override;

    const Repository& repo() const { return m_repo; }
    const RepoSnapshot& snapshot() const { return m_snapshot; }
    QString title() const { return m_repo.name(); }
    bool isBusy() const { return m_busy; }

    void refresh(bool forceLog = false);
    void scheduleRefresh();

    // Commands. Local commands run in the background and report errors in a dialog;
    // network commands show a progress dialog.
    // `failureHint` is added to the error message when a command fails.
    void runCommands(const QString& title, const QVector<GitCommand>& commands, std::function<void(bool)> done = {},
                     const QString& failureHint = {});
    void runNetwork(const QString& title, const QStringList& args, std::function<void(bool)> done = {});

    // Toolbar actions
    void fetch();
    void pull();
    void push();
    void stash();
    void popStash();
    void createBranch(const QString& startPoint = {});
    void addRemote();
    void openTerminal();
    void openFolder();
    void showChanges();
    void showHistory();
    void findCommit();

    // git-flow: the menu lists what fits the repository's state (set up, start, finish, settings).
    void populateGitFlowMenu(QMenu* menu);
    void gitFlowSetup();
    void gitFlowRemove();
    void gitFlowStart(flow::Kind kind, const QString& base = {});
    void gitFlowFinish(flow::Kind kind, const QString& name);

    // `extra` adds entries at the top of the menu.
    void showCommitMenu(const QString& sha, const QPoint& globalPos, const std::function<void(QMenu&)>& extra = {});

    // File history and blame open in their own window, one per path.
    void showFileHistory(const QString& path, bool folder = false);
    void showBlame(const QString& path, const QString& rev = {});  // empty rev: the working tree
    // Commits that changed lines `first`..`last` (1-based) of `path` as of `rev` (empty: working tree).
    void showLineHistory(const QString& path, int first, int last, const QString& rev);
    void chooseFileHistory();
    // "File History", "Blame" and "History of Folder" entries for `file` as of commit `sha`
    // (empty: the working tree).
    void addFileHistoryActions(QMenu& menu, const FileChange& file, const QString& sha = {}, const QStringList& parents = {});
    // One numbered patch file per commit (oldest first) in a folder the user picks.
    void savePatches(const QStringList& shas);
    // Shows what changed between `sha` and the working tree in All Commits.
    void compareWithWorkingTree(const QString& sha);
    // Selects the commit in All Commits and brings this tab and its window to the front.
    void revealCommit(const QString& sha);
    QVector<RefBadge> badgesFor(const QString& sha) const;

signals:
    void stateChanged();
    void busyChanged(bool busy);
    void statusMessage(const QString& message);
    void activateRequested();

private:
    void applySnapshot(RepoSnapshot snapshot);
    void updateBanner();
    void setBusy(bool busy, const QString& what = {});
    void onSidebarSelected(const SidebarEntry& entry);
    void onSidebarActivated(const SidebarEntry& entry);
    void onSidebarMenu(const SidebarEntry& entry, const QPoint& pos);
    const Ref* findRef(RefKind kind, const QString& name) const;
    QStringList remoteNames() const;
    QString defaultRemote() const;
    QString currentBranch() const { return m_snapshot.head.detached ? QString() : m_snapshot.head.branch; }
    FileHistoryWindow* fileHistoryWindow(const QString& path, bool folder);

    // Branch / commit operations
    void checkout(const QString& branch);
    void checkoutRemote(const Ref& remoteRef);
    void checkoutDetached(const QString& rev, const QString& label);
    void merge(const QString& rev, const QString& label);
    void rebase(const QString& onto, const QString& label);
    void cherryPick(const QString& sha);
    void revert(const QString& sha);
    void reset(const QString& sha);
    void createTag(const QString& sha);
    void deleteBranch(const Ref& ref);
    void renameBranch(const Ref& ref);
    void setUpstream(const Ref& ref);
    void fastForward(const Ref& ref);
    void deleteRemoteBranch(const Ref& ref);
    void pushTag(const QString& tag);
    void deleteTag(const QString& tag);
    void applyStash(const Stash& stash, bool pop);
    void dropStash(const Stash& stash);
    void editRemote(const QString& name);
    void removeRemote(const QString& name);
    void savePatch(const QString& sha);
    void interactiveRebase(const QString& base, const QString& baseLabel);
    void rewordCommit(const QString& sha);
    void startRebase(const QVector<RebaseStep>& steps, const QString& base, bool autostash);

    // GitHub / GitLab
    struct Hosted {
        Provider provider;
        RemoteRepo repo;
    };
    std::optional<Hosted> hostedRemote(const QString& remoteName) const;
    void openInBrowser(const QString& url);
    void createReviewRequest(const QString& remoteName, const QString& branch, int unpushedCommits);

    void continueOperation();
    void abortOperation();
    void skipOperation();

    Repository m_repo;
    RepoSnapshot m_snapshot;
    LogOptions m_logOptions;
    Sidebar* m_sidebar;
    QStackedWidget* m_stack;
    ChangesView* m_changes;
    HistoryView* m_history;
    QSplitter* m_split;
    QLabel* m_repoLabel;
    QFrame* m_banner;
    QLabel* m_bannerLabel;
    QPushButton* m_continue;
    QPushButton* m_skip;
    QPushButton* m_abort;
    QFileSystemWatcher* m_watcher;
    QTimer* m_refreshTimer;
    bool m_busy = false;
    bool m_refreshing = false;
    bool m_refreshPending = false;
    bool m_pendingForceLog = false;
    bool m_loaded = false;
};

}  // namespace aslgc
