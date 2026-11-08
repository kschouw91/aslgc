// SPDX-License-Identifier: MIT
#include "RepoView.h"

#include "AccountsDialog.h"
#include "Async.h"
#include "ChangesView.h"
#include "Dialogs.h"
#include "FileHistoryWindow.h"
#include "HistoryView.h"
#include "InteractiveRebaseDialog.h"
#include "OperationDialog.h"
#include "PullRequestDialog.h"
#include "Theme.h"
#include "hosting/AccountStore.h"

#include <QMessageBox>

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCompleter>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStringListModel>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <tuple>

namespace aslgc {

namespace {
QString b(const QString& s) { return QStringLiteral("<b>%1</b>").arg(s.toHtmlEscaped()); }

QString flowStartTitle(flow::Kind kind) {
    switch (kind) {
        case flow::Kind::Feature: return RepoView::tr("Start Feature");
        case flow::Kind::Bugfix: return RepoView::tr("Start Bugfix");
        case flow::Kind::Release: return RepoView::tr("Start Release");
        case flow::Kind::Hotfix: return RepoView::tr("Start Hotfix");
    }
    return {};
}

QString flowFinishTitle(flow::Kind kind) {
    switch (kind) {
        case flow::Kind::Feature: return RepoView::tr("Finish Feature");
        case flow::Kind::Bugfix: return RepoView::tr("Finish Bugfix");
        case flow::Kind::Release: return RepoView::tr("Finish Release");
        case flow::Kind::Hotfix: return RepoView::tr("Finish Hotfix");
    }
    return {};
}

QString flowFinishLabel(flow::Kind kind, const QString& name) {
    switch (kind) {
        case flow::Kind::Feature: return RepoView::tr("Finish Feature '%1'…").arg(name);
        case flow::Kind::Bugfix: return RepoView::tr("Finish Bugfix '%1'…").arg(name);
        case flow::Kind::Release: return RepoView::tr("Finish Release %1…").arg(name);
        case flow::Kind::Hotfix: return RepoView::tr("Finish Hotfix %1…").arg(name);
    }
    return {};
}
}  // namespace

RepoView::RepoView(const Repository& repo, QWidget* parent) : QWidget(parent), m_repo(repo) {
    // Sidebar column
    auto* sideCol = new QWidget;
    auto* sl = new QVBoxLayout(sideCol);
    sl->setContentsMargins(0, 0, 0, 0);
    sl->setSpacing(0);
    m_repoLabel = new QLabel;
    m_repoLabel->setContentsMargins(12, 10, 12, 8);
    m_repoLabel->setTextFormat(Qt::RichText);
    m_sidebar = new Sidebar;
    sl->addWidget(m_repoLabel);
    sl->addWidget(m_sidebar, 1);

    m_changes = new ChangesView(this);
    m_history = new HistoryView(this);
    m_stack = new QStackedWidget;
    m_stack->addWidget(m_changes);
    m_stack->addWidget(m_history);

    // Banner for merge / rebase / cherry-pick in progress
    m_banner = new QFrame;
    m_banner->setAutoFillBackground(true);
    QPalette bp = m_banner->palette();
    bp.setColor(QPalette::Window, theme::mix(palette().color(QPalette::Window), QColor(0xd4a72c), 0.28));
    m_banner->setPalette(bp);
    auto* bl = new QHBoxLayout(m_banner);
    bl->setContentsMargins(12, 6, 12, 6);
    m_bannerLabel = new QLabel;
    m_continue = new QPushButton(tr("Continue"));
    m_skip = new QPushButton(tr("Skip"));
    m_abort = new QPushButton(tr("Abort"));
    bl->addWidget(m_bannerLabel, 1);
    bl->addWidget(m_continue);
    bl->addWidget(m_skip);
    bl->addWidget(m_abort);
    m_banner->hide();

    auto* main = new QWidget;
    auto* ml = new QVBoxLayout(main);
    ml->setContentsMargins(0, 0, 0, 0);
    ml->setSpacing(0);
    ml->addWidget(m_banner);
    ml->addWidget(m_stack, 1);

    m_split = new QSplitter(Qt::Horizontal);
    m_split->addWidget(sideCol);
    m_split->addWidget(main);
    m_split->setStretchFactor(1, 1);
    m_split->setSizes({250, 1100});
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_split);

    QSettings settings;
    m_split->restoreState(settings.value(QStringLiteral("layout/repoSplit")).toByteArray());
    m_changes->restoreLayout(settings);
    m_history->restoreLayout(settings);

    connect(m_sidebar, &Sidebar::entrySelected, this, &RepoView::onSidebarSelected);
    connect(m_sidebar, &Sidebar::entryActivated, this, &RepoView::onSidebarActivated);
    connect(m_sidebar, &Sidebar::entryContextMenu, this, &RepoView::onSidebarMenu);
    connect(m_continue, &QPushButton::clicked, this, &RepoView::continueOperation);
    connect(m_skip, &QPushButton::clicked, this, &RepoView::skipOperation);
    connect(m_abort, &QPushButton::clicked, this, &RepoView::abortOperation);

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(350);
    connect(m_refreshTimer, &QTimer::timeout, this, [this] { refresh(); });

    m_watcher = new QFileSystemWatcher(this);
    if (!m_repo.gitDir().isEmpty()) {
        QStringList paths{m_repo.gitDir()};
        const QString heads = QDir(m_repo.gitDir()).filePath(QStringLiteral("refs/heads"));
        if (QFileInfo(heads).isDir()) paths << heads;
        m_watcher->addPaths(paths);
    }
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, &RepoView::scheduleRefresh);

    refresh(true);
}

RepoView::~RepoView() {
    QSettings settings;
    settings.setValue(QStringLiteral("layout/repoSplit"), m_split->saveState());
    m_changes->saveLayout(settings);
    m_history->saveLayout(settings);
}

// ---------------------------------------------------------------- refresh

void RepoView::scheduleRefresh() {
    if (!m_busy) m_refreshTimer->start();
}

void RepoView::refresh(bool forceLog) {
    if (m_refreshing) {
        m_refreshPending = true;
        m_pendingForceLog |= forceLog;
        return;
    }
    m_refreshing = true;
    const Repository repo = m_repo;
    const LogOptions options = m_logOptions;
    const QByteArray previous = forceLog ? QByteArray() : m_snapshot.fingerprint;
    runAsync(
        this, [repo, options, previous] { return loadSnapshot(repo, options, previous); },
        [this](RepoSnapshot s) {
            applySnapshot(std::move(s));
            m_refreshing = false;
            if (m_refreshPending) {
                m_refreshPending = false;
                const bool force = m_pendingForceLog;
                m_pendingForceLog = false;
                refresh(force);
            }
        });
}

void RepoView::applySnapshot(RepoSnapshot s) {
    if (!s.error.isEmpty()) {
        emit statusMessage(s.error);
        return;
    }
    const bool logChanged = s.hasLog;
    if (!logChanged) {
        s.commits = std::move(m_snapshot.commits);
        s.graph = std::move(m_snapshot.graph);
    }
    m_snapshot = std::move(s);

    const HeadInfo& h = m_snapshot.head;
    QString branch = h.detached ? tr("detached at %1").arg(h.sha.left(8)) : h.branch;
    if (h.unborn) branch = tr("%1 (no commits yet)").arg(h.branch);
    m_repoLabel->setText(QStringLiteral("<span style='font-size:large; font-weight:600'>%1</span><br>"
                                        "<span style='opacity:0.7'>⎇ %2</span>")
                             .arg(m_repo.name().toHtmlEscaped(), branch.toHtmlEscaped()));
    m_repoLabel->setToolTip(m_repo.root());

    m_sidebar->setSnapshot(m_snapshot);
    m_changes->setStatus(m_snapshot.status, m_snapshot.head, m_snapshot.operation);
    if (logChanged) {
        QHash<QString, QVector<RefBadge>> badges;
        if (h.detached && !h.sha.isEmpty()) badges[h.sha].push_back({theme::Badge::Head, QStringLiteral("HEAD")});
        for (const Ref& r : m_snapshot.refs) {
            theme::Badge kind = theme::Badge::Local;
            if (r.kind == RefKind::LocalBranch && r.isHead)
                kind = theme::Badge::CurrentBranch;
            else if (r.kind == RefKind::RemoteBranch)
                kind = theme::Badge::Remote;
            else if (r.kind == RefKind::Tag)
                kind = theme::Badge::Tag;
            auto& list = badges[r.target];
            if (kind == theme::Badge::CurrentBranch)
                list.prepend({kind, r.name});
            else
                list.push_back({kind, r.name});
        }
        m_history->setCommits(m_snapshot.commits, m_snapshot.graph, badges, h.sha);
    }
    if (!m_loaded) {
        m_loaded = true;
        // Open on history when there is nothing to commit, like most daily use.
        if (m_snapshot.status.isClean() && !m_snapshot.commits.isEmpty()) m_sidebar->selectHistory();
    }
    if (!m_busy && m_snapshot.operation == Operation::None && !m_repo.gitDir().isEmpty()) {
        QDir plan(QDir(m_repo.gitDir()).filePath(QStringLiteral("aslgc-rebase")));
        if (plan.exists()) plan.removeRecursively();  // interactive rebase plan files
    }
    updateBanner();
    emit stateChanged();
}

void RepoView::updateBanner() {
    const Operation op = m_snapshot.operation;
    if (op == Operation::None) {
        m_banner->hide();
        return;
    }
    const int conflicts = m_snapshot.status.conflictCount();
    QString what;
    switch (op) {
        case Operation::Merge: what = tr("Merge in progress"); break;
        case Operation::Rebase:
            what = m_snapshot.rebaseProgress.isEmpty() ? tr("Rebase in progress")
                                                       : tr("Rebase in progress (step %1)").arg(m_snapshot.rebaseProgress);
            break;
        case Operation::CherryPick: what = tr("Cherry-pick in progress"); break;
        case Operation::Revert: what = tr("Revert in progress"); break;
        case Operation::ApplyMailbox: what = tr("Applying patches (git am)"); break;
        case Operation::None: break;
    }
    QString detail = conflicts > 0
                         ? tr("%n conflicted file(s) — resolve them in Local Changes, then continue.", nullptr, conflicts)
                         : tr("All conflicts resolved — you can continue.");
    if (op == Operation::Rebase && conflicts == 0 && !m_snapshot.rebaseEditSha.isEmpty()) {
        detail = tr("Stopped to edit %1 — amend it in Local Changes or add commits, then continue.")
                     .arg(m_snapshot.rebaseEditSha.left(8));
    }
    m_bannerLabel->setText(QStringLiteral("<b>%1.</b> %2").arg(what, detail));
    m_continue->setText(op == Operation::Merge ? tr("Commit Merge") : tr("Continue"));
    m_continue->setEnabled(conflicts == 0);
    m_skip->setVisible(op != Operation::Merge);
    m_banner->show();
}

