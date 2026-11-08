// SPDX-License-Identifier: MIT
#include "MainWindow.h"

#include "AccountsDialog.h"
#include "Dialogs.h"
#include "Log.h"
#include "OperationDialog.h"
#include "RepoPickerDialog.h"
#include "RepoView.h"
#include "Theme.h"
#include "WelcomeWidget.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>

namespace aslgc {

namespace {
constexpr int kMaxRecent = 40;
const QString kRecentKey = QStringLiteral("recent");
}  // namespace

MainWindow::MainWindow() {
    setWindowTitle(QStringLiteral("aslgc"));
    setWindowIcon(theme::appIcon());
    resize(1400, 880);

    m_tabs = new QTabWidget;
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);
    auto* plus = new QToolButton;
    plus->setIcon(theme::actionIcon(QStringLiteral("square-plus")));
    plus->setAutoRaise(true);
    plus->setToolTip(tr("New Tab (Ctrl+T)"));
    connect(plus, &QToolButton::clicked, this, &MainWindow::newTab);
    m_tabs->setCornerWidget(plus, Qt::TopRightCorner);
    setCentralWidget(m_tabs);

    m_status = new QLabel;
    statusBar()->addWidget(m_status, 1);

    createActions();
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this] {
        updateActions();
        if (RepoView* repo = currentRepo()) repo->scheduleRefresh();
    });

    QSettings settings;
    restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray());
    restoreState(settings.value(QStringLiteral("window/state")).toByteArray());
}

