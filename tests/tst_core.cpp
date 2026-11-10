// SPDX-License-Identifier: MIT
// Integration tests for the git core: run real git against temporary repositories.

#include "git/Diff.h"
#include "git/GitFlow.h"
#include "git/Graph.h"
#include "git/Parsers.h"
#include "git/Rebase.h"
#include "git/Repository.h"
#include "hosting/Account.h"
#include "hosting/RemoteUrl.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace aslgc;

class CoreTest : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_tmp;
    int m_counter = 0;

    QString newRepo() {
        const QString dir = m_tmp.filePath(QStringLiteral("repo%1").arg(++m_counter));
        QDir().mkpath(dir);
        const GitResult r =
            Git(dir).run({QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("-b"), QStringLiteral("main")});
        if (!r.ok()) qFatal("git init failed: %s", qPrintable(r.message()));
        return dir;
    }
    static void write(const QString& dir, const QString& name, const QByteArray& data) {
        QFile f(QDir(dir).filePath(name));
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(data);
    }
    static QByteArray read(const QString& dir, const QString& name) {
        QFile f(QDir(dir).filePath(name));
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }
    static GitResult git(const QString& dir, const QStringList& args, const QByteArray& input = {}) {
        return Git(dir).run(args, input);
    }
    static void commitAll(const QString& dir, const QString& msg) {
        QVERIFY(git(dir, {QStringLiteral("add"), QStringLiteral("-A")}).ok());
        const GitResult r = git(dir, {QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-m"), msg});
        QVERIFY2(r.ok(), qPrintable(r.message()));
    }
    static QByteArray numbered(int n, const QString& prefix = QStringLiteral("line")) {
        QByteArray out;
        for (int i = 1; i <= n; ++i) out += QStringLiteral("%1 %2\n").arg(prefix).arg(i).toUtf8();
        return out;
    }
    static FileDiff onlyDiff(const QByteArray& data) {
        const QVector<FileDiff> d = parseDiff(data);
        return d.isEmpty() ? FileDiff{} : d.first();
    }
    static int lineIndex(const DiffHunk& h, char type, const QByteArray& raw) {
        for (int i = 0; i < h.lines.size(); ++i) {
            if (h.lines[i].type == type && h.lines[i].raw == raw) return i;
        }
        return -1;
    }
    static GitResult apply(const QString& dir, const QByteArray& patch, const QStringList& extra) {
        return git(dir,
                   QStringList{QStringLiteral("apply"), QStringLiteral("--recount")} + extra + QStringList{QStringLiteral("-")},
                   patch);
    }

private slots:
    void initTestCase() {
        QVERIFY(m_tmp.isValid());
        qputenv("GIT_CONFIG_GLOBAL", "/dev/null");
        qputenv("GIT_CONFIG_NOSYSTEM", "1");
        qputenv("GIT_AUTHOR_NAME", "Test Author");
        qputenv("GIT_AUTHOR_EMAIL", "author@example.com");
        qputenv("GIT_COMMITTER_NAME", "Test Author");
        qputenv("GIT_COMMITTER_EMAIL", "author@example.com");
    }

    void statusParsing() {
        const QString dir = newRepo();
        write(dir, QStringLiteral("a.txt"), "a\n");
        write(dir, QStringLiteral("b.txt"), "b\n");
        write(dir, QStringLiteral("old name.txt"), numbered(20));
        commitAll(dir, QStringLiteral("init"));

        write(dir, QStringLiteral("a.txt"), "a changed\n");  // unstaged M
        write(dir, QStringLiteral("b.txt"), "b staged\n");   // staged M
        QVERIFY(git(dir, {QStringLiteral("add"), QStringLiteral("b.txt")}).ok());
        write(dir, QStringLiteral("b.txt"), "b staged then edited\n");  // + unstaged M
        QVERIFY(git(dir, {QStringLiteral("mv"), QStringLiteral("old name.txt"), QStringLiteral("new name.txt")}).ok());  // R
        QDir(dir).mkpath(QStringLiteral("dir with space"));  // ? (untracked)
        write(dir, QStringLiteral("dir with space/untracked é.txt"), "x\n");

        const WorkingStatus st = Repository(dir).status();
        auto find = [](const QVector<FileChange>& v, const QString& p) -> const FileChange* {
            for (const auto& f : v) {
                if (f.path == p) return &f;
            }
            return nullptr;
        };
        QVERIFY(find(st.unstaged, QStringLiteral("a.txt")));
        QCOMPARE(find(st.unstaged, QStringLiteral("a.txt"))->status, 'M');
        QVERIFY(find(st.staged, QStringLiteral("b.txt")));
        QVERIFY(find(st.unstaged, QStringLiteral("b.txt")));
        const FileChange* renamed = find(st.staged, QStringLiteral("new name.txt"));
        QVERIFY(renamed);
        QCOMPARE(renamed->status, 'R');
        QCOMPARE(renamed->oldPath, QStringLiteral("old name.txt"));
        const FileChange* untracked = find(st.unstaged, QStringLiteral("dir with space/untracked é.txt"));
        QVERIFY(untracked);
        QVERIFY(untracked->isUntracked());
    }

    void hunkStaging() {
        const QString dir = newRepo();
        write(dir, QStringLiteral("f.txt"), numbered(40));
        commitAll(dir, QStringLiteral("init"));
        QByteArray content = numbered(40);
        content.replace("line 5\n", "changed 5\n");
        content.replace("line 35\n", "changed 35\n");
        write(dir, QStringLiteral("f.txt"), content);

        const Repository repo(dir);
        const FileChange fc{QStringLiteral("f.txt"), {}, 'M', {}, false};
        const FileDiff d = onlyDiff(repo.workingDiff(fc, false));
        QCOMPARE(d.hunks.size(), 2);
        const GitResult r = apply(dir, hunkPatch(d, 0), {QStringLiteral("--cached")});
        QVERIFY2(r.ok(), qPrintable(r.message()));
        const QByteArray staged = git(dir, {QStringLiteral("diff"), QStringLiteral("--cached")}).out;
        QVERIFY(staged.contains("+changed 5"));
        QVERIFY(!staged.contains("changed 35"));
        const QByteArray unstaged = git(dir, {QStringLiteral("diff")}).out;
        QVERIFY(unstaged.contains("+changed 35"));
        QVERIFY(!unstaged.contains("changed 5\n"));
    }

    void lineStagingRoundTrip() {
        const QString dir = newRepo();
        write(dir, QStringLiteral("f.txt"), "a\nb\nc\nd\ne\nf\n");
        commitAll(dir, QStringLiteral("init"));
        write(dir, QStringLiteral("f.txt"), "a\nb\nX1\nX2\nd\ne\nf\n");  // c -> X1, X2
        const Repository repo(dir);
        const FileChange fc{QStringLiteral("f.txt"), {}, 'M', {}, false};

        // Stage only "+X1": index becomes a b c X1 d e f
        FileDiff d = onlyDiff(repo.workingDiff(fc, false));
        QCOMPARE(d.hunks.size(), 1);
        const int x1 = lineIndex(d.hunks[0], '+', "X1");
        QVERIFY(x1 >= 0);
        GitResult r = apply(dir, linesPatch(d, {lineKey(0, x1)}, false), {QStringLiteral("--cached")});
        QVERIFY2(r.ok(), qPrintable(r.message()));
        QCOMPARE(git(dir, {QStringLiteral("show"), QStringLiteral(":f.txt")}).out, QByteArray("a\nb\nc\nX1\nd\ne\nf\n"));

        // Unstage "+X1" again from the staged diff: index returns to HEAD.
        d = onlyDiff(repo.workingDiff(fc, true));
        const int sx1 = lineIndex(d.hunks[0], '+', "X1");
        QVERIFY(sx1 >= 0);
        r = apply(dir, linesPatch(d, {lineKey(0, sx1)}, true), {QStringLiteral("--cached"), QStringLiteral("--reverse")});
        QVERIFY2(r.ok(), qPrintable(r.message()));
        QVERIFY(git(dir, {QStringLiteral("diff"), QStringLiteral("--cached")}).out.isEmpty());

        // Discard only the removal of "c" from the worktree: c comes back, X1/X2 stay.
        d = onlyDiff(repo.workingDiff(fc, false));
        const int c = lineIndex(d.hunks[0], '-', "c");
        QVERIFY(c >= 0);
        r = apply(dir, linesPatch(d, {lineKey(0, c)}, true), {QStringLiteral("--reverse")});
        QVERIFY2(r.ok(), qPrintable(r.message()));
        QCOMPARE(read(dir, QStringLiteral("f.txt")), QByteArray("a\nb\nc\nX1\nX2\nd\ne\nf\n"));
    }

    void partialStageOfDeletedFile() {
        const QString dir = newRepo();
        write(dir, QStringLiteral("gone.txt"), "one\ntwo\nthree\n");
        commitAll(dir, QStringLiteral("init"));
        QVERIFY(QFile::remove(QDir(dir).filePath(QStringLiteral("gone.txt"))));
        const Repository repo(dir);
        const FileChange fc{QStringLiteral("gone.txt"), {}, 'D', {}, false};
        const FileDiff d = onlyDiff(repo.workingDiff(fc, false));
        QVERIFY(d.deletedFile);
        const int one = lineIndex(d.hunks[0], '-', "one");
        const GitResult r = apply(dir, linesPatch(d, {lineKey(0, one)}, false), {QStringLiteral("--cached")});
        QVERIFY2(r.ok(), qPrintable(r.message()));
        QCOMPARE(git(dir, {QStringLiteral("show"), QStringLiteral(":gone.txt")}).out, QByteArray("two\nthree\n"));
    }

    void snapshotAndLog() {
        const QString dir = newRepo();
        write(dir, QStringLiteral("f.txt"), "1\n");
        commitAll(dir, QStringLiteral("first"));
        QVERIFY(
            git(dir, {QStringLiteral("checkout"), QStringLiteral("-q"), QStringLiteral("-b"), QStringLiteral("feature")}).ok());
        write(dir, QStringLiteral("g.txt"), "g\n");
        commitAll(dir, QStringLiteral("feature work"));
        QVERIFY(git(dir, {QStringLiteral("checkout"), QStringLiteral("-q"), QStringLiteral("main")}).ok());
        write(dir, QStringLiteral("f.txt"), "2\n");
        commitAll(dir, QStringLiteral("main work"));
        QVERIFY(git(dir, {QStringLiteral("merge"), QStringLiteral("-q"), QStringLiteral("--no-ff"), QStringLiteral("--no-edit"),
                          QStringLiteral("feature")})
                    .ok());
        QVERIFY(git(dir, {QStringLiteral("tag"), QStringLiteral("-a"), QStringLiteral("v1.0"), QStringLiteral("-m"),
                          QStringLiteral("release")})
                    .ok());
        write(dir, QStringLiteral("dirty.txt"), "x\n");

        const Repository repo(dir);
        const RepoSnapshot s = loadSnapshot(repo, LogOptions{}, {});
        QVERIFY(s.error.isEmpty());
        QCOMPARE(s.head.branch, QStringLiteral("main"));
        QVERIFY(!s.head.detached);
        QCOMPARE(s.commits.size(), 4);
        QCOMPARE(s.commits.first().parents.size(), 2);
        QCOMPARE(s.graph.size(), s.commits.size());
        bool tagPeeled = false;
        for (const Ref& r : s.refs) {
            if (r.kind == RefKind::Tag && r.name == QLatin1String("v1.0")) tagPeeled = r.target == s.head.sha;
        }
        QVERIFY(tagPeeled);
        QCOMPARE(s.status.unstaged.size(), 1);

        // Unchanged refs: the commit list is not reloaded.
        const RepoSnapshot again = loadSnapshot(repo, LogOptions{}, s.fingerprint);
        QVERIFY(!again.hasLog);

        const CommitInfo info = repo.commitInfo(s.head.sha);
        QVERIFY(info.valid);
        QVERIFY(info.message.startsWith(QLatin1String("Merge branch 'feature'")));
        const QVector<FileChange> changes = repo.commitChanges(s.head.sha, info.commit.parents);
        QCOMPARE(changes.size(), 1);
        QCOMPARE(changes.first().path, QStringLiteral("g.txt"));
    }

    // File history follows renames, includes merges (compared with the first parent) and
    // skips commits that only touch other files.
    void fileHistoryFollowsRenames() {
        const QString dir = newRepo();
        QDir(dir).mkpath(QStringLiteral("d"));
        write(dir, QStringLiteral("d/a b.txt"), numbered(20));
        write(dir, QStringLiteral("other.txt"), "x\n");
        commitAll(dir, QStringLiteral("init"));
        QByteArray content = numbered(20);
        content.replace("line 5\n", "five\n");
        write(dir, QStringLiteral("d/a b.txt"), content);
        commitAll(dir, QStringLiteral("edit"));
        QVERIFY(git(dir, {QStringLiteral("mv"), QStringLiteral("d/a b.txt"), QStringLiteral("d/c d.txt")}).ok());
        commitAll(dir, QStringLiteral("rename"));
        write(dir, QStringLiteral("other.txt"), "y\n");
        commitAll(dir, QStringLiteral("unrelated"));
        QVERIFY(git(dir, {QStringLiteral("checkout"), QStringLiteral("-q"), QStringLiteral("-b"), QStringLiteral("side")}).ok());
        QByteArray side = content;
        side.replace("line 20\n", "twenty\n");
        write(dir, QStringLiteral("d/c d.txt"), side);
        commitAll(dir, QStringLiteral("side edit"));
        QVERIFY(git(dir, {QStringLiteral("checkout"), QStringLiteral("-q"), QStringLiteral("main")}).ok());
        content.replace("line 1\n", "one\n");
        write(dir, QStringLiteral("d/c d.txt"), content);
        commitAll(dir, QStringLiteral("main edit"));
        const GitResult merged = git(dir, {QStringLiteral("merge"), QStringLiteral("-q"), QStringLiteral("--no-ff"),
                                           QStringLiteral("-m"), QStringLiteral("merge side"), QStringLiteral("side")});
        QVERIFY2(merged.ok(), qPrintable(merged.message()));

        const Repository repo(dir);
        const QVector<FileLogEntry> log = repo.fileLog(QStringLiteral("d/c d.txt"), false);
        QStringList subjects;
        for (const FileLogEntry& e : log) {
            subjects << e.commit.subject;
            QCOMPARE(e.changes.size(), 1);
        }
        QCOMPARE(subjects, (QStringList{QStringLiteral("merge side"), QStringLiteral("main edit"), QStringLiteral("side edit"),
                                        QStringLiteral("rename"), QStringLiteral("edit"), QStringLiteral("init")}));
        QCOMPARE(log[0].changes.first().path, QStringLiteral("d/c d.txt"));
        QCOMPARE(log[0].commit.parents.size(), 2);
        const FileChange& rename = log[3].changes.first();
        QCOMPARE(rename.status, 'R');
        QCOMPARE(rename.oldPath, QStringLiteral("d/a b.txt"));
        QCOMPARE(rename.path, QStringLiteral("d/c d.txt"));
        QCOMPARE(log[4].changes.first().path, QStringLiteral("d/a b.txt"));
        QCOMPARE(log[5].changes.first().status, 'A');

        // The merge's diff against its first parent shows what the side branch brought in.
        const FileDiff mergeDiff = onlyDiff(repo.commitDiff(log[0].commit.sha, log[0].commit.parents, log[0].changes.first()));
        QCOMPARE(mergeDiff.hunks.size(), 1);
        QVERIFY(lineIndex(mergeDiff.hunks.first(), '+', "twenty") >= 0);
        // An older entry's diff uses the file's old name.
        const FileDiff editDiff = onlyDiff(repo.commitDiff(log[4].commit.sha, log[4].commit.parents, log[4].changes.first()));
        QCOMPARE(editDiff.newPath, QStringLiteral("d/a b.txt"));
        QVERIFY(lineIndex(editDiff.hunks.first(), '+', "five") >= 0);

        QCOMPARE(repo.fileLog(QStringLiteral("d/c d.txt"), false, 2).size(), 2);
        QVERIFY(repo.fileLog(QStringLiteral("missing.txt"), false).isEmpty());
    }

    void folderHistory() {
        const QString dir = newRepo();
        QDir(dir).mkpath(QStringLiteral("src/ui"));
        write(dir, QStringLiteral("src/ui/view.cpp"), "v\n");
        write(dir, QStringLiteral("README"), "r\n");
        commitAll(dir, QStringLiteral("init"));
        write(dir, QStringLiteral("README"), "r2\n");
        commitAll(dir, QStringLiteral("docs"));
        write(dir, QStringLiteral("src/ui/view.cpp"), "v2\n");
        write(dir, QStringLiteral("src/main.cpp"), "m\n");
        write(dir, QStringLiteral("README"), "r3\n");
        commitAll(dir, QStringLiteral("code"));
        // Glob characters in names are taken literally.
        write(dir, QStringLiteral("a*.txt"), "star\n");
        commitAll(dir, QStringLiteral("star"));
        write(dir, QStringLiteral("ab.txt"), "plain\n");
        commitAll(dir, QStringLiteral("plain"));

        const Repository repo(dir);
        auto subjects = [](const QVector<FileLogEntry>& log) {
            QStringList out;
            for (const FileLogEntry& e : log) out << e.commit.subject;
            return out;
        };
        QCOMPARE(subjects(repo.fileLog(QStringLiteral("src"), true)),
                 (QStringList{QStringLiteral("code"), QStringLiteral("init")}));
        QCOMPARE(subjects(repo.fileLog(QStringLiteral("src/ui/"), true)),
                 (QStringList{QStringLiteral("code"), QStringLiteral("init")}));
        QCOMPARE(repo.fileLog(QString(), true).size(), 5);
        QCOMPARE(subjects(repo.fileLog(QStringLiteral("a*.txt"), false)), QStringList{QStringLiteral("star")});

        const QVector<FileLogEntry> src = repo.fileLog(QStringLiteral("src"), true);
        const QVector<FileChange> changes = repo.commitChanges(src[0].commit.sha, src[0].commit.parents, QStringLiteral("src"));
        QStringList paths;
        for (const FileChange& c : changes) paths << c.path;
        paths.sort();
        QCOMPARE(paths, (QStringList{QStringLiteral("src/main.cpp"), QStringLiteral("src/ui/view.cpp")}));
        QCOMPARE(repo.commitChanges(src[0].commit.sha, src[0].commit.parents).size(), 3);
        QVERIFY(repo.trackedFiles().contains(QStringLiteral("a*.txt")));
    }

    void blameAttribution() {
        const QString dir = newRepo();
        write(dir, QStringLiteral("a b.txt"), numbered(10));
        commitAll(dir, QStringLiteral("init"));
        QByteArray content = numbered(10);
        content.replace("line 4\n", "four\r\n");
        write(dir, QStringLiteral("a b.txt"), content);
        commitAll(dir, QStringLiteral("edit four"));
        QVERIFY(git(dir, {QStringLiteral("mv"), QStringLiteral("a b.txt"), QStringLiteral("c.txt")}).ok());
        commitAll(dir, QStringLiteral("rename"));
        content.replace("line 9\n", "nine (local)\n");
        write(dir, QStringLiteral("c.txt"), content);

        const Repository repo(dir);
        const QString init = git(dir, {QStringLiteral("rev-parse"), QStringLiteral("HEAD~2")}).outText().trimmed();
        const QString edit = git(dir, {QStringLiteral("rev-parse"), QStringLiteral("HEAD~1")}).outText().trimmed();

        const Blame b = repo.blame(QStringLiteral("c.txt"));
        QVERIFY2(b.ok(), qPrintable(b.error));
        QVERIFY(!b.binary);
        QCOMPARE(b.lines.size(), 10);
        auto commitOf = [&b](int line) -> const BlameCommit& { return b.commits[b.lines[line].commit]; };
        QCOMPARE(commitOf(0).sha, init);
        QCOMPARE(commitOf(0).summary, QStringLiteral("init"));
        QCOMPARE(commitOf(0).authorName, QStringLiteral("Test Author"));
        QCOMPARE(commitOf(0).authorEmail, QStringLiteral("author@example.com"));
        QVERIFY(commitOf(0).authorTime > 0);
        QCOMPARE(b.lines[0].path, QStringLiteral("a b.txt"));  // attributed under its old name
        QCOMPARE(b.lines[0].text, QStringLiteral("line 1"));
        QCOMPARE(b.lines[3].text, QStringLiteral("four"));  // CR stripped
        QCOMPARE(commitOf(3).sha, edit);
        QCOMPARE(commitOf(3).previousSha, init);
        QCOMPARE(commitOf(3).previousPath, QStringLiteral("a b.txt"));
        QCOMPARE(b.lines[3].origLine, 4);
        QVERIFY(commitOf(8).isUncommitted());
        QCOMPARE(b.lines[8].text, QStringLiteral("nine (local)"));
        QVERIFY(!commitOf(0).isUncommitted());
        QCOMPARE(b.lines[9].commit, b.lines[0].commit);  // commits are shared, not repeated

        // Blaming the previous revision of a line, under the file's name there.
        const Blame before = repo.blame(commitOf(3).previousPath, commitOf(3).previousSha);
        QVERIFY2(before.ok(), qPrintable(before.error));
        QCOMPARE(before.lines.size(), 10);
        QCOMPARE(before.lines[3].text, QStringLiteral("line 4"));
        QCOMPARE(before.commits.size(), 1);

        QVERIFY(!repo.blame(QStringLiteral("missing.txt"), QStringLiteral("HEAD")).ok());

        write(dir, QStringLiteral("bin.dat"), QByteArray("a\0b\n", 4));
        commitAll(dir, QStringLiteral("binary"));
        QVERIFY(repo.blame(QStringLiteral("bin.dat")).binary);
    }

    void compareCommits() {
        const QString dir = newRepo();
        write(dir, QStringLiteral("a.txt"), numbered(5));
        commitAll(dir, QStringLiteral("c1"));
        QByteArray a = numbered(5);
        a.replace("line 2\n", "two\n");
        write(dir, QStringLiteral("a.txt"), a);
        write(dir, QStringLiteral("b.txt"), numbered(20, QStringLiteral("b")));
        write(dir, QStringLiteral("logo.bin"), QByteArray("\x89PNG\0\x01\x02", 7));
        commitAll(dir, QStringLiteral("c2"));
        QVERIFY(git(dir, {QStringLiteral("mv"), QStringLiteral("b.txt"), QStringLiteral("c.txt")}).ok());
        write(dir, QStringLiteral("logo.bin"), QByteArray("\x89PNG\0\x03\x04", 7));
        commitAll(dir, QStringLiteral("c3"));
        const QString c1 = revOf(dir, QStringLiteral("HEAD~2"));
        const QString c2 = revOf(dir, QStringLiteral("HEAD~1"));
        const QString c3 = revOf(dir, QStringLiteral("HEAD"));
        const Repository repo(dir);
        auto statusOf = [](const QVector<FileChange>& changes) {
            QStringList out;
            for (const FileChange& f : changes)
                out << QStringLiteral("%1 %2%3").arg(QChar::fromLatin1(f.status),
                                                     f.oldPath.isEmpty() ? QString() : f.oldPath + QStringLiteral(" → "), f.path);
            out.sort();
            return out;
        };

        // Between two commits: c2's own changes are not included, the rename is.
        QCOMPARE(statusOf(repo.compareChanges(c2, c3)),
                 (QStringList{QStringLiteral("M logo.bin"), QStringLiteral("R b.txt → c.txt")}));
        // The range c2..c3 including c2 itself starts at c2's parent.
        QCOMPARE(repo.baseOf(c2), c1);
        QCOMPARE(statusOf(repo.compareChanges(repo.baseOf(c2), c3)),
                 (QStringList{QStringLiteral("A c.txt"), QStringLiteral("A logo.bin"), QStringLiteral("M a.txt")}));
        // A root commit starts at the empty tree.
        const QString empty = repo.baseOf(c1);
        QCOMPARE(git(dir, {QStringLiteral("cat-file"), QStringLiteral("-t"), empty}).outText().trimmed(), QStringLiteral("tree"));
        QCOMPARE(statusOf(repo.compareChanges(empty, c1)), QStringList{QStringLiteral("A a.txt")});

        const FileDiff aDiff = onlyDiff(repo.compareDiff(c1, c3, {QStringLiteral("a.txt"), {}, 'M', {}, false}));
        QVERIFY(lineIndex(aDiff.hunks.first(), '+', "two") >= 0);
        const FileDiff renamed =
            onlyDiff(repo.compareDiff(c2, c3, {QStringLiteral("c.txt"), QStringLiteral("b.txt"), 'R', {}, false}));
        QCOMPARE(renamed.oldPath, QStringLiteral("b.txt"));
        QCOMPARE(renamed.newPath, QStringLiteral("c.txt"));

        // With the working tree: staged and unstaged changes, but not untracked files.
        write(dir, QStringLiteral("a.txt"), a + "local\n");
        write(dir, QStringLiteral("new.txt"), "untracked\n");
        write(dir, QStringLiteral("c.txt"), numbered(21, QStringLiteral("b")));
        QVERIFY(git(dir, {QStringLiteral("add"), QStringLiteral("c.txt")}).ok());
        QCOMPARE(statusOf(repo.compareChanges(c3, {})), (QStringList{QStringLiteral("M a.txt"), QStringLiteral("M c.txt")}));
        QVERIFY(lineIndex(onlyDiff(repo.compareDiff(c3, {}, {QStringLiteral("a.txt"), {}, 'M', {}, false})).hunks.first(), '+',
                          "local") >= 0);

        // The patch of a comparison, binary file included, turns c1 into c3.
        const QByteArray patch = repo.comparePatch(c1, c3);
        QVERIFY(patch.contains("GIT binary patch"));
        const QString clone = m_tmp.filePath(QStringLiteral("compare-clone"));
        QVERIFY(git(dir, {QStringLiteral("clone"), QStringLiteral("-q"), dir, clone}).ok());
        QVERIFY(git(clone, {QStringLiteral("checkout"), QStringLiteral("-q"), c1}).ok());
        const GitResult applied = git(clone, {QStringLiteral("apply"), QStringLiteral("--index"), QStringLiteral("-")}, patch);
        QVERIFY2(applied.ok(), qPrintable(applied.message()));
        QVERIFY(git(clone, {QStringLiteral("diff"), QStringLiteral("--cached"), QStringLiteral("--quiet"), c3}).ok());

        // Patch files per commit, oldest first: `git am` replays them on c1.
        const QString patches = m_tmp.filePath(QStringLiteral("patches"));
        for (const GitCommand& cmd : formatPatchCommands({c2, c3}, patches)) {
            const GitResult r = git(dir, cmd.args);
            QVERIFY2(r.ok(), qPrintable(r.message()));
        }
        const QStringList files = QDir(patches).entryList({QStringLiteral("*.patch")}, QDir::Files, QDir::Name);
        QCOMPARE(files.size(), 2);
        QVERIFY(files[0].startsWith(QLatin1String("0001-")));
        QVERIFY(files[1].startsWith(QLatin1String("0002-")));
        QVERIFY(git(clone, {QStringLiteral("checkout"), QStringLiteral("-q"), QStringLiteral("-f"), c1}).ok());
        const GitResult am = git(clone, QStringList{QStringLiteral("am"), QStringLiteral("-q")} +
                                            QStringList{QDir(patches).filePath(files[0]), QDir(patches).filePath(files[1])});
        QVERIFY2(am.ok(), qPrintable(am.message()));
        QVERIFY(git(clone, {QStringLiteral("diff"), QStringLiteral("--quiet"), QStringLiteral("HEAD"), c3}).ok());
        QCOMPARE(git(clone, {QStringLiteral("log"), QStringLiteral("-1"), QStringLiteral("--format=%s")}).outText().trimmed(),
                 QStringLiteral("c3"));
    }

    void lineMapping() {
        // old: 1..10 → new: two lines inserted after 2, line 5 replaced by two lines, line 8 removed
        const FileDiff d = onlyDiff(
            "diff --git a/f b/f\n--- a/f\n+++ b/f\n"
            "@@ -2,2 +2,4 @@\n 2\n+new a\n+new b\n 3\n"
            "@@ -5,1 +7,2 @@\n-5\n+five\n+five b\n"
            "@@ -7,3 +10,2 @@\n 7\n-8\n 9\n");
        QCOMPARE(mapToOldLine(d, 1), 1);  // before any hunk
        QCOMPARE(mapToOldLine(d, 3), 2);  // added → the old line before it
        QCOMPARE(mapToOldLine(d, 5), 3);  // context inside a hunk
        QCOMPARE(mapToOldLine(d, 6), 4);  // between hunks: shifted by two
        QCOMPARE(mapToOldLine(d, 7), 5);  // replacement → the replaced line
        QCOMPARE(mapToOldLine(d, 8), 5);
        QCOMPARE(mapToOldLine(d, 9), 6);
        QCOMPARE(mapToOldLine(d, 11), 9);  // after a removal
        QCOMPARE(mapToOldLine(d, 12), 10);
        QCOMPARE(mapToOldLine(FileDiff{}, 4), 4);
        const FileDiff top = onlyDiff("diff --git a/f b/f\n--- a/f\n+++ b/f\n@@ -0,0 +1,2 @@\n+x\n+y\n");
        QCOMPARE(mapToOldLine(top, 1), 1);
        QCOMPARE(mapToOldLine(top, 3), 1);
    }

    void lineHistory() {
        const QString dir = newRepo();
        write(dir, QStringLiteral("a b.txt"), numbered(10));
        commitAll(dir, QStringLiteral("init"));
        QByteArray content = numbered(10);
        content.replace("line 5\n", "five\n");
        write(dir, QStringLiteral("a b.txt"), content);
        commitAll(dir, QStringLiteral("edit five"));
        content.replace("line 8\n", "eight\n");
        write(dir, QStringLiteral("a b.txt"), content);
        commitAll(dir, QStringLiteral("edit eight"));
        QVERIFY(git(dir, {QStringLiteral("mv"), QStringLiteral("a b.txt"), QStringLiteral("c.txt")}).ok());
        commitAll(dir, QStringLiteral("rename"));
        content.replace("line 2\n", "two\n");
        write(dir, QStringLiteral("c.txt"), content);
        commitAll(dir, QStringLiteral("edit two"));

        const Repository repo(dir);
        auto subjects = [](const QVector<LineLogEntry>& log) {
            QStringList out;
            for (const LineLogEntry& e : log) out << e.commit.subject;
            return out;
        };
        QString error;
        const QVector<LineLogEntry> five = repo.lineLog(QStringLiteral("c.txt"), 5, 5, QStringLiteral("HEAD"), 100, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(subjects(five), (QStringList{QStringLiteral("edit five"), QStringLiteral("init")}));
        QCOMPARE(five[0].diff.newPath, QStringLiteral("a b.txt"));  // followed through the rename
        QCOMPARE(five[0].diff.hunks.size(), 1);
        QVERIFY(lineIndex(five[0].diff.hunks.first(), '+', "five") >= 0);
        QVERIFY(lineIndex(five[0].diff.hunks.first(), '-', "line 5") >= 0);
        QVERIFY(five[1].diff.newFile);
        QCOMPARE(subjects(repo.lineLog(QStringLiteral("c.txt"), 7, 9, QStringLiteral("HEAD"))),
                 (QStringList{QStringLiteral("edit eight"), QStringLiteral("init")}));
        QCOMPARE(subjects(repo.lineLog(QStringLiteral("c.txt"), 2, 2, QStringLiteral("HEAD"))),
                 (QStringList{QStringLiteral("edit two"), QStringLiteral("init")}));
        // As of an older revision, under the file's old name.
        QCOMPARE(subjects(repo.lineLog(QStringLiteral("a b.txt"), 8, 8, QStringLiteral("HEAD~2"))),
                 (QStringList{QStringLiteral("edit eight"), QStringLiteral("init")}));

        // Working tree lines are translated to HEAD's: three new lines at the top move line 5 to 8.
        write(dir, QStringLiteral("c.txt"), "new 1\nnew 2\nnew 3\n" + content);
        QCOMPARE(subjects(repo.lineLog(QStringLiteral("c.txt"), 8, 8)),
                 (QStringList{QStringLiteral("edit five"), QStringLiteral("init")}));
        QCOMPARE(subjects(repo.lineLog(QStringLiteral("c.txt"), 8, 8, QStringLiteral("HEAD"))),
                 (QStringList{QStringLiteral("edit eight"), QStringLiteral("init")}));

        QVERIFY(repo.lineLog(QStringLiteral("c.txt"), 50, 60, QStringLiteral("HEAD"), 100, &error).isEmpty());
        QVERIFY(!error.isEmpty());
        error.clear();
        QVERIFY(repo.lineLog(QStringLiteral("c.txt"), 5, 3, QStringLiteral("HEAD"), 100, &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }

    // ---------------------------------------------------------------- git-flow

    static GitResult runPlan(const QString& dir, const flow::Plan& plan) {
        GitResult last;
        last.exitCode = 0;
        for (const GitCommand& c : plan.commands) {
            last = git(dir, c.args, c.input);
            if (!last.ok()) break;
        }
        return last;
    }
    static QString currentBranch(const QString& dir) {
        return git(dir, {QStringLiteral("symbolic-ref"), QStringLiteral("--short"), QStringLiteral("HEAD")}).outText().trimmed();
    }
    static QString revOf(const QString& dir, const QString& rev) {
        return git(dir, {QStringLiteral("rev-parse"), QStringLiteral("-q"), QStringLiteral("--verify"), rev}).outText().trimmed();
    }
    static int parentCount(const QString& dir, const QString& rev) {
        return int(git(dir, {QStringLiteral("rev-list"), QStringLiteral("--parents"), QStringLiteral("-n1"), rev})
                       .outText()
                       .trimmed()
                       .split(QLatin1Char(' '))
                       .size()) -
               1;
    }
    static bool contains(const QString& dir, const QString& branch, const QString& commit) {
        return git(dir, {QStringLiteral("merge-base"), QStringLiteral("--is-ancestor"), commit, branch}).exitCode == 0;
    }
    flow::Config initFlow(const QString& dir) {
        flow::Config c;
        c.production = QStringLiteral("main");
        c.develop = QStringLiteral("develop");
        c.versionTagPrefix = QStringLiteral("v");
        const Repository repo(dir);
        const flow::Plan init = flow::planInit(repo, c, {}, true);
        if (!init.ok()) qFatal("planInit: %s", qPrintable(init.error));
        const GitResult r = runPlan(dir, init);
        if (!r.ok()) qFatal("init failed: %s", qPrintable(r.message()));
        return flow::loadConfig(repo);
    }

    void gitFlowFeatures() {
        const QString dir = newRepo();
        write(dir, QStringLiteral("a.txt"), "a\n");
        commitAll(dir, QStringLiteral("init"));
        const Repository repo(dir);
        QVERIFY(!flow::loadConfig(repo).isInitialized());
        QVERIFY(!flow::planStart(repo, flow::loadConfig(repo), flow::Kind::Feature, QStringLiteral("x")).ok());

        const flow::Config c = initFlow(dir);
        QVERIFY(c.isInitialized());
        QCOMPARE(c.production, QStringLiteral("main"));
        QCOMPARE(c.develop, QStringLiteral("develop"));
        QCOMPARE(c.featurePrefix, QStringLiteral("feature/"));
        QCOMPARE(c.versionTagPrefix, QStringLiteral("v"));
        QCOMPARE(currentBranch(dir), QStringLiteral("develop"));
        // The git-flow command line tools read the same keys.
        QCOMPARE(repo.configValue(QStringLiteral("gitflow.branch.master")), QStringLiteral("main"));
        QCOMPARE(repo.configValue(QStringLiteral("gitflow.prefix.release")), QStringLiteral("release/"));

        flow::Kind kind = flow::Kind::Release;
        QString name;
        QVERIFY(c.classify(QStringLiteral("feature/log in/form"), &kind, &name));
        QCOMPARE(kind, flow::Kind::Feature);
        QCOMPARE(name, QStringLiteral("log in/form"));
        QVERIFY(c.classify(QStringLiteral("hotfix/1.0.1"), &kind, &name));
        QCOMPARE(kind, flow::Kind::Hotfix);
        QVERIFY(!c.classify(QStringLiteral("feature/"), nullptr, nullptr));
        QVERIFY(!c.classify(QStringLiteral("develop"), nullptr, nullptr));

        // A single commit is fast-forwarded into develop.
        QVERIFY(runPlan(dir, flow::planStart(repo, c, flow::Kind::Feature, QStringLiteral("login"))).ok());
        QCOMPARE(currentBranch(dir), QStringLiteral("feature/login"));
        write(dir, QStringLiteral("login.txt"), "form\n");
        commitAll(dir, QStringLiteral("login form"));
        const QString tip = revOf(dir, QStringLiteral("HEAD"));
        flow::Plan finish = flow::planFinish(repo, c, flow::Kind::Feature, QStringLiteral("login"));
        QVERIFY2(finish.ok(), qPrintable(finish.error));
        QVERIFY(finish.pushArgs.isEmpty());
        QVERIFY(runPlan(dir, finish).ok());
        QCOMPARE(currentBranch(dir), QStringLiteral("develop"));
        QCOMPARE(revOf(dir, QStringLiteral("develop")), tip);
        QVERIFY(revOf(dir, QStringLiteral("refs/heads/feature/login")).isEmpty());

        // Several commits get a merge commit.
        QVERIFY(runPlan(dir, flow::planStart(repo, c, flow::Kind::Feature, QStringLiteral("search"))).ok());
        write(dir, QStringLiteral("search.txt"), "1\n");
        commitAll(dir, QStringLiteral("search 1"));
        write(dir, QStringLiteral("search.txt"), "2\n");
        commitAll(dir, QStringLiteral("search 2"));
        QVERIFY(runPlan(dir, flow::planFinish(repo, c, flow::Kind::Feature, QStringLiteral("search"))).ok());
        QCOMPARE(parentCount(dir, QStringLiteral("develop")), 2);
        QVERIFY(repo.headMessage().contains(QLatin1String("feature/search")));

        // Squashed: one ordinary commit, and the unmerged branch is still deleted.
        QVERIFY(runPlan(dir, flow::planStart(repo, c, flow::Kind::Bugfix, QStringLiteral("crash"))).ok());
        QCOMPARE(currentBranch(dir), QStringLiteral("bugfix/crash"));
        write(dir, QStringLiteral("crash.txt"), "1\n");
        commitAll(dir, QStringLiteral("crash 1"));
        write(dir, QStringLiteral("crash.txt"), "2\n");
        commitAll(dir, QStringLiteral("crash 2"));
        flow::FinishOptions squash;
        squash.squash = true;
        const GitResult squashed = runPlan(dir, flow::planFinish(repo, c, flow::Kind::Bugfix, QStringLiteral("crash"), squash));
        QVERIFY2(squashed.ok(), qPrintable(squashed.message()));
        QCOMPARE(parentCount(dir, QStringLiteral("develop")), 1);
        QVERIFY(repo.headMessage().startsWith(QLatin1String("Squashed commit")));
        QCOMPARE(read(dir, QStringLiteral("crash.txt")), QByteArray("2\n"));
        QVERIFY(revOf(dir, QStringLiteral("refs/heads/bugfix/crash")).isEmpty());

        // Keeping the branch.
        QVERIFY(runPlan(dir, flow::planStart(repo, c, flow::Kind::Feature, QStringLiteral("kept"))).ok());
        write(dir, QStringLiteral("kept.txt"), "k\n");
        commitAll(dir, QStringLiteral("kept"));
        flow::FinishOptions keep;
        keep.keepBranch = true;
        keep.noFastForward = true;
        QVERIFY(runPlan(dir, flow::planFinish(repo, c, flow::Kind::Feature, QStringLiteral("kept"), keep)).ok());
        QVERIFY(!revOf(dir, QStringLiteral("refs/heads/feature/kept")).isEmpty());
        QCOMPARE(parentCount(dir, QStringLiteral("develop")), 2);

        // Refusals
        QVERIFY(!flow::planStart(repo, c, flow::Kind::Feature, QStringLiteral("two words")).ok());
        QVERIFY(!flow::planStart(repo, c, flow::Kind::Feature, QStringLiteral("  ")).ok());
        QVERIFY(!flow::planStart(repo, c, flow::Kind::Feature, QStringLiteral("kept")).ok());  // exists
        QVERIFY(!flow::planStart(repo, c, flow::Kind::Feature, QStringLiteral("x"), QStringLiteral("nope")).ok());
        QVERIFY(!flow::planFinish(repo, c, flow::Kind::Feature, QStringLiteral("missing")).ok());
        write(dir, QStringLiteral("a.txt"), "dirty\n");
        const flow::Plan dirty = flow::planFinish(repo, c, flow::Kind::Feature, QStringLiteral("kept"));
        QVERIFY(dirty.error.contains(QLatin1String("uncommitted")));
        QVERIFY(git(dir, {QStringLiteral("checkout"), QStringLiteral("--"), QStringLiteral("a.txt")}).ok());
        write(dir, QStringLiteral("untracked.txt"), "u\n");  // untracked files don't block
        QVERIFY(flow::planFinish(repo, c, flow::Kind::Feature, QStringLiteral("kept")).ok());

        // Removing the settings; a second removal has nothing to do.
        git(dir, {QStringLiteral("config"), QStringLiteral("--local"), QStringLiteral("gitflow.Feature.finish.squash"),
                  QStringLiteral("true")});
        const flow::Plan remove = flow::planRemove(flow::loadConfig(repo));
        QVERIFY2(remove.ok(), qPrintable(remove.error));
        QVERIFY(runPlan(dir, remove).ok());
        QVERIFY(!flow::loadConfig(repo).isInitialized());
        QVERIFY(flow::loadConfig(repo).sections.isEmpty());
        QVERIFY(!flow::planRemove(flow::loadConfig(repo)).ok());
    }

    void gitFlowReleasesAndHotfixes() {
        const QString dir = newRepo();
        write(dir, QStringLiteral("a.txt"), numbered(10));
        commitAll(dir, QStringLiteral("init"));
        const Repository repo(dir);
        const flow::Config c = initFlow(dir);
        write(dir, QStringLiteral("dev.txt"), "d\n");
        commitAll(dir, QStringLiteral("dev work"));

        QVERIFY(runPlan(dir, flow::planStart(repo, c, flow::Kind::Release, QStringLiteral("1.0"))).ok());
        QCOMPARE(currentBranch(dir), QStringLiteral("release/1.0"));
        QVERIFY(
            flow::planStart(repo, c, flow::Kind::Release, QStringLiteral("2.0")).error.contains(QLatin1String("release/1.0")));
        write(dir, QStringLiteral("VERSION"), "1.0\n");
        commitAll(dir, QStringLiteral("bump version"));
        const QString releaseTip = revOf(dir, QStringLiteral("HEAD"));
        flow::FinishOptions o;
        o.tagMessage = QStringLiteral("Release 1.0");
        const GitResult released = runPlan(dir, flow::planFinish(repo, c, flow::Kind::Release, QStringLiteral("1.0"), o));
        QVERIFY2(released.ok(), qPrintable(released.message()));
        QCOMPARE(currentBranch(dir), QStringLiteral("develop"));
        QVERIFY(contains(dir, QStringLiteral("main"), releaseTip));
        QVERIFY(contains(dir, QStringLiteral("develop"), releaseTip));
        QCOMPARE(parentCount(dir, QStringLiteral("main")), 2);
        QCOMPARE(git(dir, {QStringLiteral("cat-file"), QStringLiteral("-t"), QStringLiteral("v1.0")}).outText().trimmed(),
                 QStringLiteral("tag"));
        QCOMPARE(revOf(dir, QStringLiteral("v1.0^{commit}")), revOf(dir, QStringLiteral("main")));
        QVERIFY(git(dir, {QStringLiteral("tag"), QStringLiteral("-n1"), QStringLiteral("v1.0")})
                    .outText()
                    .contains(QLatin1String("Release 1.0")));
        QVERIFY(revOf(dir, QStringLiteral("refs/heads/release/1.0")).isEmpty());
        QVERIFY(flow::planStart(repo, c, flow::Kind::Release, QStringLiteral("1.0")).error.contains(QLatin1String("v1.0")));

        // A hotfix whose merge back into develop conflicts: finish stops there, and finishing
        // again after the merge is committed only does what is left.
        QVERIFY(runPlan(dir, flow::planStart(repo, c, flow::Kind::Hotfix, QStringLiteral("1.0.1"))).ok());
        QCOMPARE(revOf(dir, QStringLiteral("HEAD")), revOf(dir, QStringLiteral("main")));
        QByteArray fixed = numbered(10);
        fixed.replace("line 3\n", "hotfix 3\n");
        write(dir, QStringLiteral("a.txt"), fixed);
        commitAll(dir, QStringLiteral("fix line 3"));
        QVERIFY(git(dir, {QStringLiteral("checkout"), QStringLiteral("-q"), QStringLiteral("develop")}).ok());
        QByteArray dev = numbered(10);
        dev.replace("line 3\n", "develop 3\n");
        write(dir, QStringLiteral("a.txt"), dev);
        commitAll(dir, QStringLiteral("develop changes line 3"));
        QVERIFY(git(dir, {QStringLiteral("checkout"), QStringLiteral("-q"), QStringLiteral("hotfix/1.0.1")}).ok());

        const flow::Plan first = flow::planFinish(repo, c, flow::Kind::Hotfix, QStringLiteral("1.0.1"));
        QVERIFY2(first.ok(), qPrintable(first.error));
        QVERIFY(!runPlan(dir, first).ok());
        QCOMPARE(repo.operation(), Operation::Merge);
        QVERIFY(contains(dir, QStringLiteral("main"), QStringLiteral("hotfix/1.0.1")));
        QVERIFY(!revOf(dir, QStringLiteral("refs/tags/v1.0.1")).isEmpty());
        QVERIFY(
            flow::planFinish(repo, c, flow::Kind::Hotfix, QStringLiteral("1.0.1")).error.contains(QLatin1String("in progress")));
        write(dir, QStringLiteral("a.txt"), fixed);
        QVERIFY(git(dir, {QStringLiteral("add"), QStringLiteral("a.txt")}).ok());
        QVERIFY(git(dir, {QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("--no-edit")}).ok());

        const flow::Plan resume = flow::planFinish(repo, c, flow::Kind::Hotfix, QStringLiteral("1.0.1"));
        QVERIFY2(resume.ok(), qPrintable(resume.error));
        for (const GitCommand& cmd : resume.commands) {
            const QString& verb = cmd.args.first();
            QVERIFY2(verb == QLatin1String("checkout") || verb == QLatin1String("branch") || verb == QLatin1String("config"),
                     qPrintable(cmd.args.join(QLatin1Char(' '))));
        }
        QVERIFY(runPlan(dir, resume).ok());
        QVERIFY(revOf(dir, QStringLiteral("refs/heads/hotfix/1.0.1")).isEmpty());
        QCOMPARE(currentBranch(dir), QStringLiteral("develop"));

        // While a release is open, a hotfix goes into the release branch instead of develop.
        QVERIFY(runPlan(dir, flow::planStart(repo, c, flow::Kind::Release, QStringLiteral("1.1"))).ok());
        QVERIFY(runPlan(dir, flow::planStart(repo, c, flow::Kind::Hotfix, QStringLiteral("1.0.2"))).ok());
        write(dir, QStringLiteral("fix2.txt"), "f\n");
        commitAll(dir, QStringLiteral("second fix"));
        const QString fix2 = revOf(dir, QStringLiteral("HEAD"));
        QVERIFY(runPlan(dir, flow::planFinish(repo, c, flow::Kind::Hotfix, QStringLiteral("1.0.2"))).ok());
        QCOMPARE(currentBranch(dir), QStringLiteral("release/1.1"));
        QVERIFY(contains(dir, QStringLiteral("release/1.1"), fix2));
        QVERIFY(!contains(dir, QStringLiteral("develop"), fix2));

        // Publishing: production, tag, develop, and the remote copy of the release branch is deleted.
        const QString remote = m_tmp.filePath(QStringLiteral("flow-remote.git"));
        QVERIFY(git(dir, {QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("--bare"), remote}).ok());
        QVERIFY(git(dir, {QStringLiteral("remote"), QStringLiteral("add"), QStringLiteral("origin"), remote}).ok());
        QVERIFY(git(dir, {QStringLiteral("push"), QStringLiteral("-q"), QStringLiteral("origin"), QStringLiteral("--all")}).ok());
        o.remote = QStringLiteral("origin");
        o.push = true;
        o.tagMessage.clear();
        const flow::Plan publish = flow::planFinish(repo, c, flow::Kind::Release, QStringLiteral("1.1"), o);
        QVERIFY2(publish.ok(), qPrintable(publish.error));
        QCOMPARE(
            publish.pushArgs,
            (QStringList{QStringLiteral("push"), QStringLiteral("--progress"), QStringLiteral("origin"),
                         QStringLiteral("refs/heads/main:refs/heads/main"), QStringLiteral("refs/tags/v1.1:refs/tags/v1.1"),
                         QStringLiteral("refs/heads/develop:refs/heads/develop"), QStringLiteral(":refs/heads/release/1.1")}));
        QVERIFY(runPlan(dir, publish).ok());
        const GitResult pushed = git(dir, publish.pushArgs);
        QVERIFY2(pushed.ok(), qPrintable(pushed.message()));
        QVERIFY(contains(remote, QStringLiteral("main"), fix2));
        QVERIFY(contains(remote, QStringLiteral("develop"), fix2));  // the fix reached develop through the release
        QVERIFY(git(remote, {QStringLiteral("rev-parse"), QStringLiteral("-q"), QStringLiteral("--verify"),
                             QStringLiteral("refs/tags/v1.1")})
                    .ok());
        QVERIFY(!git(remote, {QStringLiteral("rev-parse"), QStringLiteral("-q"), QStringLiteral("--verify"),
                              QStringLiteral("refs/heads/release/1.1")})
                     .ok());
        QCOMPARE(git(dir, {QStringLiteral("tag"), QStringLiteral("-n1"), QStringLiteral("v1.1")}).outText().simplified(),
                 QStringLiteral("v1.1 v1.1"));  // default tag message: the tag name
    }

    void gitFlowSupportAndBases() {
        const QString dir = newRepo();
        write(dir, QStringLiteral("a.txt"), numbered(10));
        commitAll(dir, QStringLiteral("init"));
        const Repository repo(dir);
        const flow::Config c = initFlow(dir);
        auto release = [&](const QString& version) {
            QVERIFY(runPlan(dir, flow::planStart(repo, c, flow::Kind::Release, version)).ok());
            write(dir, QStringLiteral("VERSION"), version.toUtf8() + '\n');
            commitAll(dir, QStringLiteral("version ") + version);
            QVERIFY(runPlan(dir, flow::planFinish(repo, c, flow::Kind::Release, version)).ok());
        };
        release(QStringLiteral("1.0"));
        release(QStringLiteral("2.0"));
        QCOMPARE(flow::versionTags({QStringLiteral("v1.0"), QStringLiteral("v10.1"), QStringLiteral("x"), QStringLiteral("v2.0")},
                                   QStringLiteral("v")),
                 (QStringList{QStringLiteral("v10.1"), QStringLiteral("v2.0"), QStringLiteral("v1.0")}));

        // A support branch starts from a release, never from unreleased work, and is not finished.
        write(dir, QStringLiteral("dev.txt"), "unreleased\n");
        commitAll(dir, QStringLiteral("unreleased work"));
        QVERIFY(flow::planStart(repo, c, flow::Kind::Support, QStringLiteral("2.x"), QStringLiteral("develop"))
                    .error.contains(QLatin1String("main")));
        const flow::Plan support = flow::planStart(repo, c, flow::Kind::Support, QStringLiteral("1.x"), QStringLiteral("v1.0"));
        QVERIFY2(support.ok(), qPrintable(support.error));
        QVERIFY(runPlan(dir, support).ok());
        QCOMPARE(currentBranch(dir), QStringLiteral("support/1.x"));
        QCOMPARE(revOf(dir, QStringLiteral("HEAD")), revOf(dir, QStringLiteral("v1.0^{commit}")));
        flow::Kind kind = flow::Kind::Feature;
        QVERIFY(c.classify(QStringLiteral("support/1.x"), &kind, nullptr));
        QCOMPARE(kind, flow::Kind::Support);
        QVERIFY(!flow::planFinish(repo, c, flow::Kind::Support, QStringLiteral("1.x")).ok());
        QVERIFY(flow::loadConfig(repo).bases.isEmpty());

        // A hotfix of the support branch goes only into it, tagged there, and continues its versions.
        const QStringList supportTags = flow::tagsReachableFrom(repo, QStringLiteral("support/1.x"));
        QCOMPARE(supportTags, QStringList{QStringLiteral("v1.0")});
        QCOMPARE(flow::suggestVersion(supportTags, QStringLiteral("v"), flow::Kind::Hotfix), QStringLiteral("1.0.1"));
        QVERIFY(runPlan(dir, flow::planStart(repo, c, flow::Kind::Hotfix, QStringLiteral("1.0.1"), QStringLiteral("support/1.x")))
                    .ok());
        QCOMPARE(flow::loadConfig(repo).bases.value(QStringLiteral("hotfix/1.0.1")), QStringLiteral("support/1.x"));
        write(dir, QStringLiteral("fix.txt"), "old line fix\n");
        commitAll(dir, QStringLiteral("fix for 1.x"));
        const QString fix = revOf(dir, QStringLiteral("HEAD"));
        const QStringList locals{QStringLiteral("main"), QStringLiteral("develop"), QStringLiteral("support/1.x"),
                                 QStringLiteral("hotfix/1.0.1")};
        const flow::Targets targets =
            flow::finishTargets(flow::loadConfig(repo), flow::Kind::Hotfix, QStringLiteral("1.0.1"), locals);
        QCOMPARE(targets.into, QStringLiteral("support/1.x"));
        QVERIFY(targets.back.isEmpty());
        const GitResult fixed = runPlan(dir, flow::planFinish(repo, c, flow::Kind::Hotfix, QStringLiteral("1.0.1")));
        QVERIFY2(fixed.ok(), qPrintable(fixed.message()));
        QCOMPARE(currentBranch(dir), QStringLiteral("support/1.x"));
        QVERIFY(contains(dir, QStringLiteral("support/1.x"), fix));
        QVERIFY(!contains(dir, QStringLiteral("main"), fix));
        QVERIFY(!contains(dir, QStringLiteral("develop"), fix));
        QCOMPARE(revOf(dir, QStringLiteral("v1.0.1^{commit}")), revOf(dir, QStringLiteral("support/1.x")));
        QVERIFY(revOf(dir, QStringLiteral("refs/heads/hotfix/1.0.1")).isEmpty());
        QVERIFY(repo.configValue(QStringLiteral("gitflow.branch.hotfix/1.0.1.base")).isEmpty());  // cleaned up

        // A feature started from another feature goes back into it; once that one is gone, into develop.
        QVERIFY(runPlan(dir, flow::planStart(repo, c, flow::Kind::Feature, QStringLiteral("a"))).ok());
        write(dir, QStringLiteral("a-feature.txt"), "a\n");
        commitAll(dir, QStringLiteral("feature a"));
        QVERIFY(runPlan(dir, flow::planStart(repo, c, flow::Kind::Feature, QStringLiteral("a-part"), QStringLiteral("feature/a")))
                    .ok());
        write(dir, QStringLiteral("a-part.txt"), "part\n");
        commitAll(dir, QStringLiteral("part of a"));
        const QString part = revOf(dir, QStringLiteral("HEAD"));
        QVERIFY(runPlan(dir, flow::planFinish(repo, c, flow::Kind::Feature, QStringLiteral("a-part"))).ok());
        QCOMPARE(currentBranch(dir), QStringLiteral("feature/a"));
        QVERIFY(contains(dir, QStringLiteral("feature/a"), part));
        QVERIFY(!contains(dir, QStringLiteral("develop"), part));
        flow::Config withGoneBase = flow::loadConfig(repo);
        withGoneBase.bases.insert(QStringLiteral("feature/b"), QStringLiteral("feature/gone"));
        QCOMPARE(flow::finishTargets(withGoneBase, flow::Kind::Feature, QStringLiteral("b"), locals).into,
                 QStringLiteral("develop"));
    }

    void gitFlowFinishWithRemote() {
        const QString dir = newRepo();
        write(dir, QStringLiteral("a.txt"), numbered(10));
        commitAll(dir, QStringLiteral("init"));
        const Repository repo(dir);
        const flow::Config c = initFlow(dir);
        const QString remote = m_tmp.filePath(QStringLiteral("flow-sync.git"));
        QVERIFY(git(dir, {QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("--bare"), remote}).ok());
        QVERIFY(git(dir, {QStringLiteral("remote"), QStringLiteral("add"), QStringLiteral("origin"), remote}).ok());
        QVERIFY(runPlan(dir, flow::planStart(repo, c, flow::Kind::Feature, QStringLiteral("shared"))).ok());
        write(dir, QStringLiteral("shared.txt"), "mine\n");
        commitAll(dir, QStringLiteral("my part"));
        QVERIFY(git(dir, {QStringLiteral("push"), QStringLiteral("-q"), QStringLiteral("origin"), QStringLiteral("--all")}).ok());

        // A teammate adds to both develop and the feature.
        const QString other = m_tmp.filePath(QStringLiteral("flow-sync-clone"));
        QVERIFY(git(m_tmp.path(), {QStringLiteral("clone"), QStringLiteral("-q"), remote, other}).ok());
        QVERIFY(git(other, {QStringLiteral("checkout"), QStringLiteral("-q"), QStringLiteral("develop")}).ok());
        write(other, QStringLiteral("theirs.txt"), "develop\n");
        commitAll(other, QStringLiteral("their develop work"));
        QVERIFY(git(other, {QStringLiteral("checkout"), QStringLiteral("-q"), QStringLiteral("feature/shared")}).ok());
        write(other, QStringLiteral("shared.txt"), "mine\ntheirs\n");
        commitAll(other, QStringLiteral("their part"));
        QVERIFY(
            git(other, {QStringLiteral("push"), QStringLiteral("-q"), QStringLiteral("origin"), QStringLiteral("--all")}).ok());
        const QString theirDevelop = revOf(other, QStringLiteral("develop"));
        const QString theirFeature = revOf(other, QStringLiteral("feature/shared"));
        QVERIFY(git(dir, {QStringLiteral("fetch"), QStringLiteral("-q"), QStringLiteral("origin")}).ok());

        // Without a remote nothing is compared; with one, both branches are brought up to date first.
        flow::FinishOptions o;
        const flow::Plan local = flow::planFinish(repo, c, flow::Kind::Feature, QStringLiteral("shared"), o);
        for (const GitCommand& cmd : local.commands) QVERIFY(!cmd.args.contains(QLatin1String("--ff-only")));
        o.remote = QStringLiteral("origin");
        o.push = true;
        const flow::Plan synced = flow::planFinish(repo, c, flow::Kind::Feature, QStringLiteral("shared"), o);
        QVERIFY2(synced.ok(), qPrintable(synced.error));
        const GitResult finished = runPlan(dir, synced);
        QVERIFY2(finished.ok(), qPrintable(finished.message()));
        QCOMPARE(currentBranch(dir), QStringLiteral("develop"));
        QVERIFY(contains(dir, QStringLiteral("develop"), theirDevelop));
        QVERIFY(contains(dir, QStringLiteral("develop"), theirFeature));
        QCOMPARE(parentCount(dir, QStringLiteral("develop")), 2);
        QVERIFY(synced.pushArgs.contains(QStringLiteral(":refs/heads/feature/shared")));
        const GitResult pushed = git(dir, synced.pushArgs);
        QVERIFY2(pushed.ok(), qPrintable(pushed.message()));
        QCOMPARE(revOf(remote, QStringLiteral("develop")), revOf(dir, QStringLiteral("develop")));

        // Diverged: local and remote develop both have new commits.
        QVERIFY(runPlan(dir, flow::planStart(repo, c, flow::Kind::Feature, QStringLiteral("late"))).ok());
        write(dir, QStringLiteral("late.txt"), "late\n");
        commitAll(dir, QStringLiteral("late feature"));
        QVERIFY(git(dir, {QStringLiteral("checkout"), QStringLiteral("-q"), QStringLiteral("develop")}).ok());
        write(dir, QStringLiteral("local.txt"), "local\n");
        commitAll(dir, QStringLiteral("local develop work"));
        QVERIFY(git(other, {QStringLiteral("checkout"), QStringLiteral("-q"), QStringLiteral("develop")}).ok());
        QVERIFY(git(other, {QStringLiteral("pull"), QStringLiteral("-q"), QStringLiteral("--ff-only")}).ok());
        write(other, QStringLiteral("more.txt"), "more\n");
        commitAll(other, QStringLiteral("more of their work"));
        QVERIFY(
            git(other, {QStringLiteral("push"), QStringLiteral("-q"), QStringLiteral("origin"), QStringLiteral("develop")}).ok());
        QVERIFY(git(dir, {QStringLiteral("fetch"), QStringLiteral("-q"), QStringLiteral("origin")}).ok());
        const flow::Plan diverged = flow::planFinish(repo, c, flow::Kind::Feature, QStringLiteral("late"), o);
        QVERIFY(diverged.error.contains(QLatin1String("diverged")));
        QVERIFY(diverged.error.contains(QLatin1String("develop")));
    }

    void gitFlowSettings() {
        static const char raw[] =
            "gitflow.branch.master\nmain\0gitflow.branch.develop\ndev\0gitflow.prefix.versiontag\n\0"
            "gitflow.prefix.feature\nfeat-\0gitflow.Feature.finish.squash\ntrue\0gitflow.origin\norigin\0";
        const flow::Config c = flow::parseConfig(QByteArray(raw, sizeof(raw) - 1));
        QVERIFY(c.isInitialized());
        QCOMPARE(c.production, QStringLiteral("main"));
        QCOMPARE(c.develop, QStringLiteral("dev"));
        QCOMPARE(c.versionTagPrefix, QString());
        QCOMPARE(c.featurePrefix, QStringLiteral("feat-"));
        QCOMPARE(c.releasePrefix, QStringLiteral("release/"));  // default when not set
        QCOMPARE(c.sections, (QStringList{QStringLiteral("gitflow.branch"), QStringLiteral("gitflow.prefix"),
                                          QStringLiteral("gitflow.Feature.finish"), QStringLiteral("gitflow")}));
        flow::Kind kind = flow::Kind::Release;
        QVERIFY(c.classify(QStringLiteral("feat-x"), &kind, nullptr));
        QCOMPARE(kind, flow::Kind::Feature);

        const QStringList tags{QStringLiteral("v1.2.3"), QStringLiteral("v1.10.0"),    QStringLiteral("v1.9.9"),
                               QStringLiteral("other"),  QStringLiteral("v2.0.0-rc1"), QStringLiteral("x3.0")};
        QCOMPARE(flow::suggestVersion(tags, QStringLiteral("v"), flow::Kind::Release), QStringLiteral("1.11.0"));
        QCOMPARE(flow::suggestVersion(tags, QStringLiteral("v"), flow::Kind::Hotfix), QStringLiteral("1.10.1"));
        QCOMPARE(flow::suggestVersion({}, QString(), flow::Kind::Release), QStringLiteral("1.0.0"));
        QCOMPARE(flow::suggestVersion({}, QString(), flow::Kind::Hotfix), QString());
        QCOMPARE(flow::suggestVersion({QStringLiteral("1.2")}, QString(), flow::Kind::Release), QStringLiteral("1.3.0"));
        QCOMPARE(flow::suggestVersion({QStringLiteral("1.2")}, QString(), flow::Kind::Hotfix), QStringLiteral("1.2.1"));

        // Setup refuses an unborn repository and bad settings.
        const QString dir = newRepo();
        const Repository repo(dir);
        flow::Config bad;
        bad.production = QStringLiteral("main");
        bad.develop = QStringLiteral("develop");
        QVERIFY(!flow::planInit(repo, bad, {}, true).ok());
        write(dir, QStringLiteral("a.txt"), "a\n");
        commitAll(dir, QStringLiteral("init"));
        QVERIFY(flow::planInit(repo, bad, {}, true).ok());
        bad.develop = QStringLiteral("main");
        QVERIFY(!flow::planInit(repo, bad, {}, true).ok());
        bad.develop = QStringLiteral("develop");
        bad.production = QStringLiteral("master");
        QVERIFY(!flow::planInit(repo, bad, {}, true).ok());
        bad.production = QStringLiteral("main");
        bad.releasePrefix.clear();
        QVERIFY(!flow::planInit(repo, bad, {}, true).ok());
    }

    void graphLinear() {
        QVector<Commit> commits(3);
        commits[0].sha = "c3";
        commits[0].parents = {"c2"};
        commits[1].sha = "c2";
        commits[1].parents = {"c1"};
        commits[2].sha = "c1";
        const QVector<GraphRow> g = computeGraph(commits);
        for (const GraphRow& r : g) QCOMPARE(r.column, 0);
        QCOMPARE(g[0].width, 1);
        QCOMPARE(g[2].lines.size(), 1);  // only the incoming line from c2
    }

    void graphMerge() {
        // M merges A (first parent) and B; both come from Base.
        QVector<Commit> commits(4);
        commits[0].sha = "M";
        commits[0].parents = {"A", "B"};
        commits[1].sha = "A";
        commits[1].parents = {"Base"};
        commits[2].sha = "B";
        commits[2].parents = {"Base"};
        commits[3].sha = "Base";
        const QVector<GraphRow> g = computeGraph(commits);
        QCOMPARE(g[0].column, 0);
        QCOMPARE(g[1].column, 0);
        QCOMPARE(g[2].column, 1);
        QCOMPARE(g[3].column, 0);
        bool mergeEdge = false;
        for (const GraphLine& l : g[0].lines) mergeEdge |= l.kind == GraphLine::CenterToBottom && l.from == 0 && l.to == 1;
        QVERIFY(mergeEdge);
        // B's lane joins A's lane (which waits for Base) right away.
        bool joins = false;
        for (const GraphLine& l : g[2].lines) joins |= l.kind == GraphLine::CenterToBottom && l.from == 1 && l.to == 0;
        QVERIFY(joins);
        QCOMPARE(g[3].width, 1);
    }

    void parsersEdgeCases() {
        const QVector<Ref> refs = parse::refs(
            "refs/heads/main\x1f"
            "aaa\x1f\x1f"
            "origin/main\x1f"
            "ahead 2, behind 3\x1f*\n"
            "refs/remotes/origin/HEAD\x1f"
            "aaa\x1f\x1f\x1f\x1f\n"
            "refs/heads/gone\x1f"
            "bbb\x1f\x1f"
            "origin/gone\x1f"
            "gone\x1f \n");
        QCOMPARE(refs.size(), 2);
        QCOMPARE(refs[0].ahead, 2);
        QCOMPARE(refs[0].behind, 3);
        QVERIFY(refs[0].isHead);
        QVERIFY(refs[1].upstreamGone);
        const QVector<FileChange> ns = parse::nameStatus(QByteArray("M\0a.txt\0R087\0old.txt\0new.txt\0", 29));
        QCOMPARE(ns.size(), 2);
        QCOMPARE(ns[1].oldPath, QStringLiteral("old.txt"));
        QCOMPARE(ns[1].path, QStringLiteral("new.txt"));

        // Older git prints `log -L` diffs without "new/deleted file mode" lines.
        auto record = [](const QByteArray& sha, const QByteArray& subject, const QByteArray& diff) {
            return "\x1e" + QByteArrayList{sha, {}, "A", "a@x", "1", "A", "a@x", "1", subject}.join('\x1f') + "\n\n" + diff;
        };
        const QVector<LineLogEntry> lineLog = parse::lineLog(
            record("c2", "gone", "diff --git a/a b.txt b/a b.txt\n--- a/a b.txt\n+++ /dev/null\n@@ -1,1 +0,0 @@\n-1\n") +
            record("c1", "add", "diff --git a/a b.txt b/a b.txt\n--- /dev/null\n+++ b/a b.txt\n@@ -0,0 +1,1 @@\n+1\n"));
        QCOMPARE(lineLog.size(), 2);
        QVERIFY(lineLog[0].diff.deletedFile);
        QCOMPARE(lineLog[0].diff.oldPath, QStringLiteral("a b.txt"));
        QVERIFY(lineLog[1].diff.newFile);
        QVERIFY(lineLog[1].diff.oldPath.isEmpty());
        QCOMPARE(lineLog[1].diff.newPath, QStringLiteral("a b.txt"));
        QCOMPARE(lineLog[1].diff.hunks.first().lines.size(), 1);
    }

    void remoteUrlParsing_data() {
        QTest::addColumn<QString>("url");
        QTest::addColumn<QString>("host");
        QTest::addColumn<QString>("path");
        QTest::newRow("https") << "https://github.com/octo/app.git" << "github.com" << "octo/app";
        QTest::newRow("https no .git, trailing /") << "https://gitlab.com/group/sub/app/" << "gitlab.com" << "group/sub/app";
        QTest::newRow("https with user + port")
            << "https://me@git.example.com:8443/team/app.git" << "git.example.com:8443" << "team/app";
        QTest::newRow("scp-like") << "git@github.com:octo/app.git" << "github.com" << "octo/app";
        QTest::newRow("ssh url with port") << "ssh://git@gitlab.example.com:2222/team/app.git" << "gitlab.example.com"
                                           << "team/app";
        QTest::newRow("ssh.github.com alias") << "ssh://git@ssh.github.com:443/octo/app.git" << "github.com" << "octo/app";
    }

    void remoteUrlParsing() {
        QFETCH(QString, url);
        QFETCH(QString, host);
        QFETCH(QString, path);
        const RemoteRepo repo = parseRemoteUrl(url);
        QVERIFY(repo.valid);
        QCOMPARE(repo.host, host);
        QCOMPARE(repo.path, path);
    }

    void remoteUrlRejectsLocalPaths() {
        QVERIFY(!parseRemoteUrl(QStringLiteral("/srv/git/app.git")).valid);
        QVERIFY(!parseRemoteUrl(QStringLiteral("file:///srv/git/app.git")).valid);
        QVERIFY(!parseRemoteUrl(QString()).valid);
    }

    void webLinksAndProviders() {
        const RemoteRepo gh = parseRemoteUrl(QStringLiteral("git@github.com:octo/app.git"));
        QCOMPARE(webCommitUrl(Provider::GitHub, gh, QStringLiteral("abc")),
                 QStringLiteral("https://github.com/octo/app/commit/abc"));
        QCOMPARE(webBranchUrl(Provider::GitHub, gh, QStringLiteral("feature/x")),
                 QStringLiteral("https://github.com/octo/app/tree/feature/x"));
        QCOMPARE(webNewReviewUrl(Provider::GitHub, gh, QStringLiteral("feature/x"), QStringLiteral("main")),
                 QStringLiteral("https://github.com/octo/app/compare/main...feature/x?expand=1"));
        const RemoteRepo gl = parseRemoteUrl(QStringLiteral("https://gitlab.com/group/sub/app.git"));
        QCOMPARE(webCommitUrl(Provider::GitLab, gl, QStringLiteral("abc")),
                 QStringLiteral("https://gitlab.com/group/sub/app/-/commit/abc"));
        QVERIFY(webNewReviewUrl(Provider::GitLab, gl, QStringLiteral("fix"), QStringLiteral("main"))
                    .endsWith(QLatin1String(
                        "/-/merge_requests/new?merge_request%5Bsource_branch%5D=fix&merge_request%5Btarget_branch%5D=main")));

        QCOMPARE(detectProvider(QStringLiteral("github.com"), {}), std::optional<Provider>(Provider::GitHub));
        QCOMPARE(detectProvider(QStringLiteral("gitlab.com"), {}), std::optional<Provider>(Provider::GitLab));
        QVERIFY(!detectProvider(QStringLiteral("git.example.com"), {}).has_value());
        Account selfHosted{Provider::GitLab, QStringLiteral("git.example.com"), QStringLiteral("me"), {}};
        QCOMPARE(detectProvider(QStringLiteral("git.example.com"), {selfHosted}), std::optional<Provider>(Provider::GitLab));
        QCOMPARE(selfHosted.apiUrl(), QStringLiteral("https://git.example.com/api/v4"));
        QCOMPARE((Account{Provider::GitHub, QStringLiteral("github.com"), QStringLiteral("me"), {}}).apiUrl(),
                 QStringLiteral("https://api.github.com"));
        QCOMPARE((Account{Provider::GitHub, QStringLiteral("ghe.corp"), QStringLiteral("me"), {}}).apiUrl(),
                 QStringLiteral("https://ghe.corp/api/v3"));
    }

    void accountLookup() {
        const QVector<Account> list{{Provider::GitHub, QStringLiteral("github.com"), QStringLiteral("alice"), {}},
                                    {Provider::GitHub, QStringLiteral("github.com"), QStringLiteral("bob"), {}}};
        QCOMPARE(accounts::findForHost(list, QStringLiteral("GitHub.com"))->username, QStringLiteral("alice"));
        QCOMPARE(accounts::findForHost(list, QStringLiteral("github.com"), QStringLiteral("bob"))->username,
                 QStringLiteral("bob"));
        QVERIFY(!accounts::findForHost(list, QStringLiteral("github.com"), QStringLiteral("carol")));
        QVERIFY(!accounts::findForHost(list, QStringLiteral("gitlab.com")));
    }

    void interactiveRebase() {
        const QString dir = newRepo();
        QStringList shas;
        for (const char* name : {"A", "B", "C", "D", "E"}) {
            write(dir, QString::fromLatin1(name).toLower() + QStringLiteral(".txt"), QByteArray(name) + "\n");
            commitAll(dir, QString::fromLatin1(name));
            shas << git(dir, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")}).outText().trimmed();
        }
        const Repository repo(dir);
        RebaseRange range = loadRebaseRange(repo, shas[0]);
        QVERIFY2(range.error.isEmpty(), qPrintable(range.error));
        QCOMPARE(range.steps.size(), 4);
        QCOMPARE(range.steps.first().subject, QStringLiteral("B"));  // oldest first
        QVERIFY(!range.published);

        // New order: D (reworded), B, C squashed into B, E dropped.
        QVector<RebaseStep> plan{range.steps[2], range.steps[0], range.steps[1], range.steps[3]};
        plan[0].action = RebaseAction::Reword;
        plan[0].message = QStringLiteral("D, reworded\n\nwith a body line");
        plan[2].action = RebaseAction::Squash;
        plan[3].action = RebaseAction::Drop;
        QString error;
        const QStringList args =
            prepareRebase(plan, shas[0], QDir(repo.gitDir()).filePath(QStringLiteral("aslgc-rebase")), false, &error);
        QVERIFY2(!args.isEmpty(), qPrintable(error));
        const GitResult r = Git(dir).run(args);
        QVERIFY2(r.ok(), qPrintable(r.message()));
        QVERIFY(repo.operation() == Operation::None);

        const QStringList subjects = git(dir, {QStringLiteral("log"), QStringLiteral("--reverse"), QStringLiteral("--format=%s"),
                                               shas[0] + QStringLiteral("..HEAD")})
                                         .outText()
                                         .trimmed()
                                         .split(QLatin1Char('\n'));
        QCOMPARE(subjects, (QStringList{QStringLiteral("D, reworded"), QStringLiteral("B")}));
        QCOMPARE(
            git(dir, {QStringLiteral("log"), QStringLiteral("-1"), QStringLiteral("--skip=1"), QStringLiteral("--format=%b")})
                .outText()
                .trimmed(),
            QStringLiteral("with a body line"));
        QCOMPARE(git(dir, {QStringLiteral("log"), QStringLiteral("-1"), QStringLiteral("--format=%B")}).outText().trimmed(),
                 QStringLiteral("B\n\nC"));  // squash keeps both messages
        QVERIFY(QFile::exists(QDir(dir).filePath(QStringLiteral("c.txt"))));
        QVERIFY(!QFile::exists(QDir(dir).filePath(QStringLiteral("e.txt"))));
    }

    void interactiveRebaseEditStop() {
        const QString dir = newRepo();
        for (const char* name : {"A", "B", "C"}) {
            write(dir, QString::fromLatin1(name).toLower() + QStringLiteral(".txt"), QByteArray(name) + "\n");
            commitAll(dir, QString::fromLatin1(name));
        }
        const Repository repo(dir);
        RebaseRange range = loadRebaseRange(repo, QStringLiteral("HEAD~2"));
        QCOMPARE(range.steps.size(), 2);
        range.steps[0].action = RebaseAction::Edit;  // stop at B
        QString error;
        const QStringList args = prepareRebase(range.steps, QStringLiteral("HEAD~2"),
                                               QDir(repo.gitDir()).filePath(QStringLiteral("aslgc-rebase")), false, &error);
        QVERIFY2(Git(dir).run(args).ok(), "rebase should stop cleanly at the edit step");
        const RepoSnapshot stopped = loadSnapshot(repo, LogOptions{}, {});
        QVERIFY(stopped.operation == Operation::Rebase);
        QCOMPARE(stopped.rebaseEditSha, range.steps[0].sha);

        write(dir, QStringLiteral("b.txt"), "B amended\n");  // amend while stopped
        QVERIFY(git(dir, {QStringLiteral("commit"), QStringLiteral("-qam"), QStringLiteral("B (amended)")}).ok());
        const GitResult cont = git(dir, {QStringLiteral("rebase"), QStringLiteral("--continue")});
        QVERIFY2(cont.ok(), qPrintable(cont.message()));
        const RepoSnapshot done = loadSnapshot(repo, LogOptions{}, {});
        QVERIFY(done.operation == Operation::None);
        QVERIFY(done.rebaseEditSha.isEmpty());
        QCOMPARE(
            git(dir, {QStringLiteral("log"), QStringLiteral("-1"), QStringLiteral("--skip=1"), QStringLiteral("--format=%s")})
                .outText()
                .trimmed(),
            QStringLiteral("B (amended)"));
    }

    void rebasePlanValidation() {
        RebaseStep a{RebaseAction::Pick, QStringLiteral("a"), QStringLiteral("A"), QStringLiteral("A"), {}};
        RebaseStep b{RebaseAction::Squash, QStringLiteral("b"), QStringLiteral("B"), QStringLiteral("B"), {}};
        QVERIFY(validateRebasePlan({a, b}).isEmpty());
        QVERIFY(!validateRebasePlan({b, a}).isEmpty());  // nothing above to squash into
        RebaseStep dropped = a;
        dropped.action = RebaseAction::Drop;
        QVERIFY(!validateRebasePlan({dropped, b}).isEmpty());
        RebaseStep emptyReword = a;
        emptyReword.action = RebaseAction::Reword;
        emptyReword.message = QStringLiteral("  ");
        QVERIFY(!validateRebasePlan({emptyReword}).isEmpty());
        QCOMPARE(shellQuote(QStringLiteral("it's")), QStringLiteral("'it'\\''s'"));
    }

    // git must ask our helper (and only ours) for hosts that have an account.
    void credentialHelperWiring() {
        const QString helper = m_tmp.filePath(QStringLiteral("fake helper"));
        {
            QFile f(helper);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(
                "#!/bin/sh\n[ \"$1\" = credential ] && [ \"$2\" = get ] && printf "
                "'username=tok-user\\npassword=tok-secret\\n'\nexit 0\n");
        }
        QFile::setPermissions(helper, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        const QString dir = newRepo();
        // A lower-priority helper that must be overridden for the account host.
        QVERIFY(git(dir, {QStringLiteral("config"), QStringLiteral("credential.helper"),
                          QStringLiteral("!echo username=wrong; echo password=wrong; true")})
                    .ok());

        Git::setAskPassProgram(helper);
        Git::setCredentialHosts({QStringLiteral("https://github.com")});
        const GitResult filled = git(dir, {QStringLiteral("credential"), QStringLiteral("fill")},
                                     "protocol=https\nhost=github.com\npath=octo/app.git\n\n");
        QVERIFY2(filled.ok(), qPrintable(filled.message()));
        QVERIFY(filled.out.contains("username=tok-user"));
        QVERIFY(filled.out.contains("password=tok-secret"));

        // Other hosts keep the user's own helpers.
        const GitResult other =
            git(dir, {QStringLiteral("credential"), QStringLiteral("fill")}, "protocol=https\nhost=example.org\n\n");
        QVERIFY(other.out.contains("username=wrong"));
        Git::setAskPassProgram({});
        Git::setCredentialHosts({});
    }
};

QTEST_GUILESS_MAIN(CoreTest)
#include "tst_core.moc"
