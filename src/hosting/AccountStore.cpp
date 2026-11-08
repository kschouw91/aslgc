// SPDX-License-Identifier: MIT
#include "AccountStore.h"

#include "HostingClient.h"
#include "git/Git.h"

#include <qt6keychain/keychain.h>
#include <QCoreApplication>

namespace aslgc {

AccountStore* AccountStore::instance() {
    static AccountStore* store = new AccountStore(QCoreApplication::instance());
    return store;
}

AccountStore::AccountStore(QObject* parent) : QObject(parent), m_client(new HostingClient(this)) {
    m_accounts = accounts::load();
    applyCredentialHosts();
}

const Account* AccountStore::accountForHost(const QString& host) const { return accounts::findForHost(m_accounts, host); }

void AccountStore::applyCredentialHosts() {
    QStringList urls;
    for (const Account& a : m_accounts) {
        const QString url = a.webUrl();
        if (!urls.contains(url, Qt::CaseInsensitive)) urls << url;
    }
    Git::setCredentialHosts(urls);
}

void AccountStore::addAccount(const Account& account, const QString& token, std::function<void(const QString&)> done) {
    auto* job = new QKeychain::WritePasswordJob(QLatin1String(kKeychainService), this);
    job->setKey(account.keychainKey());
    job->setTextData(token);
    connect(job, &QKeychain::Job::finished, this, [this, account, token, done = std::move(done)](QKeychain::Job* j) {
        if (j->error() != QKeychain::NoError) {
            done(tr("Could not save the token in the system keyring: %1").arg(j->errorString()));
            return;
        }
        m_accounts.removeIf([&](const Account& a) { return a.keychainKey() == account.keychainKey(); });
        m_accounts.push_back(account);
        m_tokens.insert(account.keychainKey(), token);
        accounts::save(m_accounts);
        applyCredentialHosts();
        emit changed();
        done({});
    });
    job->start();
}

void AccountStore::removeAccount(const Account& account) {
    auto* job = new QKeychain::DeletePasswordJob(QLatin1String(kKeychainService), this);
    job->setKey(account.keychainKey());
    job->start();
    m_tokens.remove(account.keychainKey());
    m_accounts.removeIf([&](const Account& a) { return a.keychainKey() == account.keychainKey(); });
    accounts::save(m_accounts);
    applyCredentialHosts();
    emit changed();
}

void AccountStore::token(const Account& account, std::function<void(const QString&, const QString&)> done) {
    if (const auto it = m_tokens.constFind(account.keychainKey()); it != m_tokens.cend()) {
        done(*it, {});
        return;
    }
    auto* job = new QKeychain::ReadPasswordJob(QLatin1String(kKeychainService), this);
    job->setKey(account.keychainKey());
    connect(job, &QKeychain::Job::finished, this, [this, key = account.keychainKey(), done = std::move(done)](QKeychain::Job* j) {
        auto* read = static_cast<QKeychain::ReadPasswordJob*>(j);
        if (read->error() != QKeychain::NoError) {
            done({}, tr("Could not read the token from the system keyring: %1").arg(read->errorString()));
            return;
        }
        m_tokens.insert(key, read->textData());
        done(read->textData(), {});
    });
    job->start();
}

}  // namespace aslgc
