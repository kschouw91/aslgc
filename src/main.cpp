// SPDX-License-Identifier: MIT
#include "git/Git.h"
#include "git/Rebase.h"
#include "hosting/AccountStore.h"
#include "ui/AccountsDialog.h"
#include "ui/FileHistoryWindow.h"
#include "ui/InteractiveRebaseDialog.h"
#include "ui/Log.h"
#include "ui/MainWindow.h"
#include "ui/RepoView.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QLocale>
#include <QMessageBox>
#include <QRegularExpression>
#include <QTimer>
#include <QTranslator>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("aslgc"));
    QApplication::setOrganizationName(QStringLiteral("aslgc"));
    QApplication::setApplicationVersion(QStringLiteral(ASLGC_VERSION));
    QApplication::setDesktopFileName(QStringLiteral(ASLGC_APP_ID));
    QApplication::setWindowIcon(aslgc::theme::appIcon());
    aslgc::log::install();

    // UI translations (falls back to English, which supplies plural forms), plus Qt's own strings.
    auto* translator = new QTranslator(&app);
    if (translator->load(QLocale(), QStringLiteral("aslgc"), QStringLiteral("_"), QStringLiteral(":/i18n")) ||
        translator->load(QStringLiteral(":/i18n/aslgc_en"))) {
        QApplication::installTranslator(translator);
    }
    auto* qtTranslator = new QTranslator(&app);
    if (qtTranslator->load(QLocale(), QStringLiteral("qtbase"), QStringLiteral("_"),
                           QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        QApplication::installTranslator(qtTranslator);
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("A simple Linux Git client"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("path"), QStringLiteral("Repository to open."), QStringLiteral("[path...]"));
    parser.process(app);

    const aslgc::GitResult version = aslgc::Git().run({QStringLiteral("--version")});
    if (!version.ok()) {
        QMessageBox::critical(nullptr, QStringLiteral("aslgc"),
                              QStringLiteral("aslgc needs Git, but it could not be started.\n\n%1").arg(version.message()));
        return 1;
    }
    // "git version 2.43.0" → warn below 2.31 (GIT_CONFIG_COUNT, pathspec-from-file, restore).
    static const QRegularExpression versionRe(QStringLiteral("(\\d+)\\.(\\d+)"));
    if (const auto m = versionRe.match(version.outText()); m.hasMatch()) {
        const int major = m.captured(1).toInt();
        const int minor = m.captured(2).toInt();
        if (major < 2 || (major == 2 && minor < 31)) {
            QMessageBox::warning(nullptr, QStringLiteral("aslgc"),
                                 QStringLiteral("aslgc needs Git 2.31 or newer, but found %1.\n"
                                                "Some operations will fail until Git is updated.")
                                     .arg(version.outText().trimmed()));
        }
    }

    // Credential helper: next to the binary in a build tree, in libexec when installed.
    const QDir appDir(QCoreApplication::applicationDirPath());
    for (const QString& candidate : {appDir.filePath(QStringLiteral("aslgc-askpass")),
                                     QDir::cleanPath(appDir.filePath(QStringLiteral(ASLGC_ASKPASS_FROM_BIN)))}) {
        if (QFileInfo(candidate).isExecutable()) {
            aslgc::Git::setAskPassProgram(candidate);
            break;
        }
    }
    aslgc::AccountStore::instance();  // loads accounts and wires their hosts into git's credential lookup

    aslgc::MainWindow window;
    const QStringList paths = parser.positionalArguments();
    window.restoreSession();
    for (const QString& p : paths) window.openRepository(QFileInfo(p).absoluteFilePath(), true);
    window.show();

    // Developer aid: ASLGC_SCREENSHOT=/path.png [ASLGC_VIEW=history|changes|accounts|add-github|rebase|file-history|blame]
    // [ASLGC_FILE=path] grabs the window (or the dialog / file history window) and exits.
    const QString shot = qEnvironmentVariable("ASLGC_SCREENSHOT");
    if (!shot.isEmpty()) {
        const QString view = qEnvironmentVariable("ASLGC_VIEW");
        QWidget* target = &window;
        if (view == QLatin1String("accounts")) {
            auto* accounts = new aslgc::AccountsDialog(&window);
            accounts->show();
            target = accounts;
        } else if (view == QLatin1String("rebase") && window.currentRepo()) {
            const aslgc::RebaseRange range = aslgc::loadRebaseRange(
                window.currentRepo()->repo(), qEnvironmentVariable("ASLGC_REBASE_BASE", QStringLiteral("HEAD~3")));
            auto* rebase =
                new aslgc::InteractiveRebaseDialog(QStringLiteral("Rebase <b>main</b> (screenshot)"), range, true, &window);
            rebase->show();
            target = rebase;
        } else if (view == QLatin1String("add-github")) {
            auto* add = new aslgc::AddAccountDialog(aslgc::Provider::GitHub, &window);
            add->show();
            target = add;
        }
        const QString file = qEnvironmentVariable("ASLGC_FILE");
        QTimer::singleShot(600, &window, [&window, view, file] {
            if (aslgc::RepoView* r = window.currentRepo()) {
                if (view == QLatin1String("history"))
                    r->showHistory();
                else if (view == QLatin1String("changes"))
                    r->showChanges();
                else if (view == QLatin1String("file-history"))
                    r->showFileHistory(file, file.isEmpty() || file.endsWith(QLatin1Char('/')));
                else if (view == QLatin1String("blame"))
                    r->showBlame(file);
            }
        });
        QTimer::singleShot(qEnvironmentVariableIntValue("ASLGC_SCREENSHOT_DELAY") > 0
                               ? qEnvironmentVariableIntValue("ASLGC_SCREENSHOT_DELAY")
                               : 2500,
                           &window, [&window, target, shot] {
                               QWidget* w = target;
                               if (aslgc::RepoView* r = window.currentRepo()) {
                                   if (auto* files = r->findChild<aslgc::FileHistoryWindow*>()) w = files;
                               }
                               w->grab().save(shot);
                               QApplication::quit();
                           });
    }
    return app.exec();
}
