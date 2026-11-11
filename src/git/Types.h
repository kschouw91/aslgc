// SPDX-License-Identifier: MIT
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace aslgc {

struct Commit {
    QString sha;
    QStringList parents;
    QString authorName;
    QString authorEmail;
    qint64 authorTime = 0;
    QString committerName;
    QString committerEmail;
    qint64 commitTime = 0;
    QString subject;

    QString shortSha() const { return sha.left(8); }
    bool isMerge() const { return parents.size() > 1; }
};

struct CommitInfo {
    Commit commit;
    QString message;  // full message (subject + body)
    bool valid = false;
};

enum class RefKind { LocalBranch, RemoteBranch, Tag };

struct Ref {
    RefKind kind = RefKind::LocalBranch;
    QString fullName;  // refs/heads/main
    QString name;      // main, origin/main, v1.0
    QString target;    // commit sha (annotated tags are peeled)
    QString upstream;  // origin/main (local branches only)
    int ahead = 0;
    int behind = 0;
    bool upstreamGone = false;
    bool isHead = false;

    QString remoteName() const { return kind == RefKind::RemoteBranch ? name.section(QLatin1Char('/'), 0, 0) : QString(); }
    QString remoteBranch() const { return kind == RefKind::RemoteBranch ? name.section(QLatin1Char('/'), 1) : QString(); }
};

struct Stash {
    int index = 0;
    QString ref;  // stash@{0}
    QString sha;
    qint64 time = 0;
    QString message;
};

struct Remote {
    QString name;
    QString fetchUrl;
    QString pushUrl;
};

struct FileChange {
    QString path;
    QString oldPath;    // renames / copies
    char status = 'M';  // M A D R C T U(conflict) ?(untracked)
    QString conflict;   // XY code for unmerged entries, e.g. "UU"
    bool submodule = false;

    bool isUntracked() const { return status == '?'; }
    bool isConflict() const { return status == 'U'; }
};

struct WorkingStatus {
    QVector<FileChange> staged;
    QVector<FileChange> unstaged;  // includes untracked and conflicted files

    bool isClean() const { return staged.isEmpty() && unstaged.isEmpty(); }
    int conflictCount() const {
        int n = 0;
        for (const auto& f : unstaged) n += f.isConflict() ? 1 : 0;
        return n;
    }
};

enum class Operation { None, Merge, Rebase, CherryPick, Revert, ApplyMailbox };

// One commit in the history of a file or folder.
struct FileLogEntry {
    Commit commit;
    // File history: the file as this commit changed it, under its name at the time (renames
    // followed). Folder history: empty; the folder's changes are loaded on demand.
    QVector<FileChange> changes;
};

// One entry of a reflog: where a ref (HEAD or a branch) pointed after it moved, and why it moved.
struct ReflogEntry {
    QString selector;  // "HEAD@{2}", "main@{0}"
    QString action;    // what moved the ref: "commit", "commit (amend)", "checkout", "reset", "rebase (finish)", …
    QString message;   // the rest of the reflog message, e.g. "moving from main to dev"
    QString identity;  // who moved it
    qint64 time = 0;   // when it moved
    Commit commit;     // where it pointed afterwards
    // The commit is on no branch, remote branch or tag and is not checked out: only reflogs still
    // point to it, until they expire.
    bool lost = false;
};

struct BlameCommit {
    QString sha;
    QString authorName;
    QString authorEmail;
    qint64 authorTime = 0;
    QString summary;
    QString path;          // the file's name in this commit
    QString previousSha;   // where the lines were before this commit, if anywhere
    QString previousPath;  // the file's name in previousSha
    bool boundary = false;

    // Lines changed in the working tree but not committed are attributed to the all-zero id.
    bool isUncommitted() const { return !sha.isEmpty() && sha.count(QLatin1Char('0')) == sha.size(); }
};

struct BlameLine {
    int commit = -1;   // index into Blame::commits
    int origLine = 0;  // 1-based line number in that commit's version of the file
    QString path;      // the file's name in that commit
    QString text;
};

struct Blame {
    QVector<BlameCommit> commits;
    QVector<BlameLine> lines;
    bool binary = false;
    QString error;  // set when git blame failed

    bool ok() const { return error.isEmpty(); }
};

struct BlameOptions {
    bool ignoreWhitespace = false;  // -w
    bool detectMoves = false;       // -M: lines moved or copied within the file keep their origin
    bool detectCopies = false;      // -C: also lines from other files changed in the same commit
};

struct HeadInfo {
    QString branch;  // empty when detached
    QString sha;     // empty when unborn
    bool detached = false;
    bool unborn = false;
};

}  // namespace aslgc