// ---------------------------------------------------------------- command runners

void RepoView::setBusy(bool busy, const QString& what) {
    m_busy = busy;
    emit busyChanged(busy);
    emit statusMessage(busy ? what + QStringLiteral("…") : QString());
}

void RepoView::runCommands(const QString& title, const QVector<GitCommand>& commands, std::function<void(bool)> done,
                           const QString& failureHint) {
    if (m_busy) {
        emit statusMessage(tr("Another operation is still running."));
        return;
    }
    setBusy(true, title);
    const Git git = m_repo.git();
    runAsync(
        this,
        [git, commands] {
            GitResult last;
            last.exitCode = 0;
            for (const GitCommand& c : commands) {
                last = git.run(c.args, c.input);
                if (!last.ok()) {
                    qWarning().noquote() << "git" << c.args.join(QLatin1Char(' ')) << "failed with exit code" << last.exitCode
                                         << "in" << git.workDir() << ":" << last.message();
                    break;
                }
            }
            return last;
        },
        [this, title, done, failureHint](const GitResult& r) {
            setBusy(false);
            if (!r.ok()) showError(this, title, r.message(), failureHint);
            refresh();
            if (done) done(r.ok());
        });
}

void RepoView::runNetwork(const QString& title, const QStringList& args, std::function<void(bool)> done) {
    if (m_busy) {
        emit statusMessage(tr("Another operation is still running."));
        return;
    }
    setBusy(true, title);
    auto* dialog = new OperationDialog(window(), title, m_repo.git(), args);
    connect(dialog, &OperationDialog::completed, this, [this, done](bool ok) {
        setBusy(false);
        refresh();
        if (done) done(ok);
    });
    dialog->start();
}

// ---------------------------------------------------------------- helpers

const Ref* RepoView::findRef(RefKind kind, const QString& name) const {
    for (const Ref& r : m_snapshot.refs) {
        if (r.kind == kind && r.name == name) return &r;
    }
    return nullptr;
}

QStringList RepoView::remoteNames() const {
    QStringList names;
    for (const Remote& r : m_snapshot.remotes) names << r.name;
    return names;
}

QString RepoView::defaultRemote() const {
    const QStringList names = remoteNames();
    if (const Ref* head = findRef(RefKind::LocalBranch, currentBranch()); head && !head->upstream.isEmpty()) {
        const QString remote = head->upstream.section(QLatin1Char('/'), 0, 0);
        if (names.contains(remote)) return remote;
    }
    if (names.contains(QStringLiteral("origin"))) return QStringLiteral("origin");
    return names.value(0);
}

// ---------------------------------------------------------------- views

void RepoView::showChanges() {
    m_sidebar->selectChanges();
    m_changes->focusCommitMessage();
}

void RepoView::showHistory() { m_sidebar->selectHistory(); }

void RepoView::findCommit() {
    m_sidebar->selectHistory();
    m_history->focusSearch();
}

void RepoView::onSidebarSelected(const SidebarEntry& e) {
    switch (e.type) {
        case SidebarEntry::Changes: m_stack->setCurrentWidget(m_changes); break;
        case SidebarEntry::History: m_stack->setCurrentWidget(m_history); break;
        case SidebarEntry::LocalBranch:
        case SidebarEntry::RemoteBranch:
        case SidebarEntry::Tag:
        case SidebarEntry::Stash:
            m_stack->setCurrentWidget(m_history);
            if (!e.sha.isEmpty()) m_history->selectCommit(e.sha);
            break;
        default: break;
    }
}

void RepoView::onSidebarActivated(const SidebarEntry& e) {
    switch (e.type) {
        case SidebarEntry::LocalBranch:
            if (e.name != currentBranch()) checkout(e.name);
            break;
        case SidebarEntry::RemoteBranch:
            if (const Ref* r = findRef(RefKind::RemoteBranch, e.name)) checkoutRemote(*r);
            break;
        case SidebarEntry::Tag: checkoutDetached(e.name, tr("tag %1").arg(b(e.name))); break;
        case SidebarEntry::Stash:
            for (const Stash& s : m_snapshot.stashes) {
                if (s.sha == e.sha) applyStash(s, false);
            }
            break;
        default: break;
    }
}

void RepoView::onSidebarMenu(const SidebarEntry& e, const QPoint& pos) {
    QMenu menu(this);
    const QString current = currentBranch();
    auto copy = [&menu](const QString& label, const QString& text) {
        menu.addAction(label, [text] { QGuiApplication::clipboard()->setText(text); });
    };
    switch (e.type) {
        case SidebarEntry::LocalBranch: {
            const Ref* ref = findRef(RefKind::LocalBranch, e.name);
            if (!ref) return;
            const Ref r = *ref;
            const bool isCurrent = r.name == current;
            flow::Kind flowKind = flow::Kind::Feature;
            QString flowName;
            if (m_snapshot.gitFlow.classify(r.name, &flowKind, &flowName)) {
                menu.addAction(theme::actionIcon(QStringLiteral("flag-check")), flowFinishLabel(flowKind, flowName), this,
                               [this, flowKind, flowName] { gitFlowFinish(flowKind, flowName); });
                menu.addSeparator();
            }
            menu.addAction(theme::actionIcon(QStringLiteral("git-branch")), tr("Checkout"), this, [this, r] { checkout(r.name); })
                ->setEnabled(!isCurrent);
            if (!isCurrent && !current.isEmpty()) {
                menu.addAction(theme::actionIcon(QStringLiteral("git-merge")), tr("Merge '%1' into '%2'…").arg(r.name, current),
                               this, [this, r] { merge(r.name, b(r.name)); });
                menu.addAction(tr("Rebase '%1' onto '%2'…").arg(current, r.name), this, [this, r] { rebase(r.name, b(r.name)); });
                menu.addAction(tr("Interactively Rebase '%1' onto '%2'…").arg(current, r.name), this,
                               [this, r] { interactiveRebase(r.name, b(r.name)); });
            }
            if (!r.upstream.isEmpty() && r.behind > 0 && r.ahead == 0) {
                menu.addAction(tr("Fast-Forward to '%1'").arg(r.upstream), this, [this, r] { fastForward(r); });
            }
            menu.addSeparator();
            menu.addAction(tr("New Branch from '%1'…").arg(r.name), this, [this, r] { createBranch(r.name); });
            menu.addAction(tr("New Tag Here…"), this, [this, r] { createTag(r.target); });
            menu.addAction(tr("Rename…"), this, [this, r] { renameBranch(r); });
            menu.addAction(tr("Track Remote Branch…"), this, [this, r] { setUpstream(r); })
                ->setEnabled(!m_snapshot.remotes.isEmpty());
            menu.addAction(theme::actionIcon(QStringLiteral("arrow-bar-up")), tr("Push '%1'…").arg(r.name), this, &RepoView::push)
                ->setEnabled(isCurrent && !m_snapshot.remotes.isEmpty());
            {
                const QString upstreamRemote = r.upstream.section(QLatin1Char('/'), 0, 0);
                const QString upstreamBranch = r.upstream.section(QLatin1Char('/'), 1);
                const auto hosted = hostedRemote(r.upstream.isEmpty() ? defaultRemote() : upstreamRemote);
                if (hosted) {
                    menu.addSeparator();
                    auto* review = menu.addAction(theme::actionIcon(QStringLiteral("git-pull-request")),
                                                  tr("Create %1…").arg(reviewRequestName(hosted->provider)));
                    if (r.upstream.isEmpty() || r.upstreamGone) {
                        review->setEnabled(false);
                        review->setText(tr("Create %1… (push the branch first)").arg(reviewRequestName(hosted->provider)));
                    } else {
                        connect(review, &QAction::triggered, this, [this, upstreamRemote, upstreamBranch, ahead = r.ahead] {
                            createReviewRequest(upstreamRemote, upstreamBranch, ahead);
                        });
                        menu.addAction(theme::actionIcon(QStringLiteral("external-link")),
                                       tr("Open Branch in %1").arg(providerName(hosted->provider)), this,
                                       [this, h = *hosted, upstreamBranch] {
                                           openInBrowser(webBranchUrl(h.provider, h.repo, upstreamBranch));
                                       });
                    }
                }
            }
            menu.addSeparator();
            menu.addAction(theme::actionIcon(QStringLiteral("git-branch-deleted")), tr("Delete '%1'…").arg(r.name), this,
                           [this, r] { deleteBranch(r); })
                ->setEnabled(!isCurrent);
            menu.addSeparator();
            copy(tr("Copy Branch Name"), r.name);
            copy(tr("Copy Commit SHA"), r.target);
            break;
        }
        case SidebarEntry::RemoteBranch: {
            const Ref* ref = findRef(RefKind::RemoteBranch, e.name);
            if (!ref) return;
            const Ref r = *ref;
            menu.addAction(theme::actionIcon(QStringLiteral("git-branch")), tr("Checkout '%1'…").arg(r.name), this,
                           [this, r] { checkoutRemote(r); });
            if (!current.isEmpty()) {
                menu.addAction(theme::actionIcon(QStringLiteral("git-merge")), tr("Merge '%1' into '%2'…").arg(r.name, current),
                               this, [this, r] { merge(r.name, b(r.name)); });
                menu.addAction(tr("Rebase '%1' onto '%2'…").arg(current, r.name), this, [this, r] { rebase(r.name, b(r.name)); });
                menu.addAction(tr("Interactively Rebase '%1' onto '%2'…").arg(current, r.name), this,
                               [this, r] { interactiveRebase(r.name, b(r.name)); });
            }
            menu.addSeparator();
            menu.addAction(tr("New Branch from '%1'…").arg(r.name), this, [this, r] { createBranch(r.name); });
            menu.addAction(theme::actionIcon(QStringLiteral("git-branch-deleted")), tr("Delete '%1' on Remote…").arg(r.name),
                           this, [this, r] { deleteRemoteBranch(r); });
            if (const auto hosted = hostedRemote(r.remoteName())) {
                menu.addSeparator();
                menu.addAction(theme::actionIcon(QStringLiteral("git-pull-request")),
                               tr("Create %1…").arg(reviewRequestName(hosted->provider)), this,
                               [this, r] { createReviewRequest(r.remoteName(), r.remoteBranch(), 0); });
                menu.addAction(
                    theme::actionIcon(QStringLiteral("external-link")),
                    tr("Open Branch in %1").arg(providerName(hosted->provider)), this,
                    [this, h = *hosted, branch = r.remoteBranch()] { openInBrowser(webBranchUrl(h.provider, h.repo, branch)); });
            }
            menu.addSeparator();
            copy(tr("Copy Branch Name"), r.name);
            copy(tr("Copy Commit SHA"), r.target);
            break;
        }
        case SidebarEntry::Remote: {
            const QString name = e.name;
            QString url;
            for (const Remote& r : m_snapshot.remotes) {
                if (r.name == name) url = r.fetchUrl;
            }
            menu.addAction(theme::actionIcon(QStringLiteral("cloud-download")), tr("Fetch '%1'").arg(name), this, [this, name] {
                runNetwork(tr("Fetch %1").arg(name),
                           {QStringLiteral("fetch"), QStringLiteral("--prune"), QStringLiteral("--progress"), name});
            });
            menu.addAction(tr("Edit URL…"), this, [this, name] { editRemote(name); });
            menu.addAction(theme::actionIcon(QStringLiteral("trash")), tr("Remove '%1'…").arg(name), this,
                           [this, name] { removeRemote(name); });
            if (const auto hosted = hostedRemote(name)) {
                menu.addAction(theme::actionIcon(QStringLiteral("external-link")),
                               tr("Open in %1").arg(providerName(hosted->provider)), this,
                               [this, h = *hosted] { openInBrowser(webRepoUrl(h.provider, h.repo)); });
            }
            menu.addSeparator();
            copy(tr("Copy URL"), url);
            menu.addSeparator();
            menu.addAction(tr("Add Remote…"), this, &RepoView::addRemote);
            break;
        }
        case SidebarEntry::Tag: {
            const QString tag = e.name;
            const QString sha = e.sha;
            menu.addAction(tr("Checkout Tag (Detached)…"), this,
                           [this, tag] { checkoutDetached(tag, tr("tag %1").arg(b(tag))); });
            menu.addAction(tr("New Branch from Tag…"), this, [this, tag] { createBranch(tag); });
            if (!current.isEmpty())
                menu.addAction(tr("Merge into '%1'…").arg(current), this, [this, tag] { merge(tag, b(tag)); });
            menu.addSeparator();
            menu.addAction(theme::actionIcon(QStringLiteral("arrow-bar-up")), tr("Push Tag…"), this,
                           [this, tag] { pushTag(tag); })
                ->setEnabled(!m_snapshot.remotes.isEmpty());
            menu.addAction(theme::actionIcon(QStringLiteral("trash")), tr("Delete Tag…"), this, [this, tag] { deleteTag(tag); });
            menu.addSeparator();
            copy(tr("Copy Tag Name"), tag);
            copy(tr("Copy Commit SHA"), sha);
            break;
        }
        case SidebarEntry::Stash: {
            Stash stash;
            for (const Stash& s : m_snapshot.stashes) {
                if (s.sha == e.sha) stash = s;
            }
            menu.addAction(theme::actionIcon(QStringLiteral("package-export")), tr("Apply"), this,
                           [this, stash] { applyStash(stash, false); });
            menu.addAction(tr("Pop (Apply && Drop)"), this, [this, stash] { applyStash(stash, true); });
            menu.addAction(tr("Save as Patch…"), this, [this, stash] { savePatch(stash.sha); });
            menu.addSeparator();
            menu.addAction(theme::actionIcon(QStringLiteral("trash")), tr("Drop…"), this, [this, stash] { dropStash(stash); });
            break;
        }
        case SidebarEntry::Section:
            menu.addAction(tr("New Branch…"), this, [this] { createBranch(); });
            menu.addAction(tr("Add Remote…"), this, &RepoView::addRemote);
            menu.addAction(tr("Fetch All"), this, &RepoView::fetch);
            menu.addAction(tr("Stash Changes…"), this, &RepoView::stash);
            break;
        default: return;
    }
    if (!menu.isEmpty()) menu.exec(pos);
}

