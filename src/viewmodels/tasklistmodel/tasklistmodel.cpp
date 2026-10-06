#include "viewmodels/tasklistmodel/tasklistmodel.h"

#include <algorithm>
#include <utility>

TaskListModel::TaskListModel(QObject *parent) : QAbstractListModel(parent) {}
int TaskListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_tasks.size());
}
QVariant TaskListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.model() != this || index.column() != 0
        || index.row() < 0 || index.row() >= m_tasks.size())
        return {};
    const auto &task = m_tasks.at(index.row());
    switch (role) {
    case IdRole: return task.id;
    case TitleRole: return task.title;
    case CompletedRole: return task.completed;
    default: return {};
    }
}
QHash<int, QByteArray> TaskListModel::roleNames() const
{
    return {{IdRole, "taskId"}, {TitleRole, "taskTitle"}, {CompletedRole, "completed"}};
}
const QList<TaskItem> &TaskListModel::tasks() const { return m_tasks; }
int TaskListModel::completedCount() const
{
    return static_cast<int>(std::count_if(m_tasks.cbegin(), m_tasks.cend(),
        [](const TaskItem &task) { return task.completed; }));
}
void TaskListModel::applyTasks(QList<TaskItem> tasks)
{
    if (tasks == m_tasks)
        return;
    if (tasks.size() == m_tasks.size() + 1
        && std::equal(m_tasks.cbegin(), m_tasks.cend(), tasks.cbegin())) {
        const auto row = rowCount();
        beginInsertRows({}, row, row);
        m_tasks = std::move(tasks);
        endInsertRows();
        return;
    }
    if (tasks.size() + 1 == m_tasks.size()) {
        qsizetype removed = 0;
        while (removed < tasks.size() && tasks.at(removed) == m_tasks.at(removed))
            ++removed;
        if (std::equal(tasks.cbegin() + removed, tasks.cend(), m_tasks.cbegin() + removed + 1)) {
            beginRemoveRows({}, static_cast<int>(removed), static_cast<int>(removed));
            m_tasks = std::move(tasks);
            endRemoveRows();
            return;
        }
    }
    if (tasks.size() == m_tasks.size()
        && std::equal(tasks.cbegin(), tasks.cend(), m_tasks.cbegin(),
                      [](const TaskItem &a, const TaskItem &b) { return a.id == b.id; })) {
        for (int row = 0; row < tasks.size(); ++row) {
            if (tasks.at(row) == m_tasks.at(row))
                continue;
            m_tasks[row] = tasks.at(row);
            emit dataChanged(index(row), index(row), {TitleRole, CompletedRole});
        }
        return;
    }
    beginResetModel();
    m_tasks = std::move(tasks);
    endResetModel();
}
