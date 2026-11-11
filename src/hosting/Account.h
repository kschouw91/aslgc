// SPDX-License-Identifier: MIT
#pragma once

#include <QString>
#include <QVector>

namespace aslgc {

enum class Provider { GitHub, GitLab };

QString providerName(Provider provider);
QString providerKey(Provider provider);  // "github" / "gitlab"
// Term used for a review request on this provider ("Pull Request" / "Merge Request").
QString reviewRequestName(Provider provider);

// A signed-in hosting account. The token itself lives in the system keyring
// (service kKeychainService, key keychainKey()), never in settings.
struct Account {
    Provider provider = Provider::GitHub;
    QString host;  // github.com, gitlab.com, git.example.com[:port]
    QString username;
    QString displayName;

    QString webUrl() const;  // https://host
    QString apiUrl() const;  // REST API base
    QString keychainKey() const;
    QString label() const;  // "username @ host"
    bool isValid() const { return !host.isEmpty() && !username.isEmpty(); }
};

inline constexpr const char* kKeychainService = "aslgc";

namespace accounts {
// Persisted in QSettings under "accounts"; requires the application/organisation names to be set.
QVector<Account> load();
void save(const QVector<Account>& list);
// Account for a remote host; prefers a matching username when one is given.
const Account* findForHost(const QVector<Account>& list, const QString& host, const QString& username = {});
QString defaultHost(Provider provider);
QString tokenCreationUrl(Provider provider, const QString& host);
}  // namespace accounts

}  // namespace aslgc