void RepoView::showCommitMenu(const QString& sha, const QPoint& pos, const std::function<void(QMenu&)>& extra) {
    Commit commit;
    for (const Commit& c : m_snapshot.commits) {
        if (c.sha == sha) commit = c;
    }
    const QString current = currentBranch();
    const QString shortSha = sha.left(8);
    QMenu menu(this);
    if (extra) extra(menu);
    menu.addAction(tr("Checkout Commit (Detached)…"), this, [this, sha, shortSha] { checkoutDetached(sha, b(shortSha)); });
    menu.addAction(theme::actionIcon(QStringLiteral("git-branch")), tr("New Branch Here…"), this,
                   [this, sha] { createBranch(sha); });
    menu.addAction(tr("New Tag Here…"), this, [this, sha] { createTag(sha); });
    menu.addSeparator();
    if (!current.isEmpty() && sha != m_snapshot.head.sha) {
        menu.addAction(theme::actionIcon(QStringLiteral("git-merge")), tr("Merge into '%1'…").arg(current), this,
                       [this, sha, shortSha] { merge(sha, b(shortSha)); });
        menu.addAction(tr("Rebase '%1' onto Here…").arg(current), this, [this, sha, shortSha] { rebase(sha, b(shortSha)); });
    }
    menu.addAction(tr("Cherry-Pick…"), this, [this, sha] { cherryPick(sha); })->setEnabled(sha != m_snapshot.head.sha);
    menu.addAction(tr("Revert Commit…"), this, [this, sha] { revert(sha); });
    menu.addAction(tr("Reset '%1' to Here…").arg(current.isEmpty() ? QStringLiteral("HEAD") : current), this,
                   [this, sha] { reset(sha); });
    menu.addSeparator();
    menu.addAction(theme::actionIcon(QStringLiteral("git-commit")), tr("Edit Commit Message…"), this,
                   [this, sha] { rewordCommit(sha); });
    menu.addAction(tr("Interactive Rebase from Here…"), this, [this, sha, shortSha] { interactiveRebase(sha, b(shortSha)); })
        ->setEnabled(sha != m_snapshot.head.sha);
    menu.addSeparator();
    if (const auto hosted = hostedRemote(defaultRemote())) {
        menu.addAction(theme::actionIcon(QStringLiteral("external-link")),
                       tr("Open Commit in %1").arg(providerName(hosted->provider)), this,
                       [this, h = *hosted, sha] { openInBrowser(webCommitUrl(h.provider, h.repo, sha)); });
    }
    menu.addAction(tr("Save as Patch…"), this, [this, sha] { savePatch(sha); });
    menu.addAction(theme::actionIcon(QStringLiteral("copy")), tr("Copy SHA"), this,
                   [sha] { QGuiApplication::clipboard()->setText(sha); });
    menu.addAction(tr("Copy Short SHA"), this, [shortSha] { QGuiApplication::clipboard()->setText(shortSha); });
    if (!commit.sha.isEmpty()) {
        const QString info = QStringLiteral("%1 %2 (%3, %4)")
                                 .arg(shortSha, commit.subject, commit.authorName,
                                      QDateTime::fromSecsSinceEpoch(commit.authorTime).toString(Qt::ISODate));
        menu.addAction(tr("Copy Commit Info"), this, [info] { QGuiApplication::clipboard()->setText(info); });
    }
    menu.exec(pos);
}

// ---------------------------------------------------------------- git-flow

void RepoView::populateGitFlowMenu(QMenu* menu) {
    const flow::Config& c = m_snapshot.gitFlow;
    if (!c.isInitialized()) {
        menu->addAction(theme::actionIcon(QStringLiteral("arrows-split-2")), tr("Set Up Git Flow…"), this,
                        &RepoView::gitFlowSetup)
            ->setEnabled(!m_snapshot.head.unborn);
        return;
    }
    for (flow::Kind kind : flow::kKinds) {
        menu->addAction(theme::actionIcon(QStringLiteral("player-play")), flowStartTitle(kind) + QStringLiteral("…"), this,
                        [this, kind] { gitFlowStart(kind); });
    }
    // Open flow branches, the checked out one first.
    QVector<std::tuple<bool, flow::Kind, QString>> open;
    for (const Ref& r : m_snapshot.refs) {
        flow::Kind kind = flow::Kind::Feature;
        QString name;
        if (r.kind == RefKind::LocalBranch && c.classify(r.name, &kind, &name)) open.push_back({!r.isHead, kind, name});
    }
    std::stable_sort(open.begin(), open.end(), [](const auto& x, const auto& y) { return std::get<0>(x) < std::get<0>(y); });
    if (!open.isEmpty()) menu->addSeparator();
    for (const auto& [notCurrent, kind, name] : open) {
        menu->addAction(theme::actionIcon(QStringLiteral("flag-check")), flowFinishLabel(kind, name), this,
                        [this, kind = kind, name = name] { gitFlowFinish(kind, name); });
    }
    menu->addSeparator();
    menu->addAction(theme::actionIcon(QStringLiteral("settings")), tr("Git Flow Settings…"), this, &RepoView::gitFlowSetup);
    menu->addAction(theme::actionIcon(QStringLiteral("trash")), tr("Remove Git Flow Settings…"), this, &RepoView::gitFlowRemove);
}

