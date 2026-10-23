pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// The screen capture's card on the 插件 page (src/capture; [Capture] in
// WinShun.ini): built in, off until switched on here. Drawn like a plugin's
// card (PluginCard): icon, name, a tag, what it does and where it stands,
// the switch on the right. While it is on, its options show underneath: the
// hotkey (recorded, with what went wrong registering it), PrtScn, and
// whether letting go of a drag copies at once. Turned off, the app makes no
// window, registers no key and holds no graphics device for it.
Rectangle {
    id: card

    required property SettingsEditor editor
    property bool marked: false // what the settings search found
    // The hotkey box records: SettingsWindow's binding stops the app's hotkeys meanwhile.
    readonly property bool recordingHotkey: hotkeyRecorder.recording || pinsHotkeyRow.recording || restoreHotkeyRow.recording

    // The mouse buttons' choices (Settings::captureMouse*), the right button's with 后退 besides.
    readonly property var mouseChoices: [
        { value: "none", text: qsTr("无") },
        { value: "copy", text: qsTr("复制并关闭") },
        { value: "save", text: qsTr("保存") },
        { value: "pin", text: qsTr("贴图") },
        { value: "ocr", text: qsTr("识别文字") },
        { value: "ocrcopy", text: qsTr("识别文字并复制") },
        { value: "ocrsearch", text: qsTr("识别文字并搜索") },
        { value: "ocrweb", text: qsTr("识别文字并网页搜索") },
        { value: "cancel", text: qsTr("取消截图") }
    ]
    readonly property var rightChoices: [mouseChoices[0], { value: "back", text: qsTr("后退（重新框选）") }].concat(mouseChoices.slice(1))
    readonly property var mouseValues: [editor.captureMouseDoubleLeft, editor.captureMouseRight, editor.captureMouseMiddle,
        editor.captureMouseBack, editor.captureMouseForward]

    // The five mouse-action drop-downs share one width, the widest option's
    // (rightChoices holds them all): text + gap + chevron + FlatButton's padding.
    readonly property int pickWidth: Math.ceil(Math.max(...rightChoices.map(c => pickMetrics.advanceWidth(c.text)))) + 6 + 10 + 24

    FontMetrics { id: pickMetrics; font.pixelSize: Theme.fontBody }

    // The toolbar's buttons left off (Settings::captureToolbarHidden) and the ids it knows, in its order.
    readonly property var hiddenButtons: editor.captureToolbarHidden.split(",")
    readonly property var toolbarButtons: [
        { id: "undo", text: qsTr("撤销") },
        { id: "redo", text: qsTr("重做") },
        { id: "long", text: qsTr("长截图") },
        { id: "delay", text: qsTr("延时") },
        { id: "qr", text: qsTr("二维码") },
        { id: "ocr", text: qsTr("识别文字") },
        { id: "pin", text: qsTr("贴图") },
        { id: "save", text: qsTr("保存") },
        { id: "cancel", text: qsTr("取消") },
        { id: "copy", text: qsTr("复制") }
    ]
    function setButtonShown(id, shown) {
        const left = hiddenButtons.filter(h => h.length > 0 && h !== id)
        if (!shown)
            left.push(id)
        editor.captureToolbarHidden = left.join(",")
    }

    // A shortcut the user records (no default: PixPin and Snipaste have the usual ones), with what went wrong registering it.
    component HotkeyRow: Item {
        id: row

        property string title
        property string words
        property string hotkey
        property string error
        readonly property bool recording: recorder.recording
        signal recorded(string hotkey)

        width: parent.width
        height: Math.max(rowTexts.implicitHeight, rowControls.implicitHeight)

        Column {
            id: rowTexts
            anchors.left: parent.left
            anchors.right: rowControls.left
            anchors.rightMargin: 12
            spacing: 4

            Text {
                width: parent.width
                text: row.title
                color: Theme.text
                font.pixelSize: Theme.fontBody
            }
            Text {
                width: parent.width
                text: row.words
                color: Theme.subtext
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
            Text {
                visible: row.error.length > 0
                width: parent.width
                text: row.error
                color: Theme.danger
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
        }
        Row {
            id: rowControls
            anchors.right: parent.right
            spacing: 8

            FlatButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: row.hotkey.length > 0 && !recorder.recording
                flat: true
                text: qsTr("清除")
                onClicked: row.recorded("")
            }
            HotkeyRecorder {
                id: recorder
                anchors.verticalCenter: parent.verticalCenter
                hotkey: row.hotkey
                onRecorded: (hotkey) => row.recorded(hotkey)
            }
        }
    }

    implicitHeight: body.implicitHeight + 24
    radius: Theme.controlRadius
    color: marked ? Theme.selection : "transparent"
    border.width: 1
    border.color: Theme.cardBorder

    Column {
        id: body
        x: 14
        y: 12
        width: parent.width - 28
        spacing: 12

        Item { // the header
            width: parent.width
            height: Math.max(texts.implicitHeight, toggle.implicitHeight)

            Glyph {
                id: icon
                y: 2
                width: 32
                height: 32
                glyph: "" // Camera
                size: 24
                color: Theme.text
            }

            Column {
                id: texts
                anchors.left: icon.right
                anchors.leftMargin: 12
                anchors.right: toggle.left
                anchors.rightMargin: 12
                spacing: 4

                Row {
                    spacing: 8

                    Text {
                        text: qsTr("截图")
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        font.weight: Font.DemiBold
                    }
                    Tag {
                        text: qsTr("内置")
                    }
                }
                Text {
                    width: parent.width
                    text: qsTr("冻结屏幕，框选区域或点选窗口、控件，复制或保存；放大镜看像素和颜色")
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.Wrap
                }
                Text {
                    width: parent.width
                    text: !card.editor.captureEnabled ? qsTr("关着")
                          : card.editor.captureHotkey.length > 0 ? qsTr("开着：按 %1，或从托盘菜单截图").arg(card.editor.captureHotkey)
                          : card.editor.capturePrintScreen ? qsTr("开着：按 PrtScn，或从托盘菜单截图")
                          : qsTr("开着：从托盘菜单截图，或在下面录一个快捷键")
                    color: card.editor.captureEnabled ? Theme.text : Theme.subtext
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.Wrap
                }
            }

            ToggleSwitch {
                id: toggle
                anchors.right: parent.right
                y: 0
                checked: card.editor.captureEnabled
                onToggled: (on) => card.editor.captureEnabled = on
            }
        }

        Column { // the options, while it is on
            visible: card.editor.captureEnabled
            width: parent.width
            spacing: 12

            Rectangle { // between the header and the options
                width: parent.width
                height: Theme.edge(Screen.devicePixelRatio)
                color: Theme.cardBorder
            }

            Item {
                width: parent.width
                height: Math.max(hotkeyTexts.implicitHeight, hotkeyControls.implicitHeight)

                Column {
                    id: hotkeyTexts
                    anchors.left: parent.left
                    anchors.right: hotkeyControls.left
                    anchors.rightMargin: 12
                    spacing: 4

                    Text {
                        width: parent.width
                        text: qsTr("快捷键")
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                    }
                    Text {
                        width: parent.width
                        text: qsTr("按它开始截图，截图时再按一次取消")
                        color: Theme.subtext
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                    Text {
                        visible: card.editor.captureHotkeyError.length > 0
                        width: parent.width
                        text: card.editor.captureHotkeyError
                        color: Theme.danger
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }
                Row {
                    id: hotkeyControls
                    anchors.right: parent.right
                    spacing: 8

                    FlatButton {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: card.editor.captureHotkey.length > 0 && !hotkeyRecorder.recording
                        flat: true
                        text: qsTr("清除")
                        onClicked: card.editor.captureHotkey = ""
                    }
                    HotkeyRecorder {
                        id: hotkeyRecorder
                        anchors.verticalCenter: parent.verticalCenter
                        hotkey: card.editor.captureHotkey
                        onRecorded: (hotkey) => card.editor.captureHotkey = hotkey
                    }
                }
            }

            Item { // the screenshot history's keyword (jt)
                width: parent.width
                height: Math.max(keywordTexts.implicitHeight, keywordBox.implicitHeight)

                Column {
                    id: keywordTexts
                    anchors.left: parent.left
                    anchors.right: keywordBox.left
                    anchors.rightMargin: 12
                    spacing: 4

                    Text {
                        width: parent.width
                        text: qsTr("截图历史关键词")
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                    }
                    Text {
                        width: parent.width
                        text: qsTr("在搜索框输入“%1”加空格，列出截过的图，再输入文字按图里识别出的文字筛选；Enter 复制并粘贴，Ctrl+Enter 贴到屏幕，Delete 删除").arg(card.editor.captureKeyword)
                        color: Theme.subtext
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                    Text {
                        id: keywordError
                        visible: text.length > 0
                        width: parent.width
                        color: Theme.danger
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }
                InputBox {
                    id: keywordBox
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: 96
                    text: card.editor.captureKeyword
                    maximumLength: 16
                    function commit() {
                        keywordError.text = card.editor.saveCaptureKeyword(text)
                        if (keywordError.text.length === 0)
                            text = Qt.binding(() => card.editor.captureKeyword)
                    }
                    onAccepted: commit()
                    onEditingFinished: commit()
                }
            }

            Item {
                width: parent.width
                height: Math.max(printTexts.implicitHeight, printToggle.implicitHeight)

                Column {
                    id: printTexts
                    anchors.left: parent.left
                    anchors.right: printToggle.left
                    anchors.rightMargin: 12
                    spacing: 4

                    Text {
                        width: parent.width
                        text: qsTr("用 PrtScn 键截图")
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                    }
                    Text {
                        width: parent.width
                        text: qsTr("让 PrtScn 不再打开 Windows 的截图工具，改用这里的截图。Win顺 没在运行时，PrtScn 会把整个屏幕复制到剪贴板；关掉这项恢复原来的设置")
                        color: Theme.subtext
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                    Text {
                        visible: card.editor.captureNote.length > 0
                        width: parent.width
                        text: card.editor.captureNote
                        color: Theme.danger
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }
                ToggleSwitch {
                    id: printToggle
                    anchors.right: parent.right
                    checked: card.editor.capturePrintScreen
                    onToggled: (on) => card.editor.capturePrintScreen = on
                }
            }

            Item {
                width: parent.width
                height: Math.max(copyTexts.implicitHeight, copyToggle.implicitHeight)

                Column {
                    id: copyTexts
                    anchors.left: parent.left
                    anchors.right: copyToggle.left
                    anchors.rightMargin: 12
                    spacing: 4

                    Text {
                        width: parent.width
                        text: qsTr("选好就复制")
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                    }
                    Text {
                        width: parent.width
                        text: qsTr("松开鼠标或点选窗口后马上复制并关闭，不再调整。关着时：按 Enter 复制并关闭，Ctrl+S 保存到“图片 › 屏幕截图”，Esc 先取消选区、再退出；鼠标按键做什么在下面“框选后的鼠标操作”里设")
                        color: Theme.subtext
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }
                ToggleSwitch {
                    id: copyToggle
                    anchors.right: parent.right
                    checked: card.editor.captureCopyOnSelect
                    onToggled: (on) => card.editor.captureCopyOnSelect = on
                }
            }

            Repeater { // what goes into the picture and how controls are found (stage 7)
                model: [
                    { key: "captureCursor", title: qsTr("截图带上鼠标指针"),
                      words: qsTr("冻结的画面里画上截图那一刻的鼠标指针，复制、保存、贴图的图里都有它。关着时图里没有指针") },
                    { key: "captureControlsUia", title: qsTr("识别 Electron、WinUI 等程序里的控件"),
                      words: qsTr("鼠标移到 VS Code、微信、设置这类程序的窗口上时，也能框出里面的侧栏、编辑区等控件。查询在后台进行，最多等 150 毫秒，超时就只用普通子窗口，不会拖慢鼠标。提权运行的程序框不到控件") }
                ]
                delegate: Item {
                    id: pictureOption

                    required property var modelData

                    width: parent.width
                    height: Math.max(pictureTexts.implicitHeight, pictureToggle.implicitHeight)

                    Column {
                        id: pictureTexts
                        anchors.left: parent.left
                        anchors.right: pictureToggle.left
                        anchors.rightMargin: 12
                        spacing: 4

                        Text {
                            width: parent.width
                            text: pictureOption.modelData.title
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                        }
                        Text {
                            width: parent.width
                            text: pictureOption.modelData.words
                            color: Theme.subtext
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        }
                    }
                    ToggleSwitch {
                        id: pictureToggle
                        anchors.right: parent.right
                        checked: card.editor[pictureOption.modelData.key]
                        onToggled: (on) => card.editor[pictureOption.modelData.key] = on
                    }
                }
            }

            Item { // the delayed capture's seconds
                width: parent.width
                height: Math.max(delayTexts.implicitHeight, delayChoices.implicitHeight)

                Column {
                    id: delayTexts
                    anchors.left: parent.left
                    anchors.right: delayChoices.left
                    anchors.rightMargin: 12
                    spacing: 4

                    Text {
                        width: parent.width
                        text: qsTr("延时截图")
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                    }
                    Text {
                        width: parent.width
                        text: qsTr("托盘菜单“截图 › 延时截图”开始倒计时，屏幕角上有数字，到点才冻结屏幕，用来截菜单、悬停提示。截图时按 D 或点工具条的“延时”，也是关掉这次、倒计时后再截。再按一次截图快捷键，或点倒计时，取消")
                        color: Theme.subtext
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.Wrap
                    }
                }
                Row {
                    id: delayChoices
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8

                    Repeater {
                        model: [3, 5, 10]

                        delegate: FlatButton {
                            required property int modelData

                            highlighted: card.editor.captureDelay === modelData
                            text: qsTr("%1 秒").arg(modelData)
                            onClicked: card.editor.captureDelay = modelData
                        }
                    }
                }
            }

            HotkeyRow {
                id: pinsHotkeyRow
                title: qsTr("隐藏 / 显示全部贴图的快捷键")
                words: qsTr("按一下藏起所有贴图，再按一下放回来；新贴一张图时也会放回来。不设就只能从托盘菜单或贴图的右键菜单操作")
                hotkey: card.editor.capturePinsHotkey
                error: card.editor.capturePinsHotkeyError
                onRecorded: (hotkey) => card.editor.capturePinsHotkey = hotkey
            }

            HotkeyRow {
                id: restoreHotkeyRow
                title: qsTr("找回穿透贴图的快捷键")
                words: qsTr("设成“鼠标穿透”的贴图点不到时，按它让所有贴图都能点；平时按住 Ctrl 把鼠标放上去也行")
                hotkey: card.editor.capturePinsRestoreHotkey
                error: card.editor.capturePinsRestoreHotkeyError
                onRecorded: (hotkey) => card.editor.capturePinsRestoreHotkey = hotkey
            }

            Column { // what the mouse buttons do after a selection (issue #46)
                width: parent.width
                spacing: 4

                Text {
                    width: parent.width
                    text: qsTr("框选后的鼠标操作")
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                }
                Text {
                    width: parent.width
                    text: qsTr("选好区域之后，每个键按下去做什么。正在标注（选了标注工具、正在画或写字）时，右键用来撤回当前笔画，其他键不动作，不执行这里的设置；Esc 随时可以后退、重新框选")
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.Wrap
                }
                Text {
                    width: parent.width
                    text: qsTr("“识别文字并复制”读出图里的字放进剪贴板，“识别文字并搜索”把字送进 Win顺 的搜索框，“识别文字并网页搜索”用网页搜索列表里排第一的网站搜")
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.Wrap
                }
                Repeater {
                    model: [
                        { title: qsTr("双击左键"), prop: "captureMouseDoubleLeft", withBack: false },
                        { title: qsTr("右键"), prop: "captureMouseRight", withBack: true },
                        { title: qsTr("中键"), prop: "captureMouseMiddle", withBack: false },
                        { title: qsTr("侧键（后退）"), prop: "captureMouseBack", withBack: false },
                        { title: qsTr("侧键（前进）"), prop: "captureMouseForward", withBack: false }
                    ]

                    delegate: Item {
                        id: mouseRow

                        required property var modelData
                        required property int index
                        readonly property var choices: modelData.withBack ? card.rightChoices : card.mouseChoices
                        readonly property string value: card.mouseValues[index]
                        readonly property int current: choices.findIndex(c => c.value === value)

                        width: parent.width
                        height: 40

                        Text {
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            text: mouseRow.modelData.title
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                        }
                        FlatButton {
                            id: pick
                            anchors.right: parent.right
                            implicitWidth: card.pickWidth
                            anchors.verticalCenter: parent.verticalCenter
                            text: mouseRow.current >= 0 ? mouseRow.choices[mouseRow.current].text : ""
                            trailingGlyph: String.fromCharCode(0xE70D) // ChevronDown
                            onClicked: pickMenu.open = !pickMenu.open

                            DropMenu {
                                id: pickMenu
                                anchor: pick
                                entries: mouseRow.choices.map((c, i) => ({
                                    text: c.text,
                                    glyph: i === mouseRow.current ? String.fromCharCode(0xE73E) : "" // CheckMark
                                }))
                                onActivated: (i) => card.editor[mouseRow.modelData.prop] = mouseRow.choices[i].value
                            }
                        }
                    }
                }
            }

            Column { // smart redaction (4.1)
                width: parent.width
                spacing: 8

                Text {
                    width: parent.width
                    text: qsTr("智能打码")
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                }
                Text {
                    width: parent.width
                    text: qsTr("在打码工具里点“智能打码”（或按 Shift+B），自动找出图里的手机号、身份证号、银行卡号、邮箱、密钥和密码、本机用户名，一次全部盖住，Ctrl+Z 一次撤销。识别在本机完成，图和文字都不会发出去；自动找的可能有漏，发出去前再看一眼")
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.Wrap
                }
                Repeater {
                    model: [
                        { key: "captureSmartPrescan", title: qsTr("框选后自动识别"),
                          words: qsTr("选好区域停一会儿，就在后台找要打码的内容，找到了在工具条边上提示“发现 N 处”。关掉后，点“智能打码”时才识别") },
                        { key: "captureSmartIpMac", title: qsTr("也打码 IP 和 MAC 地址"), words: "" },
                        { key: "captureSmartAllText", title: qsTr("把所有文字都打码"), words: qsTr("点“智能打码”就盖住图里每一行字") }
                    ]
                    delegate: Item {
                        id: smartOption

                        required property var modelData

                        width: parent.width
                        height: Math.max(smartTexts.implicitHeight, smartToggle.implicitHeight)

                        Column {
                            id: smartTexts
                            anchors.left: parent.left
                            anchors.right: smartToggle.left
                            anchors.rightMargin: 12
                            spacing: 4

                            Text {
                                width: parent.width
                                text: smartOption.modelData.title
                                color: Theme.text
                                font.pixelSize: Theme.fontBody
                            }
                            Text {
                                visible: smartOption.modelData.words.length > 0
                                width: parent.width
                                text: smartOption.modelData.words
                                color: Theme.subtext
                                font.pixelSize: Theme.fontCaption
                                wrapMode: Text.Wrap
                            }
                        }
                        ToggleSwitch {
                            id: smartToggle
                            anchors.right: parent.right
                            checked: card.editor[smartOption.modelData.key]
                            onToggled: (on) => card.editor[smartOption.modelData.key] = on
                        }
                    }
                }
                Item { // the user's own words
                    width: parent.width
                    height: Math.max(wordsTexts.implicitHeight, wordsBox.implicitHeight)

                    Column {
                        id: wordsTexts
                        anchors.left: parent.left
                        anchors.right: wordsBox.left
                        anchors.rightMargin: 12
                        spacing: 4

                        Text {
                            width: parent.width
                            text: qsTr("我自己的词")
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                        }
                        Text {
                            width: parent.width
                            text: qsTr("图里出现这些词就打码，用逗号隔开，比如项目代号、客户名")
                            color: Theme.subtext
                            font.pixelSize: Theme.fontCaption
                            wrapMode: Text.Wrap
                        }
                    }
                    InputBox {
                        id: wordsBox
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: 220
                        text: card.editor.captureSmartWords
                        maximumLength: 400
                        function commit() {
                            card.editor.captureSmartWords = text
                            text = Qt.binding(() => card.editor.captureSmartWords)
                        }
                        onAccepted: commit()
                        onEditingFinished: commit()
                    }
                }
            }

            Column { // which action buttons the toolbar shows (#57)
                width: parent.width
                spacing: 8

                Text {
                    width: parent.width
                    text: qsTr("工具条上的按钮")
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                }
                Text {
                    width: parent.width
                    text: qsTr("点一下显示或隐藏，亮着的就是显示的。隐藏的按钮，快捷键照样能用：撤销 Ctrl+Z、重做 Ctrl+Y、复制 Enter、保存 Ctrl+S、贴图 F3、长截图 Ctrl+L、取消 Esc；二维码 Q、延时 D；识别文字没有快捷键，只能点工具条上的按钮，或设给上面的鼠标键。标注工具一直显示")
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.Wrap
                }
                Flow {
                    width: parent.width
                    spacing: 8

                    Repeater {
                        model: card.toolbarButtons

                        delegate: FlatButton {
                            id: toolbarChoice

                            required property var modelData
                            readonly property bool shown: card.hiddenButtons.indexOf(modelData.id) < 0

                            highlighted: shown
                            text: modelData.text
                            onClicked: card.setButtonShown(modelData.id, !shown)
                        }
                    }
                }
            }
        }
    }
}
