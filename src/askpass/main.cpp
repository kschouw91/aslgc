// SPDX-License-Identifier: MIT
// aslgc-askpass: credential prompt used by git (GIT_ASKPASS) and ssh (SSH_ASKPASS).
//
//   aslgc-askpass <prompt>            show a dialog, print the answer, exit 1 when cancelled
//   aslgc-askpass credential <action> git credential helper: answers `get` for hosts that have
//                                     a signed-in account, using the token from the keyring

#include "hosting/Account.h"

#include <qt6keychain/keychain.h>
#include <QApplication>
#include <QCoreApplication>
#include <QEventLoop>
#include <QInputDialog>
#include <QLineEdit>
#include <QMap>
#include <QMessageBox>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

namespace {

void setIdentity() {
    QCoreApplication::setOrganizationName(QStringLiteral("aslgc"));
    QCoreApplication::setApplicationName(QStringLiteral("aslgc"));
}

int credentialHelper(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    setIdentity();
    const QString action = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString();

    QMap<QString, QString> request;
    std::string line;
    while (std::getline(std::cin, line) && !line.empty()) {
        const QString l = QString::fromUtf8(line);
        const int eq = l.indexOf(QLatin1Char('='));
        if (eq > 0) request.insert(l.left(eq), l.mid(eq + 1));
    }
    if (action != QLatin1String("get")) return 0;  // store / erase: the keyring is managed by aslgc
    const QString protocol = request.value(QStringLiteral("protocol"));
    if (protocol != QLatin1String("https") && protocol != QLatin1String("http")) return 0;

    const QVector<aslgc::Account> accounts = aslgc::accounts::load();
    const aslgc::Account* account =
        aslgc::accounts::findForHost(accounts, request.value(QStringLiteral("host")), request.value(QStringLiteral("username")));
    if (!account) return 0;

    QKeychain::ReadPasswordJob job(QLatin1String(aslgc::kKeychainService));
    job.setAutoDelete(false);
    job.setKey(account->keychainKey());
    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();
    if (job.error() != QKeychain::NoError || job.textData().isEmpty()) return 0;

    const QByteArray out = "username=" + account->username.toUtf8() + "\npassword=" + job.textData().toUtf8() + "\n";
    std::fwrite(out.constData(), 1, size_t(out.size()), stdout);
    return 0;
}

int prompt(int argc, char** argv) {
    QApplication app(argc, argv);
    setIdentity();
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("dialog-password")));

    const QString text = argc > 1 ? QString::fromLocal8Bit(argv[1]).trimmed() : QStringLiteral("Password:");
    const QString title = QStringLiteral("aslgc — Authentication");

    // SSH host key confirmation: "Are you sure you want to continue connecting (yes/no/[fingerprint])?"
    if (text.contains(QLatin1String("(yes/no"))) {
        const auto answer = QMessageBox::question(nullptr, title, text, QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        std::fputs(answer == QMessageBox::Yes ? "yes\n" : "no\n", stdout);
        return 0;
    }

    const bool secret = text.contains(QLatin1String("password"), Qt::CaseInsensitive) ||
                        text.contains(QLatin1String("passphrase"), Qt::CaseInsensitive) ||
                        text.contains(QLatin1String("token"), Qt::CaseInsensitive) ||
                        text.contains(QLatin1String("PIN"), Qt::CaseSensitive);
    QInputDialog dialog;
    dialog.setWindowTitle(title);
    dialog.setLabelText(text);
    dialog.setTextEchoMode(secret ? QLineEdit::Password : QLineEdit::Normal);
    dialog.setMinimumWidth(420);
    if (dialog.exec() != QDialog::Accepted) return 1;
    const QByteArray value = dialog.textValue().toUtf8() + '\n';
    std::fwrite(value.constData(), 1, size_t(value.size()), stdout);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "credential") == 0) return credentialHelper(argc, argv);
    return prompt(argc, argv);
}
