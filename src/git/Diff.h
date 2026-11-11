// SPDX-License-Identifier: MIT
#pragma once

#include "Types.h"

#include <QByteArray>
#include <QSet>
#include <QString>
#include <QVector>

namespace aslgc {

struct DiffLine {
    char type = ' ';  // ' ', '+', '-', '\\'
    QByteArray raw;   // content without the leading type character (exact bytes)
    int oldLine = -1;
    int newLine = -1;

    QString text() const {
        QString s = QString::fromUtf8(raw);
        if (s.endsWith(QLatin1Char('\r'))) s.chop(1);
        return s;
    }
};

struct DiffHunk {
    int oldStart = 0;
    int oldCount = 0;
    int newStart = 0;
    int newCount = 0;
    QByteArray header;   // full "@@ -a,b +c,d @@ section" line
    QByteArray section;  // text after the second "@@"
    QVector<DiffLine> lines;

    int changeCount() const;
};

struct FileDiff {
    QVector<QByteArray> header;  // "diff --git", "index", "---", "+++", ...
    QVector<DiffHunk> hunks;
    QString oldPath;
    QString newPath;
    bool binary = false;
    bool newFile = false;
    bool deletedFile = false;

    int changeCount() const;
};

QVector<FileDiff> parseDiff(const QByteArray& data);

// The old version's line (1-based) that corresponds to `newLine` in the new version. A line that
// only exists in the new version maps to the old line just before it (at least 1).
int mapToOldLine(const FileDiff& diff, int newLine);

// One commit in the history of a range of lines (`git log -L`).
struct LineLogEntry {
    Commit commit;
    FileDiff diff;  // the commit's changes to those lines; no hunks for merges
};

inline quint64 lineKey(int hunk, int line) { return (quint64(quint32(hunk)) << 32) | quint32(line); }

// Patch containing a single, unmodified hunk.
QByteArray hunkPatch(const FileDiff& diff, int hunk);

// Patch containing only the selected +/- lines (keys built with lineKey()).
// `reverse` must match how the patch will be applied (`git apply --reverse`):
//   forward (stage):           unselected '-' become context, unselected '+' are dropped
//   reverse (unstage/discard): unselected '+' become context, unselected '-' are dropped
QByteArray linesPatch(const FileDiff& diff, const QSet<quint64>& selected, bool reverse);

}  // namespace aslgc
