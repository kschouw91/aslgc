// SPDX-License-Identifier: MIT
#pragma once

#include "Diff.h"
#include "Git.h"
#include "GitFlow.h"
#include "Graph.h"
#include "Types.h"

#include <QHash>
#include <utility>

namespace aslgc {

struct LogOptions {
    bool allRefs = true;  // all branches/remotes/tags, or just HEAD
    // Also the commits that only the reflogs of HEAD and the local branches still point to, such
    // as commits left behind by an amend, a reset or a rebase.
    bool reflog = false;
    int maxCount = 20000;
};

// Read-only accessors are synchronous and safe to call from worker threads.
class Repository {
public:
    Repository() = default;
    explicit Repository(const QString& root);

    // Returns the work-tree root containing `path`, or empty with `error` set.
    static QString resolveRoot(const QString& path, QString* error = nullptr);

    bool isValid() const { return !m_root.isEmpty(); }
    const QString& root() const { return m_root; }
    const QString& gitDir() const { return m_gitDir; }
    QString name() const;
    const Git& git() const { return m_git; }

    HeadInfo head() const;
    QVector<Ref> refs(QByteArray* raw = nullptr) const;
    QVector<Stash> stashes(QByteArray* raw = nullptr) const;
    QVector<Remote> remotes() const;
    WorkingStatus status() const;
    Operation operation() const;
    QString rebaseProgress() const;  // "3/7" while rebasing

    // `extraCommits` are listed too, with the commits they lead to.
    QVector<Commit> log(const LogOptions& options, const HeadInfo& head, bool hasRefs,
                        const QStringList& extraCommits = {}) const;
    CommitInfo commitInfo(const QString& sha) const;
    // `folder` limits the list to one folder ("" = everything).
    QVector<FileChange> commitChanges(const QString& sha, const QStringList& parents, const QString& folder = {}) const;
    QByteArray commitDiff(const QString& sha, const QStringList& parents, const FileChange& file) const;
    QByteArray workingDiff(const FileChange& file, bool staged) const;

    // Comparisons of `from` (any commit or tree) with `to`; an empty `to` means the working tree,
    // i.e. staged and unstaged changes to tracked files (untracked files are not included).
    QVector<FileChange> compareChanges(const QString& from, const QString& to) const;
    QByteArray compareDiff(const QString& from, const QString& to, const FileChange& file) const;
    QByteArray comparePatch(const QString& from, const QString& to) const;  // whole comparison, binary safe
    // Where the changes of `sha` start: its first parent, or the empty tree for a root commit.
    QString baseOf(const QString& sha) const;
    QString headMessage() const;
    QStringList trackedFiles() const;

    // Commits that touched `path`, newest first, reachable from `revs` (git log revision
    // arguments, e.g. a branch or "--branches"; empty: HEAD). A file's history follows renames and
    // records the file's name in each commit; a folder's history ("" = whole repository) does not.
    // Merges are compared with their first parent.
    QVector<FileLogEntry> fileLog(const QString& path, bool folder, int maxCount = 10000, const QStringList& revs = {}) const;
    // What is not committed yet under `path` (a file, or a folder; "" = everything), compared
    // with HEAD: staged and unstaged changes to tracked files, then untracked files ('?').
    QVector<FileChange> uncommittedChanges(const QString& path, bool folder) const;
    // Line-by-line origin of `path` at `rev`; an empty `rev` blames the working tree file,
    // attributing uncommitted lines to the all-zero id.
    Blame blame(const QString& path, const QString& rev = {}, const BlameOptions& options = {}) const;
    // Commits that changed lines `first`..`last` (1-based) of `path` as of `rev`, newest first,
    // following renames; each with its diff limited to those lines. An empty `rev` means the
    // working tree file: the lines are translated to HEAD's version first. Sets `error` when git
    // refuses (for example, a range past the end of the file).
    QVector<LineLogEntry> lineLog(const QString& path, int first, int last, const QString& rev = {}, int maxCount = 10000,
                                  QString* error = nullptr) const;
    QString configValue(const QString& key) const;

    // The reflog of `ref` ("HEAD", "refs/heads/main", …), newest entry first; empty when there is
    // none. Entries whose commit no branch, remote branch, tag or HEAD reaches are marked lost.
    QVector<ReflogEntry> reflog(const QString& ref, int maxCount = 10000) const;
    // The commits the reflogs of `refs` point to, each once, with the newest entry pointing to it
    // ("HEAD@{2}"); `refs` come in order of preference.
    QVector<std::pair<QString, QString>> reflogCommits(const QStringList& refs) const;

private:
    QString m_root;
    QString m_gitDir;
    Git m_git;
};

// Everything the UI needs after a refresh, loaded off the GUI thread.
struct RepoSnapshot {
    HeadInfo head;
    QVector<Ref> refs;
    QVector<Stash> stashes;
    QVector<Remote> remotes;
    WorkingStatus status;
    Operation operation = Operation::None;
    QString rebaseProgress;
    QByteArray fingerprint;  // changes whenever the commit list must be reloaded
    QString rebaseEditSha;   // set while a rebase is stopped at an "edit" step
    flow::Config gitFlow;
    bool hasLog = false;
    QVector<Commit> commits;
    QVector<GraphRow> graph;
    // With LogOptions::reflog: the listed commits that no ref reaches, each with the newest reflog
    // entry pointing to it ("HEAD@{2}") when it is the newest of its line, otherwise empty.
    QHash<QString, QString> reflogOnly;
    QString error;
};

RepoSnapshot loadSnapshot(const Repository& repo, const LogOptions& options, const QByteArray& previousFingerprint);

// `git format-patch` commands that write one numbered patch file per commit into `dir`, in the
// given order (oldest first, so `git am` can apply them). Merge commits produce no file.
QVector<GitCommand> formatPatchCommands(const QStringList& shas, const QString& dir);

}  // namespace aslgc