void RepoView::gitFlowSetup() {
    if (m_snapshot.head.unborn) {
        showError(this, tr("Git Flow"), tr("Make a first commit before setting up git-flow."));
        return;
    }
    const flow::Config current = m_snapshot.gitFlow;
    const bool editing = current.isInitialized();
    QStringList locals;
    for (const Ref& r : m_snapshot.refs) {
        if (r.kind == RefKind::LocalBranch) locals << r.name;
    }
    QString production = current.production;
    if (production.isEmpty()) {
        for (const QString& candidate : {QStringLiteral("main"), QStringLiteral("master"), currentBranch()}) {
            if (production.isEmpty() && locals.contains(candidate)) production = candidate;
        }
    }
    FormDialog dlg(this, editing ? tr("Git Flow Settings") : tr("Set Up Git Flow"), editing ? tr("Save") : tr("Set Up"));
    if (!editing) {
        dlg.addNote(
            tr("git-flow keeps released code on a production branch and brings work together on a develop "
               "branch. Features, bugfixes, releases and hotfixes each get their own branch, named with the "
               "prefixes below."));
    }
    dlg.addCombo(QStringLiteral("production"), tr("Production branch:"), locals, production, true);
    dlg.addLine(QStringLiteral("develop"), tr("Develop branch:"),
                current.develop.isEmpty() ? QStringLiteral("develop") : current.develop);
    dlg.addLine(QStringLiteral("feature"), tr("Feature prefix:"), current.featurePrefix);
    dlg.addLine(QStringLiteral("bugfix"), tr("Bugfix prefix:"), current.bugfixPrefix);
    dlg.addLine(QStringLiteral("release"), tr("Release prefix:"), current.releasePrefix);
    dlg.addLine(QStringLiteral("hotfix"), tr("Hotfix prefix:"), current.hotfixPrefix);
    dlg.addLine(QStringLiteral("tag"), tr("Version tag prefix:"), current.versionTagPrefix, tr("none, or e.g. v"));
    dlg.setValidator([&dlg] {
        for (const char* key : {"production", "develop", "feature", "bugfix", "release", "hotfix"}) {
            if (dlg.text(QLatin1String(key)).isEmpty()) return false;
        }
        return true;
    });
    if (dlg.exec() != QDialog::Accepted) return;

    flow::Config c = current;
    c.production = dlg.text(QStringLiteral("production"));
    c.develop = dlg.text(QStringLiteral("develop"));
    c.featurePrefix = dlg.text(QStringLiteral("feature"));
    c.bugfixPrefix = dlg.text(QStringLiteral("bugfix"));
    c.releasePrefix = dlg.text(QStringLiteral("release"));
    c.hotfixPrefix = dlg.text(QStringLiteral("hotfix"));
    c.versionTagPrefix = dlg.text(QStringLiteral("tag"));
    const Repository repo = m_repo;
    const QString remote = defaultRemote();
    const QString title = editing ? tr("Git Flow Settings") : tr("Set Up Git Flow");
    runAsync(
        this, [repo, c, remote, editing] { return flow::planInit(repo, c, remote, !editing); },
        [this, title](const flow::Plan& plan) {
            if (!plan.ok())
                showError(this, title, plan.error);
            else
                runCommands(title, plan.commands);
        });
}

void RepoView::gitFlowRemove() {
    if (!confirm(this, tr("Remove Git Flow Settings"),
                 tr("Remove the git-flow settings from this repository? Branches and tags stay as they are."), tr("Remove")))
        return;
    const flow::Plan plan = flow::planRemove(m_snapshot.gitFlow);
    if (!plan.ok())
        showError(this, tr("Remove Git Flow Settings"), plan.error);
    else
        runCommands(tr("Remove Git Flow Settings"), plan.commands);
}

void RepoView::gitFlowStart(flow::Kind kind) {
    const flow::Config c = m_snapshot.gitFlow;
    if (!c.isInitialized()) {
        gitFlowSetup();
        return;
    }
    const bool versioned = flow::Config::isVersioned(kind);
    QStringList branches;
    QStringList tags;
    for (const Ref& r : m_snapshot.refs) {
        if (r.kind == RefKind::Tag)
            tags << r.name;
        else
            branches << r.name;
    }
    const QString prefix = c.prefix(kind);
    const QString title = flowStartTitle(kind);
    FormDialog dlg(this, title, tr("Start"));
    QLineEdit* name = dlg.addLine(QStringLiteral("name"), versioned ? tr("Version:") : tr("Name:"),
                                  versioned ? flow::suggestVersion(tags, c.versionTagPrefix, kind) : QString(),
                                  versioned ? tr("e.g. 1.4.0") : tr("e.g. login-form"));
    auto* preview = new QLabel;
    preview->setTextFormat(Qt::RichText);
    preview->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    dlg.addWidgetRow(tr("Branch:"), preview);
    dlg.addCombo(QStringLiteral("base"), tr("Based on:"), branches, kind == flow::Kind::Hotfix ? c.production : c.develop, true);
    auto updatePreview = [=] {
        const QString n = name->text().trimmed();
        QString text = b(prefix + (n.isEmpty() ? QStringLiteral("…") : n));
        if (versioned && !n.isEmpty())
            text += QStringLiteral(" · ") + tr("tagged %1 when finished").arg(b(c.versionTagPrefix + n));
        preview->setText(text);
    };
    updatePreview();
    connect(name, &QLineEdit::textChanged, preview, updatePreview);
    dlg.setValidator([name] {
        const QString n = name->text().trimmed();
        return !n.isEmpty() && !n.contains(QLatin1Char(' '));
    });
    name->selectAll();
    if (dlg.exec() != QDialog::Accepted) return;

    const Repository repo = m_repo;
    const QString n = dlg.text(QStringLiteral("name"));
    const QString base = dlg.text(QStringLiteral("base"));
    runAsync(
        this, [repo, c, kind, n, base] { return flow::planStart(repo, c, kind, n, base); },
        [this, title](const flow::Plan& plan) {
            if (!plan.ok())
                showError(this, title, plan.error);
            else
                runCommands(title, plan.commands);
        });
}

void RepoView::gitFlowFinish(flow::Kind kind, const QString& name) {
    const flow::Config c = m_snapshot.gitFlow;
    if (!c.isInitialized()) return;
    const bool versioned = flow::Config::isVersioned(kind);
    const QString branch = c.prefix(kind) + name;
    const QString tag = c.versionTagPrefix + name;
    const QString title = flowFinishTitle(kind);

    FormDialog dlg(this, title, tr("Finish"));
    if (!versioned) {
        dlg.addNote(tr("Merges %1 into %2, then deletes it.").arg(b(branch), b(c.develop)));
        dlg.addCheck(QStringLiteral("squash"), tr("Squash into a single commit"));
        dlg.addCheck(QStringLiteral("noff"), tr("Always create a merge commit"));
    } else {
        QString back = c.develop;
        if (kind == flow::Kind::Hotfix) {
            for (const Ref& r : m_snapshot.refs) {
                if (r.kind == RefKind::LocalBranch && r.name.startsWith(c.releasePrefix) &&
                    r.name.size() > c.releasePrefix.size())
                    back = r.name;
            }
        }
        dlg.addNote(tr("Merges %1 into %2, tags it %3, merges it into %4, then deletes it.")
                        .arg(b(branch), b(c.production), b(tag), b(back)));
        QCheckBox* tagBox = dlg.addCheck(QStringLiteral("tag"), tr("Create the tag %1").arg(tag), true);
        QLineEdit* message = dlg.addLine(QStringLiteral("message"), tr("Tag message:"), tag);
        connect(tagBox, &QCheckBox::toggled, message, &QWidget::setEnabled);
    }
    dlg.addCheck(QStringLiteral("keep"), tr("Keep the branch"));
    const QString remote = defaultRemote();
    if (!remote.isEmpty()) dlg.addCheck(QStringLiteral("push"), tr("Push the result to %1").arg(remote));
    if (dlg.exec() != QDialog::Accepted) return;
    if (!m_changes->ensureIdentity()) return;

    flow::FinishOptions o;
    o.squash = !versioned && dlg.checked(QStringLiteral("squash"));
    o.noFastForward = !versioned && dlg.checked(QStringLiteral("noff"));
    o.tag = versioned && dlg.checked(QStringLiteral("tag"));
    o.tagMessage = versioned ? dlg.text(QStringLiteral("message")) : QString();
    o.keepBranch = dlg.checked(QStringLiteral("keep"));
    o.remote = !remote.isEmpty() && dlg.checked(QStringLiteral("push")) ? remote : QString();
    const Repository repo = m_repo;
    runAsync(
        this, [repo, c, kind, name, o] { return flow::planFinish(repo, c, kind, name, o); },
        [this, title](const flow::Plan& plan) {
            if (!plan.ok()) {
                showError(this, title, plan.error);
                return;
            }
            const QStringList push = plan.pushArgs;
            runCommands(
                title, plan.commands,
                [this, push](bool ok) {
                    if (ok && !push.isEmpty()) runNetwork(tr("Push"), push);
                },
                tr("If git stopped at a merge conflict, resolve it in Local Changes and commit the merge, then "
                   "choose Finish again: the steps that are already done are skipped."));
        });
}

// ---------------------------------------------------------------- file history and blame

FileHistoryWindow* RepoView::fileHistoryWindow(const QString& path, bool folder) {
    for (FileHistoryWindow* w : findChildren<FileHistoryWindow*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (w->path() == path && w->isFolder() == folder) return w;
    }
    return new FileHistoryWindow(this, path, folder);
}

void RepoView::showFileHistory(const QString& path, bool folder) {
    QString p = path;
    while (p.endsWith(QLatin1Char('/'))) p.chop(1);
    FileHistoryWindow* w = fileHistoryWindow(p, folder);
    w->showHistoryPage();
    w->show();
    w->raise();
    w->activateWindow();
}

void RepoView::showBlame(const QString& path, const QString& rev) {
    FileHistoryWindow* w = fileHistoryWindow(path, false);
    w->showBlame(rev, path);
    w->show();
    w->raise();
    w->activateWindow();
}

void RepoView::showLineHistory(const QString& path, int first, int last, const QString& rev) {
    FileHistoryWindow* w = fileHistoryWindow(path, false);
    w->showLineHistory(path, first, last, rev);
    w->show();
    w->raise();
    w->activateWindow();
}

void RepoView::addFileHistoryActions(QMenu& menu, const FileChange& file, const QString& sha, const QStringList& parents) {
    const QString path = file.path;
    menu.addAction(theme::actionIcon(QStringLiteral("history")), tr("File History"), this,
                   [this, path] { showFileHistory(path); })
        ->setEnabled(!file.isUntracked());
    // A deleted file is blamed as it was just before the deletion.
    QString rev = sha;
    if (file.status == 'D') rev = sha.isEmpty() ? QStringLiteral("HEAD") : parents.value(0);
    const bool canBlame = !file.isUntracked() && !(file.status == 'D' && rev.isEmpty());
    QString label = sha.isEmpty() ? tr("Blame") : tr("Blame at This Commit");
    if (file.status == 'D') label = sha.isEmpty() ? tr("Blame Last Committed Version") : tr("Blame Before This Commit");
    menu.addAction(theme::actionIcon(QStringLiteral("list-details")), label, this, [this, path, rev] { showBlame(path, rev); })
        ->setEnabled(canBlame);
    const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
    if (slash > 0) {
        const QString folder = path.left(slash);
        menu.addAction(tr("History of Folder '%1'").arg(folder), this, [this, folder] { showFileHistory(folder, true); });
    }
}

