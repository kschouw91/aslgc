// SPDX-License-Identifier: MIT
#pragma once

#include <QByteArray>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

namespace aslgc {

struct GitResult {
    int exitCode = -1;
    QByteArray out;
    QByteArray err;

    bool ok() const { return exitCode == 0; }
    QString outText() const { return QString::fromUtf8(out); }
    // Human readable output for error dialogs (stdout + stderr).
    QString message() const;
};

struct GitCommand {
    QStringList args;
    QByteArray input;
};

// Thin, thread-safe wrapper around the git executable. Each call spawns its own
// process, so a Git value can be copied freely into worker threads.
class Git {
public:
    Git() = default;
    explicit Git(QString workDir) : m_workDir(std::move(workDir)) {}

    const QString& workDir() const { return m_workDir; }

    // Runs git synchronously (call from worker threads for anything non-trivial).
    GitResult run(const QStringList& args, const QByteArray& input = {}) const;
    // Configures program, arguments, environment and working directory.
    void prepare(QProcess& process, const QStringList& args) const;

    static QString program();
    static QStringList globalArgs();
    static QProcessEnvironment environment();
    static void setAskPassProgram(const QString& path);
    static QString askPassProgram();
    // Base URLs (e.g. "https://github.com") whose HTTPS credentials come from signed-in
    // accounts: git is pointed at `<askpass> credential` for those hosts only.
    static void setCredentialHosts(const QStringList& baseUrls);
    static QStringList credentialHosts();
    static QByteArray nulJoin(const QStringList& items);

private:
    QString m_workDir;
};

}  // namespace aslgc
