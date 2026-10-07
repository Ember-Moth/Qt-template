#pragma once
#include <QObject>

class ViewModelTest : public QObject
{
    Q_OBJECT
private slots:
    void serviceIsInjected();
    void applicationContextOwnsViewModels();
    void applicationStartupAndStop();
    void commandsAndModelNotifications();
    void validationAndSaveFailure();
    void concurrentCommandsLockOnlyTheirTargets();
    void loadRecoveryAndDestruction();
};