void RepoView::chooseFileHistory() {
    FormDialog dlg(this, tr("File History"), tr("Show History"));
    dlg.addNote(
        tr("A file or folder, relative to the repository root. End a folder with “/”; leave the field "
           "empty for the whole repository."));
    QLineEdit* edit = dlg.addLine(QStringLiteral("path"), tr("Path"), {}, tr("e.g. src/main.cpp or src/"));
    auto* model = new QStringListModel(&dlg);
    auto* completer = new QCompleter(model, &dlg);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->setMaxVisibleItems(15);
    edit->setCompleter(completer);
    const Repository repo = m_repo;
    runAsync(
        &dlg,
        [repo] {
            const QStringList files = repo.trackedFiles();
            QSet<QString> folders;
            for (const QString& f : files) {
                for (qsizetype s = f.lastIndexOf(QLatin1Char('/')); s > 0; s = f.lastIndexOf(QLatin1Char('/'), s - 1)) {
                    if (folders.contains(f.left(s))) break;
                    folders.insert(f.left(s));
                }
            }
            QStringList paths = files;
            for (const QString& d : folders) paths << d + QLatin1Char('/');
            paths.sort(Qt::CaseInsensitive);
            return paths;
        },
        [model](const QStringList& paths) { model->setStringList(paths); });
    if (dlg.exec() != QDialog::Accepted) return;

    QString path = dlg.text(QStringLiteral("path")).trimmed();
    const QString root = m_repo.root();
    if (path == root)
        path.clear();
    else if (path.startsWith(root + QLatin1Char('/')))
        path = path.mid(root.size() + 1);
    while (path.startsWith(QLatin1String("./"))) path = path.mid(2);
    while (path.startsWith(QLatin1Char('/'))) path = path.mid(1);
    if (path == QLatin1String(".")) path.clear();
    const bool folder = path.isEmpty() || path.endsWith(QLatin1Char('/')) || QFileInfo(QDir(root).filePath(path)).isDir();
    showFileHistory(path, folder);
}

void RepoView::revealCommit(const QString& sha) {
    m_sidebar->selectHistory();
    m_history->selectCommit(sha);
    emit activateRequested();
}

QVector<RefBadge> RepoView::badgesFor(const QString& sha) const { return m_history->badgesFor(sha); }

// ---------------------------------------------------------------- toolbar actions

void RepoView::fetch() {
    if (m_snapshot.remotes.isEmpty()) {
        showError(this, tr("Fetch"), tr("This repository has no remotes. Add one first."));
        return;
    }
    runNetwork(tr("Fetch"),
               {QStringLiteral("fetch"), QStringLiteral("--all"), QStringLiteral("--prune"), QStringLiteral("--progress")});
}

void RepoView::pull() {
    const QString branch = currentBranch();
    if (branch.isEmpty()) {
        showError(this, tr("Pull"), tr("You are not on a branch (detached HEAD). Check out a branch first."));
        return;
    }
    if (m_snapshot.remotes.isEmpty()) {
        showError(this, tr("Pull"), tr("This repository has no remotes. Add one first."));
        return;
    }
    const Ref* head = findRef(RefKind::LocalBranch, branch);
    const QString upstream = head ? head->upstream : QString();
    const QString remote = upstream.isEmpty() ? defaultRemote() : upstream.section(QLatin1Char('/'), 0, 0);
    const QString remoteBranch = upstream.isEmpty() ? branch : upstream.section(QLatin1Char('/'), 1);

    FormDialog dlg(this, tr("Pull"), tr("Pull"));
    dlg.addNote(tr("Pull changes into %1.").arg(b(branch)));
    QComboBox* remoteBox = dlg.addCombo(QStringLiteral("remote"), tr("Remote:"), remoteNames(), remote);
    QComboBox* branchBox = dlg.addCombo(QStringLiteral("branch"), tr("Branch:"), {}, {}, true);
    auto fillBranches = [this, branchBox, remoteBranch](const QString& r) {
        branchBox->clear();
        for (const Ref& ref : m_snapshot.refs) {
            if (ref.kind == RefKind::RemoteBranch && ref.remoteName() == r) branchBox->addItem(ref.remoteBranch());
        }
        const int i = branchBox->findText(remoteBranch);
        if (i >= 0)
            branchBox->setCurrentIndex(i);
        else
            branchBox->setEditText(remoteBranch);
    };
    fillBranches(remote);
    connect(remoteBox, &QComboBox::currentTextChanged, &dlg, fillBranches);
    const QString pullRebase = m_repo.configValue(QStringLiteral("pull.rebase"));
    dlg.addCheck(QStringLiteral("rebase"), tr("Rebase instead of merge"), pullRebase == QLatin1String("true"));
    dlg.addCheck(QStringLiteral("autostash"), tr("Stash and reapply local changes"), !m_snapshot.status.isClean());
    dlg.setValidator(
        [&dlg] { return !dlg.text(QStringLiteral("remote")).isEmpty() && !dlg.text(QStringLiteral("branch")).isEmpty(); });
    if (dlg.exec() != QDialog::Accepted) return;
    QStringList args{QStringLiteral("pull"), QStringLiteral("--progress"),
                     dlg.checked(QStringLiteral("rebase")) ? QStringLiteral("--rebase") : QStringLiteral("--no-rebase")};
    if (dlg.checked(QStringLiteral("autostash"))) args << QStringLiteral("--autostash");
    args << dlg.text(QStringLiteral("remote")) << dlg.text(QStringLiteral("branch"));
    runNetwork(tr("Pull"), args);
}

void RepoView::push() {
    const QString branch = currentBranch();
    if (branch.isEmpty() || m_snapshot.head.unborn) {
        showError(this, tr("Push"), tr("Check out a branch with at least one commit to push."));
        return;
    }
    if (m_snapshot.remotes.isEmpty()) {
        showError(this, tr("Push"), tr("This repository has no remotes. Add one first."));
        return;
    }
    const Ref* head = findRef(RefKind::LocalBranch, branch);
    const QString upstream = head ? head->upstream : QString();
    const QString remote = upstream.isEmpty() ? defaultRemote() : upstream.section(QLatin1Char('/'), 0, 0);
    const QString target = upstream.isEmpty() ? branch : upstream.section(QLatin1Char('/'), 1);

    FormDialog dlg(this, tr("Push"), tr("Push"));
    QString note = tr("Push %1").arg(b(branch));
    if (head && head->ahead) note += tr(" — %n commit(s) ahead", nullptr, head->ahead);
    if (head && head->behind) note += tr(", %1 behind").arg(head->behind);
    dlg.addNote(note + QLatin1Char('.'));
    dlg.addCombo(QStringLiteral("remote"), tr("To remote:"), remoteNames(), remote);
    dlg.addLine(QStringLiteral("target"), tr("Remote branch:"), target);
    dlg.addCheck(QStringLiteral("upstream"), tr("Set as upstream (track this remote branch)"), upstream.isEmpty());
    dlg.addCheck(QStringLiteral("tags"), tr("Also push all tags"), false);
    dlg.addCheck(QStringLiteral("force"), tr("Force push (with lease)"), false);
    dlg.setValidator(
        [&dlg] { return !dlg.text(QStringLiteral("remote")).isEmpty() && !dlg.text(QStringLiteral("target")).isEmpty(); });
    if (dlg.exec() != QDialog::Accepted) return;
    if (dlg.checked(QStringLiteral("force")) &&
        !confirm(this, tr("Force Push"), tr("Force pushing can overwrite commits on the remote. Continue?"), tr("Force Push"),
                 true))
        return;
    QStringList args{QStringLiteral("push"), QStringLiteral("--progress")};
    if (dlg.checked(QStringLiteral("upstream"))) args << QStringLiteral("--set-upstream");
    if (dlg.checked(QStringLiteral("tags"))) args << QStringLiteral("--tags");
    if (dlg.checked(QStringLiteral("force"))) args << QStringLiteral("--force-with-lease");
    args << dlg.text(QStringLiteral("remote"))
         << QStringLiteral("refs/heads/%1:refs/heads/%2").arg(branch, dlg.text(QStringLiteral("target")));
    runNetwork(tr("Push"), args);
}

void RepoView::stash() {
    if (m_snapshot.status.isClean()) {
        emit statusMessage(tr("Nothing to stash."));
        return;
    }
    FormDialog dlg(this, tr("Stash Changes"), tr("Stash"));
    dlg.addLine(QStringLiteral("message"), tr("Message:"), {}, tr("Optional description"));
    bool hasUntracked = false;
    for (const FileChange& f : m_snapshot.status.unstaged) hasUntracked |= f.isUntracked();
    dlg.addCheck(QStringLiteral("untracked"), tr("Include untracked files"), hasUntracked);
    dlg.addCheck(QStringLiteral("keep"), tr("Keep staged changes in the working tree"), false);
    if (dlg.exec() != QDialog::Accepted) return;
    QStringList args{QStringLiteral("stash"), QStringLiteral("push")};
    if (dlg.checked(QStringLiteral("untracked"))) args << QStringLiteral("--include-untracked");
    if (dlg.checked(QStringLiteral("keep"))) args << QStringLiteral("--keep-index");
    if (!dlg.text(QStringLiteral("message")).isEmpty()) args << QStringLiteral("-m") << dlg.text(QStringLiteral("message"));
    runCommands(tr("Stash"), {{args, {}}});
}

void RepoView::popStash() {
    if (m_snapshot.stashes.isEmpty()) {
        emit statusMessage(tr("There are no stashes."));
        return;
    }
    applyStash(m_snapshot.stashes.first(), true);
}

