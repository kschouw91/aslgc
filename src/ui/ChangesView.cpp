// SPDX-License-Identifier: MIT
#include "ChangesView.h"

#include "Async.h"
#include "Dialogs.h"
#include "FileList.h"
#include "HostProcess.h"
#include "RepoView.h"
#include "Theme.h"
#include "hosting/AccountStore.h"

#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace aslgc {

namespace {

QWidget* makeHeader(QLabel*& title, QToolButton*& button, const QString& buttonText) {
    auto* w = new QWidget;
    auto* l = new QHBoxLayout(w);
    l->setContentsMargins(10, 6, 6, 4);
    title = new QLabel;
    QFont f = title->font();
    f.setBold(true);
    title->setFont(f);
    button = new QToolButton;
    button->setText(buttonText);
    button->setAutoRaise(true);
    l->addWidget(title);
    l->addStretch();
    l->addWidget(button);
    return w;
}

QStringList pathsOf(const QVector<FileChange>& files, bool includeOld = false) {
    QStringList out;
    for (const FileChange& f : files) {
        if (includeOld && !f.oldPath.isEmpty() && f.oldPath != f.path) out << f.oldPath;
        out << f.path;
    }
    return out;
}

}  // namespace

ChangesView::ChangesView(RepoView* owner) : QWidget(owner), m_owner(owner) {
    auto* unstagedPanel = new QWidget;
    auto* ul = new QVBoxLayout(unstagedPanel);
    ul->setContentsMargins(0, 0, 0, 0);
    ul->setSpacing(0);
    ul->addWidget(makeHeader(m_unstagedTitle, m_stageAll, tr("Stage All")));
    m_unstaged = new FileList;
    ul->addWidget(m_unstaged);

    auto* stagedPanel = new QWidget;
    auto* sl = new QVBoxLayout(stagedPanel);
    sl->setContentsMargins(0, 0, 0, 0);
    sl->setSpacing(0);
    sl->addWidget(makeHeader(m_stagedTitle, m_unstageAll, tr("Unstage All")));
    m_staged = new FileList;
    sl->addWidget(m_staged);

    auto* commitPanel = new QWidget;
    auto* cl = new QVBoxLayout(commitPanel);
    cl->setContentsMargins(8, 8, 8, 8);
    m_subject = new QLineEdit;
    m_subject->setPlaceholderText(tr("Commit subject"));
    m_body = new QPlainTextEdit;
    m_body->setPlaceholderText(tr("Description (optional)"));
    m_body->setTabChangesFocus(true);
    m_amend = new QCheckBox(tr("Amend"));
    m_amend->setToolTip(tr("Replace the last commit with the staged changes and this message"));
    m_commit = new QPushButton(tr("Commit"));
    m_commit->setIcon(theme::actionIcon(QStringLiteral("check")));
    m_commit->setToolTip(tr("Commit staged changes (Ctrl+Enter)"));
    auto* row = new QHBoxLayout;
    row->addWidget(m_amend);
    row->addStretch();
    row->addWidget(m_commit);
    cl->addWidget(m_subject);
    cl->addWidget(m_body, 1);
    cl->addLayout(row);

    m_leftSplit = new QSplitter(Qt::Vertical);
    m_leftSplit->addWidget(unstagedPanel);
    m_leftSplit->addWidget(stagedPanel);
    m_leftSplit->addWidget(commitPanel);
    m_leftSplit->setChildrenCollapsible(false);
    m_leftSplit->setSizes({320, 220, 170});

    auto* diffPanel = new QWidget;
    auto* dl = new QVBoxLayout(diffPanel);
    dl->setContentsMargins(0, 0, 0, 0);
    dl->setSpacing(0);
    m_diffTitle = new QLabel;
    m_diffTitle->setContentsMargins(10, 7, 10, 6);
    m_diffTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_diff = new DiffView;
    dl->addWidget(m_diffTitle);
    dl->addWidget(m_diff, 1);

    m_mainSplit = new QSplitter(Qt::Horizontal);
    m_mainSplit->addWidget(m_leftSplit);
    m_mainSplit->addWidget(diffPanel);
    m_mainSplit->setStretchFactor(1, 1);
    m_mainSplit->setSizes({380, 900});

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_mainSplit);

    for (Side side : {Side::Unstaged, Side::Staged}) {
        FileList* l = list(side);
        connect(l, &QListWidget::currentItemChanged, this, [this, side] { onCurrentChanged(side); });
        connect(l, &QListWidget::itemClicked, this, [this, side] { onCurrentChanged(side); });
        connect(l, &QWidget::customContextMenuRequested, this,
                [this, side, l](const QPoint& pos) { showFileMenu(side, l->viewport()->mapToGlobal(pos)); });
    }
    connect(m_unstaged, &FileList::fileActivated, this, [this] { stage(m_unstaged->selectedFiles()); });
    connect(m_staged, &FileList::fileActivated, this, [this] { unstage(m_staged->selectedFiles()); });
    connect(m_stageAll, &QToolButton::clicked, this, [this] { stage(m_unstaged->files()); });
    connect(m_unstageAll, &QToolButton::clicked, this, [this] { unstage(m_staged->files()); });
    connect(m_subject, &QLineEdit::textChanged, this, &ChangesView::updateCommitButton);
    connect(m_subject, &QLineEdit::returnPressed, m_body, qOverload<>(&QWidget::setFocus));
    connect(m_amend, &QCheckBox::toggled, this, &ChangesView::onAmendToggled);
    connect(m_commit, &QPushButton::clicked, this, &ChangesView::commit);
    for (const QKeySequence& seq : {QKeySequence(Qt::CTRL | Qt::Key_Return), QKeySequence(Qt::CTRL | Qt::Key_Enter)}) {
        auto* sc = new QShortcut(seq, this);
        sc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(sc, &QShortcut::activated, this, &ChangesView::commit);
    }
    connect(m_diff, &DiffView::hunkAction, this, &ChangesView::onHunkAction);
    connect(m_diff, &DiffView::linesAction, this, &ChangesView::onLinesAction);
    updateCommitButton();
}

