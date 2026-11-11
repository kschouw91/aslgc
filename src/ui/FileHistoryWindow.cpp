// SPDX-License-Identifier: MIT
#include "FileHistoryWindow.h"

#include "Async.h"
#include "BlameView.h"
#include "CommitModel.h"
#include "DiffView.h"
#include "FileList.h"
#include "RepoView.h"
#include "Theme.h"
#include "git/Diff.h"

#include <QButtonGroup>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace aslgc {

namespace {

enum Column { SubjectColumn, PathColumn, AuthorColumn, DateColumn, ShaColumn, ColumnCount };
enum PageId { HistoryPageId, BlamePageId };
constexpr int kMaxCommits = 10000;
constexpr int kUncommittedRow = -2;  // list item data of the "Uncommitted changes" row

bool sameChanges(const QVector<FileChange>& a, const QVector<FileChange>& b) {
    if (a.size() != b.size()) return false;
    for (qsizetype i = 0; i < a.size(); ++i) {
        if (a[i].path != b[i].path || a[i].oldPath != b[i].oldPath || a[i].status != b[i].status) return false;
    }
    return true;
}

// "refs/heads/main" → "main", "refs/remotes/origin/main" → "origin/main"
QString shortRefName(const QString& ref) {
    for (const QLatin1String prefix : {QLatin1String("refs/heads/"), QLatin1String("refs/remotes/")}) {
        if (ref.startsWith(prefix)) return ref.mid(prefix.size());
    }
    return ref;
}

bool isFullSha(const QString& rev) {
    if (rev.size() != 40 && rev.size() != 64) return false;
    for (QChar c : rev) {
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) return false;
    }
    return true;
}

QToolButton* pageButton(const QString& text, const QString& icon, const QString& tip) {
    auto* b = new QToolButton;
    b->setText(text);
    b->setIcon(theme::actionIcon(icon));
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    b->setCheckable(true);
    b->setAutoRaise(true);
    b->setToolTip(tip);
    return b;
}

}  // namespace

