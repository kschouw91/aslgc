// SPDX-License-Identifier: MIT
#include "Dialogs.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

namespace aslgc {

FormDialog::FormDialog(QWidget* parent, const QString& title, const QString& okText) : QDialog(parent) {
    setWindowTitle(title);
    auto* layout = new QVBoxLayout(this);
    m_form = new QFormLayout;
    m_form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    layout->addLayout(m_form);
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    if (!okText.isEmpty()) m_buttons->button(QDialogButtonBox::Ok)->setText(okText);
    layout->addWidget(m_buttons);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    setMinimumWidth(460);
}

void FormDialog::addNote(const QString& html) {
    auto* label = new QLabel(html);
    label->setWordWrap(true);
    label->setTextFormat(Qt::RichText);
    m_form->addRow(label);
}

QLineEdit* FormDialog::addLine(const QString& key, const QString& label, const QString& value, const QString& placeholder) {
    auto* edit = new QLineEdit(value);
    edit->setPlaceholderText(placeholder);
    m_form->addRow(label, edit);
    m_fields.insert(key, edit);
    connect(edit, &QLineEdit::textChanged, this, &FormDialog::revalidate);
    if (m_fields.size() == 1) edit->setFocus();
    return edit;
}

QComboBox* FormDialog::addCombo(const QString& key, const QString& label, const QStringList& items, const QString& current,
                                bool editable) {
    auto* combo = new QComboBox;
    combo->setEditable(editable);
    // An editable combo is a text field with suggestions: as wide as the line edits.
    if (editable) combo->setSizePolicy(QSizePolicy::Expanding, combo->sizePolicy().verticalPolicy());
    combo->addItems(items);
    if (!current.isEmpty()) {
        const int i = combo->findText(current);
        if (i >= 0)
            combo->setCurrentIndex(i);
        else if (editable)
            combo->setEditText(current);
    }
    m_form->addRow(label, combo);
    m_fields.insert(key, combo);
    connect(combo, &QComboBox::currentTextChanged, this, &FormDialog::revalidate);
    return combo;
}

QCheckBox* FormDialog::addCheck(const QString& key, const QString& text, bool checked) {
    auto* box = new QCheckBox(text);
    box->setChecked(checked);
    m_form->addRow(QString(), box);
    m_fields.insert(key, box);
    connect(box, &QCheckBox::toggled, this, &FormDialog::revalidate);
    return box;
}

QPlainTextEdit* FormDialog::addText(const QString& key, const QString& label, const QString& value) {
    auto* edit = new QPlainTextEdit(value);
    edit->setTabChangesFocus(true);
    edit->setMaximumHeight(110);
    m_form->addRow(label, edit);
    m_fields.insert(key, edit);
    return edit;
}

void FormDialog::addRadios(const QString& key, const QString& label, const QVector<QPair<QString, QString>>& options,
                           const QString& current) {
    auto* box = new QWidget;
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    auto* group = new QButtonGroup(this);
    QStringList values;
    for (int i = 0; i < options.size(); ++i) {
        auto* rb = new QRadioButton(options[i].second);
        rb->setChecked(options[i].first == current);
        group->addButton(rb, i);
        v->addWidget(rb);
        values << options[i].first;
    }
    if (!group->checkedButton() && !group->buttons().isEmpty()) group->buttons().first()->setChecked(true);
    m_form->addRow(label, box);
    m_radios.insert(key, group);
    m_radioValues.insert(key, values);
    connect(group, &QButtonGroup::idToggled, this, &FormDialog::revalidate);
}

void FormDialog::addWidgetRow(const QString& label, QWidget* widget) { m_form->addRow(label, widget); }

QString FormDialog::text(const QString& key) const {
    QWidget* w = m_fields.value(key);
    if (auto* e = qobject_cast<QLineEdit*>(w)) return e->text().trimmed();
    if (auto* c = qobject_cast<QComboBox*>(w)) return c->currentText().trimmed();
    if (auto* t = qobject_cast<QPlainTextEdit*>(w)) return t->toPlainText().trimmed();
    return {};
}

bool FormDialog::checked(const QString& key) const {
    auto* box = qobject_cast<QCheckBox*>(m_fields.value(key));
    return box && box->isChecked() && box->isEnabled();
}

QString FormDialog::choice(const QString& key) const {
    const QButtonGroup* group = m_radios.value(key);
    if (!group) return {};
    return m_radioValues.value(key).value(group->checkedId());
}

void FormDialog::setValidator(std::function<bool()> validator) {
    m_validator = std::move(validator);
    revalidate();
}

void FormDialog::revalidate() {
    if (m_validator) m_buttons->button(QDialogButtonBox::Ok)->setEnabled(m_validator());
}

bool confirm(QWidget* parent, const QString& title, const QString& text, const QString& okText, bool destructive) {
    QMessageBox box(destructive ? QMessageBox::Warning : QMessageBox::Question, title, text, QMessageBox::NoButton, parent);
    box.setTextFormat(Qt::RichText);
    QPushButton* ok = box.addButton(okText, destructive ? QMessageBox::DestructiveRole : QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(destructive ? qobject_cast<QPushButton*>(box.button(QMessageBox::Cancel)) : ok);
    box.exec();
    return box.clickedButton() == ok;
}

void showError(QWidget* parent, const QString& title, const QString& message, const QString& hint) {
    QMessageBox box(QMessageBox::Warning, title, QString(), QMessageBox::Ok, parent);
    if (!hint.isEmpty()) box.setInformativeText(hint);
    const QStringList lines = message.split(QLatin1Char('\n'));
    if (lines.size() > 12) {
        box.setText(lines.mid(0, 6).join(QLatin1Char('\n')) + QStringLiteral("\n…"));
        box.setDetailedText(message);
    } else {
        box.setText(message);
    }
    box.exec();
}

}  // namespace aslgc
