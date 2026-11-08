// SPDX-License-Identifier: MIT
#include "Diff.h"

#include <QRegularExpression>

#include <algorithm>

namespace aslgc {

namespace {

QString pathFromHeader(QByteArray p) {
    const int tab = p.indexOf('\t');
    if (tab >= 0) p.truncate(tab);
    if (p == "/dev/null") return {};
    if (p.size() >= 2 && p.startsWith('"') && p.endsWith('"')) p = p.mid(1, p.size() - 2);
    if (p.startsWith("a/") || p.startsWith("b/")) p = p.mid(2);
    return QString::fromUtf8(p);
}

bool parseHunkHeader(const QByteArray& line, DiffHunk& h) {
    static const QRegularExpression re(QStringLiteral("^@@ -(\\d+)(?:,(\\d+))? \\+(\\d+)(?:,(\\d+))? @@"));
    const QString latin = QString::fromLatin1(line);
    const auto m = re.match(latin);
    if (!m.hasMatch()) return false;
    h.oldStart = m.captured(1).toInt();
    h.oldCount = m.capturedLength(2) ? m.captured(2).toInt() : 1;
    h.newStart = m.captured(3).toInt();
    h.newCount = m.capturedLength(4) ? m.captured(4).toInt() : 1;
    h.header = line;
    h.section = line.mid(m.capturedEnd(0));
    return true;
}

void appendLine(QByteArray& out, char type, const QByteArray& raw) {
    out += type;
    out += raw;
    out += '\n';
}

}  // namespace

int DiffHunk::changeCount() const {
    int n = 0;
    for (const auto& l : lines) n += (l.type == '+' || l.type == '-') ? 1 : 0;
    return n;
}

int FileDiff::changeCount() const {
    int n = 0;
    for (const auto& h : hunks) n += h.changeCount();
    return n;
}

QVector<FileDiff> parseDiff(const QByteArray& data) {
    QVector<FileDiff> files;
    FileDiff* cur = nullptr;
    DiffHunk* hunk = nullptr;
    int oldNo = 0;
    int newNo = 0;
    const QList<QByteArray> lines = data.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        const QByteArray& l = lines[i];
        if (i == lines.size() - 1 && l.isEmpty()) break;
        if (l.startsWith("diff --git ") || l.startsWith("diff --cc ") || l.startsWith("diff --combined ")) {
            files.push_back({});
            cur = &files.back();
            hunk = nullptr;
            cur->header.push_back(l);
            continue;
        }
        if (!cur) continue;
        if (l.startsWith("@@ ")) {
            DiffHunk h;
            if (parseHunkHeader(l, h)) {
                cur->hunks.push_back(std::move(h));
                hunk = &cur->hunks.back();
                oldNo = hunk->oldStart;
                newNo = hunk->newStart;
                continue;
            }
        }
        if (!hunk) {
            cur->header.push_back(l);
            if (l.startsWith("--- "))
                cur->oldPath = pathFromHeader(l.mid(4));
            else if (l.startsWith("+++ "))
                cur->newPath = pathFromHeader(l.mid(4));
            else if (l.startsWith("new file mode"))
                cur->newFile = true;
            else if (l.startsWith("deleted file mode"))
                cur->deletedFile = true;
            else if (l.startsWith("rename from "))
                cur->oldPath = QString::fromUtf8(l.mid(12));
            else if (l.startsWith("rename to "))
                cur->newPath = QString::fromUtf8(l.mid(10));
            else if (l.startsWith("Binary files ") || l.startsWith("GIT binary patch"))
                cur->binary = true;
            continue;
        }
        DiffLine dl;
        dl.type = l.isEmpty() ? ' ' : l[0];
        dl.raw = l.isEmpty() ? QByteArray() : l.mid(1);
        switch (dl.type) {
            case ' ':
                dl.oldLine = oldNo++;
                dl.newLine = newNo++;
                break;
            case '-': dl.oldLine = oldNo++; break;
            case '+': dl.newLine = newNo++; break;
            case '\\': break;
            default:
                hunk = nullptr;  // unexpected line: stop treating input as hunk content
                continue;
        }
        hunk->lines.push_back(std::move(dl));
    }
    // Fill paths from "diff --git a/x b/x" when ---/+++ are absent (mode-only or binary changes).
    for (FileDiff& f : files) {
        if (f.oldPath.isEmpty() && f.newPath.isEmpty() && !f.header.isEmpty()) {
            const QByteArray first = f.header.first();
            const int b = first.lastIndexOf(" b/");
            if (b > 0) f.newPath = f.oldPath = QString::fromUtf8(first.mid(b + 3));
        }
        if (f.oldPath.isEmpty() && !f.newFile) f.oldPath = f.newPath;
        if (f.newPath.isEmpty() && !f.deletedFile) f.newPath = f.oldPath;
    }
    return files;
}

