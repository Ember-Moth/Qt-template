#include "test_core.h"
#include <QAbstractItemModelTester>
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

import std;
import Template.ViewModels.Task;
import Template.Tasks;
import Template.Storage.Mmkv;
import Template.App.Context;
import Template.Runtime.Asio;

namespace {
auto nativePath(const QString &directory)
{
#ifdef _WIN32
    return std::filesystem::path(directory.toStdWString());
#else
    return std::filesystem::path(directory.toStdString());
#endif
}
struct TaskFixture
{
    runtime::AsioRuntime asyncRuntime;
    std::shared_ptr<business::TaskService> service;
    explicit TaskFixture(const QString &directory)
        : service(std::make_shared<business::TaskService>(asyncRuntime.executor(),
              std::make_shared<storage::MmkvStore>(nativePath(directory)))) {}
    auto dependencies() const -> TaskViewModel::Dependencies { return {service}; }
};
} // namespace

void ViewModelTest::serviceIsInjected()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    runtime::AsioRuntime asyncRuntime;
    auto store = std::make_shared<storage::MmkvStore>(nativePath(directory.filePath("mmkv")));
    const auto existing = storage::Strings{"injected", "Provided by the application", "0"};
    QVERIFY(store->setStrings("tasks.items", existing).has_value());
    auto service = std::make_shared<business::TaskService>(asyncRuntime.executor(), store);
    TaskViewModel viewModel(TaskViewModel::Dependencies{service});
    service.reset();
    QVERIFY(!viewModel.ready() && !viewModel.busy());
    QVERIFY(viewModel.reload());
    QTRY_VERIFY(!viewModel.busy());
    QVERIFY(viewModel.ready());
    QCOMPARE(viewModel.tasks()->tasks().at(0).title, QString("Provided by the application"));
    QVERIFY(viewModel.addTask("Saved through the injected service"));
    QTRY_VERIFY(!viewModel.busy());
    const auto loaded = store->getStrings("tasks.items");
    QVERIFY(loaded && loaded->has_value());
    QCOMPARE(loaded->value().size(), std::size_t{6});
    QCOMPARE(loaded->value().at(4), std::string("Saved through the injected service"));
}

void ViewModelTest::applicationContextOwnsViewModels()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto context = std::make_unique<ApplicationContext>(directory.filePath("mmkv"));
    QPointer<TaskViewModel> viewModel(context->tasks());
    QCOMPARE(viewModel->parent(), context.get());
    QVERIFY(!viewModel->ready() && !viewModel->busy());
    QVERIFY(context->start());
    QVERIFY(viewModel->busy());
    context.reset();
    QVERIFY(!viewModel);
    // The context drains the shared runtime before pending Qt completions arrive.
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

void ViewModelTest::applicationStartupAndStop()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("mmkv");
    auto context = std::make_unique<ApplicationContext>(path);
    auto *viewModel = context->tasks();
    QSignalSpy counts(viewModel, &TaskViewModel::countsChanged);
    QVERIFY(context->start());
    QVERIFY(!context->start());
    QTRY_VERIFY(viewModel->ready() && !viewModel->busy());
    QCOMPARE(counts.count(), 1);
    QVERIFY(viewModel->addTask("Accepted before shutdown"));
    context->stop();
    context->stop();
    QVERIFY(!context->start());
    QVERIFY(!viewModel->reload());
    QVERIFY(!viewModel->addTask("After shutdown"));
    QTRY_VERIFY(!viewModel->busy());
    QVERIFY(!viewModel->ready());
    QCOMPARE(counts.count(), 1);
    context.reset();
    storage::MmkvStore reopened(nativePath(path));
    const auto saved = reopened.getStrings("tasks.items");
    QVERIFY(saved && saved->has_value());
    QCOMPARE(saved->value().at(1), std::string("Accepted before shutdown"));

    ApplicationContext immediate(directory.filePath("immediate"));
    QVERIFY(immediate.start());
    immediate.stop();
    QTRY_VERIFY(!immediate.tasks()->busy());
    QVERIFY(!immediate.tasks()->ready());
    QVERIFY(immediate.tasks()->errorMessage().isEmpty());
}