FileList* ChangesView::list(Side side) const { return side == Side::Unstaged ? m_unstaged : m_staged; }

void ChangesView::setStatus(const WorkingStatus& status, const HeadInfo& head, Operation operation) {
    m_head = head;
    m_operation = operation;
    m_amend->setEnabled(!head.unborn);

    const Side previousSide = m_side;
    const QString previousPath = m_hasCurrent ? m_current.path : QString();
    const int previousRow = list(previousSide)->currentRow();

    {
        QSignalBlocker b1(m_unstaged);
        QSignalBlocker b2(m_staged);
        m_unstaged->setFiles(status.unstaged);
        m_staged->setFiles(status.staged);
        m_unstagedTitle->setText(tr("Unstaged  %1").arg(status.unstaged.size()));
        m_stagedTitle->setText(tr("Staged  %1").arg(status.staged.size()));
        m_stageAll->setEnabled(!status.unstaged.isEmpty());
        m_unstageAll->setEnabled(!status.staged.isEmpty());

        Side side = previousSide;
        bool found = !previousPath.isEmpty() && list(side)->selectPath(previousPath);
        if (!found && m_hasCurrent && list(side)->count() > 0) {
            list(side)->setCurrentRow(qMin(qMax(previousRow, 0), list(side)->count() - 1), QItemSelectionModel::ClearAndSelect);
            found = true;
        }
        const Side other = side == Side::Unstaged ? Side::Staged : Side::Unstaged;
        if (!found && !previousPath.isEmpty() && list(other)->selectPath(previousPath)) {
            side = other;
            found = true;
        }
        if (!found && !m_hasCurrent) {
            if (m_unstaged->count() > 0) {
                m_unstaged->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
                side = Side::Unstaged;
                found = true;
            } else if (m_staged->count() > 0) {
                m_staged->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
                side = Side::Staged;
                found = true;
            }
        }
        if (found) {
            list(side == Side::Unstaged ? Side::Staged : Side::Unstaged)->clearSelection();
            list(side == Side::Unstaged ? Side::Staged : Side::Unstaged)->setCurrentItem(nullptr);
            const FileChange* f = list(side)->currentFile();
            const bool same = f && side == previousSide && f->path == previousPath;
            m_side = side;
            m_current = *f;
            m_hasCurrent = true;
            loadDiff(same);
        } else {
            m_hasCurrent = false;
            m_lastDiff.clear();
            m_diffTitle->clear();
            m_diff->showMessage(status.isClean() ? tr("Working tree is clean.") : tr("Select a file to see its changes."));
        }
    }
    updateCommitButton();
}

