// SPDX-License-Identifier: MIT
#pragma once

#include "git/Git.h"

#include <QDialog>
#include <QProcess>

class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

namespace aslgc {

// Runs a long git command (fetch, pull, push, clone) with live progress.
// Closes itself on success; stays open with the full log on failure.
class OperationDialog : public QDialog {
    Q_OBJECT
public:
    OperationDialog(QWidget* parent, const QString& title, const Git& git, const QStringList& args);
    void start();

signals:
    void completed(bool ok);

protected:
    void reject() override;

private:
    void onOutput();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void handleSegment(const QString& segment, bool complete);

    QProcess* m_process;
    QLabel* m_status;
    QProgressBar* m_bar;
    QPlainTextEdit* m_log;
    QPushButton* m_button;
    QByteArray m_pending;
    QString m_output;
    bool m_running = false;
    bool m_cancelled = false;
    bool m_done = false;
};

}  // namespace aslgc