void MainWindow::createActions() {
    auto make = [this](const QString& text, const QString& iconName, const QKeySequence& key, auto slot) {
        auto* a = new QAction(iconName.isEmpty() ? QIcon() : theme::actionIcon(iconName), text, this);
        if (!key.isEmpty()) a->setShortcut(key);
        connect(a, &QAction::triggered, this, slot);
        return a;
    };
    auto onRepo = [this](void (RepoView::*fn)()) {
        return [this, fn] {
            if (RepoView* r = currentRepo()) (r->*fn)();
        };
    };

    m_fetch = make(tr("Fetch"), QStringLiteral("cloud-download"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F),
                   onRepo(&RepoView::fetch));
    m_pull = make(tr("Pull"), QStringLiteral("arrow-bar-to-down"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L),
                  onRepo(&RepoView::pull));
    m_push =
        make(tr("Push"), QStringLiteral("arrow-bar-up"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P), onRepo(&RepoView::push));
    m_stash =
        make(tr("Stash"), QStringLiteral("archive"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_S), onRepo(&RepoView::stash));
    m_pop = make(tr("Pop Stash"), QStringLiteral("package-export"), QKeySequence(), onRepo(&RepoView::popStash));
    m_branch = make(tr("Branch"), QStringLiteral("git-branch"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B), [this] {
        if (RepoView* r = currentRepo()) r->createBranch();
    });
    // Git flow: a menu rebuilt from the current repository each time it opens.
    m_gitFlow = make(tr("Git Flow"), QStringLiteral("arrows-split-2"), QKeySequence(), [] {});
    m_gitFlow->setToolTip(tr("Features, bugfixes, releases and hotfixes with git-flow"));
    auto* flowMenu = new QMenu(this);
    connect(flowMenu, &QMenu::aboutToShow, this, [this, flowMenu] {
        flowMenu->clear();
        if (RepoView* r = currentRepo()) r->populateGitFlowMenu(flowMenu);
    });
    m_gitFlow->setMenu(flowMenu);
    m_terminal = make(tr("Terminal"), QStringLiteral("terminal-2"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_T),
                      onRepo(&RepoView::openTerminal));
    m_folder = make(tr("Folder"), QStringLiteral("folder-open"), QKeySequence(), onRepo(&RepoView::openFolder));
    m_refresh = make(tr("Refresh"), QStringLiteral("refresh"), QKeySequence::Refresh, [this] {
        if (RepoView* r = currentRepo()) r->refresh(true);
    });
    m_refresh->setShortcuts({QKeySequence(Qt::Key_F5), QKeySequence(Qt::CTRL | Qt::Key_R)});
    m_fetch->setToolTip(tr("Fetch all remotes (Ctrl+Shift+F)"));
    m_pull->setToolTip(tr("Pull into the current branch (Ctrl+Shift+L)"));
    m_push->setToolTip(tr("Push the current branch (Ctrl+Shift+P)"));
    m_stash->setToolTip(tr("Stash local changes (Ctrl+Alt+S)"));
    m_terminal->setToolTip(tr("Open a terminal in the repository (Ctrl+Alt+T)"));

    auto* addRemote = make(tr("Add Remote…"), QStringLiteral("server"), QKeySequence(), onRepo(&RepoView::addRemote));
    auto* showChanges = make(tr("Local Changes"), QStringLiteral("file-diff"), QKeySequence(Qt::CTRL | Qt::Key_1),
                             onRepo(&RepoView::showChanges));
    auto* showHistory =
        make(tr("All Commits"), QStringLiteral("history"), QKeySequence(Qt::CTRL | Qt::Key_2), onRepo(&RepoView::showHistory));
    auto* find = make(tr("Find Commit…"), QStringLiteral("search"), QKeySequence::Find, onRepo(&RepoView::findCommit));
    auto* fileHistory = make(tr("File History…"), QStringLiteral("history"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H),
                             onRepo(&RepoView::chooseFileHistory));
    m_repoActions = {m_fetch,  m_pull,    m_push,    m_stash,     m_pop,       m_branch, m_gitFlow,  m_terminal,
                     m_folder, m_refresh, addRemote, showChanges, showHistory, find,     fileHistory};

    // Menus
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(make(tr("New Tab"), QStringLiteral("square-plus"), QKeySequence::AddTab, &MainWindow::newTab));
    file->addAction(make(tr("Open Repository…"), QStringLiteral("folder-open"), QKeySequence::Open, &MainWindow::openDialog));
    file->addAction(make(tr("Clone Repository…"), QStringLiteral("download"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O),
                         &MainWindow::cloneDialog));
    file->addAction(make(tr("New Repository…"), QStringLiteral("folder-plus"), QKeySequence(), &MainWindow::initDialog));
    file->addSeparator();
    file->addAction(make(tr("Accounts…"), QStringLiteral("users"), QKeySequence(), [this] {
        AccountsDialog dlg(this);
        dlg.exec();
    }));
    file->addSeparator();
    file->addAction(
        make(tr("Close Tab"), QStringLiteral("x"), QKeySequence::Close, [this] { closeTab(m_tabs->currentIndex()); }));
    file->addAction(make(tr("Quit"), QStringLiteral("power"), QKeySequence::Quit, &QWidget::close));

    QMenu* repo = menuBar()->addMenu(tr("&Repository"));
    repo->addActions({m_fetch, m_pull, m_push});
    repo->addSeparator();
    repo->addActions({m_stash, m_pop, m_branch, addRemote, m_gitFlow});
    repo->addSeparator();
    repo->addActions({m_terminal, m_folder, m_refresh});

    QMenu* view = menuBar()->addMenu(tr("&View"));
    view->addActions({showChanges, showHistory, find, fileHistory});
    view->addSeparator();
    view->addAction(make(tr("Next Tab"), QString(), QKeySequence(Qt::CTRL | Qt::Key_PageDown), [this] {
        if (m_tabs->count() > 1) m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % m_tabs->count());
    }));
    view->addAction(make(tr("Previous Tab"), QString(), QKeySequence(Qt::CTRL | Qt::Key_PageUp), [this] {
        if (m_tabs->count() > 1) m_tabs->setCurrentIndex((m_tabs->currentIndex() + m_tabs->count() - 1) % m_tabs->count());
    }));

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(make(tr("Open Log Folder"), QStringLiteral("folder-open"), QKeySequence(),
                         [] { QDesktopServices::openUrl(QUrl::fromLocalFile(log::directory())); }));
    help->addAction(make(tr("About aslgc"), QStringLiteral("info-circle"), QKeySequence(), &MainWindow::about));

    // Toolbar
    QToolBar* tb = addToolBar(tr("Main"));
    tb->setObjectName(QStringLiteral("mainToolbar"));
    tb->setMovable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    tb->setIconSize(QSize(22, 22));
    tb->addActions({m_fetch, m_pull, m_push});
    tb->addSeparator();
    tb->addActions({m_stash, m_pop, m_branch, m_gitFlow});
    if (auto* flowButton = qobject_cast<QToolButton*>(tb->widgetForAction(m_gitFlow)))
        flowButton->setPopupMode(QToolButton::InstantPopup);
    tb->addSeparator();
    tb->addActions({m_terminal, m_folder});
    auto* spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(spacer);
    tb->addAction(m_refresh);
    updateActions();
}

