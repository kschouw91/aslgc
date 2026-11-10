// SPDX-License-Identifier: MIT
#include "HistoryView.h"

#include "Async.h"
#include "Dialogs.h"
#include "DiffView.h"
#include "FileList.h"
#include "RepoView.h"
#include "git/Diff.h"

#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
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

#include <algorithm>

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
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);  // two or more selected: a comparison
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

    connect(m_list->selectionModel(), &QItemSelectionModel::selectionChanged, this, &HistoryView::onSelectionChanged);
    connect(m_list, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex idx = m_list->indexAt(pos);
        if (!idx.isValid()) return;
        const QStringList selected = selectedShas();
        if (selected.size() >= 2 && selected.contains(m_model->commit(idx.row()).sha)) {
            showSelectionMenu(selected, m_list->viewport()->mapToGlobal(pos));
            return;
        }
        m_list->setCurrentIndex(idx);
        m_owner->showCommitMenu(m_model->commit(idx.row()).sha, m_list->viewport()->mapToGlobal(pos));
    });
    connect(m_search, &QLineEdit::returnPressed, this,
            [this] { findNext(QGuiApplication::keyboardModifiers() & Qt::ShiftModifier); });
    connect(m_search, &QLineEdit::textChanged, this, [this] { m_searchStatus->clear(); });
    connect(m_header, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        if (url.scheme() == QLatin1String("commit")) {
            selectCommit(url.path());
        } else if (url.scheme() == QLatin1String("compare") && m_compare) {
            if (url.path() == QLatin1String("save")) {
                saveComparisonPatch();
            } else {
                Comparison c = *m_compare;
                c.includeOldest = url.path() == QLatin1String("include");
                showComparison(c);
            }
        }
    });
    connect(m_files, &QListWidget::currentItemChanged, this, [this] { loadFileDiff(); });
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
        if (!f || (m_detailsSha.isEmpty() && !m_compare)) return;
        if (!oldSide)
            m_owner->showLineHistory(f->path, first, last, m_detailsSha);  // empty: the working tree
        else if (!m_detailsParents.isEmpty())
            m_owner->showLineHistory(f->oldPath.isEmpty() ? f->path : f->oldPath, first, last, m_detailsParents.first());
    });
    m_diff->showMessage(tr("Select a commit."));
}

