// SPDX-License-Identifier: MIT
#include "Repository.h"

#include "Diff.h"
#include "Parsers.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>

namespace aslgc {

namespace {

QStringList diffBaseArgs() {
    return {QStringLiteral("--no-color"), QStringLiteral("--no-ext-diff"), QStringLiteral("--src-prefix=a/"),
            QStringLiteral("--dst-prefix=b/")};
}

QStringList pathspec(const FileChange& file) {
    QStringList paths{QStringLiteral("--")};
    if (!file.oldPath.isEmpty() && file.oldPath != file.path) paths << file.oldPath;
    paths << file.path;
    return paths;
}

// Pathspecs without wildcard or magic interpretation, so any file name works.
QString literal(const QString& path) { return QStringLiteral(":(literal)") + path; }
QString folderPathspec(const QString& folder) {
    return folder.isEmpty() ? QStringLiteral(".")
                            : literal(folder.endsWith(QLatin1Char('/')) ? folder : folder + QLatin1Char('/'));
}

QString readFileTrimmed(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll()).trimmed();
}

}  // namespace

Repository::Repository(const QString& root) : m_root(QDir(root).absolutePath()), m_git(m_root) {
    const GitResult r = m_git.run({QStringLiteral("rev-parse"), QStringLiteral("--absolute-git-dir")});
    if (r.ok()) m_gitDir = r.outText().trimmed();
}

