pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Template.Ui

Page {
    id: page

    required property TaskViewModel viewModel

    padding: Theme.pageMargin

    function submitTask(): void {
        page.viewModel.addTask(taskInput.text);
    }

    Connections {
        target: page.viewModel
        function onTaskAdded(): void {
            taskInput.clear();
            taskInput.forceActiveFocus();
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spacing

        Label {
            text: qsTr("My tasks")
            font.pixelSize: Theme.titleSize
            font.bold: true
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("%1 remaining · %2 total")
                .arg(page.viewModel.remainingCount).arg(page.viewModel.totalCount)
            color: page.palette.placeholderText
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing

            TextField {
                id: taskInput
                objectName: "taskInput"

                Layout.fillWidth: true
                placeholderText: qsTr("What would you like to do?")
                enabled: page.viewModel.ready && !page.viewModel.busy
                selectByMouse: true
                onAccepted: page.submitTask()
                Accessible.name: qsTr("New task")
            }

            Button {
                objectName: "addTaskButton"
                text: qsTr("Add task")
                highlighted: true
                enabled: page.viewModel.ready && !page.viewModel.busy && taskInput.text.trim().length > 0
                onClicked: page.submitTask()
            }

            BusyIndicator {
                Layout.preferredWidth: 32
                Layout.preferredHeight: 32
                visible: page.viewModel.busy
                running: page.viewModel.busy
            }
        }

        Frame {
            Layout.fillWidth: true
            visible: page.viewModel.errorMessage.length > 0

            RowLayout {
                anchors.fill: parent

                Label {
                    objectName: "errorLabel"
                    Layout.fillWidth: true
                    text: page.viewModel.errorMessage
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.AlertMessage
                }

                Button {
                    text: qsTr("Retry loading")
                    visible: !page.viewModel.ready
                    enabled: !page.viewModel.busy
                    onClicked: page.viewModel.reload()
                }
            }
        }

        ListView {
            id: taskList
            objectName: "taskList"

            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 6
            model: page.viewModel.tasks
            ScrollBar.vertical: ScrollBar {}

            delegate: TaskDelegate {
                width: taskList.width
                enabled: page.viewModel.ready && !page.viewModel.busy
                onCompletionRequested: (taskId, completed) =>
                    page.viewModel.setTaskCompleted(taskId, completed)
                onRemovalRequested: taskId => page.viewModel.removeTask(taskId)
            }

            Label {
                anchors.centerIn: parent
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                visible: page.viewModel.ready && page.viewModel.totalCount === 0
                text: qsTr("A clear list. Add your first task above.")
                color: page.palette.placeholderText
            }
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Changes are saved automatically on this device.")
            color: page.palette.placeholderText
            wrapMode: Text.Wrap
            font.pixelSize: 12
        }
    }
}
