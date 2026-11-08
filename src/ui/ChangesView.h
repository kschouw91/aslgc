// SPDX-License-Identifier: MIT
#pragma once

#include "DiffView.h"
#include "git/Types.h"

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSettings;
class QSplitter;
class QToolButton;

namespace aslgc {

class FileList;
class RepoView;

// "Local Changes": unstaged / staged lists, diff with hunk & line staging, commit box.
class ChangesView : public QWidget {
    Q_OBJECT
public:
    // Git refuses to create commits without a name and email; asks once instead of failing.
    bool ensureIdentity();
    explicit ChangesView(RepoView* owner);

    void setStatus(const WorkingStatus& status, const HeadInfo& head, Operation operation);
    void focusCommitMessage();
    void saveLayout(QSettings& settings) const;
    void restoreLayout(const QSettings& settings);

private:
    enum class Side { Unstaged, Staged };

    void onCurrentChanged(Side side);
    void loadDiff(bool keepScroll);
    void stage(const QVector<FileChange>& files);
    void unstage(const QVector<FileChange>& files);
    void discard(const QVector<FileChange>& files);
    void resolve(const QVector<FileChange>& files, const QString& side);
    void ignore(const QString& pattern);
    void showFileMenu(Side side, const QPoint& globalPos);
    void onHunkAction(int hunk, DiffView::Action action);
    void onLinesAction(const QSet<quint64>& lines, DiffView::Action action);
    void applyPatch(const QByteArray& patch, DiffView::Action action);
    void commit();
    void updateCommitButton();
    void onAmendToggled(bool on);
    FileList* list(Side side) const;

    RepoView* m_owner;
    FileList* m_unstaged;
    FileList* m_staged;
    QLabel* m_unstagedTitle;
    QLabel* m_stagedTitle;
    QToolButton* m_stageAll;
    QToolButton* m_unstageAll;
    QLineEdit* m_subject;
    QPlainTextEdit* m_body;
    QCheckBox* m_amend;
    QPushButton* m_commit;
    QLabel* m_diffTitle;
    DiffView* m_diff;
    QSplitter* m_mainSplit;
    QSplitter* m_leftSplit;

    Side m_side = Side::Unstaged;
    FileChange m_current;
    bool m_hasCurrent = false;
    int m_diffGen = 0;
    QByteArray m_lastDiff;
    HeadInfo m_head;
    Operation m_operation = Operation::None;
    QString m_amendMessage;
};

}  // namespace aslgc
