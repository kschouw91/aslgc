// SPDX-License-Identifier: MIT
#include "ReflogWindow.h"

#include "Async.h"
#include "CommitModel.h"
#include "DiffView.h"
#include "FileList.h"
#include "RepoView.h"
#include "Theme.h"
#include "git/Diff.h"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QTextBrowser>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <optional>

namespace aslgc {

namespace {

enum Column { EntryColumn, ActionColumn, DescriptionColumn, CommitColumn, DateColumn, ColumnCount };
constexpr int kMaxEntries = 10000;

struct DetailsData {
    CommitInfo info;
    QVector<FileChange> files;
};

// "refs/heads/main" → "main", "refs/remotes/origin/main" → "origin/main"
QString shortRefName(const QString& ref) {
    for (const QLatin1String prefix : {QLatin1String("refs/heads/"), QLatin1String("refs/remotes/")}) {
        if (ref.startsWith(prefix)) return ref.mid(prefix.size());
    }
    return ref;
}

// A little colour by what the entry did: commits, resets, checkouts, history rewrites, merges.
QColor actionColor(const QString& action) {
    std::optional<theme::Badge> kind;
    if (action.startsWith(QLatin1String("commit")))
        kind = theme::Badge::CurrentBranch;
    else if (action.startsWith(QLatin1String("reset")))
        kind = theme::Badge::Head;
    else if (action.startsWith(QLatin1String("checkout")) || action.startsWith(QLatin1String("switch")))
        kind = theme::Badge::Local;
    else if (action.contains(QLatin1String("rebase")) || action.startsWith(QLatin1String("cherry-pick")) ||
             action.startsWith(QLatin1String("revert")))
        kind = theme::Badge::Remote;
    else if (action.startsWith(QLatin1String("merge")) || action.startsWith(QLatin1String("pull")) ||
             action.startsWith(QLatin1String("fetch")))
        kind = theme::Badge::Tag;
    if (!kind) return {};
    const QColor c = theme::badgeColor(*kind);
    return theme::isDark() ? theme::mix(c, Qt::white, 0.35) : c;
}

// "moving from 4388417855…b51 to main" → "moving from 43884178 to main", like the Commit column.
QString shortenShas(const QString& text) {
    static const QRegularExpression fullSha(QStringLiteral("\\b([0-9a-f]{8})(?:[0-9a-f]{56}|[0-9a-f]{32})\\b"));
    QString out = text;
    return out.replace(fullSha, QStringLiteral("\\1"));
}

// Identifies an entry across reloads; its name ("HEAD@{2}") shifts as new entries arrive.
QString entryKey(const ReflogEntry& e) {
    return QStringLiteral("%1 %2 %3: %4").arg(e.time).arg(e.commit.sha, e.action, e.message);
}

}  // namespace

ReflogWindow::ReflogWindow(RepoView* owner) : QWidget(owner, Qt::Window), m_owner(owner) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowIcon(theme::appIcon());
    resize(1100, 760);

    // Which reflog, filter, count, the All Commits option and reload
    auto* top = new QWidget;
    auto* tl = new QHBoxLayout(top);
    tl->setContentsMargins(10, 6, 8, 6);
    auto* refLabel = new QLabel(tr("Reflog of"));
    m_refBox = new QComboBox;
    m_refBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_refBox->setMinimumContentsLength(18);
    m_refBox->setToolTip(tr("HEAD records everything you checked out, committed or reset; a branch records where it pointed"));
    refLabel->setBuddy(m_refBox);
    m_filter = new QLineEdit;
    m_filter->setPlaceholderText(tr("Filter by action, message, commit or SHA"));
    m_filter->setClearButtonEnabled(true);
    m_count = new QLabel;
    m_count->setEnabled(false);
    m_showInGraph = new QCheckBox(tr("Show lost commits in All Commits"));
    m_showInGraph->setToolTip(
        tr("Also list the commits that are on no branch or tag but still in the reflog of HEAD or a "
           "local branch, faded, in All Commits"));
    auto* reloadButton = new QToolButton;
    reloadButton->setIcon(theme::actionIcon(QStringLiteral("refresh")));
    reloadButton->setAutoRaise(true);
    reloadButton->setToolTip(tr("Reload (F5)"));
    tl->addWidget(refLabel);
    tl->addWidget(m_refBox);
    tl->addSpacing(8);
    tl->addWidget(m_filter, 1);
    tl->addWidget(m_count);
    tl->addSpacing(12);
    tl->addWidget(m_showInGraph);
    tl->addWidget(reloadButton);

