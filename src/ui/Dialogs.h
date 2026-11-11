// SPDX-License-Identifier: MIT
#pragma once

#include <QDialog>
#include <QHash>
#include <QPair>
#include <QStringList>
#include <functional>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QFormLayout;
class QLineEdit;
class QPlainTextEdit;
class QVBoxLayout;

namespace aslgc {

// Small declarative form builder used by all simple dialogs.
class FormDialog : public QDialog {
    Q_OBJECT
public:
    FormDialog(QWidget* parent, const QString& title, const QString& okText = QString());

    void addNote(const QString& html);
    QLineEdit* addLine(const QString& key, const QString& label, const QString& value = {}, const QString& placeholder = {});
    QComboBox* addCombo(const QString& key, const QString& label, const QStringList& items, const QString& current = {},
                        bool editable = false);
    QCheckBox* addCheck(const QString& key, const QString& text, bool checked = false);
    QPlainTextEdit* addText(const QString& key, const QString& label, const QString& value = {});
    // options: (value, label) pairs
    void addRadios(const QString& key, const QString& label, const QVector<QPair<QString, QString>>& options,
                   const QString& current);
    void addWidgetRow(const QString& label, QWidget* widget);

    QString text(const QString& key) const;
    bool checked(const QString& key) const;
    QString choice(const QString& key) const;
    QWidget* field(const QString& key) const { return m_fields.value(key); }

    // OK is enabled only while the validator returns true.
    void setValidator(std::function<bool()> validator);
    void revalidate();

private:
    QFormLayout* m_form;
    QDialogButtonBox* m_buttons;
    QHash<QString, QWidget*> m_fields;
    QHash<QString, QButtonGroup*> m_radios;
    QHash<QString, QStringList> m_radioValues;
    std::function<bool()> m_validator;
};

bool confirm(QWidget* parent, const QString& title, const QString& text, const QString& okText, bool destructive = false);
// `hint` (optional) is shown under the message, never folded into the details.
void showError(QWidget* parent, const QString& title, const QString& message, const QString& hint = {});

}  // namespace aslgc
