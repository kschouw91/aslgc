// SPDX-License-Identifier: MIT
#pragma once

#include "Git.h"

#include <QStringList>

namespace aslgc {

class Repository;

// The git-flow branching model, carried out with plain git commands. Settings use the
// `gitflow.*` keys of the git-flow (AVH) command line tool, so both can be used side by side.
namespace flow {

enum class Kind { Feature, Bugfix, Release, Hotfix };
inline constexpr Kind kKinds[] = {Kind::Feature, Kind::Bugfix, Kind::Release, Kind::Hotfix};

struct Config {
    QString production;  // gitflow.branch.master
    QString develop;     // gitflow.branch.develop
    QString featurePrefix = QStringLiteral("feature/");
    QString bugfixPrefix = QStringLiteral("bugfix/");
    QString releasePrefix = QStringLiteral("release/");
    QString hotfixPrefix = QStringLiteral("hotfix/");
    QString supportPrefix = QStringLiteral("support/");
    QString versionTagPrefix;
    QStringList sections;  // configuration sections present, e.g. "gitflow.branch"

    bool isInitialized() const { return !production.isEmpty() && !develop.isEmpty(); }
    QString prefix(Kind kind) const;
    // Recognises a flow branch, e.g. "feature/login" → Feature, "login".
    bool classify(const QString& branch, Kind* kind, QString* name) const;
    // Releases and hotfixes are named after a version and get a tag when finished.
    static bool isVersioned(Kind kind) { return kind == Kind::Release || kind == Kind::Hotfix; }
};

Config parseConfig(const QByteArray& data);  // `git config -z --get-regexp ^gitflow\.`
Config loadConfig(const Repository& repo);   // the repository's own (local) settings

// Commands for an operation, or the reason it cannot run.
struct Plan {
    QVector<GitCommand> commands;
    QStringList pushArgs;  // `git push` arguments to publish the result (finish only)
    QString error;

    bool ok() const { return error.isEmpty(); }
};

// Writes the settings; creates the develop branch when it is missing (tracking `remote`'s copy
// when there is one) and, with `checkoutDevelop`, checks it out.
Plan planInit(const Repository& repo, const Config& config, const QString& remote, bool checkoutDevelop);
Plan planRemove(const Config& config);

// Creates `<prefix><name>` from `base` (empty: develop, or production for hotfixes) and checks it out.
Plan planStart(const Repository& repo, const Config& config, Kind kind, const QString& name, const QString& base = {});

struct FinishOptions {
    bool keepBranch = false;
    bool squash = false;         // features and bugfixes: one commit on develop
    bool noFastForward = false;  // features and bugfixes: always a merge commit
    bool tag = true;             // releases and hotfixes
    QString tagMessage;          // default: the tag name
    QString remote;              // for pushArgs; empty: nothing to push
};

// Features and bugfixes merge into develop. Releases and hotfixes merge into production, get a
// tag there and merge back into develop (a hotfix into the open release branch, if there is one).
// Steps that are already done are skipped, so after resolving a conflict the same call carries
// on where it stopped. Ends on develop (or the branch the hotfix went into).
Plan planFinish(const Repository& repo, const Config& config, Kind kind, const QString& name, const FinishOptions& options = {});

// Next version after the highest `<tagPrefix>X.Y.Z` tag: a minor step for releases, a patch
// step for hotfixes. Without such tags, "1.0.0" for releases and nothing for hotfixes.
QString suggestVersion(const QStringList& tags, const QString& tagPrefix, Kind kind);

}  // namespace flow
}  // namespace aslgc
