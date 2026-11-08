// SPDX-License-Identifier: MIT
#pragma once

#include "Repository.h"

namespace aslgc {

enum class RebaseAction { Pick, Reword, Edit, Squash, Fixup, Drop };

struct RebaseStep {
    RebaseAction action = RebaseAction::Pick;
    QString sha;
    QString subject;
    QString message;  // full message; the new message for Reword
    QString authorName;
};

// Commits that `git rebase -i <base>` would replay, oldest first.
struct RebaseRange {
    QVector<RebaseStep> steps;
    int mergeCount = 0;      // merge commits in the range; the rebase flattens them
    bool published = false;  // some of the commits already exist on a remote branch
    QString error;
};

QString rebaseActionName(RebaseAction action);  // "pick", "reword", …

RebaseRange loadRebaseRange(const Repository& repo, const QString& base);

// Empty when the plan can run; otherwise a reason suitable for the UI.
QString validateRebasePlan(const QVector<RebaseStep>& steps);

// Writes the todo list (and reword messages) into `dir` and returns git arguments that run
// the rebase with exactly that todo list, without opening an editor. Empty on failure.
QStringList prepareRebase(const QVector<RebaseStep>& steps, const QString& base, const QString& dir, bool autostash,
                          QString* error);

QString shellQuote(const QString& text);

}  // namespace aslgc