    // Entries on top, the commit's details / files / diff below
    m_list = new QTreeWidget;
    m_list->setColumnCount(ColumnCount);
    m_list->setHeaderLabels({tr("Entry"), tr("Action"), tr("Description"), tr("Commit"), tr("Date")});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setAllColumnsShowFocus(true);
    m_list->setIndentation(0);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setFrameShape(QFrame::NoFrame);
    QHeaderView* header = m_list->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(DescriptionColumn, QHeaderView::Stretch);
    for (int c : {EntryColumn, ActionColumn, CommitColumn, DateColumn}) header->setSectionResizeMode(c, QHeaderView::Interactive);
    header->resizeSection(EntryColumn, 120);
    header->resizeSection(ActionColumn, 140);
    header->resizeSection(CommitColumn, 110);
    header->resizeSection(DateColumn, 140);

    m_header = new QTextBrowser;
    m_header->setOpenLinks(false);
    m_header->setFrameShape(QFrame::NoFrame);
    m_files = new FileList;
    m_files->setSelectionMode(QAbstractItemView::SingleSelection);
    m_diff = new DiffView;
    m_detailsSplit = new QSplitter(Qt::Vertical);
    m_detailsSplit->addWidget(m_header);
    m_detailsSplit->addWidget(m_files);
    m_detailsSplit->setSizes({190, 180});
    m_bottom = new QSplitter(Qt::Horizontal);
    m_bottom->addWidget(m_detailsSplit);
    m_bottom->addWidget(m_diff);
    m_bottom->setStretchFactor(1, 1);
    m_bottom->setSizes({360, 900});
    m_vertical = new QSplitter(Qt::Vertical);
    m_vertical->addWidget(m_list);
    m_vertical->addWidget(m_bottom);
    m_vertical->setSizes({300, 460});
    m_vertical->setChildrenCollapsible(false);

    auto* rule = new QFrame;
    rule->setFrameShape(QFrame::HLine);
    rule->setFrameShadow(QFrame::Sunken);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(top);
    layout->addWidget(rule);
    layout->addWidget(m_vertical, 1);

    QSettings settings;
    restoreGeometry(settings.value(QStringLiteral("layout/reflogGeometry")).toByteArray());
    m_vertical->restoreState(settings.value(QStringLiteral("layout/reflogVertical")).toByteArray());
    m_bottom->restoreState(settings.value(QStringLiteral("layout/reflogBottom")).toByteArray());
    m_detailsSplit->restoreState(settings.value(QStringLiteral("layout/reflogDetails")).toByteArray());