void RepoView::createBranch(const QString& startPoint) {
    if (m_snapshot.head.unborn && startPoint.isEmpty()) {
        showError(this, tr("New Branch"), tr("Create a first commit before creating branches."));
        return;
    }
    const QString from =
        startPoint.isEmpty() ? (currentBranch().isEmpty() ? QStringLiteral("HEAD") : currentBranch()) : startPoint;
    FormDialog dlg(this, tr("New Branch"), tr("Create Branch"));
    dlg.addNote(tr("Create a branch starting at %1.").arg(b(from.size() == 40 ? from.left(8) : from)));
    QLineEdit* name = dlg.addLine(QStringLiteral("name"), tr("Branch name:"), {}, tr("feature/my-change"));
    dlg.addCheck(QStringLiteral("checkout"), tr("Check out after creating"), true);
    dlg.setValidator([name] {
        const QString n = name->text().trimmed();
        return !n.isEmpty() && !n.contains(QLatin1Char(' ')) && !n.startsWith(QLatin1Char('-'));
    });
    if (dlg.exec() != QDialog::Accepted) return;
    const QString n = dlg.text(QStringLiteral("name"));
    if (dlg.checked(QStringLiteral("checkout")))
        runCommands(tr("New Branch"), {{{QStringLiteral("checkout"), QStringLiteral("-b"), n, from}, {}}});
    else
        runCommands(tr("New Branch"), {{{QStringLiteral("branch"), n, from}, {}}});
}

void RepoView::addRemote() {
    FormDialog dlg(this, tr("Add Remote"), tr("Add"));
    dlg.addLine(QStringLiteral("name"), tr("Name:"), m_snapshot.remotes.isEmpty() ? QStringLiteral("origin") : QString());
    dlg.addLine(QStringLiteral("url"), tr("URL:"), {}, QStringLiteral("git@example.com:team/project.git"));
    dlg.addCheck(QStringLiteral("fetch"), tr("Fetch after adding"), true);
    dlg.setValidator(
        [&dlg] { return !dlg.text(QStringLiteral("name")).isEmpty() && !dlg.text(QStringLiteral("url")).isEmpty(); });
    if (dlg.exec() != QDialog::Accepted) return;
    const QString name = dlg.text(QStringLiteral("name"));
    const bool fetchAfter = dlg.checked(QStringLiteral("fetch"));
    runCommands(tr("Add Remote"),
                {{{QStringLiteral("remote"), QStringLiteral("add"), name, dlg.text(QStringLiteral("url"))}, {}}},
                [this, name, fetchAfter](bool ok) {
                    if (ok && fetchAfter)
                        runNetwork(tr("Fetch %1").arg(name), {QStringLiteral("fetch"), QStringLiteral("--progress"), name});
                });
}

void RepoView::openTerminal() {
    const QString dir = m_repo.root();
    QList<QPair<QString, QStringList>> candidates;
    const QString preferred = qEnvironmentVariable("TERMINAL");
    if (!preferred.isEmpty()) candidates.append({preferred, {}});
    candidates += {{QStringLiteral("konsole"), {QStringLiteral("--workdir"), dir}},
                   {QStringLiteral("gnome-terminal"), {QStringLiteral("--working-directory=") + dir}},
                   {QStringLiteral("kgx"), {QStringLiteral("--working-directory=") + dir}},
                   {QStringLiteral("kitty"), {QStringLiteral("--directory"), dir}},
                   {QStringLiteral("alacritty"), {QStringLiteral("--working-directory"), dir}},
                   {QStringLiteral("wezterm"), {QStringLiteral("start"), QStringLiteral("--cwd"), dir}},
                   {QStringLiteral("foot"), {QStringLiteral("--working-directory=") + dir}},
                   {QStringLiteral("ghostty"), {QStringLiteral("--working-directory=") + dir}},
                   {QStringLiteral("xfce4-terminal"), {QStringLiteral("--working-directory=") + dir}},
                   {QStringLiteral("tilix"), {QStringLiteral("--working-directory=") + dir}},
                   {QStringLiteral("xterm"), {}}};
    for (const auto& [program, args] : candidates) {
        const QString path = QStandardPaths::findExecutable(program);
        if (!path.isEmpty() && QProcess::startDetached(path, args, dir)) return;
    }
    showError(this, tr("Open Terminal"), tr("No terminal emulator found. Set the TERMINAL environment variable."));
}

void RepoView::openFolder() { QDesktopServices::openUrl(QUrl::fromLocalFile(m_repo.root())); }

// ---------------------------------------------------------------- branch & commit operations

void RepoView::checkout(const QString& branch) {
    runCommands(tr("Checkout"), {{{QStringLiteral("checkout"), branch, QStringLiteral("--")}, {}}});
}

void RepoView::checkoutRemote(const Ref& remoteRef) {
    const QString localName = remoteRef.remoteBranch();
    if (const Ref* local = findRef(RefKind::LocalBranch, localName)) {
        if (local->upstream == remoteRef.name) {
            if (local->name != currentBranch()) checkout(local->name);
            return;
        }
    }
    FormDialog dlg(this, tr("Checkout Remote Branch"), tr("Checkout"));
    dlg.addNote(tr("Create a local branch from %1.").arg(b(remoteRef.name)));
    QLineEdit* name = dlg.addLine(QStringLiteral("name"), tr("Local branch:"), localName);
    dlg.addCheck(QStringLiteral("track"), tr("Track the remote branch"), true);
    dlg.setValidator([this, name] {
        const QString n = name->text().trimmed();
        return !n.isEmpty() && !findRef(RefKind::LocalBranch, n);
    });
    if (dlg.exec() != QDialog::Accepted) return;
    QStringList args{QStringLiteral("checkout")};
    args << (dlg.checked(QStringLiteral("track")) ? QStringLiteral("--track") : QStringLiteral("--no-track"));
    args << QStringLiteral("-b") << dlg.text(QStringLiteral("name")) << remoteRef.name;
    runCommands(tr("Checkout"), {{args, {}}});
}

void RepoView::checkoutDetached(const QString& rev, const QString& label) {
    if (!confirm(
            this, tr("Checkout"),
            tr("Check out %1?<br><br>You will be in <i>detached HEAD</i> state; create a branch to keep new commits.").arg(label),
            tr("Checkout")))
        return;
    runCommands(tr("Checkout"), {{{QStringLiteral("checkout"), QStringLiteral("--detach"), rev}, {}}});
}

void RepoView::merge(const QString& rev, const QString& label) {
    const QString current = currentBranch();
    FormDialog dlg(this, tr("Merge"), tr("Merge"));
    dlg.addNote(tr("Merge %1 into %2.").arg(label, b(current)));
    dlg.addRadios(QStringLiteral("mode"), tr("Strategy:"),
                  {{QStringLiteral("default"), tr("Fast-forward if possible, otherwise create a merge commit")},
                   {QStringLiteral("no-ff"), tr("Always create a merge commit (--no-ff)")},
                   {QStringLiteral("ff-only"), tr("Fast-forward only (--ff-only)")},
                   {QStringLiteral("squash"), tr("Squash into staged changes (--squash)")}},
                  QStringLiteral("default"));
    if (dlg.exec() != QDialog::Accepted) return;
    QStringList args{QStringLiteral("merge"), QStringLiteral("--no-edit")};
    const QString mode = dlg.choice(QStringLiteral("mode"));
    if (mode != QLatin1String("default")) args << QStringLiteral("--") + mode;
    args << rev;
    runCommands(tr("Merge"), {{args, {}}}, [this, mode](bool ok) {
        if (ok && mode == QLatin1String("squash")) showChanges();
    });
}

void RepoView::rebase(const QString& onto, const QString& label) {
    FormDialog dlg(this, tr("Rebase"), tr("Rebase"));
    dlg.addNote(tr("Replay the commits of %1 on top of %2.").arg(b(currentBranch()), label));
    dlg.addCheck(QStringLiteral("autostash"), tr("Stash and reapply local changes"), !m_snapshot.status.isClean());
    if (dlg.exec() != QDialog::Accepted) return;
    QStringList args{QStringLiteral("rebase")};
    if (dlg.checked(QStringLiteral("autostash"))) args << QStringLiteral("--autostash");
    args << onto;
    runCommands(tr("Rebase"), {{args, {}}});
}

void RepoView::cherryPick(const QString& sha) {
    bool isMerge = false;
    for (const Commit& c : m_snapshot.commits) {
        if (c.sha == sha) isMerge = c.isMerge();
    }
    if (!confirm(this, tr("Cherry-Pick"),
                 tr("Apply commit %1 on top of %2?")
                     .arg(b(sha.left(8)), b(currentBranch().isEmpty() ? QStringLiteral("HEAD") : currentBranch())),
                 tr("Cherry-Pick")))
        return;
    QStringList args{QStringLiteral("cherry-pick")};
    if (isMerge) args << QStringLiteral("-m") << QStringLiteral("1");
    args << sha;
    runCommands(tr("Cherry-Pick"), {{args, {}}});
}

void RepoView::revert(const QString& sha) {
    bool isMerge = false;
    for (const Commit& c : m_snapshot.commits) {
        if (c.sha == sha) isMerge = c.isMerge();
    }
    if (!confirm(this, tr("Revert"), tr("Create a new commit that undoes %1?").arg(b(sha.left(8))), tr("Revert"))) return;
    QStringList args{QStringLiteral("revert"), QStringLiteral("--no-edit")};
    if (isMerge) args << QStringLiteral("-m") << QStringLiteral("1");
    args << sha;
    runCommands(tr("Revert"), {{args, {}}});
}

void RepoView::reset(const QString& sha) {
    FormDialog dlg(this, tr("Reset"), tr("Reset"));
    dlg.addNote(
        tr("Move %1 to %2.").arg(b(currentBranch().isEmpty() ? QStringLiteral("HEAD") : currentBranch()), b(sha.left(8))));
    dlg.addRadios(QStringLiteral("mode"), tr("Mode:"),
                  {{QStringLiteral("soft"), tr("Soft — keep all changes staged")},
                   {QStringLiteral("mixed"), tr("Mixed — keep changes in the working tree, unstaged")},
                   {QStringLiteral("hard"), tr("Hard — discard all local changes")}},
                  QStringLiteral("mixed"));
    if (dlg.exec() != QDialog::Accepted) return;
    const QString mode = dlg.choice(QStringLiteral("mode"));
    if (mode == QLatin1String("hard") &&
        !confirm(this, tr("Hard Reset"), tr("A hard reset permanently discards uncommitted changes. Continue?"), tr("Reset"),
                 true))
        return;
    runCommands(tr("Reset"), {{{QStringLiteral("reset"), QStringLiteral("--") + mode, sha}, {}}});
}

