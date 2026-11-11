// SPDX-License-Identifier: MIT
#include "InteractiveRebaseDialog.h"

#include "Theme.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSplitter>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace aslgc {

namespace {

enum Column { ActionColumn, ShaColumn, SubjectColumn, AuthorColumn };

struct ActionInfo {
    RebaseAction action;
    const char* label;
    const char* key;
    const char* help;
};

const ActionInfo kActions[] = {
    {RebaseAction::Pick, QT_TRANSLATE_NOOP("InteractiveRebase", "Pick"), "P",
     QT_TRANSLATE_NOOP("InteractiveRebase", "Keep the commit")},
    {RebaseAction::Reword, QT_TRANSLATE_NOOP("InteractiveRebase", "Reword"), "R",
     QT_TRANSLATE_NOOP("InteractiveRebase", "Keep the commit, change its message")},
    {RebaseAction::Edit, QT_TRANSLATE_NOOP("InteractiveRebase", "Edit"), "E",
     QT_TRANSLATE_NOOP("InteractiveRebase", "Stop after this commit so you can amend it")},
    {RebaseAction::Squash, QT_TRANSLATE_NOOP("InteractiveRebase", "Squash"), "S",
     QT_TRANSLATE_NOOP("InteractiveRebase", "Combine with the commit above, keeping both messages")},
    {RebaseAction::Fixup, QT_TRANSLATE_NOOP("InteractiveRebase", "Fixup"), "F",
     QT_TRANSLATE_NOOP("InteractiveRebase", "Combine with the commit above, discarding this message")},
    {RebaseAction::Drop, QT_TRANSLATE_NOOP("InteractiveRebase", "Drop"), "D",
     QT_TRANSLATE_NOOP("InteractiveRebase", "Remove the commit")},
};

QString label(RebaseAction action) {
    for (const ActionInfo& a : kActions) {
        if (a.action == action) return QCoreApplication::translate("InteractiveRebase", a.label);
    }
    return {};
}

QColor actionColor(RebaseAction action) {
    switch (action) {
        case RebaseAction::Pick: return {};
        case RebaseAction::Reword: return QColor(0x0969da);
        case RebaseAction::Edit: return QColor(0xbc4c00);
        case RebaseAction::Squash:
        case RebaseAction::Fixup: return QColor(0x8250df);
        case RebaseAction::Drop: return QColor(0xcf222e);
    }
    return {};
}

}  // namespace

