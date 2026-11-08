// SPDX-License-Identifier: MIT
#pragma once

#include "hosting/Account.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTimer;

namespace aslgc {

// Lists signed-in GitHub / GitLab accounts and lets the user add or remove them.
class AccountsDialog : public QDialog {
    Q_OBJECT
public:
    explicit AccountsDialog(QWidget* parent = nullptr);

private:
    void reload();
    void add(Provider provider);

    QListWidget* m_list;
    QPushButton* m_remove;
};

// Sign-in form. GitHub.com can sign in through the browser (OAuth device flow) when a
// client ID is configured; every provider also accepts a personal access token.
class AddAccountDialog : public QDialog {
    Q_OBJECT
public:
    AddAccountDialog(Provider provider, QWidget* parent = nullptr, const QString& host = {});

    // Client ID of the GitHub OAuth App (env ASLGC_GITHUB_CLIENT_ID overrides the build setting).
    static QString githubClientId();

private:
    QString normalizedHost() const;
    bool browserSignInAvailable() const;
    void signIn();
    void startBrowserSignIn();
    void pollBrowserSignIn();
    void stopBrowserSignIn(const QString& error = {});
    void finishSignIn(const QString& host, const QString& token);
    void setBusy(bool busy);
    void showError(const QString& message);

    Provider m_provider;
    QLineEdit* m_host;
    QLineEdit* m_token;
    QLabel* m_status;
    QPushButton* m_signIn;
    QPushButton* m_browserSignIn = nullptr;
    QWidget* m_codePanel = nullptr;
    QLabel* m_userCode = nullptr;
    QTimer* m_pollTimer = nullptr;
    QString m_deviceCode;
    QString m_verificationUri;
    QString m_deviceHost;
    qint64 m_deadline = 0;  // msecs since epoch
};

}  // namespace aslgc