void RepoView::createTag(const QString& sha) {
    FormDialog dlg(this, tr("New Tag"), tr("Create Tag"));
    dlg.addNote(tr("Tag commit %1.").arg(b(sha.left(8))));
    QLineEdit* name = dlg.addLine(QStringLiteral("name"), tr("Tag name:"), {}, QStringLiteral("v1.0.0"));
    dlg.addText(QStringLiteral("message"), tr("Message:"));
    dlg.addCheck(QStringLiteral("push"), tr("Push to %1").arg(defaultRemote()), false)->setEnabled(!m_snapshot.remotes.isEmpty());
    dlg.setValidator([name] { return !name->text().trimmed().isEmpty() && !name->text().contains(QLatin1Char(' ')); });
    if (dlg.exec() != QDialog::Accepted) return;
    const QString tag = dlg.text(QStringLiteral("name"));
    const QString message = dlg.text(QStringLiteral("message"));
    QStringList args{QStringLiteral("tag")};
    if (!message.isEmpty()) args << QStringLiteral("-a") << QStringLiteral("-m") << message;
    args << tag << sha;
    const bool pushAfter = dlg.checked(QStringLiteral("push"));
    const QString remote = defaultRemote();
    runCommands(tr("New Tag"), {{args, {}}}, [this, pushAfter, remote, tag](bool ok) {
        if (ok && pushAfter)
            runNetwork(tr("Push Tag"),
                       {QStringLiteral("push"), QStringLiteral("--progress"), remote, QStringLiteral("refs/tags/") + tag});
    });
}

void RepoView::deleteBranch(const Ref& ref) {
    FormDialog dlg(this, tr("Delete Branch"), tr("Delete"));
    dlg.addNote(tr("Delete local branch %1?").arg(b(ref.name)));
    dlg.addCheck(QStringLiteral("force"), tr("Force delete, even if it has unmerged commits"), false);
    if (!ref.upstream.isEmpty() && !ref.upstreamGone)
        dlg.addCheck(QStringLiteral("remote"), tr("Also delete '%1' on the remote").arg(ref.upstream), false);
    if (dlg.exec() != QDialog::Accepted) return;
    const bool deleteRemote = dlg.checked(QStringLiteral("remote"));
    const QString upstream = ref.upstream;
    runCommands(tr("Delete Branch"),
                {{{QStringLiteral("branch"), dlg.checked(QStringLiteral("force")) ? QStringLiteral("-D") : QStringLiteral("-d"),
                   ref.name},
                  {}}},
                [this, deleteRemote, upstream](bool ok) {
                    if (ok && deleteRemote) {
                        runNetwork(
                            tr("Delete Remote Branch"),
                            {QStringLiteral("push"), QStringLiteral("--progress"), upstream.section(QLatin1Char('/'), 0, 0),
                             QStringLiteral("--delete"), upstream.section(QLatin1Char('/'), 1)});
                    }
                });
}

void RepoView::renameBranch(const Ref& ref) {
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, tr("Rename Branch"), tr("New name for '%1':").arg(ref.name), QLineEdit::Normal, ref.name, &ok)
            .trimmed();
    if (!ok || name.isEmpty() || name == ref.name) return;
    runCommands(tr("Rename Branch"), {{{QStringLiteral("branch"), QStringLiteral("-m"), ref.name, name}, {}}});
}

void RepoView::setUpstream(const Ref& ref) {
    QStringList remotes;
    for (const Ref& r : m_snapshot.refs) {
        if (r.kind == RefKind::RemoteBranch) remotes << r.name;
    }
    if (remotes.isEmpty()) {
        showError(this, tr("Track Remote Branch"), tr("No remote branches found. Fetch first."));
        return;
    }
    FormDialog dlg(this, tr("Track Remote Branch"), tr("Track"));
    dlg.addNote(tr("Set the upstream of %1.").arg(b(ref.name)));
    QString suggestion = ref.upstream;
    if (suggestion.isEmpty() && remotes.contains(defaultRemote() + QLatin1Char('/') + ref.name))
        suggestion = defaultRemote() + QLatin1Char('/') + ref.name;
    dlg.addCombo(QStringLiteral("upstream"), tr("Remote branch:"), remotes, suggestion);
    if (dlg.exec() != QDialog::Accepted) return;
    runCommands(
        tr("Track Remote Branch"),
        {{{QStringLiteral("branch"), QStringLiteral("--set-upstream-to=") + dlg.text(QStringLiteral("upstream")), ref.name},
          {}}});
}

void RepoView::fastForward(const Ref& ref) {
    if (ref.name == currentBranch()) {
        runCommands(tr("Fast-Forward"), {{{QStringLiteral("merge"), QStringLiteral("--ff-only"), ref.upstream}, {}}});
    } else {
        runCommands(tr("Fast-Forward"),
                    {{{QStringLiteral("fetch"), QStringLiteral("."), QStringLiteral("%1:%2").arg(ref.upstream, ref.name)}, {}}});
    }
}

void RepoView::deleteRemoteBranch(const Ref& ref) {
    if (!confirm(this, tr("Delete Remote Branch"),
                 tr("Delete %1 on the remote <b>%2</b>?").arg(b(ref.remoteBranch()), ref.remoteName().toHtmlEscaped()),
                 tr("Delete"), true))
        return;
    runNetwork(tr("Delete Remote Branch"), {QStringLiteral("push"), QStringLiteral("--progress"), ref.remoteName(),
                                            QStringLiteral("--delete"), ref.remoteBranch()});
}

void RepoView::pushTag(const QString& tag) {
    FormDialog dlg(this, tr("Push Tag"), tr("Push"));
    dlg.addNote(tr("Push tag %1.").arg(b(tag)));
    dlg.addCombo(QStringLiteral("remote"), tr("To remote:"), remoteNames(), defaultRemote());
    if (dlg.exec() != QDialog::Accepted) return;
    runNetwork(tr("Push Tag"), {QStringLiteral("push"), QStringLiteral("--progress"), dlg.text(QStringLiteral("remote")),
                                QStringLiteral("refs/tags/") + tag});
}

void RepoView::deleteTag(const QString& tag) {
    FormDialog dlg(this, tr("Delete Tag"), tr("Delete"));
    dlg.addNote(tr("Delete tag %1?").arg(b(tag)));
    if (!m_snapshot.remotes.isEmpty())
        dlg.addCheck(QStringLiteral("remote"), tr("Also delete it from '%1'").arg(defaultRemote()), false);
    if (dlg.exec() != QDialog::Accepted) return;
    const bool remote = dlg.checked(QStringLiteral("remote"));
    const QString remoteName = defaultRemote();
    runCommands(tr("Delete Tag"), {{{QStringLiteral("tag"), QStringLiteral("-d"), tag}, {}}},
                [this, remote, remoteName, tag](bool ok) {
                    if (ok && remote)
                        runNetwork(tr("Delete Remote Tag"), {QStringLiteral("push"), QStringLiteral("--progress"), remoteName,
                                                             QStringLiteral("--delete"), QStringLiteral("refs/tags/") + tag});
                });
}

void RepoView::applyStash(const Stash& stash, bool pop) {
    runCommands(pop ? tr("Pop Stash") : tr("Apply Stash"),
                {{{QStringLiteral("stash"), pop ? QStringLiteral("pop") : QStringLiteral("apply"), stash.ref}, {}}},
                [this](bool ok) {
                    if (ok) showChanges();
                });
}

void RepoView::dropStash(const Stash& stash) {
    if (!confirm(this, tr("Drop Stash"), tr("Permanently delete stash %1?").arg(b(stash.message)), tr("Drop"), true)) return;
    runCommands(tr("Drop Stash"), {{{QStringLiteral("stash"), QStringLiteral("drop"), stash.ref}, {}}});
}

void RepoView::editRemote(const QString& name) {
    QString url;
    for (const Remote& r : m_snapshot.remotes) {
        if (r.name == name) url = r.fetchUrl;
    }
    bool ok = false;
    const QString newUrl =
        QInputDialog::getText(this, tr("Edit Remote"), tr("URL for '%1':").arg(name), QLineEdit::Normal, url, &ok).trimmed();
    if (!ok || newUrl.isEmpty() || newUrl == url) return;
    runCommands(tr("Edit Remote"), {{{QStringLiteral("remote"), QStringLiteral("set-url"), name, newUrl}, {}}});
}

void RepoView::removeRemote(const QString& name) {
    if (!confirm(this, tr("Remove Remote"), tr("Remove remote %1 and its remote-tracking branches?").arg(b(name)), tr("Remove"),
                 true))
        return;
    runCommands(tr("Remove Remote"), {{{QStringLiteral("remote"), QStringLiteral("remove"), name}, {}}});
}

void RepoView::savePatch(const QString& sha) {
    const QString path =
        QFileDialog::getSaveFileName(this, tr("Save Patch"), QDir(m_repo.root()).filePath(sha.left(8) + QStringLiteral(".patch")),
                                     tr("Patch files (*.patch *.diff)"));
    if (path.isEmpty()) return;
    const Git git = m_repo.git();
    runAsync(
        this,
        [git, sha] { return git.run({QStringLiteral("format-patch"), QStringLiteral("-1"), QStringLiteral("--stdout"), sha}); },
        [this, path](const GitResult& r) {
            if (!r.ok()) {
                showError(this, tr("Save Patch"), r.message());
                return;
            }
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly) || f.write(r.out) != r.out.size())
                showError(this, tr("Save Patch"), f.errorString());
            else
                emit statusMessage(tr("Saved %1").arg(path));
        });
}

// ---------------------------------------------------------------- GitHub / GitLab

std::optional<RepoView::Hosted> RepoView::hostedRemote(const QString& remoteName) const {
    for (const Remote& r : m_snapshot.remotes) {
        if (r.name != remoteName) continue;
        const RemoteRepo repo = parseRemoteUrl(r.fetchUrl);
        if (!repo.valid) return std::nullopt;
        const auto provider = detectProvider(repo.host, AccountStore::instance()->accounts());
        if (!provider) return std::nullopt;
        return Hosted{*provider, repo};
    }
    return std::nullopt;
}

void RepoView::openInBrowser(const QString& url) {
    if (!QDesktopServices::openUrl(QUrl(url))) showError(this, tr("Open in Browser"), tr("Could not open %1").arg(url));
}