RepoView* MainWindow::currentRepo() const { return qobject_cast<RepoView*>(m_tabs->currentWidget()); }

void MainWindow::updateActions() {
    RepoView* repo = currentRepo();
    const bool enabled = repo && !repo->isBusy();
    for (QAction* a : m_repoActions) a->setEnabled(enabled);
    if (!repo) {
        m_push->setText(tr("Push"));
        m_pull->setText(tr("Pull"));
        setWindowTitle(QStringLiteral("aslgc"));
        return;
    }
    const RepoSnapshot& s = repo->snapshot();
    int ahead = 0;
    int behind = 0;
    for (const Ref& r : s.refs) {
        if (r.kind == RefKind::LocalBranch && r.isHead) {
            ahead = r.ahead;
            behind = r.behind;
        }
    }
    m_push->setText(ahead ? tr("Push ↑%1").arg(ahead) : tr("Push"));
    m_pull->setText(behind ? tr("Pull ↓%1").arg(behind) : tr("Pull"));
    m_pop->setEnabled(enabled && !s.stashes.isEmpty());
    setWindowTitle(QStringLiteral("%1 — aslgc").arg(repo->title()));
    const int index = m_tabs->indexOf(repo);
    QString tabText = repo->title();
    if (!s.head.branch.isEmpty() && !s.head.detached) tabText += QStringLiteral("  (%1)").arg(s.head.branch);
    m_tabs->setTabText(index, tabText);
    m_tabs->setTabToolTip(index, repo->repo().root());
}

WelcomeWidget* MainWindow::makeWelcome() {
    auto* w = new WelcomeWidget;
    w->setRecent(recent());
    connect(w, &WelcomeWidget::openPath, this, [this](const QString& p) { openRepository(p); });
    connect(w, &WelcomeWidget::openRequested, this, &MainWindow::openDialog);
    connect(w, &WelcomeWidget::cloneRequested, this, &MainWindow::cloneDialog);
    connect(w, &WelcomeWidget::initRequested, this, &MainWindow::initDialog);
    connect(w, &WelcomeWidget::forgetPath, this, &MainWindow::forgetRecent);
    return w;
}

void MainWindow::newTab() {
    const int i = m_tabs->addTab(makeWelcome(), theme::actionIcon(QStringLiteral("square-plus")), tr("New Tab"));
    m_tabs->setCurrentIndex(i);
}

void MainWindow::closeTab(int index) {
    if (index < 0) return;
    QWidget* w = m_tabs->widget(index);
    if (auto* repo = qobject_cast<RepoView*>(w); repo && repo->isBusy()) {
        statusBar()->showMessage(tr("Wait for the running operation to finish."), 4000);
        return;
    }
    m_tabs->removeTab(index);
    w->deleteLater();
    if (m_tabs->count() == 0) newTab();
    saveSession();
}

