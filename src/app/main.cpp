#include <QDir>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QtQml/qqmlextensionplugin.h>

#include <cstdlib>

import Template.App.Context;

// Template.Ui is a static QML plugin: importing it registers its types, resources and compiled QML.
Q_IMPORT_QML_PLUGIN(Template_UiPlugin)

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Example"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("example.org"));
    QCoreApplication::setApplicationName(QStringLiteral("QtTemplate"));
    QCoreApplication::setApplicationVersion(QStringLiteral(APP_VERSION));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Qt Template"));
    QQuickStyle::setStyle(QStringLiteral("Fusion"));

    const auto dataDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    ApplicationContext context(QDir(dataDirectory).filePath(QStringLiteral("mmkv")));

    // The engine is destroyed first; injected C++ objects outlive all QML views.
    QQmlApplicationEngine engine;
    engine.setInitialProperties({{"appContext", QVariant::fromValue(&context)}});
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, [] { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
    engine.loadFromModule("Template.Ui", "Main");
    context.start();
    return app.exec();
}