QString Repository::resolveRoot(const QString& path, QString* error) {
    if (!QFileInfo(path).isDir()) {
        if (error) *error = QStringLiteral("'%1' is not a directory.").arg(path);
        return {};
    }
    const GitResult r = Git(path).run({QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel")});
    if (!r.ok()) {
        if (error) *error = r.message();
        return {};
    }
    return r.outText().trimmed();
}

QString Repository::name() const { return QFileInfo(m_root).fileName(); }

HeadInfo Repository::head() const {
    HeadInfo h;
    const GitResult sym = m_git.run({QStringLiteral("symbolic-ref"), QStringLiteral("-q"), QStringLiteral("HEAD")});
    if (sym.ok()) {
        const QString ref = sym.outText().trimmed();
        h.branch = ref.startsWith(QLatin1String("refs/heads/")) ? ref.mid(11) : ref;
    } else {
        h.detached = true;
    }
    const GitResult rev = m_git.run(
        {QStringLiteral("rev-parse"), QStringLiteral("-q"), QStringLiteral("--verify"), QStringLiteral("HEAD^{commit}")});
    if (rev.ok())
        h.sha = rev.outText().trimmed();
    else
        h.unborn = true;
    return h;
}

QVector<Ref> Repository::refs(QByteArray* raw) const {
    const GitResult r = m_git.run({QStringLiteral("for-each-ref"), QString::fromLatin1(parse::kRefFormat),
                                   QStringLiteral("refs/heads"), QStringLiteral("refs/remotes"), QStringLiteral("refs/tags")});
    if (raw) *raw = r.out;
    return r.ok() ? parse::refs(r.out) : QVector<Ref>{};
}

QVector<Stash> Repository::stashes(QByteArray* raw) const {
    const GitResult r = m_git.run({QStringLiteral("stash"), QStringLiteral("list"), QString::fromLatin1(parse::kStashFormat)});
    if (raw) *raw = r.out;
    return r.ok() ? parse::stashes(r.out) : QVector<Stash>{};
}

QVector<Remote> Repository::remotes() const {
    const GitResult r = m_git.run({QStringLiteral("remote"), QStringLiteral("-v")});
    return r.ok() ? parse::remotes(r.out) : QVector<Remote>{};
}

WorkingStatus Repository::status() const {
    const GitResult r = m_git.run({QStringLiteral("status"), QStringLiteral("--porcelain=v2"), QStringLiteral("-z"),
                                   QStringLiteral("--untracked-files=all")});
    return r.ok() ? parse::status(r.out) : WorkingStatus{};
}

Operation Repository::operation() const {
    if (m_gitDir.isEmpty()) return Operation::None;
    const QDir d(m_gitDir);
    if (d.exists(QStringLiteral("rebase-merge"))) return Operation::Rebase;
    if (d.exists(QStringLiteral("rebase-apply"))) {
        return d.exists(QStringLiteral("rebase-apply/applying")) ? Operation::ApplyMailbox : Operation::Rebase;
    }
    if (d.exists(QStringLiteral("MERGE_HEAD"))) return Operation::Merge;
    if (d.exists(QStringLiteral("CHERRY_PICK_HEAD"))) return Operation::CherryPick;
    if (d.exists(QStringLiteral("REVERT_HEAD"))) return Operation::Revert;
    return Operation::None;
}

QString Repository::rebaseProgress() const {
    const QDir d(m_gitDir);
    for (const auto& [dir, cur, end] :
         {std::tuple{"rebase-merge", "msgnum", "end"}, std::tuple{"rebase-apply", "next", "last"}}) {
        if (!d.exists(QLatin1String(dir))) continue;
        const QString a = readFileTrimmed(d.filePath(QLatin1String(dir) + QLatin1Char('/') + QLatin1String(cur)));
        const QString b = readFileTrimmed(d.filePath(QLatin1String(dir) + QLatin1Char('/') + QLatin1String(end)));
        if (!a.isEmpty() && !b.isEmpty()) return a + QLatin1Char('/') + b;
    }
    return {};
}

QVector<Commit> Repository::log(const LogOptions& options, const HeadInfo& head, bool hasRefs) const {
    if (head.unborn && !hasRefs) return {};
    QStringList args{QStringLiteral("log"), QString::fromLatin1(parse::kLogFormat), QStringLiteral("--date-order"),
                     QStringLiteral("--max-count=%1").arg(options.maxCount)};
    if (options.allRefs) args << QStringLiteral("--branches") << QStringLiteral("--remotes") << QStringLiteral("--tags");
    if (!head.unborn) args << QStringLiteral("HEAD");
    args << QStringLiteral("--");
    const GitResult r = m_git.run(args);
    return r.ok() ? parse::log(r.out) : QVector<Commit>{};
}

CommitInfo Repository::commitInfo(const QString& sha) const {
    const GitResult r = m_git.run({QStringLiteral("log"), QStringLiteral("-1"), QStringLiteral("--no-walk"),
                                   QStringLiteral("--format=%H%x1f%P%x1f%an%x1f%ae%x1f%at%x1f%cn%x1f%ce%x1f%ct%x1f%B%x1e"), sha,
                                   QStringLiteral("--")});
    return r.ok() ? parse::commitInfo(r.out) : CommitInfo{};
}

QVector<FileChange> Repository::commitChanges(const QString& sha, const QStringList& parents, const QString& folder) const {
    QStringList args{QStringLiteral("diff-tree"), QStringLiteral("--no-commit-id"), QStringLiteral("-r"), QStringLiteral("-z"),
                     QStringLiteral("-M"),        QStringLiteral("--name-status")};
    if (parents.isEmpty())
        args << QStringLiteral("--root") << sha;
    else
        args << parents.first() << sha;
    if (!folder.isEmpty()) args << QStringLiteral("--") << folderPathspec(folder);
    const GitResult r = m_git.run(args);
    return r.ok() ? parse::nameStatus(r.out) : QVector<FileChange>{};
}

QByteArray Repository::commitDiff(const QString& sha, const QStringList& parents, const FileChange& file) const {
    QStringList args{QStringLiteral("diff-tree"), QStringLiteral("--no-commit-id"), QStringLiteral("-r"), QStringLiteral("-p"),
                     QStringLiteral("-M")};
    args += diffBaseArgs();
    if (parents.isEmpty())
        args << QStringLiteral("--root") << sha;
    else
        args << parents.first() << sha;
    args += pathspec(file);
    return m_git.run(args).out;
}

QByteArray Repository::workingDiff(const FileChange& file, bool staged) const {
    QStringList args{QStringLiteral("diff")};
    args += diffBaseArgs();
    if (!staged && file.isUntracked()) {
        args << QStringLiteral("--no-index") << QStringLiteral("--") << QStringLiteral("/dev/null") << file.path;
        return m_git.run(args).out;  // exits with 1 when there are differences
    }
    if (staged) args << QStringLiteral("--cached");
    args << QStringLiteral("-M");
    args += pathspec(file);
    return m_git.run(args).out;
}

QString Repository::headMessage() const {
    const GitResult r = m_git.run({QStringLiteral("log"), QStringLiteral("-1"), QStringLiteral("--format=%B")});
    return r.ok() ? r.outText().trimmed() : QString();
}

QStringList Repository::trackedFiles() const {
    const GitResult r = m_git.run({QStringLiteral("ls-files"), QStringLiteral("-z")});
    QStringList out;
    if (!r.ok()) return out;
    for (const QByteArray& p : r.out.split('\0')) {
        if (!p.isEmpty()) out << QString::fromUtf8(p);
    }
    return out;
}

QVector<FileLogEntry> Repository::fileLog(const QString& path, bool folder, int maxCount) const {
    QStringList args{QStringLiteral("log"), QString::fromLatin1(parse::kFileLogFormat), QStringLiteral("-z"),
                     QStringLiteral("--max-count=%1").arg(maxCount)};
    if (!folder) {
        args << QStringLiteral("--follow") << QStringLiteral("-M") << QStringLiteral("--name-status")
             << QStringLiteral("--diff-merges=first-parent");
    }
    args << QStringLiteral("HEAD") << QStringLiteral("--") << (folder ? folderPathspec(path) : literal(path));
    const GitResult r = m_git.run(args);
    if (!r.ok()) return {};
    QVector<FileLogEntry> entries = parse::fileLog(r.out);
    if (folder) return entries;

    // Keep only the followed file's entry; walking back in time, a rename switches to the old name.
    QString current = path;
    for (FileLogEntry& e : entries) {
        FileChange match{current, {}, 'M', {}, false};
        for (const FileChange& c : e.changes) {
            if (c.path == current || e.changes.size() == 1) {
                match = c;
                break;
            }
        }
        e.changes = {match};
        if ((match.status == 'R' || match.status == 'C') && !match.oldPath.isEmpty())
            current = match.oldPath;
        else
            current = match.path;
    }
    return entries;
}

QVector<LineLogEntry> Repository::lineLog(const QString& path, int first, int last, const QString& rev, int maxCount,
                                          QString* error) const {
    if (first < 1 || last < first) {
        if (error) *error = QStringLiteral("Invalid line range %1–%2.").arg(first).arg(last);
        return {};
    }
    QString at = rev;
    if (at.isEmpty()) {
        QStringList args{QStringLiteral("diff")};
        args += diffBaseArgs();
        args << QStringLiteral("HEAD") << QStringLiteral("--") << path;
        const QVector<FileDiff> local = parseDiff(m_git.run(args).out);
        if (!local.isEmpty()) {
            first = mapToOldLine(local.first(), first);
            last = std::max(first, mapToOldLine(local.first(), last));
        }
        at = QStringLiteral("HEAD");
    }
    QStringList args{QStringLiteral("log"), QString::fromLatin1(parse::kFileLogFormat),
                     QStringLiteral("--max-count=%1").arg(maxCount), QStringLiteral("-L%1,%2:%3").arg(first).arg(last).arg(path)};
    args += diffBaseArgs();
    args << at << QStringLiteral("--");
    const GitResult r = m_git.run(args);
    if (!r.ok()) {
        if (error) *error = r.message();
        return {};
    }
    return parse::lineLog(r.out);
}

Blame Repository::blame(const QString& path, const QString& rev) const {
    QStringList args{QStringLiteral("blame"), QStringLiteral("--porcelain")};
    if (!rev.isEmpty()) args << rev;
    args << QStringLiteral("--") << path;
    const GitResult r = m_git.run(args);
    if (!r.ok()) {
        Blame b;
        b.error = r.message();
        return b;
    }
    return parse::blame(r.out);
}

QString Repository::configValue(const QString& key) const {
    const GitResult r = m_git.run({QStringLiteral("config"), QStringLiteral("--get"), key});
    return r.ok() ? r.outText().trimmed() : QString();
}

RepoSnapshot loadSnapshot(const Repository& repo, const LogOptions& options, const QByteArray& previousFingerprint) {
    RepoSnapshot s;
    if (!repo.isValid() || repo.gitDir().isEmpty()) {
        s.error = QStringLiteral("Not a git repository: %1").arg(repo.root());
        return s;
    }
    QByteArray refsRaw;
    QByteArray stashRaw;
    s.head = repo.head();
    s.refs = repo.refs(&refsRaw);
    s.stashes = repo.stashes(&stashRaw);
    s.remotes = repo.remotes();
    s.status = repo.status();
    s.operation = repo.operation();
    s.gitFlow = flow::loadConfig(repo);
    if (s.operation == Operation::Rebase) {
        s.rebaseProgress = repo.rebaseProgress();
        s.rebaseEditSha = readFileTrimmed(QDir(repo.gitDir()).filePath(QStringLiteral("rebase-merge/amend")));
    }

    s.fingerprint = refsRaw;
    s.fingerprint += '\0' + s.head.sha.toLatin1() + '\0' + s.head.branch.toUtf8() + '\0' + stashRaw;
    s.fingerprint += QStringLiteral("\0%1:%2").arg(options.allRefs).arg(options.maxCount).toLatin1();

    if (s.fingerprint != previousFingerprint) {
        s.commits = repo.log(options, s.head, !s.refs.isEmpty());
        s.graph = computeGraph(s.commits);
        s.hasLog = true;
    }
    return s;
}

}  // namespace aslgc