bool MainWindow::openRepository(const QString& path, bool replaceCurrentWelcome) {
    QString error;
    const QString root = Repository::resolveRoot(path, &error);
    if (root.isEmpty()) {
        showError(this, tr("Open Repository"), tr("'%1' is not a Git repository.\n\n%2").arg(path, error));
        forgetRecent(path);
        return false;
    }
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto* r = qobject_cast<RepoView*>(m_tabs->widget(i)); r && r->repo().root() == root) {
            m_tabs->setCurrentIndex(i);
            return true;
        }
    }
    auto* view = new RepoView(Repository(root));
    connect(view, &RepoView::stateChanged, this, [this, view] {
        if (view == currentRepo())
            updateActions();
        else
            m_tabs->setTabText(m_tabs->indexOf(view), view->title());
    });
    connect(view, &RepoView::busyChanged, this, [this, view] {
        if (view == currentRepo()) updateActions();
    });
    connect(view, &RepoView::statusMessage, this, [this, view](const QString& msg) {
        if (view == currentRepo()) m_status->setText(msg);
    });
    connect(view, &RepoView::activateRequested, this, [this, view] {
        m_tabs->setCurrentWidget(view);
        raise();
        activateWindow();
    });
    const QIcon icon = theme::actionIcon(QStringLiteral("folder"));
    int index = -1;
    if (replaceCurrentWelcome && qobject_cast<WelcomeWidget*>(m_tabs->currentWidget())) {
        index = m_tabs->currentIndex();
        QWidget* old = m_tabs->widget(index);
        m_tabs->insertTab(index, view, icon, view->title());
        m_tabs->removeTab(index + 1);
        old->deleteLater();
    } else {
        index = m_tabs->addTab(view, icon, view->title());
    }
    m_tabs->setCurrentIndex(index);
    addRecent(root);
    saveSession();
    return true;
}

void MainWindow::openDialog() {
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Open Repository"), QDir::homePath());
    if (!dir.isEmpty()) openRepository(dir);
}

void MainWindow::cloneDialog() {
    QSettings settings;
    FormDialog dlg(this, tr("Clone Repository"), tr("Clone"));
    QLineEdit* url =
        dlg.addLine(QStringLiteral("url"), tr("Repository URL:"), {}, QStringLiteral("https://example.com/team/project.git"));
    auto* fromAccount = new QPushButton(theme::actionIcon(QStringLiteral("users")), tr("Choose from GitHub / GitLab Account…"));
    dlg.addWidgetRow(QString(), fromAccount);
    connect(fromAccount, &QPushButton::clicked, &dlg, [&dlg, url] {
        RepoPickerDialog picker(&dlg);
        if (picker.exec() == QDialog::Accepted && !picker.selectedUrl().isEmpty()) url->setText(picker.selectedUrl());
    });
    QLineEdit* parent = dlg.addLine(QStringLiteral("parent"), tr("Parent folder:"),
                                    settings.value(QStringLiteral("cloneParent"), QDir::homePath()).toString());
    auto* browse = new QPushButton(tr("Browse…"));
    dlg.addWidgetRow(QString(), browse);
    QLineEdit* name = dlg.addLine(QStringLiteral("name"), tr("Folder name:"));
    dlg.addCheck(QStringLiteral("submodules"), tr("Clone submodules recursively"), true);
    connect(browse, &QPushButton::clicked, &dlg, [&] {
        const QString d = QFileDialog::getExistingDirectory(&dlg, tr("Parent Folder"), parent->text());
        if (!d.isEmpty()) parent->setText(d);
    });
    connect(url, &QLineEdit::textChanged, &dlg, [name](const QString& u) {
        QString n = u.trimmed().section(QLatin1Char('/'), -1).section(QLatin1Char(':'), -1);
        if (n.endsWith(QLatin1String(".git"))) n.chop(4);
        name->setText(n);
    });
    dlg.setValidator([url, parent, name] {
        return !url->text().trimmed().isEmpty() && QFileInfo(parent->text()).isDir() && !name->text().trimmed().isEmpty() &&
               !QFileInfo::exists(QDir(parent->text()).filePath(name->text().trimmed()));
    });
    if (dlg.exec() != QDialog::Accepted) return;
    const QString parentDir = dlg.text(QStringLiteral("parent"));
    const QString folder = dlg.text(QStringLiteral("name"));
    settings.setValue(QStringLiteral("cloneParent"), parentDir);
    QStringList args{QStringLiteral("clone"), QStringLiteral("--progress")};
    if (dlg.checked(QStringLiteral("submodules"))) args << QStringLiteral("--recurse-submodules");
    args << QStringLiteral("--") << dlg.text(QStringLiteral("url")) << folder;
    auto* op = new OperationDialog(this, tr("Clone %1").arg(folder), Git(parentDir), args);
    const QString target = QDir(parentDir).filePath(folder);
    connect(op, &OperationDialog::completed, this, [this, target](bool ok) {
        if (ok) openRepository(target);
    });
    op->start();
}

