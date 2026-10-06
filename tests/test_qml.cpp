#include "ui/initialize.h"
#include "test_qml.h"
#include <QDir>
#include <QFile>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>

import Template.App.Context;

namespace {
QQuickItem *findItem(QQuickItem *root, const QString &name)
{
    if (root->objectName() == name) return root;
    for (auto *child : root->childItems())
        if (auto *found = findItem(child, name)) return found;
    return nullptr;
}
void clickItem(QQuickWindow *window, QQuickItem *item)
{
    const auto center = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center.toPoint());
}
}
void QmlTest::initTestCase()
{
    initializeTemplateUi();
    QQuickStyle::setStyle("Fusion");
}

void QmlTest::viewCommandsAndSaveFailure()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("mmkv");
    ApplicationContext context(path);
    auto &viewModel = *context.tasks();
    QQmlApplicationEngine engine;
    QList<QQmlError> warnings;
    connect(&engine, &QQmlEngine::warnings, this, [&warnings](const auto &errors) { warnings.append(errors); });
    engine.setInitialProperties({{"appContext", QVariant::fromValue(&context)}});
    engine.loadFromModule("Template.Ui", "Main");
    QCOMPARE(engine.rootObjects().size(), 1);
    QVERIFY(context.start());
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    QVERIFY(window);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QTRY_VERIFY(!viewModel.busy());
    auto *input = findItem(window->contentItem(), "taskInput");
    auto *add = findItem(window->contentItem(), "addTaskButton");
    QVERIFY(input && add);
    QVERIFY(!add->isEnabled());
    input->setProperty("text", "Created from QML");
    QVERIFY(add->isEnabled());
    clickItem(window, add);
    QTRY_COMPARE(viewModel.totalCount(), 1);
    QTRY_VERIFY(!viewModel.busy());
    QTRY_COMPARE(input->property("text").toString(), QString());
    QTRY_VERIFY(findItem(window->contentItem(), "completionCheckBox"));
    auto *checkBox = findItem(window->contentItem(), "completionCheckBox");
    clickItem(window, checkBox);
    QTRY_VERIFY(!viewModel.busy());
    QCOMPARE(viewModel.remainingCount(), 0);
    QVERIFY(checkBox->property("checked").toBool());
    const auto offline = path + ".offline";
    QVERIFY(QDir().rename(path, offline));
    QFile blocker(path);
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    clickItem(window, checkBox);
    QTRY_VERIFY(!viewModel.busy());
    QCOMPARE(viewModel.remainingCount(), 0);
    QVERIFY(checkBox->property("checked").toBool());
    auto *error = findItem(window->contentItem(), "errorLabel");
    QVERIFY(error);
    QTRY_VERIFY(error->isVisible());
    QVERIFY(!error->property("text").toString().isEmpty());
    input->setProperty("text", "Retain input on failure");
    clickItem(window, add);
    QTRY_VERIFY(!viewModel.busy());
    QCOMPARE(viewModel.totalCount(), 1);
    QCOMPARE(input->property("text").toString(), QString("Retain input on failure"));
    QVERIFY(QFile::remove(path));
    QVERIFY(QDir().rename(offline, path));
    auto *remove = findItem(window->contentItem(), "removeTaskButton");
    QVERIFY(remove);
    clickItem(window, remove);
    QTRY_COMPARE(viewModel.totalCount(), 0);
    QTRY_VERIFY(!viewModel.busy());
    QVERIFY(viewModel.errorMessage().isEmpty());
    QVERIFY2(warnings.isEmpty(), warnings.isEmpty() ? "" : qPrintable(warnings.first().toString()));
}

void QmlTest::viewRequiresInjectedContext()
{
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.loadFromModule("Template.Ui", "Main");
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    QScopedPointer<QObject> root(component.create());
    QVERIFY(!root);
    QVERIFY(component.errorString().contains("Required property appContext"));
}

void QmlTest::registeredDependenciesAreProvidedByCpp()
{
    QQmlEngine engine;
    QQmlComponent context(&engine);
    context.setData("import Template.Ui\nApplicationContext {}", QUrl());
    QVERIFY(context.isError());
    QVERIFY(context.errorString().contains("The application provides the context and its ViewModels."));

    QQmlComponent viewModel(&engine);
    viewModel.setData("import Template.Ui\nTaskViewModel {}", QUrl());
    QVERIFY(viewModel.isError());
    QVERIFY(viewModel.errorString().contains("ApplicationContext provides the ViewModel."));
}
QTEST_MAIN(QmlTest)
