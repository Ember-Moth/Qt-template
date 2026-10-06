module;
#include "tasklistmodel.h"

export module Template.ViewModels.TaskList;

// Keep QObject declarations in the global module for Qt moc and QML tooling.
export using ::TaskItem;
export using ::TaskListModel;
