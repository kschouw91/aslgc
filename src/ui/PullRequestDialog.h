// SPDX-License-Identifier: MIT
#pragma once

#include "hosting/Account.h"
#include "hosting/RemoteUrl.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace aslgc {

struct ReviewRequestDraft {
    QString sourceBranch;  // branch name on the remote
    QStringList targets;   // candidate target branches on the remote
    QString defaultTarget;
    QString title;
    QString body;
    int unpushedCommits = 0;
};

// Creates a GitHub pull request or GitLab merge request through the account's API.
class PullRequestDialog : public QDialog {
    Q_OBJECT
public:
    PullRequestDialog(const Account& account, const RemoteRepo& repo, const ReviewRequestDraft& draft, QWidget* parent = nullptr);

private:
    void create();

    Account m_account;
    RemoteRepo m_repo;
    QString m_source;
    QComboBox* m_target;
    QLineEdit* m_title;
    QPlainTextEdit* m_body;
    QCheckBox* m_draft;
    QCheckBox* m_open;
    QLabel* m_status;
    QPushButton* m_create;
};

}  // namespace aslgc