    connect(m_refBox, &QComboBox::activated, this, [this](int index) { setRef(m_refBox->itemData(index).toString()); });
    connect(m_filter, &QLineEdit::textChanged, this, &ReflogWindow::applyFilter);
    m_showInGraph->setChecked(m_owner->showsLostCommits());
    connect(m_showInGraph, &QCheckBox::toggled, m_owner, &RepoView::setShowLostCommits);
    connect(m_owner, &RepoView::lostCommitsShownChanged, this, [this](bool on) {
        QSignalBlocker block(m_showInGraph);
        m_showInGraph->setChecked(on);
    });
    connect(reloadButton, &QToolButton::clicked, this, &ReflogWindow::reload);
    connect(m_list, &QTreeWidget::currentItemChanged, this, &ReflogWindow::onCurrentChanged);
    connect(m_list, &QWidget::customContextMenuRequested, this, &ReflogWindow::showListMenu);
    connect(m_list, &QTreeWidget::itemActivated, this, [this] {
        if (const int i = currentEntry(); i >= 0) showInAllCommits(m_entries[i]);
    });
    connect(m_header, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        if (url.scheme() == QLatin1String("commit"))
            m_owner->revealCommit(url.path());
        else if (url.scheme() == QLatin1String("branch"))
            m_owner->createBranch(url.path());
    });
    connect(m_files, &QListWidget::currentItemChanged, this, &ReflogWindow::loadFileDiff);
    m_files->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_files, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const FileChange* f = m_files->currentFile();
        if (!f) return;
        QMenu menu(this);
        m_owner->addFileHistoryActions(menu, *f, m_detailsSha, m_detailsParents);
        menu.addSeparator();
        menu.addAction(theme::actionIcon(QStringLiteral("copy")), tr("Copy Path"), this,
                       [path = f->path] { QGuiApplication::clipboard()->setText(path); });
        menu.exec(m_files->viewport()->mapToGlobal(pos));
    });
    m_diff->setLineHistoryEnabled(true);
    connect(m_diff, &DiffView::lineHistoryRequested, this, [this](int first, int last, bool oldSide) {
        const FileChange* f = m_files->currentFile();
        if (!f || m_detailsSha.isEmpty()) return;
        if (!oldSide)
            m_owner->showLineHistory(f->path, first, last, m_detailsSha);
        else if (!m_detailsParents.isEmpty())
            m_owner->showLineHistory(f->oldPath.isEmpty() ? f->path : f->oldPath, first, last, m_detailsParents.first());
    });
    // Reflogs only change when refs move, which is when the repository's fingerprint changes.
    connect(m_owner, &RepoView::stateChanged, this, [this] {
        if (!updateRefChoices())
            setRef(QStringLiteral("HEAD"));  // the branch is gone
        else if (m_loaded && m_owner->snapshot().fingerprint != m_loadedFingerprint)
            reload();
    });

    auto shortcut = [this](const QKeySequence& key, auto slot) {
        connect(new QShortcut(key, this), &QShortcut::activated, this, slot);
    };
    shortcut(QKeySequence::Close, [this] { close(); });
    shortcut(QKeySequence(Qt::Key_F5), [this] { reload(); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_R), [this] { reload(); });
    shortcut(QKeySequence::Find, [this] {
        m_filter->setFocus();
        m_filter->selectAll();
    });

    m_diff->showMessage(tr("Select an entry."));
    updateRefChoices();
    updateTitle();
}

ReflogWindow::~ReflogWindow() {
    QSettings settings;
    settings.setValue(QStringLiteral("layout/reflogGeometry"), saveGeometry());
    settings.setValue(QStringLiteral("layout/reflogVertical"), m_vertical->saveState());
    settings.setValue(QStringLiteral("layout/reflogBottom"), m_bottom->saveState());
    settings.setValue(QStringLiteral("layout/reflogDetails"), m_detailsSplit->saveState());
}

void ReflogWindow::setRef(const QString& ref) {
    const bool changed = ref != m_ref;
    m_ref = ref;
    if (!updateRefChoices()) m_ref = QStringLiteral("HEAD");
    updateTitle();
    if (changed) {
        m_entries.clear();
        m_list->clear();
        m_detailsSha.clear();
        m_header->clear();
        m_files->setFiles({});
        m_diff->clearView();
    }
    if (changed || !m_loaded) reload();
}

void ReflogWindow::updateTitle() {
    setWindowTitle(QStringLiteral("%1 — %2 — %3").arg(shortRefName(m_ref), tr("Reflog"), m_owner->repo().name()));
}