void ChangesView::onCurrentChanged(Side side) {
    const FileChange* f = list(side)->currentFile();
    if (!f) return;
    if (m_hasCurrent && side == m_side && f->path == m_current.path && f->status == m_current.status) return;
    FileList* other = list(side == Side::Unstaged ? Side::Staged : Side::Unstaged);
    {
        QSignalBlocker block(other);
        other->clearSelection();
        other->setCurrentItem(nullptr);
    }
    m_side = side;
    m_current = *f;
    m_hasCurrent = true;
    loadDiff(false);
}

void ChangesView::loadDiff(bool keepScroll) {
    if (!m_hasCurrent) return;
    const int gen = ++m_diffGen;
    const FileChange file = m_current;
    const bool staged = m_side == Side::Staged;
    const Repository repo = m_owner->repo();
    QString title = QStringLiteral("<b>%1</b>").arg(file.path.toHtmlEscaped());
    if (!file.oldPath.isEmpty() && file.oldPath != file.path)
        title = QStringLiteral("%1 → %2").arg(file.oldPath.toHtmlEscaped(), title);
    title +=
        QStringLiteral("  <span style='color:%1'>%2%3</span>")
            .arg(theme::statusColor(file.status).name(), theme::statusName(file.status), staged ? tr(" · staged") : QString());
    m_diffTitle->setText(title);

    runAsync(
        this,
        [repo, file, staged]() -> QByteArray {
            if (!staged && file.isConflict()) {
                QFile f(QDir(repo.root()).filePath(file.path));
                return f.open(QIODevice::ReadOnly) ? f.read(64 * 1024 * 1024) : QByteArray();
            }
            return repo.workingDiff(file, staged);
        },
        [this, gen, file, staged, keepScroll](const QByteArray& bytes) {
            if (gen != m_diffGen) return;
            if (keepScroll && bytes == m_lastDiff) return;
            m_lastDiff = bytes;
            if (!staged && file.isConflict()) {
                m_diff->setConflictText(bytes, keepScroll);
                return;
            }
            const QVector<FileDiff> diffs = parseDiff(bytes);
            if (diffs.isEmpty()) {
                m_diff->showMessage(file.submodule ? tr("Submodule has changes inside its working tree.")
                                                   : tr("No changes to display."));
                return;
            }
            const DiffView::Mode mode = staged               ? DiffView::Mode::Staged
                                        : file.isUntracked() ? DiffView::Mode::Untracked
                                                             : DiffView::Mode::Unstaged;
            m_diff->setDiff(diffs.first(), mode, keepScroll);
        });
}

void ChangesView::stage(const QVector<FileChange>& files) {
    if (files.isEmpty()) return;
    m_owner->runCommands(tr("Stage"), {{{QStringLiteral("--literal-pathspecs"), QStringLiteral("add"), QStringLiteral("-A"),
                                         QStringLiteral("--pathspec-from-file=-"), QStringLiteral("--pathspec-file-nul")},
                                        Git::nulJoin(pathsOf(files))}});
}

void ChangesView::unstage(const QVector<FileChange>& files) {
    if (files.isEmpty()) return;
    const QByteArray paths = Git::nulJoin(pathsOf(files, true));
    QStringList args{QStringLiteral("--literal-pathspecs")};
    if (m_head.unborn) {
        args << QStringLiteral("rm") << QStringLiteral("--cached") << QStringLiteral("-r") << QStringLiteral("-q");
    } else {
        args << QStringLiteral("restore") << QStringLiteral("--staged");
    }
    args << QStringLiteral("--pathspec-from-file=-") << QStringLiteral("--pathspec-file-nul");
    m_owner->runCommands(tr("Unstage"), {{args, paths}});
}

