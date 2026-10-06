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
    QString errorMessage() const;

    void initialize(Initialization initialization);
    void stop();

    // true means the asynchronous command was accepted, not yet persisted.
    Q_INVOKABLE bool addTask(const QString &title);
    Q_INVOKABLE bool setTaskCompleted(const QString &id, bool completed);
    Q_INVOKABLE bool removeTask(const QString &id);
    Q_INVOKABLE bool reload();

signals:
    void countsChanged();
    void readyChanged();
    void busyChanged();
    void errorMessageChanged();
    void taskAdded();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    TaskListModel m_tasks;
    bool m_ready = false;
    bool m_busy = false;
    bool m_stopping = false;
    QString m_errorMessage;
    void setError(QString error);
    void setReady(bool ready);
    void setBusy(bool busy);
};