FileHistoryWindow::FileHistoryWindow(RepoView* owner, const QString& path, bool folder)
    : QWidget(owner, Qt::Window), m_owner(owner), m_path(path), m_folder(folder) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowIcon(theme::appIcon());
    resize(1100, 760);

    // Breadcrumb, commit count, page switch and reload
    auto* top = new QWidget;
    auto* tl = new QHBoxLayout(top);
    tl->setContentsMargins(10, 6, 8, 6);
    m_breadcrumb = new QLabel;
    m_breadcrumb->setTextFormat(Qt::RichText);
    m_breadcrumb->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    m_breadcrumb->setToolTip(tr("Click a folder to show its history"));
    m_count = new QLabel;
    m_count->setEnabled(false);
    m_branch = new QComboBox;
    m_branch->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_branch->setMinimumContentsLength(22);
    m_historyButton = pageButton(tr("History"), QStringLiteral("history"), tr("Commits that changed this path (Ctrl+1)"));
    m_blameButton = pageButton(tr("Blame"), QStringLiteral("list-details"), tr("Who last changed each line (Ctrl+2)"));
    m_pages = new QButtonGroup(this);
    m_pages->addButton(m_historyButton, HistoryPageId);
    m_pages->addButton(m_blameButton, BlamePageId);
    auto* reloadButton = new QToolButton;
    reloadButton->setIcon(theme::actionIcon(QStringLiteral("refresh")));
    reloadButton->setAutoRaise(true);
    reloadButton->setToolTip(tr("Reload (F5)"));
    tl->addWidget(m_breadcrumb, 1);
    tl->addWidget(m_count);
    tl->addSpacing(12);
    tl->addWidget(m_branch);
    tl->addSpacing(6);
    tl->addWidget(m_historyButton);
    tl->addWidget(m_blameButton);
    tl->addWidget(reloadButton);

    // History page: commits on top, details / files / diff below
    m_list = new QTreeWidget;
    m_list->setColumnCount(ColumnCount);
    m_list->setHeaderLabels({tr("Description"), tr("Path"), tr("Author"), tr("Date"), tr("SHA")});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setAllColumnsShowFocus(true);
    m_list->setIndentation(0);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setFrameShape(QFrame::NoFrame);
    QHeaderView* header = m_list->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(SubjectColumn, QHeaderView::Stretch);
    for (int c : {PathColumn, AuthorColumn, DateColumn, ShaColumn}) header->setSectionResizeMode(c, QHeaderView::Interactive);
    header->resizeSection(PathColumn, 220);
    header->resizeSection(AuthorColumn, 150);
    header->resizeSection(DateColumn, 140);
    header->resizeSection(ShaColumn, 90);

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
    m_bottom->setSizes({360, 900});
    // Shown above the list while it holds the history of a range of lines.
    m_lineBanner = new QFrame;
    m_lineBanner->setAutoFillBackground(true);
    QPalette bannerPalette = m_lineBanner->palette();
    bannerPalette.setColor(QPalette::Window,
                           theme::mix(palette().color(QPalette::Window), palette().color(QPalette::Highlight), 0.15));
    m_lineBanner->setPalette(bannerPalette);
    auto* bannerLayout = new QHBoxLayout(m_lineBanner);
    bannerLayout->setContentsMargins(10, 5, 10, 5);
    m_lineLabel = new QLabel;
    m_lineLabel->setTextFormat(Qt::RichText);
    m_lineLabel->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    bannerLayout->addWidget(m_lineLabel, 1);
    m_lineBanner->hide();
    auto* listBox = new QWidget;
    auto* listLayout = new QVBoxLayout(listBox);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(0);
    listLayout->addWidget(m_lineBanner);
    listLayout->addWidget(m_list, 1);

    m_vertical = new QSplitter(Qt::Vertical);
    m_vertical->addWidget(listBox);
    m_vertical->addWidget(m_bottom);
    m_vertical->setSizes({280, 480});
    m_vertical->setChildrenCollapsible(false);
    m_historyPage = m_vertical;

    // Blame page
    m_blamePage = new QWidget;
    auto* bl = new QVBoxLayout(m_blamePage);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(0);
    auto* bar = new QWidget;
    auto* barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(6, 2, 10, 2);
    m_back = new QToolButton;
    m_back->setIcon(theme::actionIcon(QStringLiteral("arrow-left")));
    m_back->setText(tr("Back"));
    m_back->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_back->setAutoRaise(true);
    m_back->setToolTip(tr("Back to the previous blame (Alt+Left)"));
    m_blameLabel = new QLabel;
    m_blameLabel->setTextFormat(Qt::RichText);
    m_blameLabel->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    m_blameOptionsButton = new QToolButton;
    m_blameOptionsButton->setIcon(theme::actionIcon(QStringLiteral("settings")));
    m_blameOptionsButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_blameOptionsButton->setAutoRaise(true);
    m_blameOptionsButton->setPopupMode(QToolButton::InstantPopup);
    auto* blameMenu = new QMenu(m_blameOptionsButton);
    blameMenu->setToolTipsVisible(true);
    m_blameWhitespace = blameMenu->addAction(tr("Ignore Whitespace"));
    m_blameWhitespace->setToolTip(tr("Changes that only add, remove or re-indent whitespace don't count (git blame -w)"));
    m_blameMoves = blameMenu->addAction(tr("Detect Lines Moved Within the File"));
    m_blameMoves->setToolTip(tr("Lines moved or copied within the file keep the commit that wrote them (git blame -M)"));
    m_blameCopies = blameMenu->addAction(tr("Detect Lines Moved or Copied from Other Files"));
    m_blameCopies->setToolTip(
        tr("Also follow lines that came from other files changed in the same commit (git blame -C); slower"));
    for (QAction* a : {m_blameWhitespace, m_blameMoves, m_blameCopies}) a->setCheckable(true);
    m_blameOptionsButton->setMenu(blameMenu);
    barLayout->addWidget(m_back);
    barLayout->addWidget(m_blameLabel, 1);
    barLayout->addWidget(m_blameOptionsButton);
    m_blame = new BlameView;
    bl->addWidget(bar);
    bl->addWidget(m_blame, 1);

    m_stack = new QStackedWidget;
    m_stack->addWidget(m_historyPage);
    m_stack->addWidget(m_blamePage);

    auto* rule = new QFrame;
    rule->setFrameShape(QFrame::HLine);
    rule->setFrameShadow(QFrame::Sunken);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(top);
    layout->addWidget(rule);
    layout->addWidget(m_stack, 1);

    QSettings settings;
    restoreGeometry(settings.value(QStringLiteral("layout/fileHistoryGeometry")).toByteArray());
    m_vertical->restoreState(settings.value(QStringLiteral("layout/fileHistoryVertical")).toByteArray());
    m_bottom->restoreState(settings.value(QStringLiteral("layout/fileHistoryBottom")).toByteArray());
    m_detailsSplit->restoreState(settings.value(QStringLiteral("layout/fileHistoryDetails")).toByteArray());
    m_blameWhitespace->setChecked(settings.value(QStringLiteral("blame/ignoreWhitespace"), false).toBool());
    m_blameMoves->setChecked(settings.value(QStringLiteral("blame/detectMoves"), false).toBool());
    m_blameCopies->setChecked(settings.value(QStringLiteral("blame/detectCopies"), false).toBool());

    connect(m_breadcrumb, &QLabel::linkActivated, this, [this](const QString& link) {
        if (!link.startsWith(QLatin1String("seg:"))) return;
        const QStringList parts = m_path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        setTarget(parts.mid(0, link.mid(4).toInt()).join(QLatin1Char('/')), true);
    });
    connect(m_pages, &QButtonGroup::idClicked, this, [this](int id) {
        if (id == BlamePageId)
            showDefaultBlame();
        else
            showHistoryPage();
    });
    connect(reloadButton, &QToolButton::clicked, this, &FileHistoryWindow::reload);
    connect(m_branch, &QComboBox::activated, this, &FileHistoryWindow::chooseBranch);
    for (QAction* a : {m_blameWhitespace, m_blameMoves, m_blameCopies})
        connect(a, &QAction::toggled, this, &FileHistoryWindow::blameOptionsChanged);
    connect(m_list, &QTreeWidget::currentItemChanged, this, &FileHistoryWindow::onCurrentEntryChanged);
    connect(m_list, &QWidget::customContextMenuRequested, this, &FileHistoryWindow::showListMenu);
    connect(m_header, &QTextBrowser::anchorClicked, this, &FileHistoryWindow::onHeaderLink);
    connect(m_files, &QListWidget::currentItemChanged, this, &FileHistoryWindow::loadFileDiff);
    m_files->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_files, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const FileChange* f = m_files->currentFile();
        if (!f) return;
        QMenu menu(this);
        if (m_folder) {
            m_owner->addFileHistoryActions(menu, *f, m_detailsSha, m_detailsParents);
            menu.addSeparator();
        }
        menu.addAction(theme::actionIcon(QStringLiteral("copy")), tr("Copy Path"), this,
                       [path = f->path] { QGuiApplication::clipboard()->setText(path); });
        menu.exec(m_files->viewport()->mapToGlobal(pos));
    });
    connect(m_back, &QToolButton::clicked, this, &FileHistoryWindow::blameBack);
    connect(m_blameLabel, &QLabel::linkActivated, this, [this](const QString& link) {
        if (link.startsWith(QLatin1String("commit:"))) showCommit(link.mid(7), m_blameTarget.path);
    });
    connect(m_lineLabel, &QLabel::linkActivated, this, [this](const QString& link) {
        if (link == QLatin1String("all"))
            clearLineHistory();
        else if (link.startsWith(QLatin1String("commit:")) && m_lines)
            showCommit(link.mid(7), m_lines->path);
    });
    m_diff->setLineHistoryEnabled(true);
    connect(m_diff, &DiffView::lineHistoryRequested, this, &FileHistoryWindow::requestLineHistory);
    connect(m_blame, &BlameView::commitClicked, this, &FileHistoryWindow::showCommitOfLine);
    m_blame->setMenuProvider([this](QMenu& menu, int line) { fillBlameMenu(menu, line); });
    connect(m_owner, &RepoView::stateChanged, this, [this] {
        if (updateBranchChoices())
            chooseBranch(m_branch->currentIndex());  // the chosen branch is gone
        else if (historyTip() != m_loadedTip)
            reload();
        else
            loadUncommitted();
    });

    auto shortcut = [this](const QKeySequence& key, auto slot) {
        connect(new QShortcut(key, this), &QShortcut::activated, this, slot);
    };
    shortcut(QKeySequence::Close, [this] { close(); });
    shortcut(QKeySequence(Qt::Key_F5), [this] { reload(); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_R), [this] { reload(); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_1), [this] { showHistoryPage(); });
    shortcut(QKeySequence(Qt::CTRL | Qt::Key_2), [this] { showDefaultBlame(); });
    shortcut(QKeySequence(Qt::ALT | Qt::Key_Left), [this] {
        if (m_stack->currentWidget() == m_blamePage) blameBack();
    });

    updateBranchChoices();
    blameOptionsChanged();
    setTarget(path, folder);
}