void ChangesView::discard(const QVector<FileChange>& files) {
    QVector<FileChange> tracked;
    QStringList untracked;
    for (const FileChange& f : files) {
        if (f.isUntracked())
            untracked << f.path;
        else if (!f.isConflict())
            tracked << f;
    }
    if (tracked.isEmpty() && untracked.isEmpty()) return;
    QString text = tr("Discard all changes in %n file(s)?", nullptr, int(tracked.size() + untracked.size()));
    if (!untracked.isEmpty())
        text += QStringLiteral("<br><br>") + tr("Untracked files will be moved to the trash.");
    else
        text += QStringLiteral("<br><br>") + tr("This cannot be undone.");
    if (!confirm(this, tr("Discard Changes"), text, tr("Discard"), true)) return;

    QStringList failed;
    for (const QString& path : untracked) {
        const QString abs = QDir(m_owner->repo().root()).filePath(path);
        if (!QFile::moveToTrash(abs) && !QFile::remove(abs)) failed << path;
    }
    if (!failed.isEmpty())
        showError(this, tr("Discard Changes"), tr("Could not remove:\n%1").arg(failed.join(QLatin1Char('\n'))));
    if (tracked.isEmpty()) {
        m_owner->refresh();
        return;
    }
    m_owner->runCommands(tr("Discard Changes"),
                         {{{QStringLiteral("--literal-pathspecs"), QStringLiteral("restore"), QStringLiteral("--worktree"),
                            QStringLiteral("--pathspec-from-file=-"), QStringLiteral("--pathspec-file-nul")},
                           Git::nulJoin(pathsOf(tracked))}});
}

void ChangesView::resolve(const QVector<FileChange>& files, const QString& side) {
    QVector<GitCommand> cmds;
    const QStringList paths = pathsOf(files);
    if (!side.isEmpty()) {
        cmds.push_back(
            {QStringList{QStringLiteral("--literal-pathspecs"), QStringLiteral("checkout"), side, QStringLiteral("--")} + paths,
             {}});
    }
    cmds.push_back({QStringList{QStringLiteral("--literal-pathspecs"), QStringLiteral("add"), QStringLiteral("--")} + paths, {}});
    m_owner->runCommands(tr("Resolve Conflict"), cmds);
}

void ChangesView::ignore(const QString& pattern) {
    QFile f(QDir(m_owner->repo().root()).filePath(QStringLiteral(".gitignore")));
    QByteArray existing;
    if (f.open(QIODevice::ReadOnly)) {
        existing = f.readAll();
        f.close();
    }
    if (!f.open(QIODevice::Append)) {
        showError(this, tr("Ignore"), tr("Could not write .gitignore: %1").arg(f.errorString()));
        return;
    }
    if (!existing.isEmpty() && !existing.endsWith('\n')) f.write("\n");
    f.write(pattern.toUtf8() + '\n');
    f.close();
    m_owner->refresh();
}

void ChangesView::showFileMenu(Side side, const QPoint& globalPos) {
    const QVector<FileChange> files = list(side)->selectedFiles();
    if (files.isEmpty()) return;
    QVector<FileChange> conflicts;
    bool allUntracked = true;
    for (const FileChange& f : files) {
        if (f.isConflict()) conflicts << f;
        if (!f.isUntracked()) allUntracked = false;
    }
    const QString root = m_owner->repo().root();
    QMenu menu(this);
    if (side == Side::Unstaged) {
        menu.addAction(theme::actionIcon(QStringLiteral("plus")), tr("Stage"), this, [this, files] { stage(files); });
        if (!conflicts.isEmpty()) {
            menu.addSeparator();
            menu.addAction(tr("Resolve Using Ours (HEAD)"), this,
                           [this, conflicts] { resolve(conflicts, QStringLiteral("--ours")); });
            menu.addAction(tr("Resolve Using Theirs (Incoming)"), this,
                           [this, conflicts] { resolve(conflicts, QStringLiteral("--theirs")); });
            menu.addAction(tr("Mark as Resolved"), this, [this, conflicts] { resolve(conflicts, {}); });
            if (conflicts.size() == 1) {
                menu.addAction(tr("Open in Merge Tool"), this, [this, path = conflicts.first().path] {
                    const QStringList args{QStringLiteral("mergetool"), QStringLiteral("--no-prompt"), QStringLiteral("--"),
                                           path};
                    bool started = false;
                    if (host::isFlatpak()) {
                        // Merge tools are installed on the host, so run the host's git mergetool.
                        started = host::startDetached(QStringLiteral("git"), args, m_owner->repo().root());
                    } else {
                        QProcess p;
                        m_owner->repo().git().prepare(p, args);
                        started = p.startDetached();
                    }
                    if (!started) showError(this, tr("Merge Tool"), tr("Could not start git mergetool."));
                });
            }
        }
        if (conflicts.size() < files.size()) {
            menu.addSeparator();
            menu.addAction(theme::actionIcon(QStringLiteral("arrow-back-up")), tr("Discard Changes…"), this,
                           [this, files] { discard(files); });
        }
        if (allUntracked) {
            QMenu* ig = menu.addMenu(tr("Ignore"));
            for (const FileChange& f : files) {
                ig->addAction(tr("Ignore '%1'").arg(f.path), this, [this, p = f.path] { ignore(QLatin1Char('/') + p); });
            }
            if (files.size() == 1) {
                const QString suffix = QFileInfo(files.first().path).suffix();
                if (!suffix.isEmpty())
                    ig->addAction(tr("Ignore All '*.%1' Files").arg(suffix), this,
                                  [this, suffix] { ignore(QStringLiteral("*.") + suffix); });
            }
        }
    } else {
        menu.addAction(theme::actionIcon(QStringLiteral("minus")), tr("Unstage"), this, [this, files] { unstage(files); });
    }
    menu.addSeparator();
    if (files.size() == 1) {
        m_owner->addFileHistoryActions(menu, files.first());
        menu.addSeparator();
        const QString abs = QDir(root).filePath(files.first().path);
        if (QFileInfo::exists(abs)) {
            menu.addAction(tr("Open File"), this, [abs] { QDesktopServices::openUrl(QUrl::fromLocalFile(abs)); });
        }
        menu.addAction(tr("Show in Folder"), this,
                       [abs] { QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(abs).absolutePath())); });
    }
    menu.addAction(tr("Copy Path"), this,
                   [files] { QGuiApplication::clipboard()->setText(pathsOf(files).join(QLatin1Char('\n'))); });
    menu.addAction(tr("Copy Absolute Path"), this, [files, root] {
        QStringList abs;
        for (const QString& p : pathsOf(files)) abs << QDir(root).filePath(p);
        QGuiApplication::clipboard()->setText(abs.join(QLatin1Char('\n')));
    });
    menu.exec(globalPos);
}

