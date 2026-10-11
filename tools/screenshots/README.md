# 截图工具

README 里的图（`docs/images/*-light.png`、`*-dark.png`）和发帖用的图（小红书 3:4、1:1、X、GitHub 社交预览）都由这里生成：用演示数据启动 Win顺，用真实键鼠输入把窗口摆好，黑白两次背板抠出带真实透明度的窗口，再合成到生成的背景上。图里不会出现自己电脑上的文件。

## 目录

| 文件 | 作用 |
| --- | --- |
| `shots.py` | 命令行入口：`scene` / `restore` / `profile` / `shoot` / `matte` / `compose` / `cover` / `taskbar` / `sheet` / `render`，各命令的说明见文件开头 |
| `art.py` | 出图：背景、去掉 Windows 自带阴影（`lifted`）、三层阴影、README 图、帖子内页、封面、任务栏那一页 |
| `capture.py` | 抠图：窗口后面放黑、白背板各拍一张算出透明度，第三张黑的用来发现光标闪烁和没停的动画 |
| `stage.py` | 布景：只索引演示文件夹的配置（`--profile`），临时换应用主题和壁纸、拍完换回 |
| `agent.ps1` | 提权运行的输入代理：执行场景文件里的按键、点击、抠图命令，每次按键前确认前台是 Win顺 |
| `scenes/` | 场景文件（代理的命令序列）：`all.txt` 拍全部，`light.txt`/`dark.txt` → `windows.txt`，`clipboard.txt` 填剪贴板历史，`taskbar.txt` 任务栏搜索框；`区域销售额.png` 是剪贴板历史里那张图 |
| `demo/` | 演示文件夹的内容，`profile` 发现 `F:\Win顺演示` 不在时复制过去（这个路径会出现在对话框截图里） |
| `work/` | 不进 git：`shots/` 原始抠图，`profile/` 演示配置，`out/` 帖子用图，`agent.txt` 代理日志 |

需要 Python 3.10+、Pillow、numpy。

## 只重新出图

改了背景、标题、排版，抠图不用重拍：

```
python tools/screenshots/shots.py render            # 全部
python tools/screenshots/shots.py render readme     # 只 README 的 14 张（写进 docs/images）
python tools/screenshots/shots.py render pages square covers   # 帖子用图，写进 work/out
```

帖子各页的标题、副标题，封面用哪张抠图、哪种背景，都在 `shots.py` 的 `PAGES`、`TASKBAR_PAGE`、`COVERS` 里。挑背景：`shots.py sheet work/shots/L-search.png -o sheet.png`。

## 重拍

1. 构建并部署到 `dist\WinShun`（代理启动的就是那里的 `WinShun.exe`）。
2. 在**提权**的 PowerShell 里：

   ```
   tools\screenshots\agent.ps1 scenes\all.txt
   ```

   依次：用演示配置重启 Win顺 → 填剪贴板历史 → 浅色主题 + dawn 壁纸拍 7 个窗口 → 任务栏搜索框 → 深色主题 + aurora 壁纸再拍 7 个 → 换回原来的主题和壁纸 → 用自己的配置重启 Win顺。抠图写进 `work/shots/`。拍的时候别动键鼠；哪一步前台不对，代理就停下，原因写在 `work/agent.txt`。
3. 逐张看 `work/shots/` 的抠图：范围标签对不对、查询文字有没有被选中、有没有自己的文件。
4. `shots.py render`。

只补拍一部分：照 `all.txt` 写一个场景文件（先 `profile start`，需要剪贴板就 `run scenes\clipboard.txt`，最后 `restore`、`profile stop`）。不带参数运行 `agent.ps1` 时，它执行每次写进 `work\cmd.txt` 的一批命令，适合边看边拍。

## 换了屏幕要改的

这几处是按 2560×1600 的屏幕量的：

- `scenes/windows.txt` 里点对话框下搜索框的 `clickws 400 956`；
- `scenes/taskbar.txt` 里点任务栏搜索框的 `clickbox 397 1564`；
- `art.py` 的 `TASKBAR`：全屏截图 `screen-taskbar.png` 上启动器、任务栏、搜索框的位置。

## 注意

- 启动器记住上次的范围，`Tab` 依次是 全部 → 文件 → 文件夹 → 内容，Win顺 刚启动时在“全部”。`windows.txt` 从“全部”开始，也停在“全部”，顺序改了按键就要跟着改。
- 坑和原因见 `docs/pitfalls.md` 的“README 和帖子的截图”一节：Mica 取壁纸颜色、提权后置顶失效、剪贴板窗口不在前台、演示配置混进自己的东西、去阴影时窗口和 logo 分开算。