FileHistoryWindow::~FileHistoryWindow() {
    QSettings settings;
    settings.setValue(QStringLiteral("layout/fileHistoryGeometry"), saveGeometry());
    settings.setValue(QStringLiteral("layout/fileHistoryVertical"), m_vertical->saveState());
    settings.setValue(QStringLiteral("layout/fileHistoryBottom"), m_bottom->saveState());
    settings.setValue(QStringLiteral("layout/fileHistoryDetails"), m_detailsSplit->saveState());
}

// ---------------------------------------------------------------- target and pages

void FileHistoryWindow::setTarget(const QString& path, bool folder) {
    m_path = path;
    m_folder = folder;
    m_entries.clear();
    m_detailsSha.clear();
    m_blameHistory.clear();
    m_blameLoaded = false;
    m_blame->showMessage({});
    m_lines.reset();
    m_lineDiffs.clear();
    m_uncommitted.clear();
    m_uncommittedSelected = false;
    updateLineBanner();
    m_blameButton->setEnabled(!folder);
    m_blameButton->setToolTip(folder ? tr("Blame is available for files") : tr("Who last changed each line (Ctrl+2)"));
    m_files->setVisible(folder);
    updateTitle();
    showHistoryPage();
    loadHistory({});
}

void FileHistoryWindow::updateTitle() {
    const QString repoName = m_owner->repo().name();
    const QStringList parts = m_path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QColor dim = palette().color(QPalette::Text);
    dim.setAlphaF(0.5f);
    const QString slash = QStringLiteral(" <span style='color:%1'>/</span> ").arg(dim.name(QColor::HexArgb));
    auto link = [](int n, const QString& text) {
        return QStringLiteral("<a href='seg:%1'>%2</a>").arg(n).arg(text.toHtmlEscaped());
    };
    QString html = parts.isEmpty() ? QStringLiteral("<b>%1</b>").arg(repoName.toHtmlEscaped()) : link(0, repoName);
    for (int i = 0; i < parts.size(); ++i) {
        html += slash;
        html += i == parts.size() - 1 ? QStringLiteral("<b>%1</b>").arg(parts[i].toHtmlEscaped()) : link(i + 1, parts[i]);
    }
    if (m_folder && !parts.isEmpty()) html += slash;
    m_breadcrumb->setText(html);
    const QString name = parts.isEmpty() ? repoName : parts.last();
    const QString kind = m_folder ? tr("Folder History") : m_lines ? tr("Line History") : tr("File History");
    setWindowTitle(QStringLiteral("%1 — %2 — %3").arg(name, kind, repoName));
}

bool FileHistoryWindow::updateBranchChoices() {
    const RepoSnapshot& s = m_owner->snapshot();
    QSignalBlocker block(m_branch);
    m_branch->clear();
    m_branch->addItem(
        theme::actionIcon(QStringLiteral("git-branch")),
        s.head.branch.isEmpty() ? tr("Current commit (detached HEAD)") : tr("Current branch (%1)").arg(s.head.branch),
        QStringList{});
    QVector<const Ref*> locals;
    QVector<const Ref*> remotes;
    for (const Ref& r : s.refs) {
        if (r.kind == RefKind::LocalBranch && r.name != s.head.branch)
            locals << &r;
        else if (r.kind == RefKind::RemoteBranch && !r.name.endsWith(QLatin1String("/HEAD")))
            remotes << &r;
    }
    if (!locals.isEmpty() || !remotes.isEmpty()) m_branch->insertSeparator(m_branch->count());
    for (const Ref* r : locals)
        m_branch->addItem(theme::actionIcon(QStringLiteral("git-branch")), r->name, QStringList{r->fullName});
    for (const Ref* r : remotes) m_branch->addItem(theme::actionIcon(QStringLiteral("cloud")), r->name, QStringList{r->fullName});
    m_branch->insertSeparator(m_branch->count());
    m_branch->addItem(theme::actionIcon(QStringLiteral("arrows-split-2")), tr("All branches"),
                      QStringList{QStringLiteral("--branches"), QStringLiteral("--remotes")});
    const int index = m_branch->findData(m_revs);
    m_branch->setCurrentIndex(std::max(0, index));
    m_branch->setToolTip(tr("Which commits to list: the current branch, another branch or all branches"));
    return index < 0;
}

