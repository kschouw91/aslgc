// SPDX-License-Identifier: MIT
#include "HostProcess.h"

#include <QFile>
#include <QProcess>
#include <QStandardPaths>

namespace aslgc::host {

namespace {
const QString kSpawn = QStringLiteral("flatpak-spawn");
}

bool isFlatpak() {
    static const bool flatpak = QFile::exists(QStringLiteral("/.flatpak-info"));
    return flatpak;
}

QString findExecutable(const QStringList& programs) {
    if (!isFlatpak()) {
        for (const QString& program : programs) {
            const QString path = QStandardPaths::findExecutable(program);
            if (!path.isEmpty()) return path;
        }
        return {};
    }
    // One round trip for all candidates: the host shell prints the first one it finds.
    QProcess shell;
    shell.start(kSpawn, QStringList{QStringLiteral("--host"), QStringLiteral("sh"), QStringLiteral("-c"),
                                    QStringLiteral("for p; do command -v \"$p\" >/dev/null && { echo \"$p\"; exit 0; }; "
                                                   "done; exit 1"),
                                    QStringLiteral("sh")} +
                            programs);
    if (!shell.waitForFinished(5000) || shell.exitStatus() != QProcess::NormalExit || shell.exitCode() != 0) return {};
    return QString::fromLocal8Bit(shell.readAllStandardOutput()).trimmed();
}

bool startDetached(const QString& program, const QStringList& args, const QString& workDir) {
    if (!isFlatpak()) return QProcess::startDetached(program, args, workDir);
    return QProcess::startDetached(
        kSpawn, QStringList{QStringLiteral("--host"), QStringLiteral("--directory=") + workDir, program} + args, workDir);
}

}  // namespace aslgc::host
