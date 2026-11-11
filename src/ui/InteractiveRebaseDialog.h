// SPDX-License-Identifier: MIT
#pragma once

#include "git/Rebase.h"

#include <QDialog>
#include <QHash>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace aslgc {

// Lets the user reorder commits and choose pick / reword / edit / squash / fixup / drop
// for each before an interactive rebase. Commits are listed oldest first, top to bottom.
class InteractiveRebaseDialog : public QDialog {
    Q_OBJECT
public:
    InteractiveRebaseDialog(const QString& heading, const RebaseRange& range, bool dirtyWorkTree, QWidget* parent = nullptr);

    QVector<RebaseStep> steps() const;
    bool autostash() const;

private:
    void setAction(RebaseAction action);
    void moveSelection(int delta);
    void refreshItem(QTreeWidgetItem* item);
    void showCurrentMessage();
    void onMessageEdited();
    void validate();
    QTreeWidgetItem* currentItem() const;

    QTreeWidget* m_list;
    QLabel* m_messageLabel;
    QPlainTextEdit* m_message;
    QLabel* m_summary;
    QCheckBox* m_autostash;
    QPushButton* m_start;
    QHash<QString, RebaseStep> m_steps;          // by sha
    QHash<QString, QString> m_originalMessages;  // by sha
    bool m_loadingMessage = false;
};

}  // namespace aslgc
