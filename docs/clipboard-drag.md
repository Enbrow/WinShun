# 剪贴板图片拖出 / Dragging Clipboard Images

按住剪贴板列表中的图片条目，用鼠标左键拖到资源管理器文件夹，
即可保存为 `image_<记录编号>.png`。拖动途中按 Esc 可以取消。

导出是复制，不会移动或删除剪贴板历史中的原图。拖动不会触发点击粘贴，
文字条目、右键菜单、条目操作按钮以及触摸滚动保留原有行为。
当前只拖出鼠标按住的一个图片条目，不导出其他选中的条目。

文件名使用历史记录编号，同一条目再次导出时名称相同，
目标文件夹有同名文件时由资源管理器处理冲突。
导出直接复制 Win顺已经保存的 PNG，不重新编码或缩放；
它不是恢复来源图片的原文件名或原文件格式。

临时导出副本保留到程序退出，让目标程序能够完成异步复制。
正常退出时自动清理；异常终止可能留下临时目录。
导出只允许复制，不允许移动或创建指向临时文件的快捷方式。

## English

Hold the left mouse button on an image entry in clipboard history and drag it
into an Explorer folder to save it as `image_<entry-id>.png`. Press Escape
during the drag to cancel.

Exporting copies the image and does not move or delete the history entry.
The drag does not trigger click-to-paste. Text entries, context menus, action
buttons and touch scrolling retain their existing behavior. Only the image
entry being dragged is exported, not other selected entries.

The filename uses the history entry ID, so exporting the same entry again
uses the same name. Explorer handles destination filename conflicts.
The export copies the stored PNG without re-encoding or resizing it; it does
not recover the source image's original filename or file format.

Temporary exports remain available until application shutdown so drop targets
can finish asynchronous copies. Normal shutdown removes them; an abnormal
termination may leave the temporary directory behind. Drops allow copying
only, not moving or creating shortcuts to temporary files.
