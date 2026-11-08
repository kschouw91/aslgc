// SPDX-License-Identifier: MIT
#include "Sidebar.h"

#include "Theme.h"

#include <QCollator>
#include <QHeaderView>
#include <QScrollBar>
#include <QTreeWidgetItemIterator>

#include <algorithm>

namespace aslgc {

namespace {
enum Role { KeyRole = Qt::UserRole, TypeRole, NameRole, ShaRole, IndexRole };
}

Sidebar::Sidebar(QWidget* parent) : QTreeWidget(parent) {
    setHeaderHidden(true);
    setColumnCount(1);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setUniformRowHeights(true);
    setIndentation(14);
    setFrameShape(QFrame::NoFrame);
    setExpandsOnDoubleClick(false);
    setIconSize(QSize(16, 16));

    connect(this, &QTreeWidget::itemSelectionChanged, this, [this] {
        const auto items = selectedItems();
        if (!items.isEmpty()) emit entrySelected(entry(items.first()));
    });
    connect(this, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        const SidebarEntry e = entry(item);
        if (e.type == SidebarEntry::Section || e.type == SidebarEntry::Folder || e.type == SidebarEntry::Remote) {
            item->setExpanded(!item->isExpanded());
            return;
        }
        emit entryActivated(e);
    });
    connect(this, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        if (QTreeWidgetItem* item = itemAt(pos)) emit entryContextMenu(entry(item), viewport()->mapToGlobal(pos));
    });
}

SidebarEntry Sidebar::entry(const QTreeWidgetItem* item) const {
    SidebarEntry e;
    if (!item) return e;
    e.type = SidebarEntry::Type(item->data(0, TypeRole).toInt());
    e.name = item->data(0, NameRole).toString();
    e.sha = item->data(0, ShaRole).toString();
    e.index = item->data(0, IndexRole).toInt();
    return e;
}

QTreeWidgetItem* Sidebar::addItem(QTreeWidgetItem* parent, const QString& key, const QString& text, SidebarEntry::Type type,
                                  const QIcon& icon, const QString& name, const QString& sha, int index) {
    auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(this);
    item->setText(0, text);
    item->setIcon(0, icon);
    item->setData(0, KeyRole, key);
    item->setData(0, TypeRole, int(type));
    item->setData(0, NameRole, name);
    item->setData(0, ShaRole, sha);
    item->setData(0, IndexRole, index);
    return item;
}

QTreeWidgetItem* Sidebar::addSection(const QString& key, const QString& title, int count) {
    auto* item =
        addItem(nullptr, key, count > 0 ? QStringLiteral("%1  %2").arg(title).arg(count) : title, SidebarEntry::Section, QIcon());
    item->setFlags(Qt::ItemIsEnabled);
    QFont f = font();
    f.setBold(true);
    f.setPointSizeF(f.pointSizeF() * 0.86);
    item->setFont(0, f);
    QColor c = palette().color(QPalette::Text);
    c.setAlphaF(0.6f);
    item->setForeground(0, c);
    return item;
}

QTreeWidgetItem* Sidebar::folderFor(QTreeWidgetItem* root, const QString& keyPrefix, const QString& path,
                                    QHash<QString, QTreeWidgetItem*>& folders) {
    const QStringList parts = path.split(QLatin1Char('/'));
    QTreeWidgetItem* parent = root;
    QString accum;
    for (int i = 0; i + 1 < parts.size(); ++i) {
        accum += (accum.isEmpty() ? QString() : QStringLiteral("/")) + parts[i];
        const QString key = keyPrefix + accum;
        QTreeWidgetItem*& folder = folders[key];
        if (!folder) {
            folder = addItem(parent, key, parts[i], SidebarEntry::Folder, theme::actionIcon(QStringLiteral("folder")), accum);
            folder->setFlags(Qt::ItemIsEnabled);
        }
        parent = folder;
    }
    return parent;
}

QTreeWidgetItem* Sidebar::findKey(const QString& key) const {
    for (QTreeWidgetItemIterator it(const_cast<Sidebar*>(this)); *it; ++it) {
        if ((*it)->data(0, KeyRole).toString() == key) return *it;
    }
    return nullptr;
}

QSet<QString> Sidebar::expandedKeys() const {
    QSet<QString> keys;
    for (QTreeWidgetItemIterator it(const_cast<Sidebar*>(this)); *it; ++it) {
        if ((*it)->isExpanded()) keys.insert((*it)->data(0, KeyRole).toString());
    }
    return keys;
}