bool ReflogWindow::updateRefChoices() {
    const RepoSnapshot& s = m_owner->snapshot();
    QSignalBlocker block(m_refBox);
    m_refBox->clear();
    m_refBox->addItem(theme::actionIcon(QStringLiteral("restore")), QStringLiteral("HEAD"), QStringLiteral("HEAD"));
    QVector<const Ref*> locals;
    QVector<const Ref*> remotes;
    for (const Ref& r : s.refs) {
        if (r.kind == RefKind::LocalBranch)
            locals << &r;
        else if (r.kind == RefKind::RemoteBranch)
            remotes << &r;
    }
    if (!locals.isEmpty() || !remotes.isEmpty()) m_refBox->insertSeparator(m_refBox->count());
    for (const Ref* r : locals) {
        m_refBox->addItem(theme::actionIcon(r->isHead ? QStringLiteral("circle-check") : QStringLiteral("git-branch")), r->name,
                          r->fullName);
    }
    for (const Ref* r : remotes) m_refBox->addItem(theme::actionIcon(QStringLiteral("cloud")), r->name, r->fullName);
    const int index = m_refBox->findData(m_ref);
    m_refBox->setCurrentIndex(std::max(0, index));
    return index >= 0;
}

void ReflogWindow::reload() {
    const int gen = ++m_loadGen;
    m_loaded = true;
    m_loadedFingerprint = m_owner->snapshot().fingerprint;
    m_count->setText(tr("Loading…"));
    const Repository repo = m_owner->repo();
    const QString ref = m_ref;
    runAsync(
        this, [repo, ref] { return repo.reflog(ref, kMaxEntries); },
        [this, gen](const QVector<ReflogEntry>& entries) {
            if (gen == m_loadGen) populate(entries);
        });
}

void ReflogWindow::populate(const QVector<ReflogEntry>& entries) {
    // Nothing new: keep the selection, the scroll position and the diff as they are.
    const bool same =
        entries.size() == m_entries.size() &&
        std::equal(entries.cbegin(), entries.cend(), m_entries.cbegin(), [](const ReflogEntry& a, const ReflogEntry& b) {
            return a.selector == b.selector && a.lost == b.lost && entryKey(a) == entryKey(b);
        });
    if (same && !entries.isEmpty() && m_list->topLevelItemCount() == entries.size()) {
        updateCount();
        return;
    }
    const int current = currentEntry();
    const QString keep = current >= 0 ? entryKey(m_entries[current]) : QString();
    const int scroll = m_list->verticalScrollBar()->value();
    m_entries = entries;

    QColor dim = palette().color(QPalette::Text);
    dim.setAlphaF(0.6f);
    QFont mono = font();
    mono.setFamilies({QStringLiteral("monospace")});
    mono.setStyleHint(QFont::Monospace);
    const QIcon lostIcon = theme::actionIcon(QStringLiteral("git-branch-deleted"));
    int select = 0;
    QList<QTreeWidgetItem*> items;
    items.reserve(m_entries.size());
    for (int i = 0; i < m_entries.size(); ++i) {
        const ReflogEntry& e = m_entries[i];
        if (!keep.isEmpty() && entryKey(e) == keep) select = i;
        auto* item = new QTreeWidgetItem;
        item->setData(EntryColumn, Qt::UserRole, i);
        item->setText(EntryColumn, e.selector);
        item->setForeground(EntryColumn, dim);
        if (e.action.isEmpty()) {
            item->setText(ActionColumn, tr("(no message)"));
            item->setForeground(ActionColumn, dim);
        } else {
            item->setText(ActionColumn, e.action);
            if (const QColor c = actionColor(e.action); c.isValid()) item->setForeground(ActionColumn, c);
        }
        item->setText(DescriptionColumn, shortenShas(e.message.isEmpty() ? e.commit.subject : e.message));
        item->setToolTip(
            DescriptionColumn,
            QStringLiteral("%1\n%2 · %3").arg(e.commit.subject, e.commit.authorName, formatTime(e.commit.authorTime)));
        item->setText(CommitColumn, e.commit.shortSha());
        item->setFont(CommitColumn, mono);
        if (e.lost) {
            item->setIcon(CommitColumn, lostIcon);
            item->setToolTip(CommitColumn, tr("Lost commit: on no branch or tag, only the reflog still remembers it"));
        } else {
            item->setToolTip(CommitColumn, e.commit.sha);
        }
        item->setText(DateColumn, e.time > 0 ? formatTime(e.time) : QString());
        item->setToolTip(DateColumn,
                         tr("%1 by %2").arg(QDateTime::fromSecsSinceEpoch(e.time).toString(Qt::RFC2822Date), e.identity));
        items << item;
    }
    {
        QSignalBlocker block(m_list);
        m_list->clear();
        m_list->addTopLevelItems(items);
    }
    applyFilter();
    if (m_entries.isEmpty()) {
        m_detailsSha.clear();
        m_header->clear();
        m_files->setFiles({});
        m_diff->showMessage(m_ref == QLatin1String("HEAD") ? tr("No reflog entries yet.")
                                                           : tr("%1 has no reflog entries.").arg(shortRefName(m_ref)));
        return;
    }
    m_list->setCurrentItem(itemOf(select));
    if (!keep.isEmpty()) m_list->verticalScrollBar()->setValue(scroll);
}