InteractiveRebaseDialog::InteractiveRebaseDialog(const QString& heading, const RebaseRange& range, bool dirtyWorkTree,
                                                 QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Interactive Rebase"));
    resize(860, 620);
    auto* layout = new QVBoxLayout(this);

    auto* title = new QLabel(heading);
    title->setTextFormat(Qt::RichText);
    title->setWordWrap(true);
    layout->addWidget(title);
    auto* hint =
        new QLabel(tr("Commits are applied from top to bottom. Drag rows or use Alt+↑/↓ to reorder; "
                      "press P, R, E, S, F or D to change the action of the selected commits."));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    QStringList warnings;
    if (range.published)
        warnings << tr("Some of these commits are already on a remote. After rebasing you will need to force push.");
    if (range.mergeCount > 0) warnings << tr("%n merge commit(s) in this range will be flattened.", nullptr, range.mergeCount);
    if (!warnings.isEmpty()) {
        auto* warn = new QLabel(QStringLiteral("⚠ ") + warnings.join(QStringLiteral("<br>⚠ ")));
        warn->setTextFormat(Qt::RichText);
        warn->setWordWrap(true);
        layout->addWidget(warn);
    }

    // Action buttons
    auto* actions = new QHBoxLayout;
    for (const ActionInfo& a : kActions) {
        auto* button = new QToolButton;
        button->setText(
            QStringLiteral("%1 (%2)").arg(QCoreApplication::translate("InteractiveRebase", a.label), QLatin1String(a.key)));
        button->setToolTip(QCoreApplication::translate("InteractiveRebase", a.help));
        connect(button, &QToolButton::clicked, this, [this, action = a.action] { setAction(action); });
        actions->addWidget(button);
    }
    actions->addStretch();
    auto* up = new QToolButton;
    up->setText(QStringLiteral("↑"));
    up->setToolTip(tr("Move up (Alt+Up)"));
    auto* down = new QToolButton;
    down->setText(QStringLiteral("↓"));
    down->setToolTip(tr("Move down (Alt+Down)"));
    connect(up, &QToolButton::clicked, this, [this] { moveSelection(-1); });
    connect(down, &QToolButton::clicked, this, [this] { moveSelection(1); });
    actions->addWidget(up);
    actions->addWidget(down);
    layout->addLayout(actions);

    // Commit list
    m_list = new QTreeWidget;
    m_list->setColumnCount(4);
    m_list->setHeaderLabels({tr("Action"), tr("Commit"), tr("Message"), tr("Author")});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setDragDropMode(QAbstractItemView::InternalMove);
    m_list->setDefaultDropAction(Qt::MoveAction);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->header()->setStretchLastSection(false);
    m_list->header()->setSectionResizeMode(SubjectColumn, QHeaderView::Stretch);
    m_list->header()->resizeSection(ActionColumn, 90);
    m_list->header()->resizeSection(ShaColumn, 90);
    m_list->header()->resizeSection(AuthorColumn, 150);
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    for (const RebaseStep& s : range.steps) {
        m_steps.insert(s.sha, s);
        m_originalMessages.insert(s.sha, s.message);
        auto* item = new QTreeWidgetItem(m_list);
        item->setData(0, Qt::UserRole, s.sha);
        item->setText(ShaColumn, s.sha.left(8));
        item->setFont(ShaColumn, mono);
        item->setText(AuthorColumn, s.authorName);
        item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsDragEnabled);  // no drop *onto* rows
        refreshItem(item);
    }

    // Message editor
    auto* messagePanel = new QWidget;
    auto* ml = new QVBoxLayout(messagePanel);
    ml->setContentsMargins(0, 0, 0, 0);
    m_messageLabel = new QLabel;
    m_message = new QPlainTextEdit;
    m_message->setTabChangesFocus(true);
    ml->addWidget(m_messageLabel);
    ml->addWidget(m_message, 1);

    auto* split = new QSplitter(Qt::Vertical);
    split->addWidget(m_list);
    split->addWidget(messagePanel);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    split->setChildrenCollapsible(false);
    layout->addWidget(split, 1);

    m_summary = new QLabel;
    m_summary->setWordWrap(true);
    m_summary->setTextFormat(Qt::RichText);
    m_autostash = new QCheckBox(tr("Stash and reapply local changes"));
    m_autostash->setChecked(dirtyWorkTree);
    m_autostash->setVisible(dirtyWorkTree);
    layout->addWidget(m_summary);
    layout->addWidget(m_autostash);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    m_start = buttons->addButton(tr("Start Rebase"), QDialogButtonBox::AcceptRole);
    m_start->setIcon(theme::actionIcon(QStringLiteral("git-commit")));
    m_start->setDefault(true);
    layout->addWidget(buttons);

    // Keyboard: single-letter actions and Alt+arrows act on the list only.
    for (const ActionInfo& a : kActions) {
        auto* sc = new QShortcut(QKeySequence(QLatin1String(a.key)), m_list);
        sc->setContext(Qt::WidgetShortcut);
        connect(sc, &QShortcut::activated, this, [this, action = a.action] { setAction(action); });
    }
    for (const auto& [seq, delta] :
         {std::pair{QKeySequence(Qt::ALT | Qt::Key_Up), -1}, std::pair{QKeySequence(Qt::ALT | Qt::Key_Down), 1}}) {
        auto* sc = new QShortcut(seq, m_list);
        sc->setContext(Qt::WidgetShortcut);
        connect(sc, &QShortcut::activated, this, [this, d = delta] { moveSelection(d); });
    }
    connect(m_list, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QMenu menu(this);
        for (const ActionInfo& a : kActions) {
            menu.addAction(QCoreApplication::translate("InteractiveRebase", a.label), this,
                           [this, action = a.action] { setAction(action); })
                ->setShortcut(QKeySequence(QLatin1String(a.key)));
        }
        menu.exec(m_list->viewport()->mapToGlobal(pos));
    });
    connect(m_list, &QTreeWidget::currentItemChanged, this, &InteractiveRebaseDialog::showCurrentMessage);
    connect(m_list->model(), &QAbstractItemModel::rowsMoved, this, &InteractiveRebaseDialog::validate);
    connect(m_list->model(), &QAbstractItemModel::rowsInserted, this, &InteractiveRebaseDialog::validate);
    connect(m_message, &QPlainTextEdit::textChanged, this, &InteractiveRebaseDialog::onMessageEdited);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_start, &QPushButton::clicked, this, [this] {
        bool allDropped = true;
        for (const RebaseStep& s : steps()) allDropped &= s.action == RebaseAction::Drop;
        if (allDropped &&
            QMessageBox::warning(this, tr("Interactive Rebase"), tr("Every commit is set to Drop. Remove them all?"),
                                 QMessageBox::Yes | QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        accept();
    });

    if (m_list->topLevelItemCount() > 0) m_list->setCurrentItem(m_list->topLevelItem(0));
    m_list->setFocus();
    validate();
}

QTreeWidgetItem* InteractiveRebaseDialog::currentItem() const { return m_list->currentItem(); }

QVector<RebaseStep> InteractiveRebaseDialog::steps() const {
    QVector<RebaseStep> out;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        out.push_back(m_steps.value(m_list->topLevelItem(i)->data(0, Qt::UserRole).toString()));
    }
    return out;
}

bool InteractiveRebaseDialog::autostash() const { return m_autostash->isChecked(); }

