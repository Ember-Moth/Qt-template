#pragma once

#include "viewmodels/taskviewmodel/taskviewmodel.h"
#include <QObject>
#include <memory>

class ApplicationContext : public QObject
{
    Q_OBJECT
    Q_PROPERTY(TaskViewModel *tasks READ tasks CONSTANT)

public:
    explicit ApplicationContext(const QString &storageDirectory, QObject *parent = nullptr);
    ~ApplicationContext() override;
    TaskViewModel *tasks();
    bool start();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