void MainWindow::initDialog() {
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose a Folder for the New Repository"), QDir::homePath());
    if (dir.isEmpty()) return;
    if (QDir(dir).exists(QStringLiteral(".git"))) {
        openRepository(dir);
        return;
    }
    const GitResult r = Git(dir).run({QStringLiteral("init")});
    if (!r.ok()) {
        showError(this, tr("New Repository"), r.message());
        return;
    }
    openRepository(dir);
}

QStringList MainWindow::recent() const { return QSettings().value(kRecentKey).toStringList(); }

void MainWindow::addRecent(const QString& path) {
    QStringList list = recent();
    list.removeAll(path);
    list.prepend(path);
    while (list.size() > kMaxRecent) list.removeLast();
    QSettings().setValue(kRecentKey, list);
    refreshWelcomePages();
}

void MainWindow::forgetRecent(const QString& path) {
    QStringList list = recent();
    if (list.removeAll(path) == 0) return;
    QSettings().setValue(kRecentKey, list);
    refreshWelcomePages();
}

void MainWindow::refreshWelcomePages() {
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto* w = qobject_cast<WelcomeWidget*>(m_tabs->widget(i))) w->setRecent(recent());
    }
}

void MainWindow::saveSession() {
    QStringList tabs;
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto* r = qobject_cast<RepoView*>(m_tabs->widget(i))) tabs << r->repo().root();
    }
    QSettings settings;
    settings.setValue(QStringLiteral("session/tabs"), tabs);
    settings.setValue(QStringLiteral("session/current"), m_tabs->currentIndex());
}

void MainWindow::restoreSession() {
    QSettings settings;
    const QStringList tabs = settings.value(QStringLiteral("session/tabs")).toStringList();
    const int current = settings.value(QStringLiteral("session/current"), 0).toInt();
    for (const QString& path : tabs) {
        if (QFileInfo(path).isDir()) openRepository(path, false);
    }
    if (m_tabs->count() == 0)
        newTab();
    else
        m_tabs->setCurrentIndex(qBound(0, current, m_tabs->count() - 1));
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveSession();
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/state"), saveState());
    // Destroy repo views now so they persist their layout.
    while (m_tabs->count() > 0) {
        QWidget* w = m_tabs->widget(0);
        m_tabs->removeTab(0);
        delete w;
    }
    event->accept();
}

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && isActiveWindow()) {
        if (RepoView* r = currentRepo()) r->scheduleRefresh();
    }
}

void MainWindow::about() {
    QMessageBox::about(this, tr("About aslgc"),
                       tr("<h3>aslgc %1</h3>"
                          "<p>A simple Linux Git client, built with Qt %2.</p>"
                          "<p>Git: <code>%3</code></p>")
                           .arg(QStringLiteral(ASLGC_VERSION), QString::fromLatin1(qVersion()), Git::program().toHtmlEscaped()));
}

}  // namespace aslgc
