#pragma once
#include <QObject>

class QmlTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void viewCommandsAndSaveFailure();
    void pendingTaskLocksOnlyItsRow();
    void failedAddRemainsVisibleDuringOtherChanges();
    void viewRequiresInjectedContext();
    void registeredDependenciesAreProvidedByCpp();
};
