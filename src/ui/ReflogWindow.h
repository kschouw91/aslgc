// SPDX-License-Identifier: MIT
#pragma once

#include "git/Types.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSplitter;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;

namespace aslgc {

class DiffView;
class FileList;
class RepoView;

// A window with the reflog of HEAD or of a branch: each time it moved, why, and to which commit.
// Commits that only the reflog still remembers (left behind by an amend, a reset, a rebase or a
// deleted branch) are marked, so they can be brought back with a branch, a reset or a cherry-pick.
class ReflogWindow : public QWidget {
    Q_OBJECT
public:
    explicit ReflogWindow(RepoView* owner);
    ~ReflogWindow() override;

    // "HEAD", "refs/heads/main", "refs/remotes/origin/main"
    void setRef(const QString& ref);
    const QString& ref() const { return m_ref; }
    void reload();

private:
    // Refills the choice of reflogs; returns false when the chosen one is gone.
    bool updateRefChoices();
    void populate(const QVector<ReflogEntry>& entries);
    void applyFilter();
    void updateCount();
    int currentEntry() const;  // index into m_entries, -1 when none is selected
    QTreeWidgetItem* itemOf(int entry) const;
    void onCurrentChanged();
    void showDetails(int entry);
    void loadFileDiff();
    void showListMenu(const QPoint& pos);
    void showInAllCommits(const ReflogEntry& entry);
    void updateTitle();

    RepoView* m_owner;
    QString m_ref = QStringLiteral("HEAD");
    QVector<ReflogEntry> m_entries;
    QByteArray m_loadedFingerprint;  // the repository state the list was loaded for
    bool m_loaded = false;

    QComboBox* m_refBox;
    QLineEdit* m_filter;
    QLabel* m_count;
    QCheckBox* m_showInGraph;
    QTreeWidget* m_list;
    QTextBrowser* m_header;
    FileList* m_files;
    DiffView* m_diff;
    QSplitter* m_vertical;
    QSplitter* m_bottom;
    QSplitter* m_detailsSplit;
    QString m_detailsSha;
    QStringList m_detailsParents;

    int m_loadGen = 0;
    int m_detailsGen = 0;
    int m_diffGen = 0;
};

}  // namespace aslgc
