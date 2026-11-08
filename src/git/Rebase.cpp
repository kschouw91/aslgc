// SPDX-License-Identifier: MIT
#include "Rebase.h"

#include <QDir>
#include <QFile>

namespace aslgc {

QString rebaseActionName(RebaseAction action) {
    switch (action) {
        case RebaseAction::Pick: return QStringLiteral("pick");
        case RebaseAction::Reword: return QStringLiteral("reword");
        case RebaseAction::Edit: return QStringLiteral("edit");
        case RebaseAction::Squash: return QStringLiteral("squash");
        case RebaseAction::Fixup: return QStringLiteral("fixup");
        case RebaseAction::Drop: return QStringLiteral("drop");
    }
    return QStringLiteral("pick");
}

QString shellQuote(const QString& text) {
    QString quoted = text;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

RebaseRange loadRebaseRange(const Repository& repo, const QString& base) {
    RebaseRange range;
    const Git& git = repo.git();
    const QString revs = base + QStringLiteral("..HEAD");
    const GitResult log = git.run({QStringLiteral("log"), QStringLiteral("--reverse"), QStringLiteral("--no-merges"),
                                   QStringLiteral("--format=%H%x1f%an%x1f%s%x1f%B%x1e"), revs, QStringLiteral("--")});
    if (!log.ok()) {
        range.error = log.message();
        return range;
    }
    for (const QByteArray& raw : log.out.split('\x1e')) {
        QByteArray rec = raw;
        while (rec.startsWith('\n')) rec.remove(0, 1);
        if (rec.isEmpty()) continue;
        const QList<QByteArray> f = rec.split('\x1f');
        if (f.size() < 4) continue;
        RebaseStep step;
        step.sha = QString::fromLatin1(f[0]);
        step.authorName = QString::fromUtf8(f[1]);
        step.subject = QString::fromUtf8(f[2]);
        step.message = QString::fromUtf8(f[3]).trimmed();
        range.steps.push_back(step);
    }
    if (range.steps.isEmpty()) {
        range.error = QStringLiteral("There are no commits to rebase.");
        return range;
    }
    const GitResult merges = git.run({QStringLiteral("rev-list"), QStringLiteral("--count"), QStringLiteral("--merges"), revs});
    range.mergeCount = merges.ok() ? merges.outText().trimmed().toInt() : 0;
    const GitResult remote =
        git.run({QStringLiteral("for-each-ref"), QStringLiteral("--count=1"), QStringLiteral("--format=%(refname)"),
                 QStringLiteral("--contains=") + range.steps.first().sha, QStringLiteral("refs/remotes")});
    range.published = remote.ok() && !remote.outText().trimmed().isEmpty();
    return range;
}

QString validateRebasePlan(const QVector<RebaseStep>& steps) {
    if (steps.isEmpty()) return QStringLiteral("There are no commits to rebase.");
    bool hasEarlier = false;
    for (const RebaseStep& s : steps) {
        if ((s.action == RebaseAction::Squash || s.action == RebaseAction::Fixup) && !hasEarlier) {
            return QStringLiteral("“%1” can't be squashed or fixed up: there is no kept commit above it to combine it with.")
                .arg(s.subject);
        }
        if (s.action == RebaseAction::Reword && s.message.trimmed().isEmpty()) {
            return QStringLiteral("The new message for “%1” is empty.").arg(s.subject);
        }
        if (s.action != RebaseAction::Drop) hasEarlier = true;
    }
    return {};
}

QStringList prepareRebase(const QVector<RebaseStep>& steps, const QString& base, const QString& dir, bool autostash,
                          QString* error) {
    auto fail = [error](const QString& message) {
        if (error) *error = message;
        return QStringList();
    };
    if (const QString invalid = validateRebasePlan(steps); !invalid.isEmpty()) return fail(invalid);
    if (!QDir().mkpath(dir)) return fail(QStringLiteral("Could not create %1").arg(dir));

    QByteArray todo;
    int rewordIndex = 0;
    for (const RebaseStep& s : steps) {
        const RebaseAction action = s.action == RebaseAction::Reword ? RebaseAction::Pick : s.action;
        todo += rebaseActionName(action).toLatin1() + ' ' + s.sha.toLatin1() + ' ' + s.subject.toUtf8() + '\n';
        if (s.action == RebaseAction::Reword) {
            // Amend right after picking, so later squash / fixup steps build on the new message.
            const QString path = QDir(dir).filePath(QStringLiteral("message-%1.txt").arg(++rewordIndex));
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(s.message.toUtf8() + '\n') < 0) {
                return fail(QStringLiteral("Could not write %1").arg(path));
            }
            todo += "exec git commit --amend --allow-empty --cleanup=whitespace -F " + shellQuote(path).toUtf8() + '\n';
        }
    }
    const QString todoPath = QDir(dir).filePath(QStringLiteral("todo"));
    QFile f(todoPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(todo) != todo.size()) {
        return fail(QStringLiteral("Could not write %1").arg(todoPath));
    }
    // git runs `<sequence.editor> <its todo file>`: replace its proposal with ours.
    return {QStringLiteral("-c"),
            QStringLiteral("sequence.editor=cp ") + shellQuote(todoPath),
            QStringLiteral("rebase"),
            QStringLiteral("--interactive"),
            autostash ? QStringLiteral("--autostash") : QStringLiteral("--no-autostash"),
            base};
}

}  // namespace aslgc