int mapToOldLine(const FileDiff& diff, int newLine) {
    int offset = 0;  // old - new, after the hunks passed so far
    for (const DiffHunk& h : diff.hunks) {
        if (newLine < h.newStart) break;
        if (newLine < h.newStart + h.newCount) {
            int lastOld = h.oldStart - 1;
            for (const DiffLine& l : h.lines) {
                if (l.type == '+' && l.newLine == newLine) return std::max(1, lastOld);
                if (l.type == ' ' && l.newLine == newLine) return l.oldLine;
                if (l.oldLine > 0) lastOld = l.oldLine;
            }
            return std::max(1, lastOld);
        }
        offset = (h.oldStart + h.oldCount) - (h.newStart + h.newCount);
    }
    return std::max(1, newLine + offset);
}

QByteArray hunkPatch(const FileDiff& diff, int index) {
    if (index < 0 || index >= diff.hunks.size()) return {};
    QByteArray out;
    for (const QByteArray& h : diff.header) {
        out += h;
        out += '\n';
    }
    const DiffHunk& hunk = diff.hunks[index];
    out += hunk.header;
    out += '\n';
    for (const DiffLine& l : hunk.lines) appendLine(out, l.type, l.raw);
    return out;
}

QByteArray linesPatch(const FileDiff& diff, const QSet<quint64>& selected, bool reverse) {
    int selectedChanges = 0;
    for (int h = 0; h < diff.hunks.size(); ++h) {
        const auto& lines = diff.hunks[h].lines;
        for (int i = 0; i < lines.size(); ++i) {
            if ((lines[i].type == '+' || lines[i].type == '-') && selected.contains(lineKey(h, i))) ++selectedChanges;
        }
    }
    if (selectedChanges == 0) return {};
    const bool partial = selectedChanges < diff.changeCount();

    QByteArray out;
    for (const QByteArray& h : diff.header) {
        if (partial) {
            // A partial patch never creates or removes the whole file.
            if (h.startsWith("new file mode") || h.startsWith("deleted file mode") || h.startsWith("index ")) continue;
            if (h == "--- /dev/null") {
                out += "--- a/" + diff.newPath.toUtf8() + '\n';
                continue;
            }
            if (h == "+++ /dev/null") {
                out += "+++ b/" + diff.oldPath.toUtf8() + '\n';
                continue;
            }
        }
        out += h;
        out += '\n';
    }

    int delta = 0;  // accumulated (new - old) line count of emitted hunks
    for (int h = 0; h < diff.hunks.size(); ++h) {
        const DiffHunk& hunk = diff.hunks[h];
        QByteArray body;
        int oldCount = 0;
        int newCount = 0;
        bool hasChange = false;
        bool lastKept = false;
        for (int i = 0; i < hunk.lines.size(); ++i) {
            const DiffLine& l = hunk.lines[i];
            const bool sel = selected.contains(lineKey(h, i));
            switch (l.type) {
                case ' ':
                    appendLine(body, ' ', l.raw);
                    ++oldCount;
                    ++newCount;
                    lastKept = true;
                    break;
                case '-':
                    if (sel) {
                        appendLine(body, '-', l.raw);
                        ++oldCount;
                        hasChange = true;
                        lastKept = true;
                    } else if (!reverse) {
                        appendLine(body, ' ', l.raw);
                        ++oldCount;
                        ++newCount;
                        lastKept = true;
                    } else {
                        lastKept = false;
                    }
                    break;
                case '+':
                    if (sel) {
                        appendLine(body, '+', l.raw);
                        ++newCount;
                        hasChange = true;
                        lastKept = true;
                    } else if (reverse) {
                        appendLine(body, ' ', l.raw);
                        ++oldCount;
                        ++newCount;
                        lastKept = true;
                    } else {
                        lastKept = false;
                    }
                    break;
                case '\\':
                    if (lastKept) appendLine(body, '\\', l.raw);
                    break;
                default: break;
            }
        }
        if (!hasChange) continue;
        int oldStart;
        int newStart;
        if (!reverse) {
            oldStart = hunk.oldStart;
            newStart = hunk.oldStart + delta;
        } else {
            newStart = hunk.newStart;
            oldStart = hunk.newStart - delta;
        }
        out += QStringLiteral("@@ -%1,%2 +%3,%4 @@").arg(oldStart).arg(oldCount).arg(newStart).arg(newCount).toLatin1();
        out += hunk.section;
        out += '\n';
        out += body;
        delta += newCount - oldCount;
    }
    return out;
}

}  // namespace aslgc
