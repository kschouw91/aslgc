// SPDX-License-Identifier: MIT
#pragma once

#include "hosting/HostingClient.h"

#include <QDialog>
#include <QHash>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace aslgc {

// Lets the user pick a repository from one of their accounts, for cloning.
class RepoPickerDialog : public QDialog {
    Q_OBJECT
public:
    explicit RepoPickerDialog(QWidget* parent = nullptr);
    QString selectedUrl() const;

private:
    void reloadAccounts();
    void loadRepositories();
    void showRepositories();

    QComboBox* m_account;
    QLineEdit* m_filter;
    QListWidget* m_list;
    QCheckBox* m_ssh;
    QLabel* m_status;
    QPushButton* m_ok;
    QHash<QString, QVector<HostedRepo>> m_cache;  // account key -> repositories
    int m_requestGen = 0;
};

}  // namespace aslgc
