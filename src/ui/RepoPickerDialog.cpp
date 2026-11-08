// SPDX-License-Identifier: MIT
#include "RepoPickerDialog.h"

#include "AccountsDialog.h"
#include "Theme.h"
#include "hosting/AccountStore.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

namespace aslgc {

namespace {
enum Role { HttpsRole = Qt::UserRole, SshRole };
}

RepoPickerDialog::RepoPickerDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Clone from Account"));
    setMinimumSize(560, 460);
    auto* layout = new QVBoxLayout(this);

    auto* top = new QHBoxLayout;
    m_account = new QComboBox;
    auto* manage = new QPushButton(theme::actionIcon(QStringLiteral("users")), tr("Accounts…"));
    top->addWidget(new QLabel(tr("Account:")));
    top->addWidget(m_account, 1);
    top->addWidget(manage);

    m_filter = new QLineEdit;
    m_filter->setPlaceholderText(tr("Filter repositories…"));
    m_filter->setClearButtonEnabled(true);
    m_list = new QListWidget;
    m_list->setIconSize(QSize(16, 16));
    m_ssh = new QCheckBox(tr("Use SSH URL (requires an SSH key on the account)"));
    m_status = new QLabel;
    m_status->setWordWrap(true);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    m_ok = buttons->addButton(tr("Use Repository"), QDialogButtonBox::AcceptRole);
    m_ok->setEnabled(false);

    layout->addLayout(top);
    layout->addWidget(m_filter);
    layout->addWidget(m_list, 1);
    layout->addWidget(m_ssh);
    layout->addWidget(m_status);
    layout->addWidget(buttons);

    connect(manage, &QPushButton::clicked, this, [this] {
        AccountsDialog dlg(this);
        dlg.exec();
        reloadAccounts();
    });
    connect(m_account, &QComboBox::currentIndexChanged, this, &RepoPickerDialog::loadRepositories);
    connect(m_filter, &QLineEdit::textChanged, this, &RepoPickerDialog::showRepositories);
    connect(m_list, &QListWidget::currentRowChanged, this,
            [this](int row) { m_ok->setEnabled(row >= 0 && m_list->item(row)->flags() & Qt::ItemIsEnabled); });
    connect(m_list, &QListWidget::itemActivated, this, [this] {
        if (m_ok->isEnabled()) accept();
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    reloadAccounts();
}

void RepoPickerDialog::reloadAccounts() {
    QSignalBlocker block(m_account);
    m_account->clear();
    for (const Account& a : AccountStore::instance()->accounts()) {
        m_account->addItem(
            theme::actionIcon(a.provider == Provider::GitHub ? QStringLiteral("brand-github") : QStringLiteral("brand-gitlab")),
            a.label(), a.keychainKey());
    }
    loadRepositories();
}

void RepoPickerDialog::loadRepositories() {
    const auto& list = AccountStore::instance()->accounts();
    const int index = m_account->currentIndex();
    m_list->clear();
    m_ok->setEnabled(false);
    if (index < 0 || index >= list.size()) {
        m_status->setText(tr("No accounts yet — click <b>Accounts…</b> to sign in to GitHub or GitLab."));
        return;
    }
    const Account account = list[index];
    if (m_cache.contains(account.keychainKey())) {
        showRepositories();
        return;
    }
    const int gen = ++m_requestGen;
    m_status->setText(tr("Loading repositories from %1…").arg(account.host));
    QPointer<RepoPickerDialog> self(this);
    AccountStore::instance()->token(account, [this, self, account, gen](const QString& token, const QString& error) {
        if (!self || gen != m_requestGen) return;
        if (!error.isEmpty()) {
            m_status->setText(error);
            return;
        }
        AccountStore::instance()->client()->listRepositories(
            account, token, [this, self, account, gen](const QVector<HostedRepo>& repos, const QString& listError) {
                if (!self || gen != m_requestGen) return;
                m_cache.insert(account.keychainKey(), repos);
                showRepositories();
                if (!listError.isEmpty()) m_status->setText(listError);
            });
    });
}

void RepoPickerDialog::showRepositories() {
    const QString key = m_account->currentData().toString();
    const QVector<HostedRepo> repos = m_cache.value(key);
    const QString filter = m_filter->text().trimmed();
    m_list->clear();
    int shown = 0;
    for (const HostedRepo& r : repos) {
        if (!filter.isEmpty() && !r.fullName.contains(filter, Qt::CaseInsensitive) &&
            !r.description.contains(filter, Qt::CaseInsensitive))
            continue;
        auto* item = new QListWidgetItem(
            theme::actionIcon(r.isPrivate ? QStringLiteral("lock") : QStringLiteral("folder")),
            r.description.isEmpty() ? r.fullName : QStringLiteral("%1  —  %2").arg(r.fullName, r.description), m_list);
        item->setData(HttpsRole, r.httpsUrl);
        item->setData(SshRole, r.sshUrl);
        item->setToolTip(r.webUrl);
        ++shown;
    }
    m_status->setText(repos.isEmpty() ? tr("No repositories found for this account.")
                                      : tr("%1 of %2 repositories").arg(shown).arg(repos.size()));
}

QString RepoPickerDialog::selectedUrl() const {
    const QListWidgetItem* item = m_list->currentItem();
    if (!item) return {};
    return item->data(m_ssh->isChecked() ? SshRole : HttpsRole).toString();
}

}  // namespace aslgc