void FileHistoryWindow::chooseBranch(int index) {
    const QStringList revs = m_branch->itemData(index).toStringList();
    if (revs == m_revs) return;
    m_revs = revs;
    // The blame follows the chosen branch.
    m_blameHistory.clear();
    m_blameLoaded = false;
    loadHistory(m_detailsSha);
    if (m_stack->currentWidget() == m_blamePage) showDefaultBlame();
}

bool FileHistoryWindow::isAllBranches() const { return m_revs.value(0).startsWith(QLatin1String("--")); }

QString FileHistoryWindow::historyTip() const {
    const RepoSnapshot& s = m_owner->snapshot();
    if (m_revs.isEmpty()) return s.head.sha;
    QString tip;
    for (const Ref& r : s.refs) {
        if (r.kind != RefKind::LocalBranch && r.kind != RefKind::RemoteBranch) continue;
        if (isAllBranches())
            tip += r.fullName + QLatin1Char(' ') + r.target + QLatin1Char('\n');
        else if (r.fullName == m_revs.first())
            return r.target;
    }
    return tip;
}

void FileHistoryWindow::showHistoryPage() {
    m_stack->setCurrentWidget(m_historyPage);
    m_historyButton->setChecked(true);
}

void FileHistoryWindow::reload() {
    loadHistory(m_detailsSha);
    if (m_blameLoaded) {
        BlameTarget t = m_blameTarget;
        t.line = m_blame->topLine();
        t.center = false;
        loadBlame(t, false, true);
    }
}

void FileHistoryWindow::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    // Coming back after editing files: pick up the new status (and with it the uncommitted
    // changes), and refresh a working tree blame.
    if (event->type() == QEvent::ActivationChange && isActiveWindow()) m_owner->scheduleRefresh();
    if (event->type() == QEvent::ActivationChange && isActiveWindow() && m_blameLoaded && m_blameTarget.rev.isEmpty()) {
        const QFileInfo fi(QDir(m_owner->repo().root()).filePath(m_blameTarget.path));
        if (fi.lastModified() != m_blameFileTime) {
            BlameTarget t = m_blameTarget;
            t.line = m_blame->topLine();
            t.center = false;
            loadBlame(t, false, true);
        }
    }
}

// ---------------------------------------------------------------- history

void FileHistoryWindow::loadHistory(const QString& selectSha) {
    const int gen = ++m_historyGen;
    m_count->setText(tr("Loading…"));
    m_loadedTip = historyTip();
    const Repository repo = m_owner->repo();
    if (m_lines) {
        const LineRange range = *m_lines;
        runAsync(
            this,
            [repo, range] {
                QString error;
                QVector<LineLogEntry> log = repo.lineLog(range.path, range.first, range.last, range.rev, kMaxCommits, &error);
                return std::pair{log, error};
            },
            [this, gen, selectSha, range](const std::pair<QVector<LineLogEntry>, QString>& result) {
                if (gen != m_historyGen) return;
                m_entries.clear();
                m_lineDiffs.clear();
                m_lineError = result.second;
                QString current = range.path;  // merges have no diff: they keep the newer entry's name
                for (const LineLogEntry& e : result.first) {
                    FileChange f{current, {}, 'M', {}, false};
                    if (!e.diff.newPath.isEmpty() || !e.diff.oldPath.isEmpty()) {
                        f.path = e.diff.newPath.isEmpty() ? e.diff.oldPath : e.diff.newPath;
                        if (e.diff.newFile) {
                            f.status = 'A';
                        } else if (!e.diff.oldPath.isEmpty() && e.diff.oldPath != f.path) {
                            f.status = 'R';
                            f.oldPath = e.diff.oldPath;
                        }
                        current = f.oldPath.isEmpty() ? f.path : f.oldPath;
                    }
                    m_entries.push_back({e.commit, {f}});
                    m_lineDiffs.push_back(e.diff);
                }
                updateLineBanner();
                populateList(selectSha);
            });
        return;
    }
    const QString path = m_path;
    const bool folder = m_folder;
    const QStringList revs = m_revs;
    const bool uncommitted = showsUncommitted();
    ++m_uncommittedGen;  // this load brings the uncommitted changes too
    using Result = std::pair<QVector<FileLogEntry>, QVector<FileChange>>;
    runAsync(
        this,
        [repo, path, folder, revs, uncommitted] {
            return Result{repo.fileLog(path, folder, kMaxCommits, revs),
                          uncommitted ? repo.uncommittedChanges(path, folder) : QVector<FileChange>{}};
        },
        [this, gen, selectSha](const Result& result) {
            if (gen != m_historyGen) return;
            m_entries = result.first;
            m_uncommitted = result.second;
            m_lineDiffs.clear();
            populateList(selectSha);
        });
}

void FileHistoryWindow::showLineHistory(const QString& path, int first, int last, const QString& rev) {
    if (m_folder) {
        m_owner->showLineHistory(path, first, last, rev);
        return;
    }
    m_lines = LineRange{path, first, last, rev};
    m_lineDiffs.clear();
    m_lineError.clear();
    updateTitle();
    updateLineBanner();
    showHistoryPage();
    loadHistory({});
}

void FileHistoryWindow::clearLineHistory() {
    m_lines.reset();
    m_lineDiffs.clear();
    updateTitle();
    updateLineBanner();
    loadHistory(m_detailsSha);
}

void FileHistoryWindow::updateLineBanner() {
    m_lineBanner->setVisible(m_lines.has_value());
    m_branch->setEnabled(!m_lines);
    if (!m_lines) return;
    const LineRange& r = *m_lines;
    const QString file = QStringLiteral("<b>%1</b>").arg(r.path.toHtmlEscaped());
    QString text = r.first == r.last ? tr("Commits that changed line %1 of %2").arg(r.first).arg(file)
                                     : tr("Commits that changed lines %1–%2 of %3").arg(r.first).arg(r.last).arg(file);
    text += QLatin1Char(' ') + (r.rev.isEmpty() ? tr("in the working tree") : tr("as of %1").arg(describeRev(r.rev)));
    m_lineLabel->setText(text + QStringLiteral(" · <a href='all'>%1</a>").arg(tr("Show all commits")));
}

