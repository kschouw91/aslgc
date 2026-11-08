// SPDX-License-Identifier: MIT
#include "AccountsDialog.h"

#include "Dialogs.h"
#include "Theme.h"
#include "hosting/AccountStore.h"
#include "hosting/HostingClient.h"

#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace aslgc {

namespace {
QIcon providerIcon(Provider p) {
    return theme::actionIcon(p == Provider::GitHub ? QStringLiteral("brand-github") : QStringLiteral("brand-gitlab"));
}
}  // namespace

AccountsDialog::AccountsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Accounts"));
    setMinimumSize(520, 340);
    auto* layout = new QVBoxLayout(this);
    auto* intro =
        new QLabel(tr("Signed-in accounts are used to clone your repositories, create pull and merge requests, "
                      "and authenticate HTTPS fetch / pull / push. Tokens are stored in the system keyring."));
    intro->setWordWrap(true);
    m_list = new QListWidget;
    m_list->setIconSize(QSize(24, 24));
    m_list->setSpacing(2);

    auto* addGitHub = new QPushButton(providerIcon(Provider::GitHub), tr("Add GitHub…"));
    auto* addGitLab = new QPushButton(providerIcon(Provider::GitLab), tr("Add GitLab…"));
    m_remove = new QPushButton(theme::actionIcon(QStringLiteral("trash")), tr("Remove"));
    auto* buttonRow = new QHBoxLayout;
    buttonRow->addWidget(addGitHub);
    buttonRow->addWidget(addGitLab);
    buttonRow->addStretch();
    buttonRow->addWidget(m_remove);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);

    layout->addWidget(intro);
    layout->addWidget(m_list, 1);
    layout->addLayout(buttonRow);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(addGitHub, &QPushButton::clicked, this, [this] { add(Provider::GitHub); });
    connect(addGitLab, &QPushButton::clicked, this, [this] { add(Provider::GitLab); });
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) { m_remove->setEnabled(row >= 0); });
    connect(m_remove, &QPushButton::clicked, this, [this] {
        const int row = m_list->currentRow();
        const auto& list = AccountStore::instance()->accounts();
        if (row < 0 || row >= list.size()) return;
        const Account account = list[row];
        if (confirm(this, tr("Remove Account"),
                    tr("Remove <b>%1</b>? Its token is deleted from the keyring.").arg(account.label().toHtmlEscaped()),
                    tr("Remove"), true)) {
            AccountStore::instance()->removeAccount(account);
        }
    });
    connect(AccountStore::instance(), &AccountStore::changed, this, &AccountsDialog::reload);
    reload();
}

void AccountsDialog::reload() {
    m_list->clear();
    for (const Account& a : AccountStore::instance()->accounts()) {
        QString text = a.username;
        if (!a.displayName.isEmpty() && a.displayName != a.username) text += QStringLiteral(" (%1)").arg(a.displayName);
        text += QStringLiteral("\n%1 · %2").arg(providerName(a.provider), a.host);
        auto* item = new QListWidgetItem(providerIcon(a.provider), text, m_list);
        item->setToolTip(a.apiUrl());
    }
    if (m_list->count() == 0) {
        auto* item = new QListWidgetItem(tr("No accounts yet. Add a GitHub or GitLab account below."), m_list);
        item->setFlags(Qt::NoItemFlags);
    }
    m_remove->setEnabled(false);
}

void AccountsDialog::add(Provider provider) {
    AddAccountDialog dlg(provider, this);
    dlg.exec();
}

QString AddAccountDialog::githubClientId() {
    const QString env = qEnvironmentVariable("ASLGC_GITHUB_CLIENT_ID").trimmed();
    return env.isEmpty() ? QStringLiteral(ASLGC_GITHUB_CLIENT_ID) : env;
}

