// SPDX-License-Identifier: MIT
#pragma once

#include <QWidget>

class QLineEdit;
class QListWidget;

namespace aslgc {

// Start page shown in a new tab: open / clone / init, plus recent repositories.
class WelcomeWidget : public QWidget {
    Q_OBJECT
public:
    explicit WelcomeWidget(QWidget* parent = nullptr);
    void setRecent(const QStringList& paths);

signals:
    void openPath(const QString& path);
    void openRequested();
    void cloneRequested();
    void initRequested();
    void forgetPath(const QString& path);

private:
    void applyFilter();

    QLineEdit* m_filter;
    QListWidget* m_recent;
};

}  // namespace aslgc
