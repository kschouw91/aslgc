// SPDX-License-Identifier: MIT
#include "RemoteUrl.h"

#include <QRegularExpression>
#include <QUrl>

namespace aslgc {

namespace {

QString cleanPath(QString path) {
    while (path.startsWith(QLatin1Char('/'))) path.remove(0, 1);
    while (path.endsWith(QLatin1Char('/'))) path.chop(1);
    if (path.endsWith(QLatin1String(".git"))) path.chop(4);
    return path;
}

// SSH endpoints that serve the same repositories as the web host.
QString webHostFor(const QString& sshHost) {
    const QString h = sshHost.toLower();
    if (h == QLatin1String("ssh.github.com")) return QStringLiteral("github.com");
    if (h == QLatin1String("altssh.gitlab.com")) return QStringLiteral("gitlab.com");
    return h;
}

QString encodeRef(const QString& ref) { return QString::fromUtf8(QUrl::toPercentEncoding(ref, "/")); }

}  // namespace

RemoteRepo parseRemoteUrl(const QString& input) {
    RemoteRepo repo;
    const QString url = input.trimmed();
    if (url.isEmpty()) return repo;

    static const QRegularExpression scpLike(QStringLiteral("^(?:[^@/]+@)?([^:/]+):(?!//)(.+)$"));
    if (!url.contains(QLatin1String("://"))) {
        const auto m = scpLike.match(url);
        if (!m.hasMatch()) return repo;
        repo.host = webHostFor(m.captured(1));
        repo.path = cleanPath(m.captured(2));
    } else {
        const QUrl u(url);
        if (!u.isValid() || u.host().isEmpty()) return repo;
        const QString scheme = u.scheme().toLower();
        if (scheme == QLatin1String("https") || scheme == QLatin1String("http")) {
            repo.host = u.host().toLower();
            if (u.port() != -1 && u.port() != 443 && u.port() != 80) repo.host += QLatin1Char(':') + QString::number(u.port());
        } else {
            repo.host = webHostFor(u.host());  // ssh / git: the SSH port is not the web port
        }
        repo.path = cleanPath(u.path());
    }
    repo.valid = !repo.host.isEmpty() && repo.path.contains(QLatin1Char('/'));
    return repo;
}

std::optional<Provider> detectProvider(const QString& host, const QVector<Account>& accounts) {
    const QString h = host.toLower();
    if (h == QLatin1String("github.com")) return Provider::GitHub;
    if (h == QLatin1String("gitlab.com")) return Provider::GitLab;
    for (const Account& a : accounts) {
        if (a.host.compare(h, Qt::CaseInsensitive) == 0) return a.provider;
    }
    return std::nullopt;
}

QString webRepoUrl(Provider, const RemoteRepo& repo) { return QStringLiteral("https://%1/%2").arg(repo.host, repo.path); }

QString webCommitUrl(Provider provider, const RemoteRepo& repo, const QString& sha) {
    return webRepoUrl(provider, repo) +
           (provider == Provider::GitHub ? QStringLiteral("/commit/") : QStringLiteral("/-/commit/")) + sha;
}

QString webBranchUrl(Provider provider, const RemoteRepo& repo, const QString& branch) {
    return webRepoUrl(provider, repo) + (provider == Provider::GitHub ? QStringLiteral("/tree/") : QStringLiteral("/-/tree/")) +
           encodeRef(branch);
}

QString webNewReviewUrl(Provider provider, const RemoteRepo& repo, const QString& source, const QString& target) {
    if (provider == Provider::GitHub) {
        return webRepoUrl(provider, repo) + QStringLiteral("/compare/%1...%2?expand=1").arg(encodeRef(target), encodeRef(source));
    }
    return webRepoUrl(provider, repo) +
           QStringLiteral("/-/merge_requests/new?merge_request%5Bsource_branch%5D=%1&merge_request%5Btarget_branch%5D=%2")
               .arg(QString::fromUtf8(QUrl::toPercentEncoding(source)), QString::fromUtf8(QUrl::toPercentEncoding(target)));
}

}  // namespace aslgc