void RepoView::createReviewRequest(const QString& remoteName, const QString& branch, int unpushedCommits) {
    const auto hosted = hostedRemote(remoteName);
    if (!hosted) {
        showError(this, tr("Create Pull Request"),
                  tr("Remote '%1' is not a recognised GitHub or GitLab repository.").arg(remoteName));
        return;
    }
    const QString kind = reviewRequestName(hosted->provider);

    QStringList targets;
    for (const Ref& r : m_snapshot.refs) {
        if (r.kind == RefKind::RemoteBranch && r.remoteName() == remoteName && r.remoteBranch() != branch)
            targets << r.remoteBranch();
    }
    if (targets.isEmpty()) {
        showError(this, tr("Create %1").arg(kind), tr("'%1' has no other branches to merge into. Fetch first.").arg(remoteName));
        return;
    }
    QString target;
    const GitResult head = m_repo.git().run({QStringLiteral("symbolic-ref"), QStringLiteral("--quiet"), QStringLiteral("--short"),
                                             QStringLiteral("refs/remotes/%1/HEAD").arg(remoteName)});
    if (head.ok()) target = head.outText().trimmed().section(QLatin1Char('/'), 1);
    for (const QString& candidate : {QStringLiteral("main"), QStringLiteral("master"), QStringLiteral("develop")}) {
        if (!targets.contains(target) && targets.contains(candidate)) target = candidate;
    }
    if (!targets.contains(target)) target = targets.first();

    // Title and description from the commits that the request would contain.
    ReviewRequestDraft draft{branch, targets, target, branch, {}, unpushedCommits};
    const GitResult log =
        m_repo.git().run({QStringLiteral("log"), QStringLiteral("--format=%s%x1f%b%x1e"), QStringLiteral("--max-count=50"),
                          QStringLiteral("%1/%2..%1/%3").arg(remoteName, target, branch), QStringLiteral("--")});
    QStringList subjects;
    QString onlyBody;
    for (const QByteArray& raw : log.out.split('\x1e')) {
        const QString rec = QString::fromUtf8(raw).trimmed();
        if (rec.isEmpty()) continue;
        subjects << rec.section(QChar(0x1f), 0, 0).trimmed();
        onlyBody = rec.section(QChar(0x1f), 1).trimmed();
    }
    if (!subjects.isEmpty()) draft.title = subjects.first();
    if (subjects.size() == 1) {
        draft.body = onlyBody;
    } else if (subjects.size() > 1) {
        QStringList bullets;
        for (auto it = subjects.crbegin(); it != subjects.crend(); ++it) bullets << QStringLiteral("- ") + *it;
        draft.body = bullets.join(QLatin1Char('\n'));
    }

    const Account* account = AccountStore::instance()->accountForHost(hosted->repo.host);
    if (!account) {
        QMessageBox box(QMessageBox::Information, tr("Create %1").arg(kind),
                        tr("Sign in to %1 to create the %2 here, or continue in your browser.")
                            .arg(providerName(hosted->provider), kind.toLower()),
                        QMessageBox::Cancel, this);
        QPushButton* add = box.addButton(tr("Add Account…"), QMessageBox::ActionRole);
        QPushButton* web = box.addButton(tr("Open in Browser"), QMessageBox::AcceptRole);
        box.exec();
        if (box.clickedButton() == web) {
            openInBrowser(webNewReviewUrl(hosted->provider, hosted->repo, branch, target));
        } else if (box.clickedButton() == add) {
            AddAccountDialog dlg(hosted->provider, this, hosted->repo.host);
            if (dlg.exec() == QDialog::Accepted) createReviewRequest(remoteName, branch, unpushedCommits);
        }
        return;
    }
    PullRequestDialog dlg(*account, hosted->repo, draft, this);
    dlg.exec();
}

// ---------------------------------------------------------------- interactive rebase

void RepoView::interactiveRebase(const QString& base, const QString& baseLabel) {
    if (m_busy) {
        emit statusMessage(tr("Another operation is still running."));
        return;
    }
    if (m_snapshot.operation != Operation::None) {
        showError(this, tr("Interactive Rebase"), tr("Finish or abort the operation in progress first."));
        return;
    }
    const RebaseRange range = loadRebaseRange(m_repo, base);
    if (!range.error.isEmpty()) {
        showError(this, tr("Interactive Rebase"), range.error);
        return;
    }
    const QString branch = currentBranch().isEmpty() ? QStringLiteral("HEAD") : currentBranch();
    const QString heading =
        tr("Rebase %1: %n commit(s) on top of %2", nullptr, int(range.steps.size())).arg(b(branch), baseLabel);
    InteractiveRebaseDialog dlg(heading, range, !m_snapshot.status.isClean(), this);
    if (dlg.exec() != QDialog::Accepted) return;
    startRebase(dlg.steps(), base, dlg.autostash());
}

void RepoView::startRebase(const QVector<RebaseStep>& steps, const QString& base, bool autostash) {
    const QString dir = QDir(m_repo.gitDir()).filePath(QStringLiteral("aslgc-rebase"));
    QDir(dir).removeRecursively();
    QString error;
    const QStringList args = prepareRebase(steps, base, dir, autostash, &error);
    if (args.isEmpty()) {
        showError(this, tr("Interactive Rebase"), error);
        return;
    }
    runCommands(tr("Interactive Rebase"), {{args, {}}});
}

void RepoView::rewordCommit(const QString& sha) {
    if (m_busy) {
        emit statusMessage(tr("Another operation is still running."));
        return;
    }
    const CommitInfo info = m_repo.commitInfo(sha);
    if (!info.valid) return;
    FormDialog dlg(this, tr("Edit Commit Message"), tr("Save"));
    dlg.addNote(tr("Commit %1 by %2").arg(b(sha.left(8)), info.commit.authorName.toHtmlEscaped()));
    QLineEdit* subject = dlg.addLine(QStringLiteral("subject"), tr("Subject:"), info.message.section(QLatin1Char('\n'), 0, 0));
    dlg.addText(QStringLiteral("body"), tr("Description:"), info.message.section(QLatin1Char('\n'), 1).trimmed());
    dlg.setValidator([subject] { return !subject->text().trimmed().isEmpty(); });
    if (dlg.exec() != QDialog::Accepted) return;
    QString message = dlg.text(QStringLiteral("subject"));
    if (const QString body = dlg.text(QStringLiteral("body")); !body.isEmpty()) message += QStringLiteral("\n\n") + body;
    if (message == info.message) return;

    if (sha == m_snapshot.head.sha) {  // latest commit: amend only the message, never staged changes
        runCommands(tr("Edit Commit Message"), {{{QStringLiteral("commit"), QStringLiteral("--amend"), QStringLiteral("--only"),
                                                  QStringLiteral("--allow-empty"), QStringLiteral("--cleanup=whitespace"),
                                                  QStringLiteral("-F"), QStringLiteral("-")},
                                                 message.toUtf8()}});
        return;
    }
    if (m_snapshot.operation != Operation::None) {
        showError(this, tr("Edit Commit Message"), tr("Finish or abort the operation in progress first."));
        return;
    }
    if (info.commit.parents.isEmpty()) {
        showError(this, tr("Edit Commit Message"),
                  tr("The first commit's message can only be changed while it is the latest commit."));
        return;
    }
    const QString base = sha + QStringLiteral("^");
    RebaseRange range = loadRebaseRange(m_repo, base);
    bool found = false;
    for (RebaseStep& s : range.steps) {
        if (s.sha == sha) {
            s.action = RebaseAction::Reword;
            s.message = message;
            found = true;
        }
    }
    if (!range.error.isEmpty() || !found) {
        showError(this, tr("Edit Commit Message"),
                  tr("Only commits in the history of the current branch can be reworded, and not merge commits."));
        return;
    }
    QStringList notes;
    if (range.published) notes << tr("This commit is already on a remote. You will need to force push afterwards.");
    if (range.mergeCount > 0) notes << tr("%n merge commit(s) after it will be flattened.", nullptr, range.mergeCount);
    notes << tr("%n later commit(s) will be rewritten.", nullptr, int(range.steps.size()) - 1);
    if ((range.published || range.mergeCount > 0) &&
        !confirm(this, tr("Edit Commit Message"), notes.join(QStringLiteral("<br><br>")), tr("Rewrite History"), true))
        return;
    startRebase(range.steps, base, !m_snapshot.status.isClean());
}

void RepoView::continueOperation() {
    switch (m_snapshot.operation) {
        case Operation::Merge:
            runCommands(tr("Commit Merge"), {{{QStringLiteral("commit"), QStringLiteral("--no-edit")}, {}}});
            break;
        case Operation::Rebase:
            runCommands(tr("Continue Rebase"), {{{QStringLiteral("rebase"), QStringLiteral("--continue")}, {}}});
            break;
        case Operation::CherryPick:
            runCommands(tr("Continue Cherry-Pick"), {{{QStringLiteral("cherry-pick"), QStringLiteral("--continue")}, {}}});
            break;
        case Operation::Revert:
            runCommands(tr("Continue Revert"), {{{QStringLiteral("revert"), QStringLiteral("--continue")}, {}}});
            break;
        case Operation::ApplyMailbox:
            runCommands(tr("Continue"), {{{QStringLiteral("am"), QStringLiteral("--continue")}, {}}});
            break;
        case Operation::None: break;
    }
}

void RepoView::skipOperation() {
    QString cmd;
    switch (m_snapshot.operation) {
        case Operation::Rebase: cmd = QStringLiteral("rebase"); break;
        case Operation::CherryPick: cmd = QStringLiteral("cherry-pick"); break;
        case Operation::Revert: cmd = QStringLiteral("revert"); break;
        case Operation::ApplyMailbox: cmd = QStringLiteral("am"); break;
        default: return;
    }
    if (!confirm(this, tr("Skip"), tr("Skip the current commit? Its changes will be dropped."), tr("Skip"), true)) return;
    runCommands(tr("Skip"), {{{cmd, QStringLiteral("--skip")}, {}}});
}

void RepoView::abortOperation() {
    QString cmd;
    switch (m_snapshot.operation) {
        case Operation::Merge: cmd = QStringLiteral("merge"); break;
        case Operation::Rebase: cmd = QStringLiteral("rebase"); break;
        case Operation::CherryPick: cmd = QStringLiteral("cherry-pick"); break;
        case Operation::Revert: cmd = QStringLiteral("revert"); break;
        case Operation::ApplyMailbox: cmd = QStringLiteral("am"); break;
        case Operation::None: return;
    }
    if (!confirm(this, tr("Abort"), tr("Abort and return to the state before the %1 started?").arg(cmd), tr("Abort"), true))
        return;
    runCommands(tr("Abort"), {{{cmd, QStringLiteral("--abort")}, {}}});
}

}  // namespace aslgc