void FileHistoryWindow::requestLineHistory(int first, int last, bool oldSide) {
    const FileChange* f = m_files->currentFile();
    if (f && m_uncommittedSelected) {
        if (!oldSide)
            showLineHistory(f->path, first, last, {});
        else if (!f->isUntracked())
            showLineHistory(f->oldPath.isEmpty() ? f->path : f->oldPath, first, last, QStringLiteral("HEAD"));
        return;
    }
    if (!f || m_detailsSha.isEmpty()) return;
    if (!oldSide) {
        showLineHistory(f->path, first, last, m_detailsSha);
    } else if (!m_detailsParents.isEmpty()) {
        showLineHistory(f->oldPath.isEmpty() ? f->path : f->oldPath, first, last, m_detailsParents.first());
    }
}

int FileHistoryWindow::currentEntry() const {
    const QTreeWidgetItem* item = m_list->currentItem();
    if (!item) return -1;
    const int i = item->data(SubjectColumn, Qt::UserRole).toInt();
    return i >= 0 && i < m_entries.size() ? i : -1;
}

void FileHistoryWindow::populateList(const QString& selectSha) {
    const QString keep = selectSha.isEmpty() ? m_detailsSha : selectSha;
    const bool keepUncommitted = m_uncommittedSelected;
    QColor dim = palette().color(QPalette::Text);
    dim.setAlphaF(0.7f);
    QFont mono = font();
    mono.setFamilies({QStringLiteral("monospace")});
    mono.setStyleHint(QFont::Monospace);

    QList<QTreeWidgetItem*> items;
    items.reserve(m_entries.size() + 1);
    bool otherNames = false;
    const bool uncommittedRow = showsUncommitted() && !m_uncommitted.isEmpty();
    if (uncommittedRow) {
        auto* item = new QTreeWidgetItem;
        fillUncommittedItem(item);
        items << item;
        otherNames |= !m_folder && m_uncommitted.first().path != m_path;
    }
    for (int i = 0; i < m_entries.size(); ++i) {
        const Commit& c = m_entries[i].commit;
        auto* item = new QTreeWidgetItem;
        item->setData(SubjectColumn, Qt::UserRole, i);
        item->setText(SubjectColumn, c.subject);
        item->setText(AuthorColumn, c.authorName);
        item->setToolTip(AuthorColumn, QStringLiteral("%1 <%2>").arg(c.authorName, c.authorEmail));
        item->setText(DateColumn, formatTime(c.authorTime));
        item->setToolTip(DateColumn, QDateTime::fromSecsSinceEpoch(c.authorTime).toString(Qt::RFC2822Date));
        item->setText(ShaColumn, c.shortSha());
        item->setToolTip(ShaColumn, c.sha);
        item->setFont(ShaColumn, mono);
        for (int col : {PathColumn, AuthorColumn, DateColumn, ShaColumn}) item->setForeground(col, dim);
        if (!m_folder && !m_entries[i].changes.isEmpty()) {
            const FileChange& f = m_entries[i].changes.first();
            item->setIcon(SubjectColumn, theme::statusIcon(f.status));
            item->setText(PathColumn, f.path);
            const QString status = theme::statusName(f.status);
            item->setToolTip(SubjectColumn, f.oldPath.isEmpty() ? status : tr("%1 from %2").arg(status, f.oldPath));
            item->setToolTip(PathColumn, f.oldPath.isEmpty() ? f.path : QStringLiteral("%1 → %2").arg(f.oldPath, f.path));
            otherNames |= f.path != (m_lines ? m_lines->path : m_path);
        }
        items << item;
    }
    {
        QSignalBlocker block(m_list);
        m_list->clear();
        m_list->addTopLevelItems(items);
    }
    m_list->setColumnHidden(PathColumn, !otherNames);
    const int n = int(m_entries.size());
    m_count->setText(n >= kMaxCommits ? tr("latest %n commit(s)", nullptr, n) : tr("%n commit(s)", nullptr, n));

    if (items.isEmpty()) {
        m_uncommittedSelected = false;
        m_detailsSha.clear();
        m_header->clear();
        m_files->setFiles({});
        const QString what = m_path.isEmpty() ? m_owner->repo().name() : m_path;
        if (m_lines && !m_lineError.isEmpty())
            m_diff->showMessage(tr("Could not load the history of these lines:\n\n%1").arg(m_lineError));
        else if (m_lines)
            m_diff->showMessage(tr("No commit on the current branch changed these lines."));
        else if (m_revs.isEmpty())
            m_diff->showMessage(tr("No commit on the current branch touches %1.").arg(what));
        else if (isAllBranches())
            m_diff->showMessage(tr("No commit on any branch touches %1.").arg(what));
        else
            m_diff->showMessage(tr("No commit on %1 touches %2.").arg(shortRefName(m_revs.first()), what));
        return;
    }
    QTreeWidgetItem* item = uncommittedRow && keepUncommitted ? uncommittedItem() : entryItem(entryIndex(keep));
    if (!item) item = m_list->topLevelItem(0);
    {
        QSignalBlocker block(m_list);
        m_list->setCurrentItem(item);
    }
    m_list->scrollToItem(item, QAbstractItemView::PositionAtCenter);
    onCurrentEntryChanged();
}

QTreeWidgetItem* FileHistoryWindow::entryItem(int index) const {
    if (index < 0 || index >= m_entries.size()) return nullptr;
    return m_list->topLevelItem(index + (uncommittedItem() ? 1 : 0));
}

int FileHistoryWindow::entryIndex(const QString& sha) const {
    if (sha.isEmpty()) return -1;
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].commit.sha == sha) return i;
    }
    return -1;
}

void FileHistoryWindow::selectEntry(const QString& sha) {
    QTreeWidgetItem* item = entryItem(entryIndex(sha));
    if (!item) return;
    m_list->setCurrentItem(item);
    m_list->scrollToItem(item, QAbstractItemView::PositionAtCenter);
}

