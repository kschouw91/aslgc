// SPDX-License-Identifier: MIT
#include "Account.h"

#include <QSettings>
#include <QUrl>

namespace aslgc {

QString providerName(Provider provider) {
    return provider == Provider::GitHub ? QStringLiteral("GitHub") : QStringLiteral("GitLab");
}

QString providerKey(Provider provider) {
    return provider == Provider::GitHub ? QStringLiteral("github") : QStringLiteral("gitlab");
}

QString reviewRequestName(Provider provider) {
    return provider == Provider::GitHub ? QStringLiteral("Pull Request") : QStringLiteral("Merge Request");
}

QString Account::webUrl() const { return QStringLiteral("https://") + host; }

QString Account::apiUrl() const {
    if (provider == Provider::GitHub) {
        return host.compare(QLatin1String("github.com"), Qt::CaseInsensitive) == 0 ? QStringLiteral("https://api.github.com")
                                                                                   : webUrl() + QStringLiteral("/api/v3");
    }
    return webUrl() + QStringLiteral("/api/v4");
}

QString Account::keychainKey() const {
    return providerKey(provider) + QLatin1Char(':') + host.toLower() + QLatin1Char(':') + username;
}

QString Account::label() const { return username + QStringLiteral(" @ ") + host; }

namespace accounts {

QVector<Account> load() {
    QVector<Account> list;
    QSettings settings;
    const int n = settings.beginReadArray(QStringLiteral("accounts"));
    for (int i = 0; i < n; ++i) {
        settings.setArrayIndex(i);
        Account a;
        a.provider = settings.value(QStringLiteral("provider")).toString() == QLatin1String("gitlab") ? Provider::GitLab
                                                                                                      : Provider::GitHub;
        a.host = settings.value(QStringLiteral("host")).toString();
        a.username = settings.value(QStringLiteral("username")).toString();
        a.displayName = settings.value(QStringLiteral("name")).toString();
        if (a.isValid()) list.push_back(a);
    }
    settings.endArray();
    return list;
}

void save(const QVector<Account>& list) {
    QSettings settings;
    settings.remove(QStringLiteral("accounts"));
    settings.beginWriteArray(QStringLiteral("accounts"), int(list.size()));
    for (int i = 0; i < list.size(); ++i) {
        settings.setArrayIndex(i);
        settings.setValue(QStringLiteral("provider"), providerKey(list[i].provider));
        settings.setValue(QStringLiteral("host"), list[i].host);
        settings.setValue(QStringLiteral("username"), list[i].username);
        settings.setValue(QStringLiteral("name"), list[i].displayName);
    }
    settings.endArray();
}

const Account* findForHost(const QVector<Account>& list, const QString& host, const QString& username) {
    const Account* fallback = nullptr;
    for (const Account& a : list) {
        if (a.host.compare(host, Qt::CaseInsensitive) != 0) continue;
        if (username.isEmpty() || a.username.compare(username, Qt::CaseInsensitive) == 0) return &a;
        if (!fallback) fallback = &a;
    }
    return username.isEmpty() ? fallback : nullptr;
}

QString defaultHost(Provider provider) {
    return provider == Provider::GitHub ? QStringLiteral("github.com") : QStringLiteral("gitlab.com");
}

QString tokenCreationUrl(Provider provider, const QString& host) {
    if (provider == Provider::GitHub) {
        return QStringLiteral("https://%1/settings/tokens/new?scopes=repo,read:org,workflow&description=aslgc").arg(host);
    }
    return QStringLiteral(
               "https://%1/-/user_settings/"
               "personal_access_tokens?name=aslgc&scopes=api,read_user,read_repository,write_repository")
        .arg(host);
}

}  // namespace accounts

}  // namespace aslgc
