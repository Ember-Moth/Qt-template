import QtQuick
import QtQuick.Controls
import Template.Ui

ApplicationWindow {
    id: window

    required property ApplicationContext appContext

    width: 860
    height: 620
    minimumWidth: 560
    minimumHeight: 420
    visible: true
    title: qsTr("Qt Template")

    TasksPage {
        anchors.fill: parent
        viewModel: window.appContext.tasks
    }
}
