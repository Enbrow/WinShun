# 截图工具

README 里的图（`docs/images/*.jpg`：帖子的方形页面和方形封面，1080 宽的 JPEG）和发帖用的图（小红书 3:4、1:1、X、GitHub 社交预览）都由这里生成：用演示数据启动 Win顺，用真实键鼠输入把窗口摆好，黑白两次背板抠出带真实透明度的窗口，再合成到生成的背景上。图里不会出现自己电脑上的文件。

## 目录

| 文件 | 作用 |
| --- | --- |
| `shots.py` | 命令行入口：`scene` / `restore` / `profile` / `shoot` / `matte` / `compose` / `cover` / `taskbar` / `sheet` / `render`，各命令的说明见文件开头 |
| `art.py` | 出图：背景、去掉 Windows 自带阴影（`lifted`）、三层阴影、README 图、帖子内页、封面、任务栏那一页 |
| `capture.py` | 抠图：窗口后面放黑、白背板各拍一张算出透明度，第三张黑的用来发现光标闪烁和没停的动画 |
| `stage.py` | 布景：只索引演示文件夹的配置（`--profile`，每次 `profile start` 重新生成：演示文件的修改时间、收藏夹和关键词、剪贴板历史都直接写进配置），临时换应用主题和壁纸、拍完换回 |
| `agent.ps1` | 提权运行的输入代理：执行场景文件里的按键、点击、拖动（`dragws`，`dragfg` 从前台窗口的边框量起）、抠图命令，每次按键前确认前台是 Win顺 |
| `scenes/` | 场景文件（代理的命令序列）：`all.txt` 拍全部，`light.txt`/`dark.txt` → `windows.txt`（也拍 README 另有的系统开关那三个窗口），`taskbar.txt` 任务栏搜索框，`post.txt` 帖子另有的几张（“全部”里的序号、最新的在前、网站搜索、剪贴板搜图里的字、设置搜索、设置和 AI 页），`more.txt` README 另有的七张（按意思找、只搜一个文件夹、外观设置，再跑 `features.txt`：搜索面板和预览，`foldermenu.txt`：资源管理器里双击空白处的菜单，`edit.txt`：截图编辑窗口，`edit-one.txt` 是它的一遍，`edit-style.txt` 是文字样式那几下点击）；`edit-dark.txt` 拍深色的编辑窗口，要单独跑一次（编辑窗口在同一次运行里第二次打开，没有阴影和圆角，抠出来是不透明的）；`区域销售额.png` 是剪贴板历史里那张图（数字用普惠体，Windows 的文字识别才不会把 371 认成 571） |
| `demo_docs.py` | 生成演示文件夹里的 Word、Excel、PowerPoint、PDF（有内容、有大小，面板里不显示 0 B，PDF 有缩略图，内容搜索搜得到） |
| `demo/` | 演示文件夹的内容，`profile` 发现 `F:\Win顺演示` 不在时复制过去（这个路径会出现在对话框截图里） |
| `work/` | 不进 git：`shots/` 原始抠图，`profile/` 演示配置，`out/` 帖子用图，`agent.txt` 代理日志 |

需要 Python 3.10+、Pillow、numpy。

## 只重新出图

改了背景、标题、排版，抠图不用重拍：

```
python tools/screenshots/shots.py render            # 全部
python tools/screenshots/shots.py render readme     # 只 README 的 25 张（写进 docs/images）
python tools/screenshots/shots.py render pages square covers   # 帖子用图，写进 work/out
```

README 的图是帖子各页加上 `shots.py` 的 `README_ONLY`（帖子最多 18 张，放不下的只进 README）；帖子各页的标题、副标题，封面用哪张抠图、哪种背景，都在 `PAGES`、`TASKBAR_PAGE`、`COVERS` 里。几张矮窗口可以用 `+` 连起来叠成一张（`L-plug1+L-plug2+L-plug3`）。挑背景：`shots.py sheet work/shots/L-search.png -o sheet.png`。

## 重拍