void ReflogWindow::applyFilter() {
    const QString needle = m_filter->text().trimmed();
    for (int row = 0; row < m_list->topLevelItemCount(); ++row) {
        QTreeWidgetItem* item = m_list->topLevelItem(row);
        const int i = item->data(EntryColumn, Qt::UserRole).toInt();
        if (i < 0 || i >= m_entries.size()) continue;
        const ReflogEntry& e = m_entries[i];
        const bool match = needle.isEmpty() || e.selector.contains(needle, Qt::CaseInsensitive) ||
                           e.action.contains(needle, Qt::CaseInsensitive) || e.message.contains(needle, Qt::CaseInsensitive) ||
                           e.commit.subject.contains(needle, Qt::CaseInsensitive) ||
                           e.commit.authorName.contains(needle, Qt::CaseInsensitive) ||
                           e.commit.sha.startsWith(needle, Qt::CaseInsensitive);
        item->setHidden(!match);
    }
    updateCount();
}

void ReflogWindow::updateCount() {
    int shown = 0;
    for (int row = 0; row < m_list->topLevelItemCount(); ++row) shown += m_list->topLevelItem(row)->isHidden() ? 0 : 1;
    QSet<QString> lost;
    for (const ReflogEntry& e : std::as_const(m_entries)) {
        if (e.lost) lost.insert(e.commit.sha);
    }
    const int total = int(m_entries.size());
    QString text = shown == total ? tr("%n entry(s)", nullptr, total) : tr("%1 of %n entry(s)", nullptr, total).arg(shown);
    if (!lost.isEmpty()) text += QStringLiteral(" · ") + tr("%n lost commit(s)", nullptr, int(lost.size()));
    m_count->setText(text);
}

int ReflogWindow::currentEntry() const {
    const QTreeWidgetItem* item = m_list->currentItem();
    if (!item) return -1;
    const int i = item->data(EntryColumn, Qt::UserRole).toInt();
    return i >= 0 && i < m_entries.size() ? i : -1;
}

QTreeWidgetItem* ReflogWindow::itemOf(int entry) const {
    for (int row = 0; row < m_list->topLevelItemCount(); ++row) {
        QTreeWidgetItem* item = m_list->topLevelItem(row);
        if (item->data(EntryColumn, Qt::UserRole).toInt() == entry) return item;
    }
    return nullptr;
}

void ReflogWindow::onCurrentChanged() {
    if (const int i = currentEntry(); i >= 0) showDetails(i);
}