void Sidebar::setSnapshot(const RepoSnapshot& s) {
    const QSet<QString> expanded = m_initialized
                                       ? expandedKeys()
                                       : QSet<QString>{QStringLiteral("section:branches"), QStringLiteral("section:remotes"),
                                                       QStringLiteral("section:stashes")};
    const QList<QTreeWidgetItem*> sel = selectedItems();
    const QString selectedKey = sel.isEmpty() ? QStringLiteral("changes") : sel.first()->data(0, KeyRole).toString();
    const int scroll = verticalScrollBar() ? verticalScrollBar()->value() : 0;

    QSignalBlocker blocker(this);
    clear();

    const int changeCount = int(s.status.staged.size() + s.status.unstaged.size());
    m_changes =
        addItem(nullptr, QStringLiteral("changes"), changeCount ? tr("Local Changes (%1)").arg(changeCount) : tr("Local Changes"),
                SidebarEntry::Changes, theme::actionIcon(QStringLiteral("file-diff")));
    m_history = addItem(nullptr, QStringLiteral("history"), tr("All Commits"), SidebarEntry::History,
                        theme::actionIcon(QStringLiteral("history")));

    QVector<const Ref*> locals;
    QVector<const Ref*> remotesRefs;
    QVector<const Ref*> tags;
    for (const Ref& r : s.refs) {
        if (r.kind == RefKind::LocalBranch)
            locals << &r;
        else if (r.kind == RefKind::RemoteBranch)
            remotesRefs << &r;
        else
            tags << &r;
    }

    const QIcon branchIcon = theme::actionIcon(QStringLiteral("git-branch"));
    const QIcon currentIcon = theme::actionIcon(QStringLiteral("circle-check"));
    QHash<QString, QTreeWidgetItem*> folders;

    // Local branches
    QTreeWidgetItem* branches = addSection(QStringLiteral("section:branches"), tr("BRANCHES"), int(locals.size()));
    for (const Ref* r : locals) {
        QTreeWidgetItem* parent = folderFor(branches, QStringLiteral("lfolder:"), r->name, folders);
        QString text = r->name.section(QLatin1Char('/'), -1);
        if (r->ahead) text += QStringLiteral("  ↑%1").arg(r->ahead);
        if (r->behind) text += QStringLiteral("  ↓%1").arg(r->behind);
        if (r->upstreamGone) text += tr("  (upstream gone)");
        auto* item = addItem(parent, QStringLiteral("local:") + r->name, text, SidebarEntry::LocalBranch,
                             r->isHead ? currentIcon : branchIcon, r->name, r->target);
        QString tip = r->name;
        if (!r->upstream.isEmpty()) tip += tr("\nTracking %1").arg(r->upstream);
        item->setToolTip(0, tip);
        if (r->isHead) {
            QFont f = font();
            f.setBold(true);
            item->setFont(0, f);
        }
    }

    // Remotes
    QTreeWidgetItem* remotes = addSection(QStringLiteral("section:remotes"), tr("REMOTES"), int(s.remotes.size()));
    QHash<QString, QTreeWidgetItem*> remoteItems;
    const QIcon remoteIcon = theme::actionIcon(QStringLiteral("cloud"));
    for (const Remote& rm : s.remotes) {
        auto* item = addItem(remotes, QStringLiteral("remote:") + rm.name, rm.name, SidebarEntry::Remote, remoteIcon, rm.name);
        item->setToolTip(0, rm.fetchUrl);
        remoteItems.insert(rm.name, item);
    }
    for (const Ref* r : remotesRefs) {
        QTreeWidgetItem* remoteItem = remoteItems.value(r->remoteName());
        if (!remoteItem) {
            remoteItem = addItem(remotes, QStringLiteral("remote:") + r->remoteName(), r->remoteName(), SidebarEntry::Remote,
                                 remoteIcon, r->remoteName());
            remoteItems.insert(r->remoteName(), remoteItem);
        }
        QTreeWidgetItem* parent =
            folderFor(remoteItem, QStringLiteral("rfolder:") + r->remoteName() + QLatin1Char('/'), r->remoteBranch(), folders);
        addItem(parent, QStringLiteral("rbranch:") + r->name, r->remoteBranch().section(QLatin1Char('/'), -1),
                SidebarEntry::RemoteBranch, branchIcon, r->name, r->target)
            ->setToolTip(0, r->name);
    }

    // Tags (newest version first)
    QTreeWidgetItem* tagSection = addSection(QStringLiteral("section:tags"), tr("TAGS"), int(tags.size()));
    QCollator collator;
    collator.setNumericMode(true);
    std::sort(tags.begin(), tags.end(), [&](const Ref* a, const Ref* b) { return collator.compare(a->name, b->name) > 0; });
    const QIcon tagIcon = theme::actionIcon(QStringLiteral("tag"));
    for (const Ref* r : tags) {
        addItem(tagSection, QStringLiteral("tag:") + r->name, r->name, SidebarEntry::Tag, tagIcon, r->name, r->target);
    }

    // Stashes
    QTreeWidgetItem* stashSection = addSection(QStringLiteral("section:stashes"), tr("STASHES"), int(s.stashes.size()));
    const QIcon stashIcon = theme::actionIcon(QStringLiteral("archive"));
    for (const Stash& st : s.stashes) {
        auto* item = addItem(stashSection, QStringLiteral("stash:") + st.sha, st.message, SidebarEntry::Stash, stashIcon, st.ref,
                             st.sha, st.index);
        item->setToolTip(0, st.ref + QStringLiteral(": ") + st.message);
    }

    for (QTreeWidgetItemIterator it(this); *it; ++it) {
        if (expanded.contains((*it)->data(0, KeyRole).toString())) (*it)->setExpanded(true);
    }
    // Always reveal the current branch on first load.
    if (!m_initialized) {
        for (QTreeWidgetItemIterator it(this); *it; ++it) {
            if ((*it)->font(0).bold() && (*it)->data(0, TypeRole).toInt() == SidebarEntry::LocalBranch) {
                for (QTreeWidgetItem* p = (*it)->parent(); p; p = p->parent()) p->setExpanded(true);
            }
        }
    }
    QTreeWidgetItem* toSelect = findKey(selectedKey);
    if (!toSelect) toSelect = m_changes;
    toSelect->setSelected(true);
    setCurrentItem(toSelect);
    if (verticalScrollBar()) verticalScrollBar()->setValue(scroll);
    m_initialized = true;
}

void Sidebar::selectChanges() {
    if (m_changes) setCurrentItem(m_changes);
}

void Sidebar::selectHistory() {
    if (m_history) setCurrentItem(m_history);
}

}  // namespace aslgc
