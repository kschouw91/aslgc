// SPDX-License-Identifier: MIT
#include "GitFlow.h"

#include "Repository.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QVersionNumber>

#include <algorithm>

namespace aslgc::flow {

namespace {

QString tr(const char* text) { return QCoreApplication::translate("GitFlow", text); }

QString s(const char* text) { return QString::fromLatin1(text); }

bool refExists(const Git& git, const QString& ref) {
    return git.run({s("rev-parse"), s("--verify"), s("-q"), ref + s("^{commit}")}).ok();
}

bool tagExists(const Git& git, const QString& tag) {
    return git.run({s("rev-parse"), s("--verify"), s("-q"), s("refs/tags/") + tag}).ok();
}

bool isAncestor(const Git& git, const QString& commit, const QString& of) {
    return git.run({s("merge-base"), s("--is-ancestor"), commit, of}).exitCode == 0;
}

int commitsMissing(const Git& git, const QString& from, const QString& to) {  // in `to`, not in `from`
    const GitResult r = git.run({s("rev-list"), s("--count"), from + s("..") + to});
    return r.ok() ? r.outText().trimmed().toInt() : 0;
}

bool validBranchName(const Git& git, const QString& name) {
    return !name.isEmpty() && git.run({s("check-ref-format"), s("--branch"), name}).ok();
}

QStringList localBranches(const Git& git) {
    const GitResult r = git.run({s("for-each-ref"), s("--format=%(refname:strip=2)"), s("refs/heads")});
    return r.ok() ? r.outText().split(QLatin1Char('\n'), Qt::SkipEmptyParts) : QStringList{};
}

QStringList withPrefix(const QStringList& branches, const QString& prefix) {
    QStringList out;
    for (const QString& b : branches) {
        if (b.startsWith(prefix) && b.size() > prefix.size()) out << b;
    }
    return out;
}

GitCommand cmd(QStringList args) { return {std::move(args), {}}; }

// Local changes to tracked files would get in the way of the checkouts and merges.
QString checkWorkTree(const Repository& repo) {
    if (repo.operation() != Operation::None)
        return tr("Another operation (merge, rebase, cherry-pick or revert) is in progress. Finish or abort it first.");
    const WorkingStatus st = repo.status();
    bool dirty = !st.staged.isEmpty();
    for (const FileChange& f : st.unstaged) dirty |= !f.isUntracked();
    return dirty ? tr("There are uncommitted changes. Commit or stash them first.") : QString();
}

}  // namespace

QString Config::prefix(Kind kind) const {
    switch (kind) {
        case Kind::Feature: return featurePrefix;
        case Kind::Bugfix: return bugfixPrefix;
        case Kind::Release: return releasePrefix;
        case Kind::Hotfix: return hotfixPrefix;
        case Kind::Support: return supportPrefix;
    }
    return {};
}

bool Config::classify(const QString& branch, Kind* kind, QString* name) const {
    if (!isInitialized()) return false;
    for (Kind k : kKinds) {
        const QString p = prefix(k);
        if (!p.isEmpty() && branch.startsWith(p) && branch.size() > p.size()) {
            if (kind) *kind = k;
            if (name) *name = branch.mid(p.size());
            return true;
        }
    }
    return false;
}

Config parseConfig(const QByteArray& data) {
    Config c;
    for (const QByteArray& entry : data.split('\0')) {
        if (entry.isEmpty()) continue;
        const qsizetype nl = entry.indexOf('\n');
        const QString rawKey = QString::fromUtf8(nl < 0 ? entry : entry.left(nl));
        const QString key = rawKey.toLower();  // subsection names keep their case in `sections`
        const QString value = nl < 0 ? QString() : QString::fromUtf8(entry.mid(nl + 1));
        const QString section = rawKey.left(rawKey.lastIndexOf(QLatin1Char('.')));
        if (!section.isEmpty() && !c.sections.contains(section)) c.sections << section;
        if (key == QLatin1String("gitflow.branch.master"))
            c.production = value;
        else if (key == QLatin1String("gitflow.branch.develop"))
            c.develop = value;
        else if (key == QLatin1String("gitflow.prefix.feature"))
            c.featurePrefix = value;
        else if (key == QLatin1String("gitflow.prefix.bugfix"))
            c.bugfixPrefix = value;
        else if (key == QLatin1String("gitflow.prefix.release"))
            c.releasePrefix = value;
        else if (key == QLatin1String("gitflow.prefix.hotfix"))
            c.hotfixPrefix = value;
        else if (key == QLatin1String("gitflow.prefix.support"))
            c.supportPrefix = value;
        else if (key == QLatin1String("gitflow.prefix.versiontag"))
            c.versionTagPrefix = value;
        else if (key.startsWith(QLatin1String("gitflow.branch.")) && key.endsWith(QLatin1String(".base")))
            c.bases.insert(rawKey.mid(15, rawKey.size() - 20), value);  // the branch name keeps its case
    }
    return c;
}

Config loadConfig(const Repository& repo) {
    // Exit code 1 just means there are no git-flow settings.
    const GitResult r = repo.git().run({s("config"), s("--local"), s("-z"), s("--get-regexp"), QStringLiteral("^gitflow\\.")});
    return parseConfig(r.out);
}

Plan planInit(const Repository& repo, const Config& c, const QString& remote, bool checkoutDevelop) {
    Plan p;
    const Git& git = repo.git();
    if (repo.head().unborn) {
        p.error = tr("Make a first commit before setting up git-flow.");
        return p;
    }
    if (c.production.isEmpty() || c.develop.isEmpty()) {
        p.error = tr("Both a production and a develop branch are needed.");
        return p;
    }
    if (c.production == c.develop) {
        p.error = tr("The production and develop branches must be different.");
        return p;
    }
    for (const QString& prefix : {c.featurePrefix, c.bugfixPrefix, c.releasePrefix, c.hotfixPrefix, c.supportPrefix}) {
        if (prefix.isEmpty() || !validBranchName(git, prefix + s("x"))) {
            p.error = tr("“%1” cannot be used as a branch prefix.").arg(prefix);
            return p;
        }
    }
    if (!validBranchName(git, c.develop)) {
        p.error = tr("“%1” is not a valid branch name.").arg(c.develop);
        return p;
    }
    // Missing local branches are created from the remote's copy when it has one.
    auto ensureBranch = [&](const QString& branch, const QString& fallback) {
        if (refExists(git, s("refs/heads/") + branch)) return true;
        const QString remoteRef = remote.isEmpty() ? QString() : s("refs/remotes/%1/%2").arg(remote, branch);
        if (!remoteRef.isEmpty() && refExists(git, remoteRef)) {
            p.commands << cmd({s("branch"), s("--track"), branch, remoteRef});
            return true;
        }
        if (fallback.isEmpty()) return false;
        p.commands << cmd({s("branch"), s("--no-track"), branch, fallback});
        return true;
    };
    if (!ensureBranch(c.production, {})) {
        p.error = tr("The production branch “%1” does not exist.").arg(c.production);
        p.commands.clear();
        return p;
    }
    ensureBranch(c.develop, s("refs/heads/") + c.production);

    QVector<GitCommand> settings;
    auto set = [&settings](const char* key, const QString& value) {
        settings << cmd({s("config"), s("--local"), s(key), value});
    };
    set("gitflow.branch.master", c.production);
    set("gitflow.branch.develop", c.develop);
    set("gitflow.prefix.feature", c.featurePrefix);
    set("gitflow.prefix.bugfix", c.bugfixPrefix);
    set("gitflow.prefix.release", c.releasePrefix);
    set("gitflow.prefix.hotfix", c.hotfixPrefix);
    set("gitflow.prefix.support", c.supportPrefix);
    set("gitflow.prefix.versiontag", c.versionTagPrefix);
    p.commands = settings + p.commands;
    if (checkoutDevelop) p.commands << cmd({s("checkout"), c.develop});
    return p;
}

Plan planRemove(const Config& c) {
    Plan p;
    for (const QString& section : c.sections) {
        if (section.compare(QLatin1String("gitflow"), Qt::CaseInsensitive) == 0 ||
            section.startsWith(QLatin1String("gitflow."), Qt::CaseInsensitive))
            p.commands << cmd({s("config"), s("--local"), s("--remove-section"), section});
    }
    if (p.commands.isEmpty()) p.error = tr("This repository has no git-flow settings.");
    return p;
}

Plan planStart(const Repository& repo, const Config& c, Kind kind, const QString& rawName, const QString& rawBase) {
    Plan p;
    const Git& git = repo.git();
    if (!c.isInitialized()) {
        p.error = tr("Git-flow is not set up in this repository.");
        return p;
    }
    const QString name = rawName.trimmed();
    const QString branch = c.prefix(kind) + name;
    if (name.isEmpty() || !validBranchName(git, branch)) {
        p.error = tr("“%1” is not a valid branch name.").arg(branch);
        return p;
    }
    if (refExists(git, s("refs/heads/") + branch)) {
        p.error = tr("The branch “%1” already exists.").arg(branch);
        return p;
    }
    if (Config::isVersioned(kind)) {
        // Like the git-flow tools: one release and one hotfix at a time.
        const QStringList open = withPrefix(localBranches(git), c.prefix(kind));
        if (!open.isEmpty()) {
            p.error = tr("Finish “%1” first: git-flow works on one %2 branch at a time.")
                          .arg(open.first(), kind == Kind::Release ? tr("release") : tr("hotfix"));
            return p;
        }
        if (tagExists(git, c.versionTagPrefix + name)) {
            p.error = tr("The tag “%1” already exists.").arg(c.versionTagPrefix + name);
            return p;
        }
    }
    QString base = rawBase.trimmed();
    if (base.isEmpty()) base = kind == Kind::Feature || kind == Kind::Bugfix || kind == Kind::Release ? c.develop : c.production;
    if (!refExists(git, base)) {
        p.error = tr("“%1” does not exist.").arg(base);
        return p;
    }
    // Like the git-flow tools: a support branch carries on from something already released.
    if (kind == Kind::Support && !isAncestor(git, base, s("refs/heads/") + c.production)) {
        p.error = tr("A support branch starts from a commit of “%1”, usually a version tag. “%2” is not on it.")
                      .arg(c.production, base);
        return p;
    }
    p.commands << cmd({s("checkout"), s("-b"), branch, base});
    if (Config::isFinishable(kind) && refExists(git, s("refs/heads/") + base))
        p.commands << cmd({s("config"), s("--local"), s("gitflow.branch.%1.base").arg(branch), base});
    return p;
}

Targets finishTargets(const Config& c, Kind kind, const QString& name, const QStringList& localBranches) {
    const QString branch = c.prefix(kind) + name;
    const QString base = c.bases.value(branch);
    const bool baseExists = !base.isEmpty() && base != branch && localBranches.contains(base);
    switch (kind) {
        case Kind::Feature:
        case Kind::Bugfix: return {baseExists ? base : c.develop, {}};
        case Kind::Release: return {c.production, c.develop};
        case Kind::Hotfix: {
            if (baseExists && base.startsWith(c.supportPrefix) && base.size() > c.supportPrefix.size()) return {base, {}};
            // A hotfix also goes into the open release branch instead of develop, so it is not lost.
            const QStringList releases = withPrefix(localBranches, c.releasePrefix);
            return {c.production, releases.isEmpty() ? c.develop : releases.first()};
        }
        case Kind::Support: break;
    }
    return {};
}

Plan planFinish(const Repository& repo, const Config& config, Kind kind, const QString& name, const FinishOptions& o) {
    Plan p;
    const Git& git = repo.git();
    // Branch bases are recorded when a branch starts, so they may be newer than `config`.
    Config c = config;
    const Config stored = loadConfig(repo);
    c.bases = stored.bases;
    c.sections = stored.sections;
    if (!c.isInitialized()) {
        p.error = tr("Git-flow is not set up in this repository.");
        return p;
    }
    if (!Config::isFinishable(kind)) {
        p.error = tr("Support branches stay open; they are not finished.");
        return p;
    }
    const QString branch = c.prefix(kind) + name;
    const QStringList locals = localBranches(git);
    if (!locals.contains(branch)) {
        p.error = tr("The branch “%1” does not exist.").arg(branch);
        return p;
    }
    const Targets targets = finishTargets(c, kind, name, locals);
    for (const QString& b : {targets.into, targets.back}) {
        if (!b.isEmpty() && !locals.contains(b)) {
            p.error = tr("The branch “%1” does not exist.").arg(b);
            return p;
        }
    }
    p.error = checkWorkTree(repo);
    if (!p.ok()) return p;

    // Like the git-flow tools: work on top of what is already on the remote. A branch that is
    // behind its remote copy is fast-forwarded first; one that has diverged needs a pull.
    QHash<QString, QString> fastForward;  // branch → remote-tracking ref it moves to
    for (const QString& b : {branch, targets.into, targets.back}) {
        if (b.isEmpty() || o.remote.isEmpty()) continue;
        const QString local = s("refs/heads/") + b;
        const QString remoteRef = s("refs/remotes/%1/%2").arg(o.remote, b);
        if (!refExists(git, remoteRef) || isAncestor(git, remoteRef, local)) continue;
        if (!isAncestor(git, local, remoteRef)) {
            p.error = tr("“%1” and “%2/%1” have diverged. Pull “%1” first, then finish again.").arg(b, o.remote);
            return p;
        }
        fastForward.insert(b, remoteRef);
    }
    // Where a branch will be once it is up to date, for deciding which steps are left.
    auto tip = [&fastForward](const QString& b) { return fastForward.value(b, s("refs/heads/") + b); };
    auto checkout = [&](const QString& b) {
        p.commands << cmd({s("checkout"), b});
        if (fastForward.contains(b)) p.commands << cmd({s("merge"), s("--ff-only"), fastForward.value(b)});
    };

    QStringList push;
    auto pushBranch = [&push](const QString& b) { push << s("refs/heads/%1:refs/heads/%1").arg(b); };
    if (fastForward.contains(branch)) checkout(branch);
    if (!Config::isVersioned(kind)) {
        const QString& target = targets.into;
        checkout(target);
        if (o.squash) {
            if (commitsMissing(git, tip(target), tip(branch)) > 0) {
                p.commands << cmd({s("merge"), s("--squash"), branch});
                p.commands << cmd({s("commit")});  // takes git's prepared "Squashed commit of …" message
            }
        } else if (!isAncestor(git, tip(branch), tip(target))) {
            // A single commit is fast-forwarded when possible, like the git-flow tools do.
            const bool single = commitsMissing(git, tip(target), tip(branch)) == 1;
            p.commands << cmd({s("merge"), o.noFastForward || !single ? s("--no-ff") : s("--ff"), branch});
        }
        pushBranch(target);
    } else {
        const QString tag = c.versionTagPrefix + name;
        checkout(targets.into);
        if (!isAncestor(git, tip(branch), tip(targets.into))) p.commands << cmd({s("merge"), s("--no-ff"), branch});
        pushBranch(targets.into);
        if (o.tag) {
            if (!tagExists(git, tag)) {
                const QString message = o.tagMessage.trimmed().isEmpty() ? tag : o.tagMessage.trimmed();
                p.commands << cmd({s("tag"), s("-a"), tag, s("-m"), message});
            }
            push << s("refs/tags/%1:refs/tags/%1").arg(tag);
        }
        if (!targets.back.isEmpty()) {
            checkout(targets.back);
            if (!isAncestor(git, tip(branch), tip(targets.back))) p.commands << cmd({s("merge"), s("--no-ff"), branch});
            pushBranch(targets.back);
        }
    }
    const bool publish = o.push && !o.remote.isEmpty();
    if (!o.keepBranch) {
        p.commands << cmd({s("branch"), o.squash && !Config::isVersioned(kind) ? s("-D") : s("-d"), branch});
        const QString section = s("gitflow.branch.") + branch;
        if (c.sections.contains(section)) p.commands << cmd({s("config"), s("--local"), s("--remove-section"), section});
        if (publish && refExists(git, s("refs/remotes/%1/%2").arg(o.remote, branch))) push << s(":refs/heads/%1").arg(branch);
    }
    if (publish) p.pushArgs = QStringList{s("push"), s("--progress"), o.remote} + push;
    return p;
}

namespace {
// The `<tagPrefix>X[.Y[.Z[.W]]]` tags among `tags` with their versions, highest first.
QVector<std::pair<QVersionNumber, QString>> parseVersionTags(const QStringList& tags, const QString& tagPrefix) {
    static const QRegularExpression versionRe(QStringLiteral("^\\d+(\\.\\d+){0,3}$"));
    QVector<std::pair<QVersionNumber, QString>> out;
    for (const QString& tag : tags) {
        if (!tag.startsWith(tagPrefix)) continue;
        const QString v = tag.mid(tagPrefix.size());
        if (versionRe.match(v).hasMatch()) out.push_back({QVersionNumber::fromString(v), tag});
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    return out;
}
}  // namespace

QStringList versionTags(const QStringList& tags, const QString& tagPrefix) {
    QStringList out;
    for (const auto& entry : parseVersionTags(tags, tagPrefix)) out << entry.second;
    return out;
}

QStringList tagsReachableFrom(const Repository& repo, const QString& ref) {
    const GitResult r = repo.git().run({s("tag"), s("--list"), s("--merged"), ref});
    return r.ok() ? r.outText().split(QLatin1Char('\n'), Qt::SkipEmptyParts) : QStringList{};
}

QString suggestVersion(const QStringList& tags, const QString& tagPrefix, Kind kind) {
    const auto parsed = parseVersionTags(tags, tagPrefix);
    const QVersionNumber highest = parsed.isEmpty() ? QVersionNumber() : parsed.first().first;
    if (highest.isNull()) return kind == Kind::Release ? QStringLiteral("1.0.0") : QString();
    QList<int> parts = highest.segments();
    while (parts.size() < 3) parts << 0;
    if (kind == Kind::Hotfix) {
        ++parts[2];
    } else {
        ++parts[1];
        parts[2] = 0;
        for (qsizetype i = 3; i < parts.size(); ++i) parts[i] = 0;
    }
    return QVersionNumber(parts).toString();
}

}  // namespace aslgc::flow
