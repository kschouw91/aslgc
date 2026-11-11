// SPDX-License-Identifier: MIT
#pragma once

#include "git/Repository.h"

#include <QSet>
#include <QTreeWidget>

namespace aslgc {

struct SidebarEntry {
    enum Type { None, Changes, History, Section, Folder, LocalBranch, RemoteBranch, Remote, Tag, Stash };
    Type type = None;
    QString name;  // branch / remote / tag name, stash ref
    QString sha;
    int index = -1;  // stash index
};

class Sidebar : public QTreeWidget {
    Q_OBJECT
public:
    explicit Sidebar(QWidget* parent = nullptr);

    void setSnapshot(const RepoSnapshot& snapshot);
    void selectChanges();
    void selectHistory();
    SidebarEntry entry(const QTreeWidgetItem* item) const;

signals:
    void entrySelected(const aslgc::SidebarEntry& entry);
    void entryActivated(const aslgc::SidebarEntry& entry);
    void entryContextMenu(const aslgc::SidebarEntry& entry, const QPoint& globalPos);

private:
    QTreeWidgetItem* addItem(QTreeWidgetItem* parent, const QString& key, const QString& text, SidebarEntry::Type type,
                             const QIcon& icon, const QString& name = {}, const QString& sha = {}, int index = -1);
    QTreeWidgetItem* addSection(const QString& key, const QString& title, int count);
    QTreeWidgetItem* folderFor(QTreeWidgetItem* root, const QString& keyPrefix, const QString& path,
                               QHash<QString, QTreeWidgetItem*>& folders);
    QTreeWidgetItem* findKey(const QString& key) const;
    QSet<QString> expandedKeys() const;

    bool m_initialized = false;
    QTreeWidgetItem* m_changes = nullptr;
    QTreeWidgetItem* m_history = nullptr;
};

}  // namespace aslgc