void ChangesView::onHunkAction(int hunk, DiffView::Action action) {
    if (m_diff->mode() == DiffView::Mode::Untracked) {
        stage({m_current});
        return;
    }
    const QByteArray patch = hunkPatch(m_diff->fileDiff(), hunk);
    if (!patch.isEmpty()) applyPatch(patch, action);
}

void ChangesView::onLinesAction(const QSet<quint64>& lines, DiffView::Action action) {
    if (m_diff->mode() == DiffView::Mode::Untracked) {
        stage({m_current});
        return;
    }
    const QByteArray patch = linesPatch(m_diff->fileDiff(), lines, action != DiffView::Action::Stage);
    if (!patch.isEmpty()) applyPatch(patch, action);
}

void ChangesView::applyPatch(const QByteArray& patch, DiffView::Action action) {
    QStringList args{QStringLiteral("apply"), QStringLiteral("--recount"), QStringLiteral("--whitespace=nowarn")};
    QString title;
    switch (action) {
        case DiffView::Action::Stage:
            args << QStringLiteral("--cached");
            title = tr("Stage");
            break;
        case DiffView::Action::Unstage:
            args << QStringLiteral("--cached") << QStringLiteral("--reverse");
            title = tr("Unstage");
            break;
        case DiffView::Action::Discard:
            if (!confirm(this, tr("Discard Changes"), tr("Discard the selected changes? This cannot be undone."), tr("Discard"),
                         true))
                return;
            args << QStringLiteral("--reverse");
            title = tr("Discard");
            break;
    }
    args << QStringLiteral("-");
    m_owner->runCommands(title, {{args, patch}});
}

void ChangesView::updateCommitButton() {
    const int staged = m_staged->count();
    const bool amend = m_amend->isChecked();
    m_commit->setEnabled(!m_subject->text().trimmed().isEmpty() &&
                         (staged > 0 || amend || m_unstaged->count() > 0 || m_operation == Operation::Merge));
    if (amend)
        m_commit->setText(tr("Amend Commit"));
    else if (staged > 0)
        m_commit->setText(tr("Commit %n File(s)", nullptr, staged));
    else
        m_commit->setText(tr("Commit"));
}

