// SPDX-License-Identifier: MIT
#include "HistoryView.h"

#include "Async.h"
#include "DiffView.h"
#include "FileList.h"
#include "RepoView.h"
#include "git/Diff.h"

#include <QClipboard>
#include <QDateTime>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QTextBrowser>
#include <QTreeView>
#include <QVBoxLayout>

namespace aslgc {

namespace {
struct DetailsData {
    CommitInfo info;
    QVector<FileChange> files;
};
}  // namespace

HistoryView::HistoryView(RepoView* owner) : QWidget(owner), m_owner(owner) {
    m_model = new CommitModel(this);

    // Search bar
    auto* searchRow = new QWidget;
    auto* sr = new QHBoxLayout(searchRow);
    sr->setContentsMargins(8, 6, 8, 6);
    m_search = new QLineEdit;
    m_search->setPlaceholderText(tr("Find commit by message, author or SHA  (Enter = next, Shift+Enter = previous)"));
    m_search->setClearButtonEnabled(true);
    m_searchStatus = new QLabel;
    sr->addWidget(m_search, 1);
    sr->addWidget(m_searchStatus);

    m_list = new QTreeView;
    m_list->setModel(m_model);
    m_list->setItemDelegate(new CommitDelegate(m_list));
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setItemsExpandable(false);
    m_list->setAllColumnsShowFocus(true);
    m_list->setIndentation(0);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->header()->setStretchLastSection(false);
    m_list->header()->setSectionResizeMode(CommitModel::Description, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(CommitModel::Author, QHeaderView::Interactive);
    m_list->header()->setSectionResizeMode(CommitModel::Date, QHeaderView::Interactive);
    m_list->header()->setSectionResizeMode(CommitModel::Sha, QHeaderView::Interactive);
    m_list->header()->resizeSection(CommitModel::Author, 150);
    m_list->header()->resizeSection(CommitModel::Date, 140);
    m_list->header()->resizeSection(CommitModel::Sha, 90);

    auto* top = new QWidget;
    auto* tl = new QVBoxLayout(top);
    tl->setContentsMargins(0, 0, 0, 0);
    tl->setSpacing(0);
    tl->addWidget(searchRow);
    tl->addWidget(m_list, 1);

    m_header = new QTextBrowser;
    m_header->setOpenLinks(false);
    m_header->setFrameShape(QFrame::NoFrame);
    m_files = new FileList;
    m_files->setSelectionMode(QAbstractItemView::SingleSelection);
    m_diff = new DiffView;

    m_detailsSplit = new QSplitter(Qt::Vertical);
    m_detailsSplit->addWidget(m_header);
    m_detailsSplit->addWidget(m_files);
    m_detailsSplit->setSizes({170, 200});
    m_bottom = new QSplitter(Qt::Horizontal);
    m_bottom->addWidget(m_detailsSplit);
    m_bottom->addWidget(m_diff);
    m_bottom->setStretchFactor(1, 1);
    m_bottom->setSizes({380, 900});

    m_vertical = new QSplitter(Qt::Vertical);
    m_vertical->addWidget(top);
    m_vertical->addWidget(m_bottom);
    m_vertical->setSizes({420, 380});
    m_vertical->setChildrenCollapsible(false);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_vertical);

    connect(m_list->selectionModel(), &QItemSelectionModel::currentRowChanged, this, &HistoryView::onCurrentRowChanged);
    connect(m_list, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex idx = m_list->indexAt(pos);
        if (!idx.isValid()) return;
        m_list->setCurrentIndex(idx);
        m_owner->showCommitMenu(m_model->commit(idx.row()).sha, m_list->viewport()->mapToGlobal(pos));
    });
    connect(m_search, &QLineEdit::returnPressed, this,
            [this] { findNext(QGuiApplication::keyboardModifiers() & Qt::ShiftModifier); });
    connect(m_search, &QLineEdit::textChanged, this, [this] { m_searchStatus->clear(); });
    connect(m_header, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        if (url.scheme() == QLatin1String("commit")) selectCommit(url.path());
    });
    connect(m_files, &QListWidget::currentItemChanged, this, &HistoryView::loadFileDiff);
    m_files->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_files, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const FileChange* f = m_files->currentFile();
        if (!f) return;
        QMenu menu(this);
        const QString path = f->path;
        m_owner->addFileHistoryActions(menu, *f, m_detailsSha, m_detailsParents);
        menu.addSeparator();
        menu.addAction(theme::actionIcon(QStringLiteral("copy")), tr("Copy Path"), this,
                       [path] { QGuiApplication::clipboard()->setText(path); });
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
    m_diff->showMessage(tr("Select a commit."));
}