void FileHistoryWindow::onCurrentEntryChanged() {
    if (QTreeWidgetItem* item = m_list->currentItem(); item && item == uncommittedItem()) {
        showUncommitted();
        return;
    }
    const int i = currentEntry();
    if (i >= 0) showDetails(m_entries[i].commit.sha, m_entries[i].changes);
}

// ---------------------------------------------------------------- uncommitted changes

bool FileHistoryWindow::showsUncommitted() const { return m_revs.isEmpty() && !m_lines && !m_owner->snapshot().head.unborn; }

QTreeWidgetItem* FileHistoryWindow::uncommittedItem() const {
    QTreeWidgetItem* item = m_list->topLevelItem(0);
    return item && item->data(SubjectColumn, Qt::UserRole).toInt() == kUncommittedRow ? item : nullptr;
}

void FileHistoryWindow::fillUncommittedItem(QTreeWidgetItem* item) const {
    item->setData(SubjectColumn, Qt::UserRole, kUncommittedRow);
    QFont italic = font();
    italic.setItalic(true);
    item->setFont(SubjectColumn, italic);
    if (m_folder) {
        item->setText(SubjectColumn, tr("Uncommitted changes in %n file(s)", nullptr, int(m_uncommitted.size())));
        item->setIcon(SubjectColumn, theme::actionIcon(QStringLiteral("file-diff")));
        item->setToolTip(SubjectColumn, tr("Staged and unstaged changes compared with HEAD, and new files"));
        return;
    }
    const FileChange& f = m_uncommitted.first();
    item->setText(SubjectColumn, tr("Uncommitted changes"));
    item->setIcon(SubjectColumn, theme::statusIcon(f.status));
    item->setToolTip(SubjectColumn, theme::statusName(f.status));
    item->setText(PathColumn, f.path);
    QColor dim = palette().color(QPalette::Text);
    dim.setAlphaF(0.7f);
    item->setForeground(PathColumn, dim);
}

void FileHistoryWindow::loadUncommitted() {
    const int gen = ++m_uncommittedGen;
    if (!showsUncommitted()) return;
    const Repository repo = m_owner->repo();
    const QString path = m_path;
    const bool folder = m_folder;
    runAsync(
        this, [repo, path, folder] { return repo.uncommittedChanges(path, folder); },
        [this, gen](const QVector<FileChange>& changes) {
            if (gen == m_uncommittedGen) applyUncommitted(changes);
        });
}

void FileHistoryWindow::applyUncommitted(const QVector<FileChange>& changes) {
    if (!sameChanges(changes, m_uncommitted)) {
        m_uncommitted = changes;
        populateList(m_detailsSha);  // the row comes, goes or changes; the selection stays
        return;
    }
    if (m_uncommittedSelected) loadFileDiff();  // same files, but maybe edited again
}

void FileHistoryWindow::showUncommitted() {
    m_uncommittedSelected = true;
    m_detailsSha.clear();
    m_detailsParents.clear();
    ++m_detailsGen;
    m_uncommittedDiff.clear();
    m_header->setHtml(QStringLiteral("<p><b>%1</b></p><p>%2</p><p><a href='changes:'>%3</a></p>")
                          .arg(tr("Uncommitted changes"),
                               tr("Staged and unstaged changes compared with HEAD, and new files that are not tracked yet."),
                               tr("Commit them in Local Changes")));
    {
        QSignalBlocker block(m_files);
        m_files->setFiles(m_uncommitted);
    }
    m_files->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    loadFileDiff();
}

void FileHistoryWindow::showDetails(const QString& sha, const QVector<FileChange>& fileChanges) {
    m_uncommittedSelected = false;
    m_detailsSha = sha;
    const int gen = ++m_detailsGen;
    const Repository repo = m_owner->repo();
    const QString path = m_path;
    const bool folder = m_folder;
    runAsync(
        this,
        [repo, sha, fileChanges, path, folder] {
            DetailsData d;
            d.info = repo.commitInfo(sha);
            if (d.info.valid) d.files = folder ? repo.commitChanges(sha, d.info.commit.parents, path) : fileChanges;
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
            m_header->setHtml(commitHeaderHtml(d.info, m_owner->badgesFor(d.info.commit.sha), palette()));
            {
                QSignalBlocker block(m_files);
                m_files->setFiles(d.files);
            }
            if (d.files.isEmpty()) {
                m_diff->showMessage(tr("This commit changes nothing in this folder."));
                return;
            }
            m_files->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
            loadFileDiff();
        });
}

