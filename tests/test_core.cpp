#include <asio/post.hpp>
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
// Holds the runtime thread so accepted commands stay in flight until open() or destruction.
struct RuntimeGate
{
    std::promise<void> release;
    bool opened = false;
    explicit RuntimeGate(runtime::AsioRuntime &runtime)
    {
        asio::post(runtime.executor(), [gate = release.get_future().share()] { gate.wait(); });
    }
    void open()
    {
        if (!opened) release.set_value();
        opened = true;
    }
    ~RuntimeGate() { open(); }
};
auto roleChanges(const QSignalSpy &spy, int role)
{
    return std::ranges::count_if(spy, [role](const QList<QVariant> &arguments) {
        return qvariant_cast<QList<int>>(arguments.at(2)).contains(role);
    });
}
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
    QCOMPARE(roleChanges(changed, TaskListModel::CompletedRole), 1);
    // The row was marked pending while saving and released afterwards.
    QCOMPARE(roleChanges(changed, TaskListModel::PendingRole), 2);
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
    // The row recovered; its success must keep the separate failed add visible.
    QVERIFY(!viewModel.errorMessage().isEmpty());
    QVERIFY(viewModel.addTask("Unsaved"));
    QTRY_VERIFY(!viewModel.busy());
    QVERIFY(viewModel.errorMessage().isEmpty());
}

void ViewModelTest::concurrentCommandsLockOnlyTheirTargets()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TaskFixture fixture(directory.filePath("mmkv"));
    TaskViewModel viewModel(fixture.dependencies());
    QVERIFY(viewModel.reload());
    QTRY_VERIFY(!viewModel.busy());
    QVERIFY(viewModel.addTask("First"));
    QTRY_VERIFY(!viewModel.busy());
    QVERIFY(viewModel.addTask("Second"));
    QTRY_VERIFY(!viewModel.busy());
    const auto first = viewModel.tasks()->tasks().at(0).id;
    const auto second = viewModel.tasks()->tasks().at(1).id;
    QSignalSpy rejected(&viewModel, &TaskViewModel::commandRejected);

    RuntimeGate gate(fixture.asyncRuntime);
    QVERIFY(viewModel.setTaskCompleted(first, true));
    QVERIFY(viewModel.tasks()->isPending(first));
    QVERIFY(viewModel.tasks()->data(viewModel.tasks()->index(0), TaskListModel::PendingRole).toBool());
    QVERIFY(!viewModel.setTaskCompleted(first, false));
    QVERIFY(!viewModel.removeTask(first));
    // Other tasks and the input stay available while the first task saves.
    QVERIFY(viewModel.removeTask(second));
    QVERIFY(viewModel.addTask("Third"));
    QVERIFY(viewModel.adding() && viewModel.busy() && !viewModel.loading());
    QVERIFY(!viewModel.addTask("Fourth"));
    QVERIFY(!viewModel.reload());
    QCOMPARE(rejected.count(), 4);
    for (const auto &arguments : rejected)
        QVERIFY(!arguments.at(0).toString().isEmpty());

    gate.open();
    QTRY_VERIFY(!viewModel.busy());
    QVERIFY(!viewModel.adding() && !viewModel.tasks()->isPending(first) && !viewModel.tasks()->isPending(second));
    QCOMPARE(viewModel.totalCount(), 2);
    QCOMPARE(viewModel.tasks()->tasks().at(0).id, first);
    QVERIFY(viewModel.tasks()->tasks().at(0).completed);
    QCOMPARE(viewModel.tasks()->tasks().at(1).title, QString("Third"));
    QVERIFY(viewModel.errorMessage().isEmpty());

    // A reload locks the whole list until it completes.
    RuntimeGate loading(fixture.asyncRuntime);
    QVERIFY(viewModel.reload());
    QVERIFY(viewModel.loading());
    QVERIFY(!viewModel.addTask("While loading") && !viewModel.setTaskCompleted(first, false));
    QCOMPARE(rejected.count(), 6);
    loading.open();
    QTRY_VERIFY(!viewModel.busy());
    QVERIFY(!viewModel.loading() && viewModel.ready());
}

void ViewModelTest::concurrentErrorsStayWithTheirTargets()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("mmkv");
    TaskFixture fixture(path);
    TaskViewModel viewModel(fixture.dependencies());
    QVERIFY(viewModel.reload());
    QTRY_VERIFY(!viewModel.busy());
    QVERIFY(viewModel.addTask("Existing"));
    QTRY_VERIFY(!viewModel.busy());
    const auto id = viewModel.tasks()->tasks().at(0).id;

    // Both outcomes arrive together: the unrelated success must not clear the add error.
    RuntimeGate first(fixture.asyncRuntime);
    QVERIFY(viewModel.addTask(QString(121, QChar('x'))));
    QVERIFY(viewModel.setTaskCompleted(id, true));
    first.open();
    QTRY_VERIFY(!viewModel.busy());
    QVERIFY(viewModel.tasks()->tasks().at(0).completed);
    QVERIFY(!viewModel.errorMessage().isEmpty());
    const auto addError = viewModel.errorMessage();

    // Keep both failures, then resolve them independently.
    const auto offline = path + ".offline";
    QVERIFY(QDir().rename(path, offline));
    QFile blocker(path);
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    QVERIFY(viewModel.setTaskCompleted(id, false));
    QTRY_VERIFY(!viewModel.busy());
    QVERIFY(viewModel.errorMessage().contains(addError));
    QVERIFY(viewModel.errorMessage().contains("Storage error:"));
    QVERIFY(QFile::remove(path));
    QVERIFY(QDir().rename(offline, path));

    QVERIFY(viewModel.addTask("Corrected title"));
    QTRY_VERIFY(!viewModel.busy());
    QVERIFY(!viewModel.errorMessage().contains(addError));
    QVERIFY(viewModel.errorMessage().contains("Storage error:"));
    // Removing that same task also resolves its earlier update failure.
    QVERIFY(viewModel.removeTask(id));
    QTRY_VERIFY(!viewModel.busy());
    QVERIFY(viewModel.errorMessage().isEmpty());
}

void ViewModelTest::errorsLeaveWithTheirTasks()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    TaskFixture fixture(directory.filePath("mmkv"));
    TaskViewModel viewModel(fixture.dependencies());
    QVERIFY(viewModel.reload());
    QTRY_VERIFY(!viewModel.busy());
    QVERIFY(viewModel.setTaskCompleted("missing", true));
    QTRY_VERIFY(!viewModel.busy());
    QVERIFY(!viewModel.errorMessage().isEmpty());
    // No later success can target a task outside the list, so the next snapshot drops its error.
    QVERIFY(viewModel.addTask("Unrelated"));
    QTRY_VERIFY(!viewModel.busy());
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