void ChangesView::onAmendToggled(bool on) {
    if (on) {
        if (m_subject->text().trimmed().isEmpty() && m_body->toPlainText().trimmed().isEmpty()) {
            m_amendMessage = m_owner->repo().headMessage();
            m_subject->setText(m_amendMessage.section(QLatin1Char('\n'), 0, 0));
            m_body->setPlainText(m_amendMessage.section(QLatin1Char('\n'), 1).trimmed());
        }
    } else if (!m_amendMessage.isEmpty()) {
        const QString current = (m_subject->text() + QStringLiteral("\n\n") + m_body->toPlainText()).trimmed();
        const QString original = (m_amendMessage.section(QLatin1Char('\n'), 0, 0) + QStringLiteral("\n\n") +
                                  m_amendMessage.section(QLatin1Char('\n'), 1).trimmed())
                                     .trimmed();
        if (current == original) {
            m_subject->clear();
            m_body->clear();
        }
        m_amendMessage.clear();
    }
    updateCommitButton();
}

// Git refuses to commit without a name and email; ask once instead of failing.
bool ChangesView::ensureIdentity() {
    const Repository& repo = m_owner->repo();
    QString name = repo.configValue(QStringLiteral("user.name"));
    const QString email = repo.configValue(QStringLiteral("user.email"));
    if (!name.isEmpty() && !email.isEmpty()) return true;
    if (name.isEmpty()) {
        for (const Account& a : AccountStore::instance()->accounts()) {
            if (!a.displayName.isEmpty()) {
                name = a.displayName;
                break;
            }
        }
    }
    FormDialog dlg(this, tr("Commit Author"), tr("Save && Continue"));
    dlg.addNote(tr("Git records your name and email address with every commit. They are stored in your git configuration."));
    dlg.addLine(QStringLiteral("name"), tr("Name:"), name);
    dlg.addLine(QStringLiteral("email"), tr("Email:"), email, QStringLiteral("you@example.com"));
    dlg.addCheck(QStringLiteral("global"), tr("Use for all repositories (global git config)"), true);
    dlg.setValidator([&dlg] {
        return !dlg.text(QStringLiteral("name")).isEmpty() && dlg.text(QStringLiteral("email")).contains(QLatin1Char('@'));
    });
    if (dlg.exec() != QDialog::Accepted) return false;
    QStringList scope;
    if (dlg.checked(QStringLiteral("global"))) scope << QStringLiteral("--global");
    for (const auto& [key, field] : {std::pair{QStringLiteral("user.name"), QStringLiteral("name")},
                                     std::pair{QStringLiteral("user.email"), QStringLiteral("email")}}) {
        const GitResult r = repo.git().run(QStringList{QStringLiteral("config")} + scope + QStringList{key, dlg.text(field)});
        if (!r.ok()) {
            showError(this, tr("Commit Author"), r.message());
            return false;
        }
    }
    return true;
}

void ChangesView::commit() {
    if (!m_commit->isEnabled()) return;
    if (!ensureIdentity()) return;
    const QString subject = m_subject->text().trimmed();
    const QString body = m_body->toPlainText().trimmed();
    QString message = subject;
    if (!body.isEmpty()) message += QStringLiteral("\n\n") + body;
    const bool amend = m_amend->isChecked();

    QVector<GitCommand> cmds;
    if (!amend && m_staged->count() == 0 && m_operation != Operation::Merge) {
        if (!confirm(this, tr("Commit"), tr("Nothing is staged. Stage all changes and commit them?"), tr("Stage All && Commit")))
            return;
        cmds.push_back({{QStringLiteral("add"), QStringLiteral("-A")}, {}});
    }
    QStringList args{QStringLiteral("commit"), QStringLiteral("--cleanup=strip"), QStringLiteral("-F"), QStringLiteral("-")};
    if (amend) args << QStringLiteral("--amend");
    cmds.push_back({args, message.toUtf8()});
    m_owner->runCommands(amend ? tr("Amend Commit") : tr("Commit"), cmds, [this](bool ok) {
        if (!ok) return;
        m_subject->clear();
        m_body->clear();
        m_amendMessage.clear();
        QSignalBlocker block(m_amend);
        m_amend->setChecked(false);
        updateCommitButton();
    });
}

void ChangesView::focusCommitMessage() { m_subject->setFocus(); }

void ChangesView::saveLayout(QSettings& settings) const {
    settings.setValue(QStringLiteral("layout/changesMain"), m_mainSplit->saveState());
    settings.setValue(QStringLiteral("layout/changesLeft"), m_leftSplit->saveState());
}

void ChangesView::restoreLayout(const QSettings& settings) {
    m_mainSplit->restoreState(settings.value(QStringLiteral("layout/changesMain")).toByteArray());
    m_leftSplit->restoreState(settings.value(QStringLiteral("layout/changesLeft")).toByteArray());
}

}  // namespace aslgc
