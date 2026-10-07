#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QSet>
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
    enum Role { IdRole = Qt::UserRole + 1, TitleRole, CompletedRole, PendingRole };
    Q_ENUM(Role)
    explicit TaskListModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    const QList<TaskItem> &tasks() const;
    int completedCount() const;
    void applyTasks(QList<TaskItem> tasks);
    // A task with a change in flight; the view locks only that row.
    bool isPending(const QString &id) const;
    void setPending(const QString &id, bool pending);

private:
    QList<TaskItem> m_tasks;
    QSet<QString> m_pending;
};
