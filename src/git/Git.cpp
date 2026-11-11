// SPDX-License-Identifier: MIT
#include "Git.h"

#include <QMutex>
#include <QStandardPaths>

namespace aslgc {

namespace {
QMutex g_askPassMutex;
QString g_askPass;
QStringList g_credentialHosts;
}  // namespace

QString GitResult::message() const {
    const QString o = QString::fromUtf8(out).trimmed();
    const QString e = QString::fromUtf8(err).trimmed();
    QString text;
    if (!o.isEmpty()) text = o;
    if (!e.isEmpty()) text = text.isEmpty() ? e : text + QLatin1Char('\n') + e;
    if (text.isEmpty()) text = QStringLiteral("git exited with code %1").arg(exitCode);
    return text;
}

QString Git::program() {
    static const QString path = [] {
        QString p = qEnvironmentVariable("ASLGC_GIT");
        if (!p.isEmpty()) return p;
        p = QStandardPaths::findExecutable(QStringLiteral("git"));
        return p.isEmpty() ? QStringLiteral("git") : p;
    }();
    return path;
}

QStringList Git::globalArgs() {
    return {QStringLiteral("--no-pager"),
            QStringLiteral("-c"),
            QStringLiteral("core.quotepath=false"),
            QStringLiteral("-c"),
            QStringLiteral("color.ui=false"),
            QStringLiteral("-c"),
            QStringLiteral("i18n.logOutputEncoding=UTF-8"),
            QStringLiteral("-c"),
            QStringLiteral("log.showSignature=false")};
}

QProcessEnvironment Git::environment() {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // Stable, parseable English output while keeping UTF-8 everywhere.
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
    env.remove(QStringLiteral("LANGUAGE"));
    // Never block on a terminal prompt or pager; GUI handles credentials.
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    env.insert(QStringLiteral("GIT_PAGER"), QStringLiteral("cat"));
    // Read-only commands (status) must not rewrite the index; avoids refresh loops.
    env.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
    // Accept default messages when git would otherwise open an editor.
    env.insert(QStringLiteral("GIT_EDITOR"), QStringLiteral("true"));
    // Interactive rebases supply their todo list through `-c sequence.editor`.
    env.remove(QStringLiteral("GIT_SEQUENCE_EDITOR"));
    env.insert(QStringLiteral("GIT_MERGE_AUTOEDIT"), QStringLiteral("no"));
    const QString askPass = askPassProgram();
    if (!askPass.isEmpty()) {
        env.insert(QStringLiteral("GIT_ASKPASS"), askPass);
        env.insert(QStringLiteral("SSH_ASKPASS"), askPass);
        env.insert(QStringLiteral("SSH_ASKPASS_REQUIRE"), QStringLiteral("force"));

        // For hosts with a signed-in account, reset the credential helper list for that URL and use
        // ours, which answers with the account's token from the system keyring. Injected through
        // GIT_CONFIG_* so the user's own configuration files are never modified.
        const QStringList hosts = credentialHosts();
        if (!hosts.isEmpty()) {
            bool ok = false;
            int n = env.value(QStringLiteral("GIT_CONFIG_COUNT")).toInt(&ok);
            if (!ok || n < 0) n = 0;
            QString quoted = askPass;
            quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
            const QString helper = QStringLiteral("!'%1' credential").arg(quoted);
            auto add = [&env, &n](const QString& key, const QString& value) {
                env.insert(QStringLiteral("GIT_CONFIG_KEY_%1").arg(n), key);
                env.insert(QStringLiteral("GIT_CONFIG_VALUE_%1").arg(n), value);
                ++n;
            };
            for (const QString& url : hosts) {
                const QString key = QStringLiteral("credential.%1.helper").arg(url);
                add(key, QString());
                add(key, helper);
            }
            env.insert(QStringLiteral("GIT_CONFIG_COUNT"), QString::number(n));
        }
    }
    return env;
}

void Git::setAskPassProgram(const QString& path) {
    QMutexLocker lock(&g_askPassMutex);
    g_askPass = path;
}

QString Git::askPassProgram() {
    QMutexLocker lock(&g_askPassMutex);
    return g_askPass;
}

void Git::setCredentialHosts(const QStringList& baseUrls) {
    QMutexLocker lock(&g_askPassMutex);
    g_credentialHosts = baseUrls;
}

QStringList Git::credentialHosts() {
    QMutexLocker lock(&g_askPassMutex);
    return g_credentialHosts;
}

QByteArray Git::nulJoin(const QStringList& items) {
    QByteArray out;
    for (const QString& s : items) {
        out += s.toUtf8();
        out += '\0';
    }
    return out;
}

void Git::prepare(QProcess& process, const QStringList& args) const {
    process.setProgram(program());
    process.setArguments(globalArgs() + args);
    process.setProcessEnvironment(environment());
    if (!m_workDir.isEmpty()) process.setWorkingDirectory(m_workDir);
}

GitResult Git::run(const QStringList& args, const QByteArray& input) const {
    GitResult result;
    QProcess process;
    prepare(process, args);
    process.start();
    if (!process.waitForStarted(15000)) {
        result.err = QStringLiteral("Failed to start git (%1): %2").arg(program(), process.errorString()).toUtf8();
        return result;
    }
    if (!input.isEmpty()) process.write(input);
    process.closeWriteChannel();
    process.waitForFinished(-1);
    result.out = process.readAllStandardOutput();
    result.err = process.readAllStandardError();
    if (process.exitStatus() == QProcess::NormalExit) {
        result.exitCode = process.exitCode();
    } else {
        result.exitCode = -1;
        if (result.err.isEmpty()) result.err = "git terminated unexpectedly";
    }
    return result;
}

}  // namespace aslgc
