#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QString>

struct TaskItem
{
    QString id;
    QString title;
    bool completed = false;
    bool operator==(const TaskItem &) const = default;
};

// This is a Qt presentation adapter; business data is defined in Template.Models.
class TaskListModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role { IdRole = Qt::UserRole + 1, TitleRole, CompletedRole };
    Q_ENUM(Role)
    explicit TaskListModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    const QList<TaskItem> &tasks() const;
    int completedCount() const;
    void applyTasks(QList<TaskItem> tasks);

private:
    QList<TaskItem> m_tasks;
};