void FileHistoryWindow::loadFileDiff() {
    const FileChange* f = m_files->currentFile();
    if (!f || (m_detailsSha.isEmpty() && !m_uncommittedSelected)) return;
    const int gen = ++m_diffGen;
    if (m_uncommittedSelected) {
        const Repository repo = m_owner->repo();
        const FileChange file = *f;
        runAsync(
            this,
            [repo, file] {
                return file.isUntracked() ? repo.workingDiff(file, false) : repo.compareDiff(QStringLiteral("HEAD"), {}, file);
            },
            [this, gen, file](const QByteArray& bytes) {
                if (gen != m_diffGen || !m_uncommittedSelected) return;
                const QByteArray shown = file.path.toUtf8() + '\0' + bytes;
                if (shown == m_uncommittedDiff) return;  // unchanged: keep the scroll position
                m_uncommittedDiff = shown;
                const QVector<FileDiff> diffs = parseDiff(bytes);
                if (diffs.isEmpty())
                    m_diff->showMessage(tr("No textual changes."));
                else
                    m_diff->setDiff(diffs.first(), DiffView::Mode::ReadOnly);
            });
        return;
    }
    // Line history: the diff of just those lines came with the log.
    const int entry = currentEntry();
    if (m_lines && entry >= 0 && entry < m_lineDiffs.size() && m_entries[entry].commit.sha == m_detailsSha) {
        const FileDiff& d = m_lineDiffs[entry];
        if (!d.hunks.isEmpty())
            m_diff->setDiff(d, DiffView::Mode::ReadOnly);
        else if (m_entries[entry].commit.isMerge())
            m_diff->showMessage(tr("This merge brought in changes to these lines from the merged branch."));
        else
            m_diff->showMessage(tr("No changes to these lines."));
        return;
    }
    const Repository repo = m_owner->repo();
    const QString sha = m_detailsSha;
    const QStringList parents = m_detailsParents;
    const FileChange file = *f;
    runAsync(
        this, [repo, sha, parents, file] { return repo.commitDiff(sha, parents, file); },
        [this, gen, file](const QByteArray& bytes) {
            if (gen != m_diffGen) return;
            const QVector<FileDiff> diffs = parseDiff(bytes);
            const bool moved = (file.status == 'R' || file.status == 'C') && !file.oldPath.isEmpty();
            if (moved && (diffs.isEmpty() || (diffs.first().hunks.isEmpty() && !diffs.first().binary)))
                m_diff->showMessage(file.status == 'R' ? tr("Renamed from %1 without changes.").arg(file.oldPath)
                                                       : tr("Copied from %1 without changes.").arg(file.oldPath));
            else if (diffs.isEmpty())
                m_diff->showMessage(tr("No textual changes."));
            else
                m_diff->setDiff(diffs.first(), DiffView::Mode::ReadOnly);
        });
}

void FileHistoryWindow::showListMenu(const QPoint& pos) {
    QTreeWidgetItem* item = m_list->itemAt(pos);
    if (!item) return;
    m_list->setCurrentItem(item);
    if (item == uncommittedItem()) {
        QMenu menu(this);
        const char status = m_uncommitted.value(0).status;
        if (!m_folder && status != 'D' && status != '?')
            menu.addAction(theme::actionIcon(QStringLiteral("list-details")), tr("Blame Working Tree File"), this,
                           [this] { showBlame({}, m_path); });
        menu.addAction(theme::actionIcon(QStringLiteral("file-diff")), tr("Show in Local Changes"), this,
                       [this] { m_owner->revealChanges(); });
        menu.exec(m_list->viewport()->mapToGlobal(pos));
        return;
    }
    const int i = item->data(SubjectColumn, Qt::UserRole).toInt();
    if (i < 0 || i >= m_entries.size()) return;
    const FileLogEntry e = m_entries[i];
    m_owner->showCommitMenu(e.commit.sha, m_list->viewport()->mapToGlobal(pos), [this, e](QMenu& menu) {
        if (!m_folder && !e.changes.isEmpty()) {
            const FileChange f = e.changes.first();
            const bool deleted = f.status == 'D';
            const QString rev = deleted ? e.commit.parents.value(0) : e.commit.sha;
            menu.addAction(theme::actionIcon(QStringLiteral("list-details")),
                           deleted ? tr("Blame File Before This Commit") : tr("Blame File at This Commit"), this,
                           [this, rev, path = f.path] { showBlame(rev, path); })
                ->setEnabled(!rev.isEmpty());
        }
        menu.addAction(theme::actionIcon(QStringLiteral("git-commit")), tr("Show in All Commits"), this,
                       [this, sha = e.commit.sha] { m_owner->revealCommit(sha); });
        menu.addSeparator();
    });
}

void FileHistoryWindow::onHeaderLink(const QUrl& url) {
    if (url.scheme() == QLatin1String("changes")) {
        m_owner->revealChanges();
        return;
    }
    if (url.scheme() != QLatin1String("commit")) return;
    const QString sha = url.path();
    if (entryIndex(sha) >= 0)
        selectEntry(sha);
    else
        m_owner->revealCommit(sha);
}

// ---------------------------------------------------------------- blame

void FileHistoryWindow::showDefaultBlame() {
    if (m_folder) {
        showHistoryPage();
        return;
    }
    if (!m_blameLoaded) {
        // The chosen branch's version, or the working tree while following the current branch.
        BlameTarget t{m_revs.size() == 1 ? m_revs.first() : QString(), m_path};
        if (t.rev.isEmpty() && !QFileInfo::exists(QDir(m_owner->repo().root()).filePath(m_path))) {
            // Not in the working tree: blame the last commit that still had the file.
            t.rev = QStringLiteral("HEAD");
            if (m_revs.isEmpty() && !m_entries.isEmpty()) {
                const FileLogEntry& newest = m_entries.first();
                if (!newest.changes.isEmpty() && newest.changes.first().status == 'D' && !newest.commit.parents.isEmpty())
                    t.rev = newest.commit.parents.first();
            }
        }
        loadBlame(t, false);
    }
    m_stack->setCurrentWidget(m_blamePage);
    m_blameButton->setChecked(true);
    m_blame->setFocus();
}

void FileHistoryWindow::showBlame(const QString& rev, const QString& blamePath) {
    if (m_folder) return;
    const BlameTarget t{rev, blamePath.isEmpty() ? m_path : blamePath};
    if (!m_blameLoaded || t.rev != m_blameTarget.rev || t.path != m_blameTarget.path) loadBlame(t, true);
    m_stack->setCurrentWidget(m_blamePage);
    m_blameButton->setChecked(true);
    m_blame->setFocus();
}

BlameOptions FileHistoryWindow::blameOptions() const {
    BlameOptions o;
    o.ignoreWhitespace = m_blameWhitespace->isChecked();
    o.detectMoves = m_blameMoves->isChecked();
    o.detectCopies = m_blameCopies->isChecked();
    return o;
}

