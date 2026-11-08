// SPDX-License-Identifier: MIT
#include "GitFlow.h"

#include "Repository.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QVersionNumber>

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
    for (const QString& prefix : {c.featurePrefix, c.bugfixPrefix, c.releasePrefix, c.hotfixPrefix}) {
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
    if (base.isEmpty()) base = kind == Kind::Hotfix ? c.production : c.develop;
    if (!refExists(git, base)) {
        p.error = tr("“%1” does not exist.").arg(base);
        return p;
    }
    p.commands << cmd({s("checkout"), s("-b"), branch, base});
    return p;
}

Plan planFinish(const Repository& repo, const Config& c, Kind kind, const QString& name, const FinishOptions& o) {
    Plan p;
    const Git& git = repo.git();
    if (!c.isInitialized()) {
        p.error = tr("Git-flow is not set up in this repository.");
        return p;
    }
    const QString branch = c.prefix(kind) + name;
    if (!refExists(git, s("refs/heads/") + branch)) {
        p.error = tr("The branch “%1” does not exist.").arg(branch);
        return p;
    }
    for (const QString& b : {c.production, c.develop}) {
        if (!refExists(git, s("refs/heads/") + b)) {
            p.error = tr("The branch “%1” does not exist.").arg(b);
            return p;
        }
    }
    p.error = checkWorkTree(repo);
    if (!p.ok()) return p;

    QStringList push;
    auto pushBranch = [&push](const QString& b) { push << s("refs/heads/%1:refs/heads/%1").arg(b); };
    if (!Config::isVersioned(kind)) {
        const QString target = c.develop;
        p.commands << cmd({s("checkout"), target});
        if (o.squash) {
            if (commitsMissing(git, target, branch) > 0) {
                p.commands << cmd({s("merge"), s("--squash"), branch});
                p.commands << cmd({s("commit")});  // takes git's prepared "Squashed commit of …" message
            }
        } else if (!isAncestor(git, branch, target)) {
            // A single commit is fast-forwarded when possible, like the git-flow tools do.
            const bool single = commitsMissing(git, target, branch) == 1;
            p.commands << cmd({s("merge"), o.noFastForward || !single ? s("--no-ff") : s("--ff"), branch});
        }
        pushBranch(target);
    } else {
        const QString tag = c.versionTagPrefix + name;
        // A hotfix also goes into the open release branch instead of develop, so it is not lost.
        QString back = c.develop;
        if (kind == Kind::Hotfix) {
            const QStringList releases = withPrefix(localBranches(git), c.releasePrefix);
            if (!releases.isEmpty()) back = releases.first();
        }
        p.commands << cmd({s("checkout"), c.production});
        if (!isAncestor(git, branch, c.production)) p.commands << cmd({s("merge"), s("--no-ff"), branch});
        pushBranch(c.production);
        if (o.tag) {
            if (!tagExists(git, tag)) {
                const QString message = o.tagMessage.trimmed().isEmpty() ? tag : o.tagMessage.trimmed();
                p.commands << cmd({s("tag"), s("-a"), tag, s("-m"), message});
            }
            push << s("refs/tags/%1:refs/tags/%1").arg(tag);
        }
        p.commands << cmd({s("checkout"), back});
        if (!isAncestor(git, branch, back)) p.commands << cmd({s("merge"), s("--no-ff"), branch});
        pushBranch(back);
    }
    if (!o.keepBranch) {
        p.commands << cmd({s("branch"), o.squash && !Config::isVersioned(kind) ? s("-D") : s("-d"), branch});
        if (!o.remote.isEmpty() && refExists(git, s("refs/remotes/%1/%2").arg(o.remote, branch)))
            push << s(":refs/heads/%1").arg(branch);
    }
    if (!o.remote.isEmpty()) p.pushArgs = QStringList{s("push"), s("--progress"), o.remote} + push;
    return p;
}

QString suggestVersion(const QStringList& tags, const QString& tagPrefix, Kind kind) {
    static const QRegularExpression versionRe(QStringLiteral("^\\d+(\\.\\d+){0,3}$"));
    QVersionNumber highest;
    for (const QString& tag : tags) {
        if (!tag.startsWith(tagPrefix)) continue;
        const QString v = tag.mid(tagPrefix.size());
        if (!versionRe.match(v).hasMatch()) continue;
        const QVersionNumber n = QVersionNumber::fromString(v);
        if (n > highest) highest = n;
    }
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
