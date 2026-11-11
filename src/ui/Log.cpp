// SPDX-License-Identifier: MIT
#include "Log.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>

namespace aslgc::log {

namespace {

constexpr qint64 kMaxBytes = 1024 * 1024;

QMutex g_mutex;
QFile* g_file = nullptr;  // intentionally never destroyed: messages may arrive during shutdown
QtMessageHandler g_previous = nullptr;

const char* levelName(QtMsgType type) {
    switch (type) {
        case QtDebugMsg: return "debug";
        case QtInfoMsg: return "info";
        case QtWarningMsg: return "warning";
        case QtCriticalMsg: return "critical";
        case QtFatalMsg: return "fatal";
    }
    return "?";
}

void handler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    if (type != QtDebugMsg) {
        QMutexLocker lock(&g_mutex);
        if (g_file) {
            g_file->write(QDateTime::currentDateTime().toString(Qt::ISODateWithMs).toUtf8() + ' ' + levelName(type) + ": " +
                          message.toUtf8() + '\n');
            g_file->flush();
        }
    }
    if (g_previous) g_previous(type, context, message);
}

}  // namespace

QString directory() {
    QString base = qEnvironmentVariable("XDG_STATE_HOME");
    if (base.isEmpty()) base = QDir::homePath() + QStringLiteral("/.local/state");
    return base + QStringLiteral("/aslgc");
}

void install() {
    const QString dir = directory();
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/aslgc.log");
    if (QFileInfo(path).size() > kMaxBytes) {
        QFile::remove(path + QStringLiteral(".1"));
        QFile::rename(path, path + QStringLiteral(".1"));
    }
    auto* file = new QFile(path);
    if (file->open(QIODevice::Append | QIODevice::Text)) {
        QMutexLocker lock(&g_mutex);
        g_file = file;
    } else {
        delete file;
    }
    g_previous = qInstallMessageHandler(handler);
    qInfo("aslgc %s started (Qt %s)", ASLGC_VERSION, qVersion());
}

}  // namespace aslgc::log
