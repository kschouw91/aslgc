// SPDX-License-Identifier: MIT
#pragma once

#include "Git.h"

#include <QHash>
#include <QStringList>

namespace aslgc {

class Repository;

// The git-flow branching model, carried out with plain git commands. Settings use the
// `gitflow.*` keys of the git-flow (AVH) command line tool, so both can be used side by side.
namespace flow {

// Support branches are long-lived: they carry an older release line and are never finished.
enum class Kind { Feature, Bugfix, Release, Hotfix, Support };
inline constexpr Kind kKinds[] = {Kind::Feature, Kind::Bugfix, Kind::Release, Kind::Hotfix, Kind::Support};

struct Config {
    QString production;  // gitflow.branch.master
    QString develop;     // gitflow.branch.develop
    QString featurePrefix = QStringLiteral("feature/");
    QString bugfixPrefix = QStringLiteral("bugfix/");
    QString releasePrefix = QStringLiteral("release/");
    QString hotfixPrefix = QStringLiteral("hotfix/");
    QString supportPrefix = QStringLiteral("support/");
    QString versionTagPrefix;
    QHash<QString, QString> bases;  // gitflow.branch.<branch>.base: what a flow branch was started from
    QStringList sections;           // configuration sections present, e.g. "gitflow.branch"

    bool isInitialized() const { return !production.isEmpty() && !develop.isEmpty(); }
    QString prefix(Kind kind) const;
    // Recognises a flow branch, e.g. "feature/login" → Feature, "login".
    bool classify(const QString& branch, Kind* kind, QString* name) const;
    // Releases and hotfixes are named after a version and get a tag when finished.
    static bool isVersioned(Kind kind) { return kind == Kind::Release || kind == Kind::Hotfix; }
    static bool isFinishable(Kind kind) { return kind != Kind::Support; }
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

// Creates `<prefix><name>` from `base` and checks it out. An empty base means develop, or production
// for hotfixes and support branches; a support branch must start from a commit of production. A
// local branch used as the base is recorded, so finishing merges back into it.
Plan planStart(const Repository& repo, const Config& config, Kind kind, const QString& name, const QString& base = {});

// Where finishing a flow branch merges it: into `into` (where releases and hotfixes are tagged),
// then into `back` unless it is empty.
struct Targets {
    QString into;
    QString back;
};

// Features and bugfixes go into the branch they started from, or develop. Releases go into
// production and back into develop. Hotfixes do the same, but go back into the open release
// branch instead of develop when there is one; a hotfix started from a support branch only goes
// into that support branch. `localBranches` are the repository's local branch names.
Targets finishTargets(const Config& config, Kind kind, const QString& name, const QStringList& localBranches);

struct FinishOptions {
    bool keepBranch = false;
    bool squash = false;         // features and bugfixes: one commit on the target
    bool noFastForward = false;  // features and bugfixes: always a merge commit
    bool tag = true;             // releases and hotfixes
    QString tagMessage;          // default: the tag name
    // The branches involved are compared with their copies on this remote: one that is behind is
    // fast-forwarded first, one that has diverged stops the finish. Empty: no comparison.
    QString remote;
    bool push = false;  // fill pushArgs to publish the result to `remote`
};

// Merges as described for finishTargets, tags releases and hotfixes, and deletes the branch.
// Steps that are already done are skipped, so after resolving a conflict the same call carries
// on where it stopped. Ends on the last branch merged into.
Plan planFinish(const Repository& repo, const Config& config, Kind kind, const QString& name, const FinishOptions& options = {});

// Next version after the highest `<tagPrefix>X.Y.Z` tag: a minor step for releases, a patch
// step for hotfixes. Without such tags, "1.0.0" for releases and nothing for hotfixes.
QString suggestVersion(const QStringList& tags, const QString& tagPrefix, Kind kind);
// The `<tagPrefix>X.Y.Z` tags among `tags`, highest version first.
QStringList versionTags(const QStringList& tags, const QString& tagPrefix);
// Tags that are reachable from `ref`, e.g. the releases a support branch is built on.
QStringList tagsReachableFrom(const Repository& repo, const QString& ref);

}  // namespace flow
}  // namespace aslgc