void HistoryView::setCommits(const QVector<Commit>& commits, const QVector<GraphRow>& graph,
                             const QHash<QString, QVector<RefBadge>>& badges, const QString& headSha) {
    const QString keep = selectedSha();
    const QStringList keepSelected = selectedShas();
    const int scroll = m_list->verticalScrollBar()->value();
    {
        QSignalBlocker block(m_list->selectionModel());
        m_model->setCommits(commits, graph, badges, headSha);
    }
    QString target = keep;
    if (target.isEmpty() || m_model->rowOf(target) < 0) target = m_firstLoad ? headSha : QString();
    if (!target.isEmpty() && m_model->rowOf(target) >= 0) {
        const QModelIndex idx = m_model->index(m_model->rowOf(target), 0);
        int restored = 1;
        {
            QSignalBlocker block(m_list->selectionModel());
            m_list->selectionModel()->setCurrentIndex(idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            for (const QString& sha : keepSelected) {
                const int row = m_model->rowOf(sha);
                if (sha == target || row < 0) continue;
                m_list->selectionModel()->select(m_model->index(row, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
                ++restored;
            }
        }
        if (m_firstLoad)
            m_list->scrollTo(idx, QAbstractItemView::PositionAtCenter);
        else
            m_list->verticalScrollBar()->setValue(scroll);
        // A comparison survives a reload when all its commits are still listed.
        const bool keepComparison =
            m_compare && (m_compare->newest.isEmpty() ? m_compare->oldest == target : restored == keepSelected.size());
        if (!keepComparison && (m_compare || target != m_detailsSha)) onSelectionChanged();
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

QStringList HistoryView::selectedShas() const {
    QList<int> rows;
    for (const QModelIndex& idx : m_list->selectionModel()->selectedRows()) rows << idx.row();
    std::sort(rows.begin(), rows.end());
    QStringList out;
    for (int row : rows) out << m_model->commit(row).sha;
    return out;
}

void HistoryView::onSelectionChanged() {
    const QStringList selected = selectedShas();
    if (selected.size() >= 2) {
        // A range of three or more shows the combined changes; two show the difference between them.
        const Comparison c{selected.last(), selected.first(), int(selected.size()), selected.size() > 2};
        if (!m_compare || !m_compare->sameAs(c)) showComparison(c);
    } else if (selected.size() == 1) {
        if (m_compare || selected.first() != m_detailsSha) showCommit(selected.first());
    }
}

void HistoryView::compareWithWorkingTree(const QString& sha) {
    const int row = m_model->rowOf(sha);
    if (row >= 0) {
        QSignalBlocker block(m_list->selectionModel());
        const QModelIndex idx = m_model->index(row, 0);
        m_list->selectionModel()->setCurrentIndex(idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        m_list->scrollTo(idx, QAbstractItemView::PositionAtCenter);
    }
    showComparison({sha, {}, 1, false});
}

void HistoryView::refreshWorkingTreeComparison() {
    if (m_compare && m_compare->newest.isEmpty()) showComparison(*m_compare, true);
}

void HistoryView::showComparison(const Comparison& comparison, bool refresh) {
    m_compare = comparison;
    m_detailsSha = comparison.newest;
    const int gen = ++m_detailsGen;
    const Repository repo = m_owner->repo();
    struct Data {
        QString from;
        QVector<FileChange> files;
    };
    runAsync(
        this,
        [repo, comparison] {
            Data d;
            d.from = comparison.includeOldest ? repo.baseOf(comparison.oldest) : comparison.oldest;
            d.files = repo.compareChanges(d.from, comparison.newest);
            return d;
        },
        [this, gen, refresh](const Data& d) {
            if (gen != m_detailsGen) return;
            m_compareFrom = d.from;
            m_detailsParents = {d.from};
            renderComparisonHeader(int(d.files.size()));
            const QString keepPath = refresh && m_files->currentFile() ? m_files->currentFile()->path : QString();
            {
                QSignalBlocker block(m_files);
                m_files->setFiles(d.files);
                if (keepPath.isEmpty() || !m_files->selectPath(keepPath))
                    m_files->setCurrentRow(d.files.isEmpty() ? -1 : 0, QItemSelectionModel::ClearAndSelect);
            }
            if (d.files.isEmpty())
                m_diff->showMessage(tr("No differences."));
            else
                loadFileDiff(refresh && !keepPath.isEmpty() && m_files->currentFile() &&
                             m_files->currentFile()->path == keepPath);
        });
}

void HistoryView::renderComparisonHeader(int fileCount) {
    const Comparison& c = *m_compare;
    QColor dim = palette().color(QPalette::Text);
    dim.setAlphaF(0.6f);
    const QString dimCss = dim.name(QColor::HexArgb);
    auto describe = [this](const QString& sha) {
        QString text = QStringLiteral("<a href='commit:%1'><code>%2</code></a>").arg(sha, sha.left(8));
        const int row = m_model->rowOf(sha);
        if (row >= 0) {
            const Commit& commit = m_model->commit(row);
            text += QStringLiteral(" %1 <span style='opacity:0.7'>· %2 · %3</span>")
                        .arg(commit.subject.toHtmlEscaped(), commit.authorName.toHtmlEscaped(), formatTime(commit.authorTime));
        }
        return text;
    };
    const bool workTree = c.newest.isEmpty();
    QString title;
    if (workTree)
        title = c.includeOldest ? tr("Changes since before %1, up to the working tree").arg(c.oldest.left(8))
                                : tr("Comparing %1 with the working tree").arg(c.oldest.left(8));
    else
        title = c.includeOldest ? tr("Combined changes of the selected commits") : tr("Differences between two commits");
    QString html = QStringLiteral("<div style='font-size:large; font-weight:600'>%1</div>").arg(title.toHtmlEscaped());
    html += QStringLiteral("<table cellspacing='0' cellpadding='2' style='margin-top:8px'>");
    auto row = [&](const QString& label, const QString& value) {
        html += QStringLiteral("<tr><td style='color:%1; padding-right:10px'>%2</td><td>%3</td></tr>").arg(dimCss, label, value);
    };
    row(tr("From"), c.includeOldest ? tr("before %1").arg(describe(c.oldest)) : describe(c.oldest));
    row(tr("To"),
        workTree ? tr("the working tree (staged and unstaged changes; untracked files are not included)") : describe(c.newest));
    if (c.count > 2) row(tr("Selected"), tr("%n commit(s)", nullptr, c.count));
    row(tr("Files"), tr("%n file(s) changed", nullptr, fileCount));
    html += QStringLiteral("</table><p>");
    const QString newer = workTree ? tr("the working tree") : c.newest.left(8);
    html += c.includeOldest
                ? QStringLiteral("<a href='compare:exclude'>%1</a>")
                      .arg(tr("Show only the differences between %1 and %2").arg(c.oldest.left(8), newer).toHtmlEscaped())
                : QStringLiteral("<a href='compare:include'>%1</a>")
                      .arg(tr("Include the changes made by %1 itself").arg(c.oldest.left(8)).toHtmlEscaped());
    if (fileCount > 0) html += QStringLiteral(" · <a href='compare:save'>%1</a>").arg(tr("Save as Patch…").toHtmlEscaped());
    html += QStringLiteral("</p>");
    m_header->setHtml(html);
}

void HistoryView::saveComparisonPatch() {
    if (!m_compare) return;
    const QString from = m_compareFrom;
    const QString to = m_compare->newest;
    const QString name =
        QStringLiteral("%1..%2.patch").arg(from.left(8), to.isEmpty() ? QStringLiteral("working-tree") : to.left(8));
    const QString path = QFileDialog::getSaveFileName(this, tr("Save Patch"), QDir(m_owner->repo().root()).filePath(name),
                                                      tr("Patch files (*.patch *.diff)"));
    if (path.isEmpty()) return;
    const Repository repo = m_owner->repo();
    runAsync(
        this, [repo, from, to] { return repo.comparePatch(from, to); },
        [this, path](const QByteArray& patch) {
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly) || f.write(patch) != patch.size())
                showError(this, tr("Save Patch"), f.errorString());
            else
                emit m_owner->statusMessage(tr("Saved %1").arg(path));
        });
}

void HistoryView::showSelectionMenu(const QStringList& shas, const QPoint& globalPos) {
    QStringList oldestFirst = shas;
    std::reverse(oldestFirst.begin(), oldestFirst.end());
    QMenu menu(this);
    menu.addAction(theme::actionIcon(QStringLiteral("download")),
                   tr("Save %n Commit(s) as Patch Files…", nullptr, int(shas.size())), this,
                   [this, oldestFirst] { m_owner->savePatches(oldestFirst); });
    menu.addAction(theme::actionIcon(QStringLiteral("file-diff")), tr("Save Diff as Patch…"), this,
                   &HistoryView::saveComparisonPatch)
        ->setEnabled(m_compare.has_value());
    menu.addSeparator();
    menu.addAction(theme::actionIcon(QStringLiteral("copy")), tr("Copy SHAs"), this,
                   [oldestFirst] { QGuiApplication::clipboard()->setText(oldestFirst.join(QLatin1Char('\n'))); });
    menu.exec(globalPos);
}

void HistoryView::showCommit(const QString& sha) {
    m_compare.reset();
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

void HistoryView::loadFileDiff(bool keepScroll) {
    const FileChange* f = m_files->currentFile();
    if (!f || (m_detailsSha.isEmpty() && !m_compare)) return;
    const int gen = ++m_diffGen;
    const Repository repo = m_owner->repo();
    const QString sha = m_detailsSha;
    const QStringList parents = m_detailsParents;
    const FileChange file = *f;
    const bool comparing = m_compare.has_value();
    const QString from = m_compareFrom;
    runAsync(
        this,
        [repo, sha, parents, file, comparing, from] {
            return comparing ? repo.compareDiff(from, sha, file) : repo.commitDiff(sha, parents, file);
        },
        [this, gen, keepScroll](const QByteArray& bytes) {
            if (gen != m_diffGen) return;
            const QVector<FileDiff> diffs = parseDiff(bytes);
            if (diffs.isEmpty())
                m_diff->showMessage(tr("No textual changes."));
            else
                m_diff->setDiff(diffs.first(), DiffView::Mode::ReadOnly, keepScroll);
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
