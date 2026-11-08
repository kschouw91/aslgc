// SPDX-License-Identifier: MIT
#pragma once

#include "Account.h"

#include <QHash>
#include <QObject>
#include <functional>

namespace aslgc {

class HostingClient;

// Signed-in GitHub / GitLab accounts. Metadata is kept in settings; tokens are kept
// in the system keyring (Secret Service / KWallet) via QtKeychain.
class AccountStore : public QObject {
    Q_OBJECT
public:
    static AccountStore* instance();

    const QVector<Account>& accounts() const { return m_accounts; }
    const Account* accountForHost(const QString& host) const;
    HostingClient* client() const { return m_client; }

    void addAccount(const Account& account, const QString& token, std::function<void(const QString& error)> done);
    void removeAccount(const Account& account);
    void token(const Account& account, std::function<void(const QString& token, const QString& error)> done);

signals:
    void changed();

private:
    explicit AccountStore(QObject* parent = nullptr);
    void applyCredentialHosts();

    QVector<Account> m_accounts;
    QHash<QString, QString> m_tokens;  // keychainKey -> token (session cache)
    HostingClient* m_client;
};

}  // namespace aslgc
