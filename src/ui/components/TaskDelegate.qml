import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Template.Ui

Frame {
    id: taskDelegate

    required property string taskId
    required property string taskTitle
    required property bool completed

    signal completionRequested(string taskId, bool completed)
    signal removalRequested(string taskId)

    padding: Theme.spacing

    RowLayout {
        anchors.fill: parent
        spacing: Theme.spacing

        CheckBox {
            id: completionCheckBox
            objectName: "completionCheckBox"

            // Keep checked bound to committed state, including after a failed save.
            nextCheckState: function(): int {
                taskDelegate.completionRequested(taskDelegate.taskId, !taskDelegate.completed);
                return taskDelegate.completed ? Qt.Checked : Qt.Unchecked;
            }
            checked: taskDelegate.completed
            Accessible.name: qsTr("Complete task: %1").arg(taskDelegate.taskTitle)
        }

        Label {
            Layout.fillWidth: true
            text: taskDelegate.taskTitle
            wrapMode: Text.Wrap
            font.strikeout: taskDelegate.completed
            opacity: taskDelegate.completed ? 0.55 : 1
        }

        Button {
            objectName: "removeTaskButton"
            text: qsTr("Remove")
            flat: true
            onClicked: taskDelegate.removalRequested(taskDelegate.taskId)
            Accessible.name: qsTr("Remove task: %1").arg(taskDelegate.taskTitle)
        }
    }
}
