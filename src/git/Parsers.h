// SPDX-License-Identifier: MIT
#pragma once

#include "Diff.h"
#include "Types.h"

namespace aslgc::parse {

// Format strings passed to git; the matching parser below understands the output.
extern const char* const kLogFormat;
extern const char* const kRefFormat;
extern const char* const kStashFormat;
extern const char* const kFileLogFormat;  // record separator first, so `--name-status -z` output follows it
extern const char* const kReflogFormat;   // with --walk-reflogs

QVector<Commit> log(const QByteArray& data);
QVector<Ref> refs(const QByteArray& data);
QVector<Stash> stashes(const QByteArray& data);
QVector<Remote> remotes(const QByteArray& data);         // `git remote -v`
WorkingStatus status(const QByteArray& data);            // `git status --porcelain=v2 -z`
QVector<FileChange> nameStatus(const QByteArray& data);  // `--name-status -z`
CommitInfo commitInfo(const QByteArray& data);           // kLogFormat with %B instead of %s
QVector<FileLogEntry> fileLog(const QByteArray& data);   // kFileLogFormat [--name-status] -z
Blame blame(const QByteArray& data);                     // `git blame --porcelain`
QVector<LineLogEntry> lineLog(const QByteArray& data);   // kFileLogFormat with -L (patch output)
QVector<ReflogEntry> reflog(const QByteArray& data);     // kReflogFormat; `time` is left 0
// `--walk-reflogs --date=unix --format=%gd`: when each entry was made, in the same order.
QVector<qint64> reflogTimes(const QByteArray& data);

}  // namespace aslgc::parse