void InteractiveRebaseDialog::refreshItem(QTreeWidgetItem* item) {
    const RebaseStep& s = m_steps[item->data(0, Qt::UserRole).toString()];
    item->setText(ActionColumn, label(s.action));
    QFont bold = m_list->font();
    bold.setBold(s.action != RebaseAction::Pick);
    item->setFont(ActionColumn, bold);
    const QColor color = actionColor(s.action);
    item->setForeground(ActionColumn, color.isValid() ? QBrush(color) : m_list->palette().text());
    const QString subject = s.action == RebaseAction::Reword ? s.message.section(QLatin1Char('\n'), 0, 0) : s.subject;
    item->setText(SubjectColumn, subject);
    QFont subjectFont = m_list->font();
    subjectFont.setStrikeOut(s.action == RebaseAction::Drop);
    subjectFont.setItalic(s.action == RebaseAction::Fixup);
    item->setFont(SubjectColumn, subjectFont);
    QColor dim = m_list->palette().color(QPalette::Text);
    if (s.action == RebaseAction::Drop || s.action == RebaseAction::Fixup) dim.setAlphaF(0.5f);
    item->setForeground(SubjectColumn, dim);
    item->setToolTip(SubjectColumn, s.message);
}

void InteractiveRebaseDialog::setAction(RebaseAction action) {
    const QList<QTreeWidgetItem*> items = m_list->selectedItems();
    for (QTreeWidgetItem* item : items) {
        RebaseStep& s = m_steps[item->data(0, Qt::UserRole).toString()];
        if (s.action == RebaseAction::Reword && action != RebaseAction::Reword) {
            s.message = m_originalMessages.value(s.sha);  // leaving reword discards the edited message
        }
        s.action = action;
        refreshItem(item);
    }
    showCurrentMessage();
    if (action == RebaseAction::Reword && items.size() == 1) m_message->setFocus();
    validate();
}

void InteractiveRebaseDialog::moveSelection(int delta) {
    QList<QTreeWidgetItem*> items = m_list->selectedItems();
    if (items.isEmpty()) return;
    std::sort(items.begin(), items.end(), [this](QTreeWidgetItem* a, QTreeWidgetItem* b) {
        return m_list->indexOfTopLevelItem(a) < m_list->indexOfTopLevelItem(b);
    });
    if (delta < 0 && m_list->indexOfTopLevelItem(items.first()) == 0) return;
    if (delta > 0 && m_list->indexOfTopLevelItem(items.last()) == m_list->topLevelItemCount() - 1) return;
    if (delta > 0) std::reverse(items.begin(), items.end());
    QTreeWidgetItem* current = m_list->currentItem();
    for (QTreeWidgetItem* item : items) {
        const int row = m_list->indexOfTopLevelItem(item);
        m_list->takeTopLevelItem(row);
        m_list->insertTopLevelItem(row + delta, item);
    }
    for (QTreeWidgetItem* item : items) item->setSelected(true);
    if (current) m_list->setCurrentItem(current, 0, QItemSelectionModel::NoUpdate);
    validate();
}

void InteractiveRebaseDialog::showCurrentMessage() {
    QTreeWidgetItem* item = currentItem();
    m_loadingMessage = true;
    if (!item) {
        m_message->clear();
        m_message->setReadOnly(true);
        m_messageLabel->clear();
    } else {
        const RebaseStep& s = m_steps[item->data(0, Qt::UserRole).toString()];
        m_message->setPlainText(s.message);
        const bool editable = s.action == RebaseAction::Pick || s.action == RebaseAction::Reword;
        m_message->setReadOnly(!editable);
        m_messageLabel->setText(editable ? tr("Message of %1 — edit it to reword the commit:").arg(s.sha.left(8))
                                         : tr("Message of %1 (%2):").arg(s.sha.left(8), label(s.action)));
    }
    m_loadingMessage = false;
}

void InteractiveRebaseDialog::onMessageEdited() {
    if (m_loadingMessage) return;
    QTreeWidgetItem* item = currentItem();
    if (!item) return;
    RebaseStep& s = m_steps[item->data(0, Qt::UserRole).toString()];
    if (s.action != RebaseAction::Pick && s.action != RebaseAction::Reword) return;
    s.message = m_message->toPlainText();
    if (s.action == RebaseAction::Pick) s.action = RebaseAction::Reword;
    refreshItem(item);
    validate();
}

void InteractiveRebaseDialog::validate() {
    const QVector<RebaseStep> plan = steps();
    const QString error = validateRebasePlan(plan);
    QHash<RebaseAction, int> counts;
    for (const RebaseStep& s : plan) ++counts[s.action];
    QStringList parts;
    for (const ActionInfo& a : kActions) {
        if (counts.value(a.action)) parts << QStringLiteral("%1 %2").arg(counts.value(a.action)).arg(label(a.action).toLower());
    }
    if (error.isEmpty()) {
        m_summary->setText(parts.join(QStringLiteral(" · ")));
    } else {
        m_summary->setText(QStringLiteral("<span style='color:#d1242f'>%1</span>").arg(error.toHtmlEscaped()));
    }
    m_start->setEnabled(error.isEmpty());
}

}  // namespace aslgc
