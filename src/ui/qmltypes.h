#pragma once

#include "app/applicationcontext/applicationcontext.h"
#include "viewmodels/tasklistmodel/tasklistmodel.h"
#include "viewmodels/taskviewmodel/taskviewmodel.h"

#include <QtQml/qqmlregistration.h>

// QML registration belongs to the UI adapter; business modules have no Qt dependency.
struct ApplicationContextQml
{
    Q_GADGET
    QML_FOREIGN(ApplicationContext)
    QML_NAMED_ELEMENT(ApplicationContext)
    QML_UNCREATABLE("The application provides the context and its ViewModels.")
};

struct TaskListModelQml
{
    Q_GADGET
    QML_FOREIGN(TaskListModel)
    QML_NAMED_ELEMENT(TaskListModel)
    QML_UNCREATABLE("The ViewModel owns the list model.")
};

struct TaskViewModelQml
{
    Q_GADGET
    QML_FOREIGN(TaskViewModel)
    QML_NAMED_ELEMENT(TaskViewModel)
    QML_UNCREATABLE("ApplicationContext provides the ViewModel.")
};
