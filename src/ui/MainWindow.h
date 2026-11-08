// SPDX-License-Identifier: MIT
#pragma once

#include <QMainWindow>

class QAction;
class QLabel;
class QTabWidget;

namespace aslgc {

class RepoView;
class WelcomeWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();

    bool openRepository(const QString& path, bool replaceCurrentWelcome = true);
    void restoreSession();
    RepoView* currentRepo() const;

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void createActions();
    void updateActions();
    void newTab();
    void closeTab(int index);
    void openDialog();
    void cloneDialog();
    void initDialog();
    void addRecent(const QString& path);
    void forgetRecent(const QString& path);
    QStringList recent() const;
    void refreshWelcomePages();
    void saveSession();
    WelcomeWidget* makeWelcome();
    void about();

    QTabWidget* m_tabs;
    QLabel* m_status;
    QAction* m_fetch;
    QAction* m_pull;
    QAction* m_push;
    QAction* m_stash;
    QAction* m_pop;
    QAction* m_branch;
    QAction* m_gitFlow;
    QAction* m_terminal;
    QAction* m_folder;
    QAction* m_refresh;
    QList<QAction*> m_repoActions;
};

}  // namespace aslgc
