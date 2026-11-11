// SPDX-License-Identifier: MIT
#include "OperationDialog.h"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QTimer>
#include <QVBoxLayout>

namespace aslgc {

OperationDialog::OperationDialog(QWidget* parent, const QString& title, const Git& git, const QStringList& args)
    : QDialog(parent), m_process(new QProcess(this)) {
    setWindowTitle(title);
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowModality(Qt::WindowModal);
    setMinimumWidth(520);

    auto* layout = new QVBoxLayout(this);
    auto* heading = new QLabel(QStringLiteral("<b>%1</b>").arg(title.toHtmlEscaped()));
    m_status = new QLabel(tr("Starting…"));
    m_status->setWordWrap(true);
    m_bar = new QProgressBar;
    m_bar->setRange(0, 0);
    m_bar->setTextVisible(false);
    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_log->setMinimumHeight(140);
    m_log->setVisible(false);
    auto* details = new QPushButton(tr("Show Output"));
    details->setCheckable(true);
    details->setFlat(true);
    connect(details, &QPushButton::toggled, this, [this, details](bool on) {
        m_log->setVisible(on);
        details->setText(on ? tr("Hide Output") : tr("Show Output"));
        adjustSize();
    });
    m_button = new QPushButton(tr("Cancel"));
    connect(m_button, &QPushButton::clicked, this, &OperationDialog::reject);
    auto* row = new QHBoxLayout;
    row->addWidget(details);
    row->addStretch();
    row->addWidget(m_button);

    layout->addWidget(heading);
    layout->addWidget(m_status);
    layout->addWidget(m_bar);
    layout->addWidget(m_log, 1);
    layout->addLayout(row);

    git.prepare(*m_process, args);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::readyRead, this, &OperationDialog::onOutput);
    connect(m_process, &QProcess::finished, this, &OperationDialog::onFinished);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) onFinished(-1, QProcess::CrashExit);
    });
    m_log->appendPlainText(QStringLiteral("$ git ") + args.join(QLatin1Char(' ')));
}

void OperationDialog::start() {
    m_running = true;
    m_process->start();
    m_process->closeWriteChannel();
    show();
}

void OperationDialog::onOutput() {
    m_pending += m_process->readAll();
    int start = 0;
    for (int i = 0; i < m_pending.size(); ++i) {
        const char ch = m_pending[i];
        if (ch == '\n' || ch == '\r') {
            handleSegment(QString::fromUtf8(m_pending.mid(start, i - start)), ch == '\n');
            start = i + 1;
        }
    }
    m_pending.remove(0, start);
}

void OperationDialog::handleSegment(const QString& segment, bool complete) {
    static const QRegularExpression percentRe(QStringLiteral("^(.*?):\\s+(\\d+)%"));
    const QString s = segment.trimmed();
    if (s.isEmpty()) return;
    if (const auto m = percentRe.match(s); m.hasMatch()) {
        m_bar->setRange(0, 100);
        m_bar->setValue(m.captured(2).toInt());
        m_status->setText(m.captured(1).remove(QStringLiteral("remote: ")) + QStringLiteral("…"));
    } else if (!complete) {
        m_status->setText(s);
    }
    if (complete) {
        m_log->appendPlainText(s);
        m_output += s + QLatin1Char('\n');
        if (!percentRe.match(s).hasMatch()) m_status->setText(s);
    }
}

void OperationDialog::onFinished(int exitCode, QProcess::ExitStatus status) {
    if (m_done) return;
    m_done = true;
    m_running = false;
    if (!m_pending.isEmpty()) {
        handleSegment(QString::fromUtf8(m_pending), true);
        m_pending.clear();
    }
    const bool ok = !m_cancelled && status == QProcess::NormalExit && exitCode == 0;
    m_bar->setRange(0, 100);
    m_bar->setValue(100);
    if (ok) {
        m_status->setText(tr("Done."));
        emit completed(true);
        QTimer::singleShot(250, this, &QDialog::accept);
        return;
    }
    if (m_cancelled) {
        m_status->setText(tr("Cancelled."));
        emit completed(false);
        QTimer::singleShot(250, this, &QDialog::accept);
        return;
    }
    if (status != QProcess::NormalExit && m_output.isEmpty()) {
        m_log->appendPlainText(m_process->errorString());
        m_output = m_process->errorString();
    }
    m_status->setText(
        QStringLiteral("<span style='color:#d1242f'><b>%1</b></span>").arg(tr("Failed.")) + QStringLiteral(" ") +
        m_output.trimmed().section(QLatin1Char('\n'), -3).toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
    m_status->setTextFormat(Qt::RichText);
    m_log->setVisible(true);
    m_button->setText(tr("Close"));
    adjustSize();
    qWarning().noquote() << windowTitle() << "failed:" << m_output.trimmed();
    emit completed(false);
}

void OperationDialog::reject() {
    if (m_running) {
        m_cancelled = true;
        m_status->setText(tr("Cancelling…"));
        m_process->terminate();
        QTimer::singleShot(3000, m_process, [p = m_process] {
            if (p->state() != QProcess::NotRunning) p->kill();
        });
        return;
    }
    QDialog::reject();
}

}  // namespace aslgc
