// SPDX-License-Identifier: MIT
#include "Parsers.h"

#include <QHash>
#include <QRegularExpression>

namespace aslgc::parse {

const char* const kLogFormat = "--format=%H%x1f%P%x1f%an%x1f%ae%x1f%at%x1f%cn%x1f%ce%x1f%ct%x1f%s%x1e";
const char* const kRefFormat =
    "--format=%(refname)%1f%(objectname)%1f%(*objectname)%1f%(upstream:short)%1f%(upstream:track,nobracket)%1f%(HEAD)";
const char* const kStashFormat = "--format=%gd%x1f%H%x1f%ct%x1f%gs%x1e";
const char* const kFileLogFormat = "--format=%x1e%H%x1f%P%x1f%an%x1f%ae%x1f%at%x1f%cn%x1f%ce%x1f%ct%x1f%s";
const char* const kReflogFormat = "--format=%H%x1f%P%x1f%an%x1f%ae%x1f%at%x1f%cn%x1f%ce%x1f%ct%x1f%s%x1f%gd%x1f%gn%x1f%gs%x1e";

namespace {

QByteArray stripLeadingNewlines(const QByteArray& rec) {
    int s = 0;
    while (s < rec.size() && (rec[s] == '\n' || rec[s] == '\r')) ++s;
    return rec.mid(s);
}

QString utf8(const QByteArray& b) { return QString::fromUtf8(b); }

bool fillCommit(const QList<QByteArray>& f, Commit& c) {
    if (f.size() < 9) return false;
    c.sha = QString::fromLatin1(f[0]);
    c.parents = QString::fromLatin1(f[1]).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    c.authorName = utf8(f[2]);
    c.authorEmail = utf8(f[3]);
    c.authorTime = f[4].toLongLong();
    c.committerName = utf8(f[5]);
    c.committerEmail = utf8(f[6]);
    c.commitTime = f[7].toLongLong();
    c.subject = utf8(f[8]);
    return !c.sha.isEmpty();
}

bool isHex(const QByteArray& s) {
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return !s.isEmpty();
}

// Splits off `n` space separated fields; the remainder (a path that may contain
// spaces) is returned in `rest`.
bool splitFields(const QByteArray& line, int n, QList<QByteArray>& fields, QByteArray& rest) {
    int pos = 0;
    for (int i = 0; i < n; ++i) {
        const int sp = line.indexOf(' ', pos);
        if (sp < 0) return false;
        fields.push_back(line.mid(pos, sp - pos));
        pos = sp + 1;
    }
    rest = line.mid(pos);
    return true;
}

}  // namespace

QVector<Commit> log(const QByteArray& data) {
    QVector<Commit> out;
    const QList<QByteArray> records = data.split('\x1e');
    out.reserve(records.size());
    for (const QByteArray& raw : records) {
        const QByteArray rec = stripLeadingNewlines(raw);
        if (rec.isEmpty()) continue;
        Commit c;
        if (fillCommit(rec.split('\x1f'), c)) out.push_back(std::move(c));
    }
    return out;
}

CommitInfo commitInfo(const QByteArray& data) {
    CommitInfo info;
    QByteArray rec = stripLeadingNewlines(data);
    const int end = rec.indexOf('\x1e');
    if (end >= 0) rec.truncate(end);
    const QList<QByteArray> f = rec.split('\x1f');
    if (fillCommit(f, info.commit)) {
        info.message = utf8(f.mid(8).join('\x1f')).trimmed();
        info.commit.subject = info.message.section(QLatin1Char('\n'), 0, 0);
        info.valid = true;
    }
    return info;
}

QVector<Ref> refs(const QByteArray& data) {
    static const QRegularExpression aheadRe(QStringLiteral("ahead (\\d+)"));
    static const QRegularExpression behindRe(QStringLiteral("behind (\\d+)"));
    QVector<Ref> out;
    for (const QByteArray& line : data.split('\n')) {
        if (line.isEmpty()) continue;
        const QList<QByteArray> f = line.split('\x1f');
        if (f.size() < 6) continue;
        Ref r;
        r.fullName = utf8(f[0]);
        if (r.fullName.startsWith(QLatin1String("refs/heads/"))) {
            r.kind = RefKind::LocalBranch;
            r.name = r.fullName.mid(11);
        } else if (r.fullName.startsWith(QLatin1String("refs/remotes/"))) {
            if (r.fullName.endsWith(QLatin1String("/HEAD"))) continue;  // symbolic remote HEAD
            r.kind = RefKind::RemoteBranch;
            r.name = r.fullName.mid(13);
        } else if (r.fullName.startsWith(QLatin1String("refs/tags/"))) {
            r.kind = RefKind::Tag;
            r.name = r.fullName.mid(10);
        } else {
            continue;
        }
        r.target = QString::fromLatin1(f[2].isEmpty() ? f[1] : f[2]);
        r.upstream = utf8(f[3]);
        const QString track = QString::fromLatin1(f[4]);
        r.upstreamGone = track == QLatin1String("gone");
        if (auto m = aheadRe.match(track); m.hasMatch()) r.ahead = m.captured(1).toInt();
        if (auto m = behindRe.match(track); m.hasMatch()) r.behind = m.captured(1).toInt();
        r.isHead = f[5].trimmed() == "*";
        out.push_back(std::move(r));
    }
    return out;
}

QVector<Stash> stashes(const QByteArray& data) {
    static const QRegularExpression indexRe(QStringLiteral("\\{(\\d+)\\}"));
    QVector<Stash> out;
    for (const QByteArray& raw : data.split('\x1e')) {
        const QByteArray rec = stripLeadingNewlines(raw);
        if (rec.isEmpty()) continue;
        const QList<QByteArray> f = rec.split('\x1f');
        if (f.size() < 4) continue;
        Stash s;
        s.ref = utf8(f[0]);
        if (auto m = indexRe.match(s.ref); m.hasMatch()) s.index = m.captured(1).toInt();
        s.sha = QString::fromLatin1(f[1]);
        s.time = f[2].toLongLong();
        s.message = utf8(f[3]).trimmed();
        out.push_back(std::move(s));
    }
    return out;
}

QVector<ReflogEntry> reflog(const QByteArray& data) {
    QVector<ReflogEntry> out;
    for (const QByteArray& raw : data.split('\x1e')) {
        const QByteArray rec = stripLeadingNewlines(raw);
        if (rec.isEmpty()) continue;
        const QList<QByteArray> f = rec.split('\x1f');
        ReflogEntry e;
        if (f.size() < 12 || !fillCommit(f, e.commit)) continue;
        e.selector = utf8(f[9]);
        e.identity = utf8(f[10]);
        // "checkout: moving from main to dev": the command that moved the ref, then what it did.
        const QString subject = utf8(f.mid(11).join('\x1f')).trimmed();
        const int colon = subject.indexOf(QLatin1String(": "));
        e.action = colon > 0 ? subject.left(colon) : subject;
        if (colon > 0) e.message = subject.mid(colon + 2).trimmed();
        out.push_back(std::move(e));
    }
    return out;
}

QVector<qint64> reflogTimes(const QByteArray& data) {
    QVector<qint64> out;
    for (const QByteArray& line : data.split('\n')) {
        const int open = line.lastIndexOf('{');
        const int close = line.lastIndexOf('}');
        if (open < 0 || close <= open) continue;
        out.push_back(line.mid(open + 1, close - open - 1).toLongLong());
    }
    return out;
}

QVector<Remote> remotes(const QByteArray& data) {
    QVector<Remote> out;
    for (const QByteArray& line : data.split('\n')) {
        const int tab = line.indexOf('\t');
        if (tab <= 0) continue;
        const QString name = utf8(line.left(tab));
        QString rest = utf8(line.mid(tab + 1));
        const bool isPush = rest.endsWith(QLatin1String(" (push)"));
        const int paren = rest.lastIndexOf(QLatin1String(" ("));
        if (paren >= 0) rest.truncate(paren);
        Remote* remote = nullptr;
        for (auto& r : out) {
            if (r.name == name) remote = &r;
        }
        if (!remote) {
            out.push_back({name, {}, {}});
            remote = &out.last();
        }
        (isPush ? remote->pushUrl : remote->fetchUrl) = rest;
    }
    return out;
}

WorkingStatus status(const QByteArray& data) {
    WorkingStatus st;
    const QList<QByteArray> tokens = data.split('\0');
    for (int i = 0; i < tokens.size(); ++i) {
        const QByteArray& t = tokens[i];
        if (t.size() < 2) continue;
        const char type = t[0];
        QList<QByteArray> fields;
        QByteArray path;
        if (type == '1' || type == '2') {
            if (!splitFields(t, type == '1' ? 8 : 9, fields, path)) continue;
            QString origPath;
            if (type == '2' && i + 1 < tokens.size()) origPath = utf8(tokens[++i]);
            const QByteArray xy = fields[1];
            if (xy.size() < 2) continue;
            const bool submodule = fields[2].startsWith('S');
            if (xy[0] != '.') {
                FileChange f{utf8(path), {}, xy[0], {}, submodule};
                if (xy[0] == 'R' || xy[0] == 'C') f.oldPath = origPath;
                st.staged.push_back(f);
            }
            if (xy[1] != '.') {
                FileChange f{utf8(path), {}, xy[1], {}, submodule};
                if (xy[1] == 'R' || xy[1] == 'C') f.oldPath = origPath;
                st.unstaged.push_back(f);
            }
        } else if (type == 'u') {
            if (!splitFields(t, 10, fields, path)) continue;
            st.unstaged.push_back({utf8(path), {}, 'U', QString::fromLatin1(fields[1]), fields[2].startsWith('S')});
        } else if (type == '?') {
            st.unstaged.push_back({utf8(t.mid(2)), {}, '?', {}, false});
        }
    }
    return st;
}

QVector<FileChange> nameStatus(const QByteArray& data) {
    QVector<FileChange> out;
    const QList<QByteArray> tokens = data.split('\0');
    for (int i = 0; i < tokens.size(); ++i) {
        const QByteArray& code = tokens[i];
        if (code.isEmpty()) continue;
        FileChange f;
        f.status = code[0];
        if ((f.status == 'R' || f.status == 'C') && i + 2 < tokens.size()) {
            f.oldPath = utf8(tokens[++i]);
            f.path = utf8(tokens[++i]);
        } else if (i + 1 < tokens.size()) {
            f.path = utf8(tokens[++i]);
        } else {
            break;
        }
        out.push_back(std::move(f));
    }
    return out;
}

QVector<FileLogEntry> fileLog(const QByteArray& data) {
    // Each record: \x1e <fields> \0 [\n <name-status -z tokens>]
    QVector<FileLogEntry> out;
    for (const QByteArray& rec : data.split('\x1e')) {
        if (rec.isEmpty()) continue;
        const int nul = rec.indexOf('\0');
        FileLogEntry e;
        if (!fillCommit((nul < 0 ? rec : rec.left(nul)).split('\x1f'), e.commit)) continue;
        if (nul >= 0) e.changes = nameStatus(stripLeadingNewlines(rec.mid(nul + 1)));
        out.push_back(std::move(e));
    }
    return out;
}

QVector<LineLogEntry> lineLog(const QByteArray& data) {
    // Each record: \x1e <fields> \n [\n <unified diff limited to the range>]
    QVector<LineLogEntry> out;
    for (const QByteArray& rec : data.split('\x1e')) {
        if (rec.isEmpty()) continue;
        const qsizetype nl = rec.indexOf('\n');
        LineLogEntry e;
        if (!fillCommit((nl < 0 ? rec : rec.left(nl)).split('\x1f'), e.commit)) continue;
        if (nl >= 0) {
            const QVector<FileDiff> diffs = parseDiff(rec.mid(nl + 1));
            if (!diffs.isEmpty()) e.diff = diffs.first();
        }
        out.push_back(std::move(e));
    }
    return out;
}

Blame blame(const QByteArray& data) {
    // Porcelain: every group of lines starts with "<sha> <orig line> <final line> [<count>]",
    // followed by "key value" lines the first time a commit is seen (and "filename" whenever it
    // changes), then one TAB-prefixed content line.
    Blame b;
    QHash<QByteArray, int> index;
    int current = -1;
    int origLine = 0;
    QString path;
    qsizetype pos = 0;
    const qsizetype n = data.size();
    while (pos < n) {
        qsizetype end = data.indexOf('\n', pos);
        if (end < 0) end = n;
        const QByteArray line = data.mid(pos, end - pos);
        pos = end + 1;
        if (line.startsWith('\t')) {
            if (current < 0) continue;
            QByteArray content = line.mid(1);
            if (content.contains('\0')) b.binary = true;
            if (content.endsWith('\r')) content.chop(1);
            b.lines.push_back({current, origLine, path, QString::fromUtf8(content)});
            continue;
        }
        const qsizetype sp = line.indexOf(' ');
        const QByteArray key = sp < 0 ? line : line.left(sp);
        const QByteArray value = sp < 0 ? QByteArray() : line.mid(sp + 1);
        if (key.size() >= 40 && isHex(key)) {
            auto it = index.find(key);
            if (it == index.end()) {
                it = index.insert(key, int(b.commits.size()));
                BlameCommit c;
                c.sha = QString::fromLatin1(key);
                b.commits.push_back(std::move(c));
            }
            current = it.value();
            const QList<QByteArray> numbers = value.split(' ');  // orig line, final line[, group size]
            origLine = numbers.value(0).toInt();
            // A new group starts with the commit's file unless a "filename" line follows; the
            // rest of a group keeps the group's file (with -C, a commit can have several).
            if (numbers.size() >= 3) path = b.commits[current].path;
            continue;
        }
        if (current < 0) continue;
        BlameCommit& c = b.commits[current];
        if (key == "author") {
            c.authorName = utf8(value);
        } else if (key == "author-mail") {
            QString mail = utf8(value);
            if (mail.startsWith(QLatin1Char('<')) && mail.endsWith(QLatin1Char('>'))) mail = mail.mid(1, mail.size() - 2);
            c.authorEmail = mail;
        } else if (key == "author-time") {
            c.authorTime = value.toLongLong();
        } else if (key == "summary") {
            c.summary = utf8(value);
        } else if (key == "boundary") {
            c.boundary = true;
        } else if (key == "previous") {
            const qsizetype s = value.indexOf(' ');
            if (s > 0) {
                c.previousSha = QString::fromLatin1(value.left(s));
                c.previousPath = utf8(value.mid(s + 1));
            }
        } else if (key == "filename") {
            path = utf8(value);
            if (c.path.isEmpty()) c.path = path;
        }
    }
    return b;
}

}  // namespace aslgc::parse