AddAccountDialog::AddAccountDialog(Provider provider, QWidget* parent, const QString& host)
    : QDialog(parent), m_provider(provider) {
    const QString name = providerName(provider);
    setWindowTitle(tr("Add %1 Account").arg(name));
    setMinimumWidth(520);
    auto* layout = new QVBoxLayout(this);

    const QString scopes =
        provider == Provider::GitHub
            ? QStringLiteral("<code>repo</code>, <code>read:org</code>, <code>workflow</code>")
            : QStringLiteral(
                  "<code>api</code>, <code>read_user</code>, <code>read_repository</code>, <code>write_repository</code>");
    const bool canUseBrowser = provider == Provider::GitHub && !githubClientId().isEmpty();

    // Browser sign-in (GitHub OAuth device flow)
    if (canUseBrowser) {
        auto* browserIntro = new QLabel(tr("Sign in through your browser — no token to copy."));
        browserIntro->setWordWrap(true);
        m_browserSignIn =
            new QPushButton(theme::actionIcon(QStringLiteral("brand-github")), tr("Sign in with GitHub in Browser…"));
        m_browserSignIn->setMinimumHeight(34);

        m_codePanel = new QWidget;
        auto* codeLayout = new QVBoxLayout(m_codePanel);
        codeLayout->setContentsMargins(0, 4, 0, 4);
        auto* step = new QLabel(tr("Enter this code on GitHub, then approve <b>aslgc</b>:"));
        step->setTextFormat(Qt::RichText);
        m_userCode = new QLabel;
        m_userCode->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_userCode->setAlignment(Qt::AlignCenter);
        QFont codeFont = m_userCode->font();
        codeFont.setPointSizeF(codeFont.pointSizeF() * 2.0);
        codeFont.setBold(true);
        codeFont.setLetterSpacing(QFont::AbsoluteSpacing, 3);
        m_userCode->setFont(codeFont);
        auto* openBrowser = new QPushButton(theme::actionIcon(QStringLiteral("copy")), tr("Copy Code && Open GitHub"));
        codeLayout->addWidget(step);
        codeLayout->addWidget(m_userCode);
        codeLayout->addWidget(openBrowser, 0, Qt::AlignCenter);
        m_codePanel->hide();

        auto* orLabel = new QLabel(tr("— or use a personal access token —"));
        orLabel->setAlignment(Qt::AlignCenter);
        QPalette dim = orLabel->palette();
        dim.setColor(QPalette::WindowText, theme::mix(dim.color(QPalette::WindowText), dim.color(QPalette::Window), 0.4));
        orLabel->setPalette(dim);

        layout->addWidget(browserIntro);
        layout->addWidget(m_browserSignIn);
        layout->addWidget(m_codePanel);
        layout->addSpacing(6);
        layout->addWidget(orLabel);

        m_pollTimer = new QTimer(this);
        m_pollTimer->setSingleShot(true);
        connect(m_pollTimer, &QTimer::timeout, this, &AddAccountDialog::pollBrowserSignIn);
        connect(m_browserSignIn, &QPushButton::clicked, this, &AddAccountDialog::startBrowserSignIn);
        connect(openBrowser, &QPushButton::clicked, this, [this] {
            QGuiApplication::clipboard()->setText(m_userCode->text());
            QDesktopServices::openUrl(QUrl(m_verificationUri));
        });
    }

    auto* intro =
        new QLabel(tr("Create a personal access token in your browser with the scopes %1, then paste it below.").arg(scopes));
    intro->setWordWrap(true);
    intro->setTextFormat(Qt::RichText);

    auto* form = new QFormLayout;
    m_host = new QLineEdit(host.isEmpty() ? accounts::defaultHost(provider) : host);
    m_host->setToolTip(provider == Provider::GitHub ? tr("Use your server's host name for GitHub Enterprise.")
                                                    : tr("Use your server's host name for a self-managed GitLab."));
    m_token = new QLineEdit;
    m_token->setEchoMode(QLineEdit::Password);
    m_token->setPlaceholderText(provider == Provider::GitHub ? QStringLiteral("ghp_…") : QStringLiteral("glpat-…"));
    auto* create = new QPushButton(theme::actionIcon(QStringLiteral("external-link")), tr("Create Token in Browser…"));
    form->addRow(tr("Host:"), m_host);
    form->addRow(tr("Token:"), m_token);
    form->addRow(QString(), create);

    m_status = new QLabel;
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::RichText);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    m_signIn = buttons->addButton(tr("Sign In with Token"), QDialogButtonBox::AcceptRole);
    m_signIn->setIcon(theme::actionIcon(QStringLiteral("key")));

    layout->addWidget(intro);
    layout->addLayout(form);
    layout->addWidget(m_status);
    layout->addWidget(buttons);

    auto validate = [this] {
        const bool idle = m_deviceCode.isEmpty();
        m_signIn->setEnabled(idle && !normalizedHost().isEmpty() && !m_token->text().trimmed().isEmpty());
        if (m_browserSignIn) m_browserSignIn->setEnabled(idle && browserSignInAvailable());
    };
    connect(m_host, &QLineEdit::textChanged, this, validate);
    connect(m_token, &QLineEdit::textChanged, this, validate);
    connect(create, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl(accounts::tokenCreationUrl(m_provider, normalizedHost())));
        m_token->setFocus();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_signIn, &QPushButton::clicked, this, &AddAccountDialog::signIn);
    validate();
    if (m_browserSignIn)
        m_browserSignIn->setFocus();
    else
        m_token->setFocus();
}

QString AddAccountDialog::normalizedHost() const {
    QString host = m_host->text().trimmed().toLower();
    host.remove(QStringLiteral("https://")).remove(QStringLiteral("http://"));
    while (host.endsWith(QLatin1Char('/'))) host.chop(1);
    return host;
}