void ViewModelTest::commandsAndModelNotifications()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TaskFixture fixture(directory.filePath("mmkv"));
    TaskViewModel viewModel(fixture.dependencies());
    QVERIFY(viewModel.reload());
    QTRY_VERIFY(!viewModel.busy());
    QVERIFY(viewModel.ready());
    QAbstractItemModelTester tester(viewModel.tasks(), QAbstractItemModelTester::FailureReportingMode::QtTest);
    QSignalSpy counts(&viewModel, &TaskViewModel::countsChanged);
    QSignalSpy inserted(viewModel.tasks(), &QAbstractItemModel::rowsInserted);
    QSignalSpy changed(viewModel.tasks(), &QAbstractItemModel::dataChanged);
    QSignalSpy removed(viewModel.tasks(), &QAbstractItemModel::rowsRemoved);
    bool guiThread = true;
    connect(&viewModel, &TaskViewModel::countsChanged, &viewModel,
            [&] { guiThread &= QThread::currentThread() == viewModel.thread(); });
    QVERIFY(viewModel.addTask("  First task  "));
    QVERIFY(!viewModel.addTask("Reject overlapping command"));
    QTRY_VERIFY(!viewModel.busy());
    QVERIFY(viewModel.addTask("Second task"));
    QTRY_VERIFY(!viewModel.busy());
    const auto first = viewModel.tasks()->tasks().at(0);
    const auto second = viewModel.tasks()->tasks().at(1);
    QCOMPARE(first.title, QString("First task"));
    QVERIFY(first.id != second.id);
    QCOMPARE(viewModel.totalCount(), 2);
    QCOMPARE(inserted.count(), 2);
    QCOMPARE(viewModel.tasks()->data(viewModel.tasks()->index(0), TaskListModel::TitleRole).toString(), first.title);
    QVERIFY(!viewModel.tasks()->data({}, TaskListModel::TitleRole).isValid());
    QVERIFY(viewModel.setTaskCompleted(first.id, true));
    QTRY_VERIFY(!viewModel.busy());
    QCOMPARE(viewModel.remainingCount(), 1);
    QCOMPARE(changed.count(), 1);
    QVERIFY(viewModel.removeTask(first.id));
    QTRY_VERIFY(!viewModel.busy());
    QVERIFY(viewModel.setTaskCompleted(second.id, true));
    QTRY_VERIFY(!viewModel.busy());
    QCOMPARE(viewModel.remainingCount(), 0);
    QCOMPARE(viewModel.totalCount(), 1);
    QCOMPARE(removed.count(), 1);
    QCOMPARE(counts.count(), 5);
    QVERIFY(viewModel.setTaskCompleted(second.id, true));
    QTRY_VERIFY(!viewModel.busy());
    QCOMPARE(counts.count(), 5);
    QVERIFY(guiThread);
}

void ViewModelTest::validationAndSaveFailure()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("mmkv");
    TaskFixture fixture(path);
    TaskViewModel viewModel(fixture.dependencies());
    QVERIFY(viewModel.reload());
    QTRY_VERIFY(!viewModel.busy());
    QVERIFY(viewModel.addTask(" \t "));
    QTRY_VERIFY(!viewModel.busy());
    QVERIFY(!viewModel.errorMessage().isEmpty());
    QVERIFY(viewModel.addTask("Existing task"));
    QTRY_VERIFY(!viewModel.busy());
    QVERIFY(viewModel.errorMessage().isEmpty());
    const auto id = viewModel.tasks()->tasks().at(0).id;
    const auto offline = path + ".offline";
    QVERIFY(QDir().rename(path, offline));
    QFile blocker(path);
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    QSignalSpy counts(&viewModel, &TaskViewModel::countsChanged);
    QSignalSpy added(&viewModel, &TaskViewModel::taskAdded);
    QVERIFY(viewModel.addTask("Unsaved"));
    QTRY_VERIFY(!viewModel.busy());
    QVERIFY(viewModel.setTaskCompleted(id, true));
    QTRY_VERIFY(!viewModel.busy());
    QVERIFY(viewModel.removeTask(id));
    QTRY_VERIFY(!viewModel.busy());
    QCOMPARE(viewModel.totalCount(), 1);
    QCOMPARE(viewModel.remainingCount(), 1);
    QCOMPARE(counts.count(), 0);
    QCOMPARE(added.count(), 0);
    QVERIFY(!viewModel.errorMessage().isEmpty());
    QVERIFY(QFile::remove(path));
    QVERIFY(QDir().rename(offline, path));
    QVERIFY(viewModel.setTaskCompleted(id, true));
    QTRY_VERIFY(!viewModel.busy());
    QCOMPARE(viewModel.remainingCount(), 0);
    QVERIFY(viewModel.errorMessage().isEmpty());
}

void ViewModelTest::loadRecoveryAndDestruction()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("mmkv");
    QFile blocker(path);
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.write("blocked");
    blocker.close();
    TaskFixture fixture(path);
    TaskViewModel viewModel(fixture.dependencies());
    QVERIFY(viewModel.reload());
    QTRY_VERIFY(!viewModel.busy());
    QVERIFY(!viewModel.ready());
    QVERIFY(!viewModel.addTask("Do not overwrite"));
    QVERIFY(blocker.open(QIODevice::ReadOnly));
    QCOMPARE(blocker.readAll(), QByteArray("blocked"));
    blocker.close();
    QVERIFY(QFile::remove(path));
    QVERIFY(viewModel.reload());
    QTRY_VERIFY(!viewModel.busy());
    QVERIFY(viewModel.ready());
    QVERIFY(viewModel.errorMessage().isEmpty());
    auto *pending = new TaskViewModel(fixture.dependencies());
    QPointer<TaskViewModel> guard(pending);
    QVERIFY(pending->reload());
    QVERIFY(pending->busy());
    delete pending;
    QVERIFY(!guard);
    fixture.asyncRuntime.finish();
    // Deliver the queued completion after its ViewModel has been destroyed.
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}
QTEST_GUILESS_MAIN(ViewModelTest)