void FileHistoryWindow::blameOptionsChanged() {
    const BlameOptions o = blameOptions();
    QSettings settings;
    settings.setValue(QStringLiteral("blame/ignoreWhitespace"), o.ignoreWhitespace);
    settings.setValue(QStringLiteral("blame/detectMoves"), o.detectMoves);
    settings.setValue(QStringLiteral("blame/detectCopies"), o.detectCopies);
    const int on = int(o.ignoreWhitespace) + int(o.detectMoves) + int(o.detectCopies);
    m_blameOptionsButton->setText(on == 0 ? tr("Options") : tr("Options (%1)").arg(on));
    m_blameOptionsButton->setToolTip(tr("How lines are attributed to commits"));
    if (m_blameLoaded) {
        BlameTarget t = m_blameTarget;
        t.line = m_blame->topLine();
        t.center = false;
        loadBlame(t, false);
    }
}

void FileHistoryWindow::loadBlame(const BlameTarget& target, bool remember, bool quiet) {
    if (remember && m_blameLoaded) {
        BlameTarget current = m_blameTarget;
        current.line = m_blame->topLine();
        current.center = false;
        m_blameHistory.push_back(current);
    }
    m_blameTarget = target;
    m_blameLoaded = true;
    updateBlameBar();
    if (!quiet) m_blame->showMessage(tr("Loading…"));
    m_blameFileTime =
        target.rev.isEmpty() ? QFileInfo(QDir(m_owner->repo().root()).filePath(target.path)).lastModified() : QDateTime();
    const int gen = ++m_blameGen;
    const Repository repo = m_owner->repo();
    const BlameOptions options = blameOptions();
    runAsync(
        this, [repo, target, options] { return repo.blame(target.path, target.rev, options); },
        [this, gen, target](const Blame& b) {
            if (gen != m_blameGen) return;
            if (!b.ok()) {
                m_blame->showMessage(tr("Could not blame %1:\n\n%2").arg(target.path, b.error));
            } else if (b.binary) {
                m_blame->showMessage(tr("%1 is a binary file.").arg(target.path));
            } else {
                m_blame->setBlame(b, target.path);
                if (target.line >= 0) m_blame->scrollToLine(target.line, target.center);
            }
        });
}

void FileHistoryWindow::blameBack() {
    if (m_blameHistory.isEmpty()) return;
    loadBlame(m_blameHistory.takeLast(), false);
}

QString FileHistoryWindow::describeRev(const QString& rev) const {
    const int i = entryIndex(rev);
    if (i >= 0) {
        const Commit& c = m_entries[i].commit;
        return QStringLiteral("<a href='commit:%1'><code>%2</code></a> %3").arg(c.sha, c.shortSha(), c.subject.toHtmlEscaped());
    }
    if (isFullSha(rev)) return QStringLiteral("<a href='commit:%1'><code>%2</code></a>").arg(rev, rev.left(8));
    return QStringLiteral("<code>%1</code>").arg(shortRefName(rev).toHtmlEscaped());
}

void FileHistoryWindow::updateBlameBar() {
    m_back->setEnabled(!m_blameHistory.isEmpty());
    const QString file = QStringLiteral("<b>%1</b>").arg(m_blameTarget.path.toHtmlEscaped());
    m_blameLabel->setText(m_blameTarget.rev.isEmpty() ? tr("%1 in the working tree").arg(file)
                                                      : tr("%1 at %2").arg(file, describeRev(m_blameTarget.rev)));
}

void FileHistoryWindow::fillBlameMenu(QMenu& menu, int line) {
    int first = line;
    int last = line;
    m_blame->selectedLineRange(line, &first, &last);
    menu.addAction(
        theme::actionIcon(QStringLiteral("history")),
        first == last ? tr("History of Line %1").arg(first + 1) : tr("History of Lines %1–%2").arg(first + 1).arg(last + 1), this,
        [this, first, last, target = m_blameTarget] { showLineHistory(target.path, first + 1, last + 1, target.rev); });
    menu.addSeparator();
    const BlameCommit* c = m_blame->commitOfLine(line);
    if (!c || c->isUncommitted()) return;
    const BlameCommit commit = *c;
    const BlameLine blameLine = m_blame->blame().lines[line];
    // git names the previous revision once per commit. Lines that came from another file of
    // that commit (detected copies) go back to that file instead.
    const QString previousPath =
        !blameLine.path.isEmpty() && blameLine.path != commit.path ? blameLine.path : commit.previousPath;
    menu.addAction(theme::actionIcon(QStringLiteral("history")), tr("Show Commit"), this,
                   [this, line] { showCommitOfLine(line); });
    menu.addAction(theme::actionIcon(QStringLiteral("git-commit")), tr("Show in All Commits"), this,
                   [this, sha = commit.sha] { m_owner->revealCommit(sha); });
    menu.addAction(theme::actionIcon(QStringLiteral("list-details")), tr("Blame Previous Revision"), this,
                   [this, commit, blameLine, previousPath] {
                       loadBlame({commit.previousSha, previousPath, blameLine.origLine - 1, true}, true);
                   })
        ->setEnabled(!commit.previousSha.isEmpty());
    menu.addSeparator();
    menu.addAction(theme::actionIcon(QStringLiteral("copy")), tr("Copy SHA"), this,
                   [sha = commit.sha] { QGuiApplication::clipboard()->setText(sha); });
}

void FileHistoryWindow::showCommitOfLine(int line) {
    const BlameCommit* c = m_blame->commitOfLine(line);
    if (!c || c->isUncommitted()) return;
    showCommit(c->sha, m_blame->blame().lines[line].path);
}

void FileHistoryWindow::showCommit(const QString& sha, const QString& pathAtCommit) {
    showHistoryPage();
    if (entryIndex(sha) >= 0) {
        selectEntry(sha);
        return;
    }
    // Not in this history (e.g. reached through a merge): show it on its own.
    {
        QSignalBlocker block(m_list);
        m_list->setCurrentItem(nullptr);
    }
    m_list->clearSelection();
    showDetails(sha, {FileChange{pathAtCommit, {}, 'M', {}, false}});
}

}  // namespace aslgc