// The OAuth App is registered on github.com; Enterprise servers would need their own.
bool AddAccountDialog::browserSignInAvailable() const {
    return m_provider == Provider::GitHub && !githubClientId().isEmpty() && normalizedHost() == QLatin1String("github.com");
}

void AddAccountDialog::setBusy(bool busy) {
    m_signIn->setEnabled(!busy && !m_token->text().trimmed().isEmpty());
    m_host->setEnabled(!busy);
    m_token->setEnabled(!busy);
    if (m_browserSignIn) m_browserSignIn->setEnabled(!busy && browserSignInAvailable());
}

void AddAccountDialog::showError(const QString& message) {
    m_status->setText(QStringLiteral("<span style='color:#d1242f'>%1</span>").arg(message.toHtmlEscaped()));
}

void AddAccountDialog::signIn() {
    const QString host = normalizedHost();
    setBusy(true);
    m_status->setText(tr("Checking token with %1…").arg(host.toHtmlEscaped()));
    finishSignIn(host, m_token->text().trimmed());
}

void AddAccountDialog::startBrowserSignIn() {
    m_deviceHost = normalizedHost();
    setBusy(true);
    m_status->setText(tr("Contacting GitHub…"));
    QPointer<AddAccountDialog> self(this);
    AccountStore::instance()->client()->requestDeviceCode(
        m_deviceHost, githubClientId(), QStringLiteral("repo read:org workflow"),
        [self](const HostingClient::DeviceAuthorization& auth, const QString& error) {
            if (!self) return;
            if (!error.isEmpty()) {
                self->stopBrowserSignIn(error);
                return;
            }
            self->m_deviceCode = auth.deviceCode;
            self->m_verificationUri = auth.verificationUri;
            self->m_deadline = QDateTime::currentMSecsSinceEpoch() + qint64(auth.expiresIn) * 1000;
            self->m_userCode->setText(auth.userCode);
            self->m_codePanel->show();
            self->m_browserSignIn->hide();
            self->m_status->setText(tr("Waiting for you to approve in the browser…"));
            QGuiApplication::clipboard()->setText(auth.userCode);
            QDesktopServices::openUrl(QUrl(auth.verificationUri));
            self->m_pollTimer->start(auth.interval * 1000);
        });
}

void AddAccountDialog::pollBrowserSignIn() {
    if (m_deviceCode.isEmpty()) return;
    if (QDateTime::currentMSecsSinceEpoch() > m_deadline) {
        stopBrowserSignIn(tr("The code expired. Start the sign-in again."));
        return;
    }
    QPointer<AddAccountDialog> self(this);
    const int current = m_pollTimer->interval();
    AccountStore::instance()->client()->pollDeviceToken(
        m_deviceHost, githubClientId(), m_deviceCode,
        [self, current](HostingClient::DevicePoll result, const QString& value, int interval) {
            if (!self || self->m_deviceCode.isEmpty()) return;
            switch (result) {
                case HostingClient::DevicePoll::Pending:
                    self->m_pollTimer->start(interval > 0 ? interval * 1000 : current);
                    break;
                case HostingClient::DevicePoll::SlowDown:
                    self->m_pollTimer->start(interval > 0 ? interval * 1000 : current + 5000);
                    break;
                case HostingClient::DevicePoll::Failed: self->stopBrowserSignIn(value); break;
                case HostingClient::DevicePoll::Granted:
                    self->m_deviceCode.clear();
                    self->m_status->setText(tr("Approved — finishing sign-in…"));
                    self->finishSignIn(self->m_deviceHost, value);
                    break;
            }
        });
}

void AddAccountDialog::stopBrowserSignIn(const QString& error) {
    m_deviceCode.clear();
    if (m_pollTimer) m_pollTimer->stop();
    if (m_codePanel) m_codePanel->hide();
    if (m_browserSignIn) m_browserSignIn->show();
    setBusy(false);
    if (!error.isEmpty())
        showError(error);
    else
        m_status->clear();
}

void AddAccountDialog::finishSignIn(const QString& host, const QString& token) {
    Account account;
    account.provider = m_provider;
    account.host = host;
    QPointer<AddAccountDialog> self(this);
    AccountStore::instance()->client()->fetchUser(
        account, token, [self, account, token](const HostedUser& user, const QString& error) mutable {
            if (!self) return;
            if (!error.isEmpty()) {
                self->stopBrowserSignIn(error);
                return;
            }
            account.username = user.username;
            account.displayName = user.name;
            AccountStore::instance()->addAccount(account, token, [self](const QString& saveError) {
                if (!self) return;
                if (!saveError.isEmpty()) {
                    self->stopBrowserSignIn(saveError);
                    return;
                }
                self->accept();
            });
        });
}

}  // namespace aslgc
