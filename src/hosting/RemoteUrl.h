// SPDX-License-Identifier: MIT
#pragma once

#include "Account.h"

#include <QString>
#include <optional>

namespace aslgc {

// A repository on a hosting service, identified from a git remote URL.
struct RemoteRepo {
    QString host;  // web host, e.g. github.com or git.example.com:8443
    QString path;  // owner/repo, or group/subgroup/repo on GitLab (no ".git")
    bool valid = false;

    QString name() const { return path.section(QLatin1Char('/'), -1); }
};

// Understands https://, http://, ssh://, git:// and scp-like (git@host:owner/repo.git) URLs.
RemoteRepo parseRemoteUrl(const QString& url);

// github.com / gitlab.com are always recognised; other hosts only via a signed-in account.
std::optional<Provider> detectProvider(const QString& host, const QVector<Account>& accounts);

QString webRepoUrl(Provider provider, const RemoteRepo& repo);
QString webCommitUrl(Provider provider, const RemoteRepo& repo, const QString& sha);
QString webBranchUrl(Provider provider, const RemoteRepo& repo, const QString& branch);
QString webNewReviewUrl(Provider provider, const RemoteRepo& repo, const QString& source, const QString& target);

}  // namespace aslgc
