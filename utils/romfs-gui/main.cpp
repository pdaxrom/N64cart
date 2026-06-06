#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
#include <QStringList>
#include <QTranslator>

#include "mainwindow.h"

QStringList translationSearchPaths()
{
    QStringList paths;
    const QString appDir = QCoreApplication::applicationDirPath();
    paths << appDir;

    QDir dir(appDir);
    if (dir.cd(QStringLiteral("translations"))) {
        paths << dir.absolutePath();
    }

#if defined(Q_OS_MAC)
    QDir bundleDir(appDir);
    if (bundleDir.cdUp() && bundleDir.cd(QStringLiteral("Resources"))) {
        paths << bundleDir.absolutePath();
        QDir resourceTranslations = bundleDir;
        if (resourceTranslations.cd(QStringLiteral("translations"))) {
            paths << resourceTranslations.absolutePath();
        }
    }
#endif

    paths << QDir::currentPath();
    paths << QStringLiteral(":/translations");
    return paths;
}

QLocale applicationLocale()
{
    QSettings settings;
    const QString languageCode = settings.value(QStringLiteral("uiLanguage"),
                                                QStringLiteral("system"))
                                 .toString();
    if (languageCode.isEmpty() || languageCode == QStringLiteral("system")) {
        return QLocale::system();
    }
    return QLocale(languageCode);
}

void loadTranslations(QApplication &app, const QLocale &locale)
{
    static QTranslator appTranslator;
    static QTranslator qtTranslator;

    const QString qtTransPath =
        QLibraryInfo::path(QLibraryInfo::TranslationsPath);
    if (!qtTransPath.isEmpty()) {
        if (qtTranslator.load(locale, QStringLiteral("qtbase"), QStringLiteral("_"),
                              qtTransPath)) {
            app.installTranslator(&qtTranslator);
        }
    }

    const QStringList paths = translationSearchPaths();
    for (const QString &path : paths) {
        if (qtTranslator.load(locale, QStringLiteral("qtbase"), QStringLiteral("_"),
                              path)) {
            app.installTranslator(&qtTranslator);
        }
        if (appTranslator.load(locale, QStringLiteral("romfs-gui"),
                               QStringLiteral("_"), path)) {
            app.installTranslator(&appTranslator);
            break;
        }
    }
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("ROMFS Manager");
    QApplication::setOrganizationName("pdaXrom");
    QApplication::setApplicationDisplayName("ROMFS Manager");

    const QLocale locale = applicationLocale();
    QLocale::setDefault(locale);
    app.setLayoutDirection(locale.textDirection());
#ifndef __APPLE__
    QApplication::setWindowIcon(QIcon(":/icons/icon.svg"));
#endif

    loadTranslations(app, locale);

    MainWindow window;
    window.show();

    return QApplication::exec();
}
