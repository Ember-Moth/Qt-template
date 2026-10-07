#pragma once

#include "viewmodels/tasklistmodel/tasklistmodel.h"
#include <QObject>
#include <memory>

class TaskViewModel : public QObject
{
    Q_OBJECT
    Q_PROPERTY(TaskListModel *tasks READ tasks CONSTANT)
    Q_PROPERTY(int totalCount READ totalCount NOTIFY countsChanged)
    Q_PROPERTY(int remainingCount READ remainingCount NOTIFY countsChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool adding READ adding NOTIFY addingChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)

public:
    struct Dependencies;
    struct Initialization;
    explicit TaskViewModel(Dependencies dependencies, QObject *parent = nullptr);
    ~TaskViewModel() override;
    TaskListModel *tasks();
    int totalCount() const;
    int remainingCount() const;
    bool ready() const;
    bool busy() const;
    bool loading() const;
    bool adding() const;
    QString errorMessage() const;

    void initialize(Initialization initialization);
    void stop();

    // true means the asynchronous command was accepted, not yet persisted. Commands on different
    // tasks run concurrently; a loading list, an add in flight or a pending task rejects new
    // commands for it and emits commandRejected with a reason for the view.
    Q_INVOKABLE bool addTask(const QString &title);
    Q_INVOKABLE bool setTaskCompleted(const QString &id, bool completed);
    Q_INVOKABLE bool removeTask(const QString &id);
    Q_INVOKABLE bool reload();

signals:
    void countsChanged();
    void readyChanged();
    void busyChanged();
    void errorMessageChanged();
    void loadingChanged();
    void addingChanged();
    void taskAdded();
    void commandRejected(const QString &reason);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    TaskListModel m_tasks;
    bool m_ready = false;
    int m_pending = 0;
    bool m_loading = false;
    bool m_adding = false;
    bool m_stopping = false;
    QString m_errorMessage;
    void setError(QString error);
    void setReady(bool ready);
    void setLoading(bool loading);
    void setAdding(bool adding);
    void changePending(int delta);
    bool accept(bool targetPending);
};