void ReflogWindow::showDetails(int entry) {
    const ReflogEntry e = m_entries[entry];
    m_detailsSha = e.commit.sha;
    const int gen = ++m_detailsGen;
    const Repository repo = m_owner->repo();
    const QString sha = e.commit.sha;
    runAsync(
        this,
        [repo, sha] {
            DetailsData d;
            d.info = repo.commitInfo(sha);
            if (d.info.valid) d.files = repo.commitChanges(sha, d.info.commit.parents);
            return d;
        },
        [this, gen, e](const DetailsData& d) {
            if (gen != m_detailsGen) return;
            if (!d.info.valid) {
                m_header->setPlainText(tr("Commit not found."));
                m_files->setFiles({});
                m_diff->clearView();
                return;
            }
            m_detailsParents = d.info.commit.parents;
            // The entry (what moved the ref, and when), a note for lost commits, then the commit.
            QColor dim = palette().color(QPalette::Text);
            dim.setAlphaF(0.6f);
            const QString what = e.message.isEmpty() ? e.action : QStringLiteral("%1: %2").arg(e.action, e.message);
            QString html = QStringLiteral("<div style='color:%1; margin-bottom:8px'><b>%2</b> · %3 · %4 · %5</div>")
                               .arg(dim.name(QColor::HexArgb), e.selector.toHtmlEscaped(), what.toHtmlEscaped(),
                                    formatTime(e.time), e.identity.toHtmlEscaped());
            if (e.lost) {
                const QColor warn = theme::badgeColor(theme::Badge::Tag);
                const QColor background = theme::mix(palette().color(QPalette::Base), warn, 0.16);
                html += QStringLiteral(
                            "<table width='100%' cellpadding='6' cellspacing='0' style='background-color:%1; "
                            "margin-bottom:10px'><tr><td>%2 <a href='branch:%3'>%4</a></td></tr></table>")
                            .arg(background.name(),
                                 tr("This commit is on no branch or tag: only the reflog still remembers it, until the "
                                    "entry expires (after 30 days by default).")
                                     .toHtmlEscaped(),
                                 e.commit.sha, tr("Create a branch here…").toHtmlEscaped());
            }
            html += commitHeaderHtml(d.info, m_owner->badgesFor(d.info.commit.sha), palette());
            m_header->setHtml(html);
            {
                QSignalBlocker block(m_files);
                m_files->setFiles(d.files);
            }
            if (d.files.isEmpty()) {
                m_diff->showMessage(tr("This commit has no file changes."));
                return;
            }
            m_files->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
            loadFileDiff();
        });
}

void ReflogWindow::loadFileDiff() {
    const FileChange* f = m_files->currentFile();
    if (!f || m_detailsSha.isEmpty()) return;
    const int gen = ++m_diffGen;
    const Repository repo = m_owner->repo();
    const QString sha = m_detailsSha;
    const QStringList parents = m_detailsParents;
    const FileChange file = *f;
    runAsync(
        this, [repo, sha, parents, file] { return repo.commitDiff(sha, parents, file); },
        [this, gen](const QByteArray& bytes) {
            if (gen != m_diffGen) return;
            const QVector<FileDiff> diffs = parseDiff(bytes);
            if (diffs.isEmpty())
                m_diff->showMessage(tr("No textual changes."));
            else
                m_diff->setDiff(diffs.first(), DiffView::Mode::ReadOnly);
        });
}

void ReflogWindow::showListMenu(const QPoint& pos) {
    QTreeWidgetItem* item = m_list->itemAt(pos);
    if (!item) return;
    m_list->setCurrentItem(item);
    const int i = currentEntry();
    if (i < 0) return;
    const ReflogEntry e = m_entries[i];
    m_owner->showCommitMenu(e.commit.sha, m_list->viewport()->mapToGlobal(pos), [this, e](QMenu& menu) {
        menu.addAction(theme::actionIcon(QStringLiteral("git-commit")), tr("Show in All Commits"), this,
                       [this, e] { showInAllCommits(e); });
        menu.addAction(theme::actionIcon(QStringLiteral("copy")), tr("Copy '%1'").arg(e.selector), this,
                       [selector = e.selector] { QGuiApplication::clipboard()->setText(selector); });
        menu.addSeparator();
    });
}

void ReflogWindow::showInAllCommits(const ReflogEntry& entry) {
    // A lost commit is only listed while lost commits are shown.
    m_owner->revealCommit(entry.commit.sha, entry.lost);
}

}  // namespace aslgc