void HistoryView::setCommits(const QVector<Commit>& commits, const QVector<GraphRow>& graph,
                             const QHash<QString, QVector<RefBadge>>& badges, const QString& headSha) {
    const QString keep = selectedSha();
    const int scroll = m_list->verticalScrollBar()->value();
    {
        QSignalBlocker block(m_list->selectionModel());
        m_model->setCommits(commits, graph, badges, headSha);
    }
    QString target = keep;
    if (target.isEmpty() || m_model->rowOf(target) < 0) target = m_firstLoad ? headSha : QString();
    if (!target.isEmpty() && m_model->rowOf(target) >= 0) {
        const QModelIndex idx = m_model->index(m_model->rowOf(target), 0);
        QSignalBlocker block(m_list->selectionModel());
        m_list->selectionModel()->setCurrentIndex(idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        if (m_firstLoad)
            m_list->scrollTo(idx, QAbstractItemView::PositionAtCenter);
        else
            m_list->verticalScrollBar()->setValue(scroll);
        if (target != m_detailsSha) showCommit(target);
    } else if (!m_firstLoad) {
        m_list->verticalScrollBar()->setValue(scroll);
    }
    if (commits.isEmpty()) {
        m_header->clear();
        m_files->setFiles({});
        m_diff->showMessage(tr("No commits yet."));
        m_detailsSha.clear();
    }
    m_firstLoad = false;
}

QString HistoryView::selectedSha() const {
    const QModelIndex idx = m_list->currentIndex();
    return idx.isValid() ? m_model->commit(idx.row()).sha : QString();
}

void HistoryView::selectCommit(const QString& sha) {
    const int row = m_model->rowOf(sha);
    if (row < 0) {
        m_list->clearSelection();
        showCommit(sha);
        return;
    }
    const QModelIndex idx = m_model->index(row, 0);
    m_list->selectionModel()->setCurrentIndex(idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_list->scrollTo(idx, QAbstractItemView::PositionAtCenter);
}

void HistoryView::onCurrentRowChanged() {
    const QString sha = selectedSha();
    if (!sha.isEmpty() && sha != m_detailsSha) showCommit(sha);
}

void HistoryView::showCommit(const QString& sha) {
    m_detailsSha = sha;
    const int gen = ++m_detailsGen;
    const Repository repo = m_owner->repo();
    runAsync(
        this,
        [repo, sha] {
            DetailsData d;
            d.info = repo.commitInfo(sha);
            if (d.info.valid) d.files = repo.commitChanges(sha, d.info.commit.parents);
            return d;
        },
        [this, gen](const DetailsData& d) {
            if (gen != m_detailsGen) return;
            if (!d.info.valid) {
                m_header->setPlainText(tr("Commit not found."));
                m_files->setFiles({});
                m_diff->clearView();
                return;
            }
            m_detailsParents = d.info.commit.parents;
            renderHeader(d.info);
            {
                QSignalBlocker block(m_files);
                m_files->setFiles(d.files);
            }
            if (!d.files.isEmpty()) {
                m_files->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
                loadFileDiff();
            } else {
                m_diff->showMessage(tr("This commit has no file changes."));
            }
        });
}

void HistoryView::renderHeader(const CommitInfo& info) {
    m_header->setHtml(commitHeaderHtml(info, m_model->badgesFor(info.commit.sha), palette()));
}

void HistoryView::loadFileDiff() {
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

void HistoryView::findNext(bool backwards) {
    const QString needle = m_search->text().trimmed();
    const int n = m_model->rowCount();
    if (needle.isEmpty() || n == 0) return;
    const int start = m_list->currentIndex().isValid() ? m_list->currentIndex().row() : (backwards ? 0 : -1);
    for (int step = 1; step <= n; ++step) {
        const int row = ((start + (backwards ? -step : step)) % n + n) % n;
        const Commit& c = m_model->commit(row);
        if (c.subject.contains(needle, Qt::CaseInsensitive) || c.authorName.contains(needle, Qt::CaseInsensitive) ||
            c.authorEmail.contains(needle, Qt::CaseInsensitive) || c.sha.startsWith(needle, Qt::CaseInsensitive)) {
            selectCommit(c.sha);
            m_searchStatus->clear();
            return;
        }
    }
    m_searchStatus->setText(tr("No match"));
}

void HistoryView::focusSearch() {
    m_search->setFocus();
    m_search->selectAll();
}

void HistoryView::saveLayout(QSettings& settings) const {
    settings.setValue(QStringLiteral("layout/historyVertical"), m_vertical->saveState());
    settings.setValue(QStringLiteral("layout/historyBottom"), m_bottom->saveState());
    settings.setValue(QStringLiteral("layout/historyDetails"), m_detailsSplit->saveState());
}

void HistoryView::restoreLayout(const QSettings& settings) {
    m_vertical->restoreState(settings.value(QStringLiteral("layout/historyVertical")).toByteArray());
    m_bottom->restoreState(settings.value(QStringLiteral("layout/historyBottom")).toByteArray());
    m_detailsSplit->restoreState(settings.value(QStringLiteral("layout/historyDetails")).toByteArray());
}

}  // namespace aslgc
