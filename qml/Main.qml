import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: window
    width: 1120; height: 760; minimumWidth: 800; minimumHeight: 600
    visible: true; title: "CharaShell"
    font.family: "Meiryo"
    Theme { id: theme }
    Material.theme: Material.Light
    Material.accent: theme.accent
    color: "#000000"
    Component.onCompleted: if (!diagnosticMode) startup.begin()
    visibility: Window.Windowed
    property bool controlsVisible: true
    property bool loadingPreview: false
    readonly property real cinemaAspect: 2.39
    readonly property real cinemaBandMinimum: Math.max(96, Math.min(controls.height + 32, height / 3))
    readonly property real cinemaFrameHeight: Math.max(1, Math.min(width / cinemaAspect, height - cinemaBandMinimum * 2))
    component CinemaButton: Button {
        id: cinemaButton
        Material.theme: Material.Dark
        Material.foreground: "#f2f2f2"
        implicitHeight: 44
        topInset: 0; bottomInset: 0; leftInset: 0; rightInset: 0
        leftPadding: 14; rightPadding: 14
        icon.color: enabled ? "#f2f2f2" : "#777777"
        icon.width: 20; icon.height: 20
        background: Rectangle {
            implicitWidth: 44; implicitHeight: 44; radius: 8
            color: cinemaButton.down ? "#282828" : cinemaButton.hovered ? "#1b1b1b" : "#080808"
            border.color: cinemaButton.enabled ? "#383838" : "#202020"
        }
    }
    function revealControls() { controlsVisible = true; hideControls.restart() }
    Shortcut {
        sequence: "Escape"
        onActivated: {
            if (settingsDialog.visible) settingsDialog.close()
            window.revealControls()
        }
    }
    AvatarView {
        id: characterStage
        objectName: "avatarView"
        anchors.centerIn: parent
        width: Math.min(window.width, window.cinemaFrameHeight * window.cinemaAspect)
        height: window.cinemaFrameHeight
        clip: true
        focus: true
        Component.onCompleted: forceActiveFocus()
        outputLevel: conversation.outputLevel
        thinking: conversation.system2Waiting
        psdPath: appSettings.values.avatarPsdPath
        loadAvatar: diagnosticMode || startup.ready
    }
    Item {
        id: topCinemaBand
        anchors.top: parent.top; anchors.bottom: characterStage.top
        anchors.left: parent.left; anchors.right: parent.right
    }
    Item {
        id: bottomCinemaBand
        anchors.top: characterStage.bottom; anchors.bottom: parent.bottom
        anchors.left: parent.left; anchors.right: parent.right
    }
    Label {
        id: subtitle
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: controls.top
        anchors.bottomMargin: 20
        width: Math.min(760, parent.width - 64)
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        maximumLineCount: 5
        elide: Text.ElideRight
        text: conversation.response.length > 0 ? conversation.response : conversation.partial
        visible: (diagnosticMode || startup.ready) && text.length > 0
        font.pixelSize: 20
        color: "#f4f4f4"
        padding: 12
        background: Rectangle { color: "#b3000000"; radius: 6 }
    }
    CinemaButton {
        id: settingsButton
        objectName: "settingsButton"
        anchors.verticalCenter: topCinemaBand.verticalCenter
        anchors.right: parent.right; anchors.rightMargin: 24
        text: ""
        implicitWidth: 44; implicitHeight: 44
        enabled: !conversation.inputLocked
        visible: diagnosticMode || startup.ready
        padding: 10
        icon.source: "qrc:/icons/lucide/settings.svg"
        icon.width: 22; icon.height: 22
        display: AbstractButton.IconOnly
        ToolTip.visible: hovered
        ToolTip.text: "設定"
        onClicked: settingsDialog.open()
        Accessible.name: "設定を開く"
    }
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.NoButton
        hoverEnabled: true
        onPositionChanged: window.revealControls()
    }
    Timer {
        id: hideControls
        interval: 5000; running: true
        onTriggered: {
            if (!conversation.inputLocked && !input.activeFocus && !controlsHover.hovered && !settingsDialog.visible && !downloadDialog.visible)
                window.controlsVisible = false
            else restart()
        }
    }
    ToolButton {
        id: statusIndicator
        objectName: "conversationStatusIndicator"
        anchors.verticalCenter: topCinemaBand.verticalCenter
        anchors.left: parent.left; anchors.leftMargin: 24
        width: 44; height: 44; padding: 11
        topInset: 0; bottomInset: 0; leftInset: 0; rightInset: 0
        Material.theme: Material.Dark
        display: AbstractButton.IconOnly
        focusPolicy: Qt.NoFocus
        hoverEnabled: true
        visible: (diagnosticMode || startup.ready) && (window.controlsVisible || conversation.listening || setup.busy || conversation.backgroundTaskCount > 0)
        readonly property string mode: setup.busy || conversation.modelsLoading ? "loading"
            : conversation.state === "ERROR" ? "error" : conversation.system2Waiting ? "thinking"
            : conversation.state === "RESPONDING" ? "speaking" : conversation.inputLocked ? "thinking"
            : conversation.listening ? "listening" : "idle"
        readonly property string statusText: mode === "loading" ? "準備中" : mode === "error" ? "入力エラー"
            : mode === "thinking" ? "思考中" : mode === "speaking" ? "応答中"
            : mode === "listening" ? "音声を聞いています" : "入力待ち"
        readonly property string iconName: mode === "loading" ? "loader-circle" : mode === "error" ? "circle-alert"
            : mode === "thinking" ? "brain-circuit" : mode === "speaking" ? "volume-2"
            : mode === "listening" ? "mic" : "message-circle"
        background: Rectangle {
            radius: 8; color: statusIndicator.hovered ? "#1b1b1b" : "#080808"
            border.color: statusIndicator.mode === "error" ? "#ef8590" : "#383838"
        }
        icon.source: "qrc:/icons/lucide/" + iconName + ".svg"
        icon.color: mode === "error" ? "#ef8590" : "#f2f2f2"
        icon.width: 22; icon.height: 22
        property real loadingAngle: 0
        contentItem.rotation: mode === "loading" ? loadingAngle : 0
        NumberAnimation on loadingAngle {
            from: 0; to: 360; duration: 1200; loops: Animation.Infinite
            running: statusIndicator.visible && statusIndicator.mode === "loading"
        }
        ToolTip.visible: hovered
        ToolTip.text: statusText
        Accessible.role: Accessible.Indicator
        Accessible.name: statusText
    }
    Rectangle {
        id: controls
        anchors.verticalCenter: bottomCinemaBand.verticalCenter
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(860, parent.width - 40)
        height: controlColumn.implicitHeight + 24
        radius: 0; color: "#000000"
        Material.theme: Material.Dark
        Material.foreground: "#f2f2f2"
        opacity: window.controlsVisible || conversation.inputLocked || conversation.errorMessage.length > 0 ? 1 : 0
        visible: (diagnosticMode || startup.ready) && opacity > 0
        Behavior on opacity { NumberAnimation { duration: theme.animationDuration } }
        HoverHandler { id: controlsHover }
        ColumnLayout {
            id: controlColumn
            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
            anchors.margins: 12; spacing: 6
            Label {
                Layout.fillWidth: true
                visible: text.length > 0
                text: conversation.errorMessage
                color: "#ef8590"; wrapMode: Text.Wrap; textFormat: Text.PlainText
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 8
                TextField {
                    id: input; Layout.fillWidth: true
                    Layout.preferredHeight: 48
                    color: "#f2f2f2"
                    placeholderTextColor: "#999999"
                    selectionColor: "#53625b"; selectedTextColor: "#ffffff"
                    background: Rectangle {
                        radius: 8; color: "#080808"
                        border.color: input.activeFocus ? "#b7c7c0" : "#383838"
                    }
                    enabled: !conversation.inputLocked
                    placeholderText: conversation.inputLocked ? "回答が終わるまでお待ちください…" : "話しかける…"
                    onActiveFocusChanged: if (activeFocus) window.revealControls()
                    onAccepted: send.clicked()
                }
                CinemaButton {
                    id: send; text: "送信"; icon.source: "qrc:/icons/lucide/send.svg"; enabled: !conversation.inputLocked && input.text.trim().length > 0
                    onClicked: { conversation.submit(input.text); input.clear(); input.focus = false; window.revealControls() }
                }
                CinemaButton {
                    text: conversation.listening ? "マイクOFF" : "マイクON"
                    icon.source: conversation.listening ? "qrc:/icons/lucide/mic-off.svg" : "qrc:/icons/lucide/mic.svg"
                    enabled: !conversation.inputLocked && !setup.busy && !downloadDialog.visible
                    onClicked: {
                        if (conversation.listening) conversation.stop()
                        else conversation.start()
                        window.revealControls()
                    }
                }
                CinemaButton { text: "中断"; icon.source: "qrc:/icons/lucide/square.svg"; enabled: conversation.inputLocked; onClicked: conversation.cancel() }
            }
        }
    }
    Dialog {
        id: downloadDialog
        objectName: "downloadDialog"
        visible: !diagnosticMode && startup.downloading
        property bool forceRepair: false
        onClosed: forceRepair = false
        anchors.centerIn: parent
        width: Math.min(560, window.width - 40)
        modal: true
        closePolicy: Popup.NoAutoClose
        title: startup.failed ? "ダウンロードできませんでした" : "モデルをダウンロードしています"
        padding: 24
        background: Rectangle { color: theme.surface; radius: theme.cardRadius }
        Overlay.modal: Rectangle { color: "#77000000" }
        enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: theme.animationDuration } }
        exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: theme.animationDuration } }
        contentItem: ColumnLayout {
            spacing: theme.spacing * 2
            Label {
                Layout.fillWidth: true
                text: startup.failed ? startup.errorString : "必要なファイルを取得し、検証しています。完了後、モデルをロードします。"
                wrapMode: Text.Wrap
                maximumLineCount: 8; elide: Text.ElideRight
            }
            Label {
                Layout.fillWidth: true
                text: "保存先: " + setup.savePath
                wrapMode: Text.Wrap; font.pixelSize: 12; color: theme.mutedText
            }
            Label {
                Layout.fillWidth: true
                text: "必要容量: 最大 " + (setup.plannedBytes / 1073741824).toFixed(2) + " GiB"
                color: theme.primaryText
            }
            ProgressBar {
                Layout.fillWidth: true
                from: 0; to: Math.max(1, setup.plannedBytes)
                value: setup.receivedBytes
                visible: setup.busy || setup.receivedBytes > 0
            }
            Label {
                Layout.fillWidth: true
                visible: setup.currentFile.length > 0
                text: setup.currentFile + "\n" +
                      (setup.fileBytes / 1048576).toFixed(1) + " / " + (setup.fileTotal / 1048576).toFixed(1) + " MiB  ·  " +
                      (setup.bytesPerSecond / 1048576).toFixed(1) + " MiB/s"
                wrapMode: Text.Wrap
            }
            ProgressBar {
                Layout.fillWidth: true
                from: 0; to: Math.max(1, setup.fileTotal)
                value: setup.fileBytes
                visible: setup.currentFile.length > 0
            }
            Label {
                Layout.fillWidth: true
                text: setup.status
                visible: text.length > 0
                maximumLineCount: 4; elide: Text.ElideRight; wrapMode: Text.Wrap
                color: theme.mutedText; font.pixelSize: 12
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                Button {
                    text: setup.busy ? "一時停止" : "再開・再試行"
                    icon.source: setup.busy ? "qrc:/icons/lucide/square.svg" : "qrc:/icons/lucide/download.svg"
                    visible: setup.busy || startup.failed
                    onClicked: setup.busy ? setup.cancel() : startup.retry()
                }
                Button { text: "設定"; icon.source: "qrc:/icons/lucide/settings.svg"; visible: !setup.busy; onClicked: settingsDialog.open() }
                Button { text: "終了"; icon.source: "qrc:/icons/lucide/log-out.svg"; visible: !setup.busy; onClicked: Qt.quit() }
            }
        }
    }
    Dialog {
        id: modelLoadDialog
        objectName: "modelLoadDialog"
        visible: window.loadingPreview || (!diagnosticMode && startup.loading)
        anchors.centerIn: parent
        width: startup.failed ? Math.min(560, window.width - 40) : 96
        modal: true
        closePolicy: Popup.NoAutoClose
        title: startup.failed ? "モデルをロードできませんでした" : ""
        enter: Transition {}
        exit: Transition {}
        padding: 24
        background: Rectangle { color: startup.failed ? theme.surface : "transparent"; radius: theme.cardRadius }
        Overlay.modal: Rectangle { color: "#77000000" }
        contentItem: ColumnLayout {
            spacing: theme.spacing * 2
            LoadingSpinner {
                objectName: "modelLoadSpinner"
                Layout.alignment: Qt.AlignHCenter
                visible: !startup.failed
            }
            Label {
                Layout.fillWidth: true
                visible: startup.failed
                text: startup.errorString
                wrapMode: Text.Wrap
                maximumLineCount: 8; elide: Text.ElideRight
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                visible: startup.failed
                Button { text: "再試行"; visible: startup.failed; onClicked: startup.retry() }
                Button { text: "設定"; visible: startup.failed; onClicked: settingsDialog.open() }
                Button { text: "終了"; onClicked: Qt.quit() }
            }
        }
    }
    Dialog {
        id: settingsDialog
        objectName: "settingsDialog"
        anchors.centerIn: parent; width: Math.min(650, window.width - 40); height: Math.min(680, window.height - 40)
        title: "設定"; modal: true; standardButtons: Dialog.Save | Dialog.Cancel
        background: Rectangle { color: theme.surface; radius: theme.cardRadius }
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        Overlay.modal: Rectangle { color: "#77000000" }
        enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: theme.animationDuration } }
        exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: theme.animationDuration } }
        readonly property bool httpBackend: appSettings.values.backend === "http"
        function selectDevice(combo, devices, id) {
            combo.model = devices
            let index = combo.indexOfValue(id)
            if (index < 0 && id.length > 0) {
                combo.model = devices.concat([{id: id, name: "保存したデバイス（未接続）"}])
                index = combo.indexOfValue(id)
            }
            combo.currentIndex = Math.max(0, index)
        }
        function refreshAudioDevices(inputId, outputId) {
            conversation.refreshDevices()
            selectDevice(inputDevice, conversation.inputDevices, inputId)
            selectDevice(outputDevice, conversation.outputDevices, outputId)
        }
        onOpened: {
            avatarPsdPath.text = appSettings.values.avatarPsdPath
            refreshAudioDevices(appSettings.values.inputDeviceId, appSettings.values.outputDeviceId)
            inferenceDevice.currentIndex = appSettings.values.useGpu ? 0 : 1
            llmUrl.text = appSettings.values.llmUrl; ttsUrl.text = appSettings.values.ttsUrl
            asr.text = appSettings.values.asrPath; voice.text = appSettings.values.voice
            ttsModel.text = appSettings.values.ttsModel
            prompt.text = appSettings.values.systemPrompt
            llmPath.text = appSettings.values.llmPath; ttsPath.text = appSettings.values.ttsPath
            ttsLibraryPath.text = appSettings.values.ttsLibraryPath
            voicePath.text = appSettings.values.voicePath
            contextSize.value = appSettings.values.contextSize; gpuLayers.value = appSettings.values.gpuLayers
            llmCache.currentIndex = appSettings.values.llmKvCache === "q8_0" ? 0 : 1
            sarashinaApiKey.text = appSettings.values.sarashinaApiKey
            sarashinaModel.text = appSettings.values.sarashinaModel
            sampleRate.value = appSettings.values.sampleRate; endpoint.value = Math.round(appSettings.values.endpointSilence * 1000)
            threshold.value = Math.round(appSettings.values.vadThreshold * 1000)
        }
        onAccepted: appSettings.save({llmUrl: llmUrl.text, ttsUrl: ttsUrl.text, asrPath: asr.text,
            avatarPsdPath: avatarPsdPath.text,
            voice: voice.text, ttsModel: ttsModel.text, systemPrompt: prompt.text,
            sampleRate: sampleRate.value, endpointSilence: endpoint.value / 1000,
            vadThreshold: threshold.value / 1000, language: "ja",
            llmPath: llmPath.text, ttsPath: ttsPath.text, voicePath: voicePath.text,
            ttsLibraryPath: ttsLibraryPath.text,
            contextSize: contextSize.value, gpuLayers: gpuLayers.value, useGpu: inferenceDevice.currentIndex === 0,
            llmKvCache: llmCache.currentIndex === 0 ? "q8_0" : "f16",
            sarashinaApiKey: sarashinaApiKey.text, sarashinaModel: sarashinaModel.text,
            inputDeviceId: inputDevice.currentValue || "", outputDeviceId: outputDevice.currentValue || ""})
        ScrollView {
            anchors.fill: parent; contentWidth: availableWidth
            ColumnLayout {
                width: settingsDialog.availableWidth; spacing: 8
                Label { text: "アバターのPSDファイル（絶対パス）" }
                TextField {
                    id: avatarPsdPath
                    objectName: "avatarPsdPathField"
                    Layout.fillWidth: true
                    placeholderText: "空欄で標準の赤髪ボブを使用"
                    selectByMouse: true
                    Accessible.name: "アバターのPSDファイルへのパス"
                }
                Label { Layout.fillWidth: true; text: "絶対パスで保存します。空欄で保存すると標準の赤髪ボブに戻ります。"; color: "#91a4ad"; wrapMode: Text.Wrap }
                Label { Layout.fillWidth: true; text: characterStage.errorString; visible: text.length > 0; color: "#8b294b"; wrapMode: Text.Wrap }
                Label { text: "音声デバイスの変更は保存時に適用されます。" }
                Label { text: "入力デバイス（マイク）" }
                ComboBox { id: inputDevice; Layout.fillWidth: true; textRole: "name"; valueRole: "id" }
                Label { text: "出力デバイス（スピーカー / ヘッドホン）" }
                ComboBox { id: outputDevice; Layout.fillWidth: true; textRole: "name"; valueRole: "id" }
                Button { text: "デバイス一覧を更新"; icon.source: "qrc:/icons/lucide/refresh-cw.svg"; onClicked: settingsDialog.refreshAudioDevices(inputDevice.currentValue || "", outputDevice.currentValue || "") }
                Label { Layout.fillWidth: true; text: conversation.deviceError; visible: text.length > 0; color: "#ffac9e"; wrapMode: Text.Wrap }
                Label { text: "モデルの推論デバイス" }
                ComboBox { id: inferenceDevice; model: ["GPU", "CPU"]; Layout.fillWidth: true }
                Label { Layout.fillWidth: true; text: "GPUは音声認識・アプリ内LLM・TTSに適用します。LLMはCUDA、ASR・TTSはVulkanを使用します。HTTP接続時はサーバー側の設定を使用します。"; color: "#91a4ad"; wrapMode: Text.Wrap }
                Label { text: "LLM GGUF"; visible: !settingsDialog.httpBackend }
                TextField { id: llmPath; Layout.fillWidth: true; visible: !settingsDialog.httpBackend }
                Label { text: "Irodori-TTS-v4.1-Small-MF GGUF（コーデック内蔵）"; visible: !settingsDialog.httpBackend }
                TextField { id: ttsPath; Layout.fillWidth: true; visible: !settingsDialog.httpBackend }
                Label { text: "IrodoriのC++推論DLL"; visible: !settingsDialog.httpBackend }
                TextField { id: ttsLibraryPath; Layout.fillWidth: true; visible: !settingsDialog.httpBackend }
                Label { text: "参照音声WAV / Irodori音声GGUF（任意）"; visible: !settingsDialog.httpBackend }
                TextField { id: voicePath; Layout.fillWidth: true; visible: !settingsDialog.httpBackend; placeholderText: "参照音声.wav / .voice.gguf（空欄も可）" }

                Label { text: "相対パスの基準は実行ファイルのフォルダです"; color: "#91a4ad" }
                Label { text: "Sarashina APIキー"; visible: true }
                TextField { id: sarashinaApiKey; Layout.fillWidth: true; echoMode: TextInput.Password; visible: true }
                Label { text: "Sarashinaモデル"; visible: true }
                TextField { id: sarashinaModel; Layout.fillWidth: true; visible: true }
                Label { text: "コンテキスト長"; visible: !settingsDialog.httpBackend }
                SpinBox { id: contextSize; from: 1024; to: 8192; stepSize: 1024; visible: !settingsDialog.httpBackend }
                Label { text: "LLMのGPU会話キャッシュ"; visible: !settingsDialog.httpBackend && inferenceDevice.currentIndex === 0 }
                ComboBox { id: llmCache; model: ["省メモリ（8bit）", "標準（16bit）"]; Layout.fillWidth: true; visible: !settingsDialog.httpBackend && inferenceDevice.currentIndex === 0 }
                Label { text: "LLMのGPUレイヤー上限（999で自動）"; visible: !settingsDialog.httpBackend && inferenceDevice.currentIndex === 0 }
                SpinBox { id: gpuLayers; from: 1; to: 999; visible: !settingsDialog.httpBackend && inferenceDevice.currentIndex === 0 }
                Label { text: "読み上げ時は音声生成に必要なGPUメモリを確保し、LLMの配置数を調整します。"; Layout.fillWidth: true; wrapMode: Text.Wrap; color: "#91a4ad"; visible: !settingsDialog.httpBackend && inferenceDevice.currentIndex === 0 }
                Label { text: "llama-server URL"; visible: settingsDialog.httpBackend }
                TextField { id: llmUrl; Layout.fillWidth: true; visible: settingsDialog.httpBackend }
                Label { text: "Irodori-TTS-Server URL"; visible: settingsDialog.httpBackend }
                TextField { id: ttsUrl; Layout.fillWidth: true; visible: settingsDialog.httpBackend }
                Label { text: "TTSの公開モデル名"; visible: settingsDialog.httpBackend }
                TextField { id: ttsModel; Layout.fillWidth: true; visible: settingsDialog.httpBackend }
                Label { text: "Haas-ja 日本語音声認識モデル（GGUF）" }
                TextField { id: asr; Layout.fillWidth: true }
                Label { text: "サーバーに登録済みの参照音声名"; visible: settingsDialog.httpBackend }
                TextField { id: voice; Layout.fillWidth: true; visible: settingsDialog.httpBackend }
                Label { text: settingsDialog.httpBackend ? "TTSサンプルレート（サーバーの出力と一致させる）" : "音声出力サンプルレート" }
                SpinBox { id: sampleRate; from: 8000; to: 48000; stepSize: 1000; editable: true }
                Label { text: "発話終了の無音時間（ms）" }
                SpinBox { id: endpoint; from: 200; to: 2400; stepSize: 100; editable: true }
                Label { text: "音声検知のRMS閾値（×1000）" }
                SpinBox { id: threshold; from: 1; to: 200; editable: true }
                Label { text: "キャラのプロンプト（LLMへの指示）" }
                TextArea { id: prompt; Layout.fillWidth: true; Layout.preferredHeight: 120; wrapMode: Text.Wrap; placeholderText: "キャラの性格・口調・一人称・ユーザーとの関係など" }
                Label { text: "保存した内容は次の応答から反映されます"; color: "#91a4ad" }

            }
        }
    }
}
