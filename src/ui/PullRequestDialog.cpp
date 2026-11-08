// SPDX-License-Identifier: MIT
#include "PullRequestDialog.h"

#include "Theme.h"
#include "hosting/AccountStore.h"
#include "hosting/HostingClient.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

namespace aslgc {

PullRequestDialog::PullRequestDialog(const Account& account, const RemoteRepo& repo, const ReviewRequestDraft& draft,
                                     QWidget* parent)
    : QDialog(parent), m_account(account), m_repo(repo), m_source(draft.sourceBranch) {
    const QString kind = reviewRequestName(account.provider);
    setWindowTitle(tr("Create %1").arg(kind));
    setMinimumWidth(560);
    auto* layout = new QVBoxLayout(this);

    auto* heading =
        new QLabel(tr("<b>%1</b> on %2 as %3")
                       .arg(repo.path.toHtmlEscaped(), providerName(account.provider), account.username.toHtmlEscaped()));
    heading->setTextFormat(Qt::RichText);
    layout->addWidget(heading);
    if (draft.unpushedCommits > 0) {
        auto* warn = new QLabel(
            tr("⚠ %n local commit(s) on this branch are not pushed yet and won't be included.", nullptr, draft.unpushedCommits));
        warn->setWordWrap(true);
        layout->addWidget(warn);
    }

    auto* form = new QFormLayout;
    auto* source = new QLabel(QStringLiteral("<code>%1</code>").arg(draft.sourceBranch.toHtmlEscaped()));
    source->setTextFormat(Qt::RichText);
    m_target = new QComboBox;
    m_target->addItems(draft.targets);
    m_target->setCurrentText(draft.defaultTarget);
    m_title = new QLineEdit(draft.title);
    m_body = new QPlainTextEdit(draft.body);
    m_body->setTabChangesFocus(true);
    m_body->setMinimumHeight(140);
    m_draft = new QCheckBox(tr("Create as draft"));
    m_open = new QCheckBox(tr("Open in browser after creating"));
    m_open->setChecked(true);
    form->addRow(tr("From:"), source);
    form->addRow(tr("Into:"), m_target);
    form->addRow(tr("Title:"), m_title);
    form->addRow(tr("Description:"), m_body);
    form->addRow(QString(), m_draft);
    form->addRow(QString(), m_open);
    layout->addLayout(form);

    m_status = new QLabel;
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::RichText);
    layout->addWidget(m_status);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    m_create = buttons->addButton(tr("Create %1").arg(kind), QDialogButtonBox::AcceptRole);
    m_create->setIcon(theme::actionIcon(QStringLiteral("git-pull-request")));
    m_create->setDefault(true);
    layout->addWidget(buttons);

    auto validate = [this] {
        m_create->setEnabled(!m_title->text().trimmed().isEmpty() && !m_target->currentText().isEmpty() &&
                             m_target->currentText() != m_source);
    };
    connect(m_title, &QLineEdit::textChanged, this, validate);
    connect(m_target, &QComboBox::currentTextChanged, this, validate);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_create, &QPushButton::clicked, this, &PullRequestDialog::create);
    validate();
}

void PullRequestDialog::create() {
    m_create->setEnabled(false);
    m_status->setText(tr("Creating…"));
    const QString target = m_target->currentText();
    const QString title = m_title->text().trimmed();
    const QString body = m_body->toPlainText().trimmed();
    const bool draft = m_draft->isChecked();
    QPointer<PullRequestDialog> self(this);
    AccountStore::instance()->token(m_account, [this, self, target, title, body, draft](const QString& token,
                                                                                        const QString& error) {
        if (!self) return;
        if (!error.isEmpty()) {
            m_status->setText(QStringLiteral("<span style='color:#d1242f'>%1</span>").arg(error.toHtmlEscaped()));
            m_create->setEnabled(true);
            return;
        }
        AccountStore::instance()->client()->createReviewRequest(
            m_account, token, m_repo.path, m_source, target, title, body, draft,
            [this, self](const QString& url, const QString& createError) {
                if (!self) return;
                if (!createError.isEmpty()) {
                    m_status->setText(QStringLiteral("<span style='color:#d1242f'>%1</span>").arg(createError.toHtmlEscaped()));
                    m_create->setEnabled(true);
                    return;
                }
                if (m_open->isChecked() && !url.isEmpty()) QDesktopServices::openUrl(QUrl(url));
                accept();
            });
    });
}

}  // namespace aslgc