1. 构建并部署到 `dist\WinShun`（代理启动的就是那里的 `WinShun.exe`）。
2. 在**提权**的 PowerShell 里：

   ```
   tools\screenshots\agent.ps1 scenes\all.txt
   ```

   依次：用演示配置重启 Win顺 → 删掉 Win顺 启动时记下的系统剪贴板那一条 → 浅色主题 + dawn 壁纸拍 14 张窗口 → 任务栏搜索框 → 深色主题 + aurora 壁纸再拍 14 张 → 换回原来的主题和壁纸 → 用自己的配置重启 Win顺。抠图写进 `work/shots/`。拍的时候别动键鼠；哪一步前台不对，代理就停下，原因写在 `work/agent.txt`。
3. 逐张看 `work/shots/` 的抠图：范围标签对不对、查询文字有没有被选中、有没有自己的文件。
4. `shots.py render`。

只补拍一部分：照 `all.txt` 写一个场景文件（先 `profile start`，拍剪贴板就照它先删掉最上面那一条，最后 `restore`、`profile stop`）。不带参数运行 `agent.ps1` 时，它执行每次写进 `work\cmd.txt` 的一批命令，适合边看边拍；`profile start` 后面可以跟 `shots.py profile` 的参数，比如 `--number-keys off`（图标上就没有序号）；`quit` 关掉 Win顺（原来就没在运行时用它代替 `profile stop`）。README 另有的七张：`agent.ps1 scenes\more.txt`。预览开在启动器旁边，是另一个窗口：`matte <名字> beside` 把贴着前台窗口的 Win顺 窗口也抠进来；演示文件夹里的 `年度报告 封面.png` 是真的图片，预览和结果里的缩略图才有东西可看。按意思找用的是自己设置里的模型服务和密钥（密钥在 Windows 凭据管理器里，不写进配置），演示配置把它指向演示文件夹，拍的时候会把这几个演示文件的文字发给那个服务算向量；拍双击菜单前关掉自己的资源管理器和 Directory Opus 窗口，菜单会列出所有开着的文件夹；`profile start` 启动 Win顺 时带 `WINSHUN_RECENT=own`，最近的文件夹只取演示配置自己的记录，不取 Windows 的。帖子最后一页（`17-follow`）不用截图，由 `art.ending` 画出来。

## 换了屏幕要改的

这几处是按 2560×1600 的屏幕量的：

- `scenes/windows.txt` 里点对话框下搜索框的 `clickws 400 1013`（搜索框紧贴在对话框下边：对话框的位置和大小变了，这个点也要跟着改）；
- `scenes/taskbar.txt` 里点任务栏搜索框的 `clickbox 192 1564`（框在任务栏最左端，一个固定的位置）；
- `scenes/more.txt` 里点设置“外观”页的 `clickfg 120 252`，`scenes/edit-one.txt` 里编辑窗口的画框、画箭头、点文字面板（`dragfg` / `clickfg`，图片 100% 显示，左上角在窗口边框内 (81,174)；换了屏幕缩放要重量），`scenes/foldermenu.txt` 里资源管理器窗口的位置（`explorer place`）和双击的点；
- `art.py` 的 `TASKBAR`：全屏截图 `screen-taskbar.png` 上启动器、任务栏、搜索框的位置。

## 注意

- 启动器记住上次的范围和排序，Win顺 刚启动时在“全部”、最匹配。`Tab` 换范围：全部 → 文件 → 内容（`Shift+Tab` 是文件类型那一排，GitHub #16）；选中的是文件夹时 `Tab` 会进入它，所以换范围前让列表里没有文件夹（先打一个什么都搜不到的词）。`windows.txt` 从“全部”开始，也停在“全部”，顺序改了按键就要跟着改。
- 代理会丢掉参数开头的空格：要打“关键词 + 空格 + 文字”，先 `tap 23`（End）、`tap 20`（空格）再 `type`。
- 开拍前系统剪贴板里要有东西（比如复制一段字）：场景开头按 Delete 删掉 Win顺 启动时记下的那一条，剪贴板是空的时它什么都没记，删掉的就是演示历史里最新的那张图片，剪贴板搜图里的字那张就没了。
- Windows 11 的文本光标指示器（品红色）可能被抠进图里；演示配置只排除了各盘的文件夹，盘根目录下的文件仍能搜到，查询别碰上它们。
- 坑和原因见 `docs/pitfalls.md` 的“README 和帖子的截图”一节：Mica 取壁纸颜色、提权后置顶失效、剪贴板窗口不在前台、演示配置混进自己的东西、去阴影时窗口和 logo 分开算。
