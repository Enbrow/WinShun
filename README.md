<p align="center">
  <img src="docs/images/icon.png" width="128" height="128" alt="Win顺 app icon">
</p>

<h1 align="center">Win顺 · WinShun</h1>

<p align="center">
  <b>双击 Ctrl，搜遍整台电脑</b> —— 免费开源的 Windows 文件搜索、快速启动器<br>
  <b>Press Ctrl twice, search your whole PC</b> — a free, open-source file search and launcher for Windows
</p>

<p align="center">
  <a href="https://github.com/LingCore/WinShun/releases/latest"><img src="https://img.shields.io/github/v/release/LingCore/WinShun?label=%E4%B8%8B%E8%BD%BD%20Download" alt="Download"></a>
  <img src="https://img.shields.io/badge/Windows-10%20%7C%2011-0078D4" alt="Windows 10 | 11">
  <img src="https://img.shields.io/badge/x64-64--bit-blue" alt="x64">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-green" alt="MIT"></a>
  <a href="https://linux.do"><img src="https://img.shields.io/badge/LINUX%20DO-%E7%A4%BE%E5%8C%BA%20Community-1f1f1f" alt="LINUX DO 社区"></a>
</p>

<p align="center">
  <a href="#中文">中文</a> · <a href="#english">English</a> · <a href="#screenshots">截图 Screenshots</a>
</p>

<p align="center">
  <img src="docs/images/cover.jpg" width="640" alt="Win顺 WinShun：让你的电脑更好用，11 合 1，双击 Ctrl 全搞定，免费开源 / a free Windows search launcher, eleven tools in one, press Ctrl twice">
</p>

<p align="center">
  拼音秒搜文件 · 文件内容全文搜索（Word / Excel / PPT / PDF） · 应用和系统设置 · 网页搜索 · 收藏夹 · 插件<br>
  “打开 / 另存为”对话框搜索 · 剪贴板历史（代替 Win+V，图片文字也能搜） · 任务栏搜索框（代替 Win+S） · AI 助手 MCP<br>
  <sub>Pinyin file search · full-text search in documents · apps and settings · web search · favorites · plugins · file dialog search · clipboard history with OCR · taskbar search box · MCP server for AI assistants</sub>
</p>

---

<a id="screenshots"></a>

## 截图 · Screenshots

<p align="center">
  <img src="docs/images/search.jpg" width="49%" alt="打拼音首字母秒找文件：ndbg 找到年度报告 / pinyin initials ndbg find 年度报告">
  <img src="docs/images/numbers.jpg" width="49%" alt="图标上的序号，Ctrl+1～9 直接打开 / row numbers on the icons, Ctrl+1–9 opens a row">
  <img src="docs/images/newest.jpg" width="49%" alt="按修改时间排序，找最新的那一版 / sort by date modified, newest first">
  <img src="docs/images/content.jpg" width="49%" alt="文件内容全文搜索：Word、Excel、PPT、PDF、代码 / full-text search inside documents and code">
  <img src="docs/images/apps.jpg" width="49%" alt="拼音搜应用：guanliqi 找到任务管理器 / launch apps by pinyin">
  <img src="docs/images/places.jpg" width="49%" alt="直达 Windows 设置：shipeiqi 找到网络连接 / open Windows settings and Control Panel items">
  <img src="docs/images/web.jpg" width="49%" alt="网页搜索：zhihu 显卡、bilibili、京东一步到位 / web search keywords">
  <img src="docs/images/favorites.jpg" width="49%" alt="收藏夹：常去的文件夹排在最前，关键词直达 / favorites with keywords">
  <img src="docs/images/plugins.jpg" width="49%" alt="插件：计算器、单位换算、颜色 / plugins: calculator, unit conversion, colors">
  <img src="docs/images/dialog.jpg" width="49%" alt="打开、另存为对话框旁的搜索框，Ctrl+G 转到资源管理器的文件夹 / search bar for Open and Save dialogs">
  <img src="docs/images/clipboard.jpg" width="49%" alt="剪贴板历史，代替 Win+V，拼音搜索、图片预览 / clipboard history replacing Win+V">
  <img src="docs/images/clipocr.jpg" width="49%" alt="剪贴板 OCR：搜图片里的文字 / clipboard OCR: search words in copied pictures">
  <img src="docs/images/taskbar.jpg" width="49%" alt="任务栏搜索框，代替 Windows 搜索 / a search box on the taskbar instead of Windows search">
  <img src="docs/images/ai.jpg" width="49%" alt="AI 助手 MCP：Claude、Cursor、VS Code 一键连接 / MCP server for Claude, Cursor and VS Code">
  <img src="docs/images/settings.jpg" width="49%" alt="设置可搜索：kjj 找到快捷键 / searchable settings">
  <img src="docs/images/dark.jpg" width="49%" alt="深色模式，Win11 云母效果 / dark theme with Windows 11 Mica">
</p>

---

<a id="中文"></a>

## 中文

**Win顺** 是 Windows 上免费开源的快速搜索启动器：连按两下 Ctrl 弹出搜索框，边打字边出结果。按文件名或拼音秒找文件，搜 Word、Excel、PDF 里的文字，打开应用和系统设置，还有剪贴板历史、对话框搜索、任务栏搜索框、插件和给 AI 用的 MCP 搜索。

它直接读 NTFS 主文件表（MFT）建索引，三百多万个文件几秒建好，增删改名实时跟上，**不用装 Everything**。不需要账号，不上传数据。界面有简体中文和英文。

### 功能

#### 🔍 文件搜索

- **双击 Ctrl** 打开，再按一次或 `Esc` 关闭；三百多万个文件里搜一次只要几毫秒。
- **拼音和首字母**：`ndbg` → 年度报告，`baog` → 报告；多音字按常用读音。打错了同音字（年度报高）找不到时，自动按读音再找一遍。
- **搜整台电脑**：所有本地磁盘，U 盘可选（没收录的 U 盘插着时，列表末尾会提示你顺手搜一下它）；**NAS、内网共享文件夹（SMB）** 也能加进来一起搜，服务器关机时收录过的照样能搜到。
- **排序懂你**：自己的文件在前，系统目录、`node_modules` 在后，名字完全一样的最前。每行显示修改日期，`Ctrl+S` 在最匹配、最新、最早之间切换。不输入时列出最近打开或最近修改的文件。
- **数字键直达**：结果的图标上标着序号，`Ctrl+1`～`Ctrl+9` 打开第几行（也可改成 `Alt`）。
- **按类型找**：选中 **文件** 后，下面一排是文件夹、文档、图片、视频、压缩包等类型，`Tab` 依次切换；点 `+` 或在设置里自己加类型。
- **拖出去就能用**：结果直接拖进微信、邮件、资源管理器。
- **实时更新**：读 USN 变更日志，改名、新建、删除马上搜得到；拔 U 盘不会提示“设备正在使用”。

| 搜索语法 | 含义 |
|---|---|
| `报告 2024` | 每个词都要有（空格分隔，顺序不限）；也可以在所在文件夹的名字里 |
| `ndbg`、`niandubaogao` | 拼音全拼、首字母，可以混用 |
| `"年度 报告"` | 引号内作为整体 |
| `!草稿`、`!node_modules\` | 排除文件名、排除文件夹 |
| `*.pdf`、`报告?.docx` | 通配符 |
| `ext:pdf,docx` | 只要这些扩展名 |
| `项目\readme` | 在名字含“项目”的文件夹下 |
| `tools/out/`、`D:\My Files\` | 路径直达：回车进去，列出里面的内容 |

#### 📄 文件内容全文搜索

- 切到 **内容** 范围，搜文件里的文字，结果显示命中的那一行。
- **能搜的文件**：
  - 纯文本：txt、md、log、csv、json、代码等；
  - 文档：**Word、Excel、PowerPoint、PDF、WPS**，结果里显示第几页、第几张幻灯片或哪个表的第几行。
- **怎么找**：可以写多个词、用引号找整个短语，结果按相关度排。
- **编码和速度**：自动识别 UTF-8、UTF-16、GBK 编码；有内容索引，几十万个文件也很快。超过大小上限的、系统目录里的、类型没列出的文件默认不搜，搜完会告诉你各有几个，点一下就临时把它们也搜了。
- **安全**：文档由一个权限受限的独立进程读取。

#### 🚀 应用和系统设置

- **应用**：开始菜单里的所有应用都能搜，含 Microsoft Store 应用。名称、拼音、首字母（`vsc`）、程序名（`winword`）都行；`Ctrl+Shift+Enter` 以管理员身份运行。
- **系统设置**：约 1200 个 Windows 设置页和控制面板项。`shipeiqi`、“网卡”、`ncpa.cpl` 都能打开网络连接；另外补了组策略、磁盘管理、hosts、AppData 等开始菜单里没有的。

#### 🌐 网页搜索 · ⭐ 收藏夹 · 🧩 插件

- **网页搜索**：`baidu 天气`、`zhihu 显卡`、`jingdong 键盘`，关键词加空格就在那个网站上搜。
  - 自带百度、必应、B 站、知乎、微博、抖音、小红书、淘宝、京东、翻译。
  - 可以用 `%s` 网址添加自己的网站，图标、列表都能导入导出。
- **收藏夹**：
  - 右键“加入收藏夹”，不打字时排在最前；
  - 起个关键词，一打就开；
  - 收藏的文件夹按 `Tab` 进去接着搜。
- **插件**：
  - 随包的小工具：`= 1+2*3` 计算器、`12 inch to cm` 单位换算、`#ff8800` 颜色，还有时间戳、UUID、Base64、哈希；
  - `kill` 结束进程，`bm` 搜浏览器书签，`tr` 翻译；
  - 自己写插件：零代码的 `plugin.json`、任意语言的脚本或常驻进程都行，附 Python / PowerShell / C++ SDK；
  - 插件以普通权限、限内存运行。见 [docs/plugins.md](docs/plugins.md)。

#### 📂 “打开”“另存为”对话框

- **对话框旁的搜索框**：输入文件夹或文件名（拼音也行），回车，对话框直接转过去；网页上传文件、WPS 的对话框也认。
- **`Ctrl+G`**：转到资源管理器、Total Commander 或 Directory Opus 正在显示的文件夹，和 Listary 的同名快捷键一样。
- **不输入时**：列出打开着的文件夹、刚复制的路径、收藏和最近用过的文件夹。
- **自动转过去**：可以设成对话框一出现就转到文件管理器的文件夹。

#### 📋 剪贴板历史（代替 Win+V）

- **记什么**：复制过的文字、图片、文件都记下来。
- **怎么打开**：`Win+V` 或自设的快捷键；开在输入光标旁，不抢焦点。
- **怎么粘贴**：双击或 `Enter` 粘贴，`Shift+Enter` 粘贴为纯文本，`Alt+1`～`9` 直接粘贴第几条（序号标在图标上）。
- **能搜**：拼音搜索，`@wx` 只看微信里复制的；**图片里的字也能搜**（Windows 自带的文字识别 OCR，在本机，不联网）。
- **预览**：全文、大图、颜色值的 HEX / RGB / HSL。
- **整理**：固定、分组、多选按顺序合并粘贴，钉住后可以一条接一条地往表格里填。
- **隐私**：密码管理器复制的密码不记录，历史只存在本机。

#### 🔎 任务栏搜索框（代替 Windows 搜索）

- 在 Windows 11 任务栏上放 Win顺 自己的搜索框（Windows 10 是固定一个按钮），结果从任务栏上方滑出。
- 可以像 Windows 自带的搜索框一样放在开始按钮右边，图标往右让出位置（设置里打开，默认关）。
- 可以接管 `Win+S`，开始菜单里打字也用 Win顺 搜。

#### 🤖 AI 助手（MCP）

- **能做什么**：Win顺 是一个 MCP 服务器，AI 能用它搜文件名、文件内容和最近修改的文件。
- **怎么连**：Claude Desktop、Claude Code、Cursor、VS Code 在设置里一键连接。
- **安全**：
  - 默认只读，只看得到 AI 客户端自己有权限看的文件；
  - 可以设不让 AI 看到的文件夹；
  - 每次调用都有记录。

#### 🎨 外观和设置

- **外观**：浅色、深色跟随系统，Windows 11 云母（Mica）效果和圆角；图标在 150% 缩放下也清晰。
- **设置**：可以搜索（`Ctrl+F`，`kjj` 找到“快捷键”），改了立即生效。
- **更新**：有新版本时在托盘提醒，Win顺 不会自己下载、替换程序文件。

| 按键 | 作用 |
|---|---|
| 双击 `Ctrl` | 打开 / 关闭搜索框（可另设组合键，如 `Alt+Space`） |
| `Enter` / `Ctrl+Enter` / `Ctrl+Shift+Enter` | 打开 / 打开所在文件夹 / 以管理员身份运行 |
| `Ctrl+C` / `Ctrl+Shift+C` | 复制文件 / 复制路径 |
| `Ctrl+1`～`Ctrl+9` | 打开第几行（图标上的数字） |
| `Tab` / `Shift+Tab` | 切换范围：全部 / 文件 / 内容；在 **文件** 里 `Tab` 切换类型 |
| `Ctrl+S` | 最匹配 → 最新在前 → 最早在前 |
| `Ctrl+G`（对话框里） | 转到文件管理器正在显示的文件夹 |
| `Win+V` / `Win+S`（设置里打开后） | 剪贴板历史 / 任务栏上方的搜索框 |

### 下载安装

1. 到 [Releases 页面](https://github.com/LingCore/WinShun/releases/latest) 下载 `WinShun-版本号-x64-setup.exe`，双击安装。Windows 提示“已保护你的电脑”时，点 **更多信息 → 仍要运行**（还没有代码签名证书）。
2. 装完托盘里出现 Win顺 的图标，索引几秒就建好。连按两下 Ctrl 开始搜索。
3. 升级时运行新的安装程序，设置和索引都保留。卸载在 Windows 的“设置 → 应用”里。

系统要求：Windows 10 或 11，64 位。

### 为什么需要管理员权限

- **为什么要**：读 NTFS 主文件表（MFT）和 USN 变更日志，才能几秒建好索引、实时跟上改动。开机自启用的是最高权限的计划任务，所以开机不弹 UAC。
- **打开的东西仍是普通权限**：从 Win顺 打开的文件和应用都以普通权限运行，只有 `Ctrl+Shift+Enter` 才以管理员身份运行。

### 常见问题

**和 Everything、Listary 有什么区别？**
Win顺 自己建索引，不用装 Everything。文件名、拼音、应用和文件内容在一个框里搜，针对中文做了优化。对话框里有和 Listary 一样的 `Ctrl+G` 和搜索框，但不嵌进资源管理器窗口。

**双击 Ctrl 和别的软件冲突？**
Listary、PowerToys 的“查找鼠标”默认也用双击 Ctrl，关掉其中一边就行。也可以在 Win顺 里改用别的组合键。游戏全屏、藏起鼠标时，Win顺 默认不响应双击 Ctrl。

**AutoHotkey 改的键在 Win顺 里不起作用？**
Win顺 以管理员身份运行，Windows 不让普通权限的程序拦截发给它的按键。用 AutoHotkey 的 UI Access 模式，或以管理员身份运行脚本。

**占多少内存？索引能不放 C 盘吗？**
- **内存**：每 100 万个文件约 30 MB，整个程序一般 120～140 MB。
- **索引位置**：在设置 → 高级 → 索引位置里，可以移到其他内置硬盘。

**收费吗？会上传数据吗？**
完全免费，MIT 开源。只在两种时候联网：到 GitHub 检查新版本（可以关掉），第一次显示某个网页搜索时取一次那个网站的图标。索引和历史只存在本机（`%LOCALAPPDATA%\WinShun`）。

### 反馈

遇到问题或有建议，欢迎在 [Issues](https://github.com/LingCore/WinShun/issues) 里提出。

### 从源码编译

需要 Visual Studio 2022（“使用 C++ 的桌面开发”）、Qt 6.8 或更新版本（推荐 6.12，MSVC 2022 64 位套件）、CMake 3.25+ 和 Ninja。

```powershell
./scripts/build.ps1                                # 编译 + 运行单元测试
./scripts/build.ps1 -QtDir C:\Qt\6.12.0\msvc2022_64 # 指定 Qt（默认用 QTDIR 或 C:\Qt 下最新的版本）
./scripts/build.ps1 -Deploy                        # 生成可分发的文件夹 dist\WinShun
./scripts/release.ps1                              # 打包安装程序和 SHA256SUMS.txt（需要 Inno Setup 6.5+）；加 -Publish 发布到 GitHub
```

也可以用 Qt Creator 或 VS Code（CMake Tools）打开，预设见 `CMakePresets.json`。源码结构和设计见 [docs/architecture.md](docs/architecture.md)，踩过的坑见 [docs/pitfalls.md](docs/pitfalls.md)。

---

<a id="english"></a>

## English

**WinShun** (Win顺, "Windows made smooth") is a free, open-source search launcher for Windows. Press Ctrl twice and a search bar pops up, with results as you type:

- find files and folders by name or by pinyin;
- search the text inside Word, Excel and PDF files;
- launch apps and open Windows settings.

It also brings a clipboard history, a search bar for file dialogs, a taskbar search box, plugins and an MCP server for AI assistants.

It reads the NTFS master file table (MFT) directly, indexing three million files in a few seconds and following every change live: **no Everything install needed**. It needs no account and uploads nothing. The interface comes in English and Simplified Chinese. Screenshots: [above](#screenshots).

### Features

- 🔍 **File search**
  - **Speed**: press Ctrl twice; a search over three million files takes milliseconds.
  - **Pinyin**: `ndbg` finds 年度报告; a mistyped homophone (年度报高) is retried by sound.
  - **Where**: all local drives, USB drives if you like (an unindexed one that is plugged in is offered at the end of the list), and **NAS / SMB network shares**, still searchable while their server is off.
  - **Order**: your own files first, the date modified on each row, `Ctrl+S` for best match / newest / oldest. Recent files are listed when the box is empty.
  - **Shortcuts**: `Ctrl+1`…`Ctrl+9` opens a row (its number is on its icon).
  - **By kind**: under **Files**, a row of kinds (folders, documents, pictures, videos, archives…); `Tab` steps through them, `+` adds your own.
  - **Drag out**: drag results into a chat, a mail or Explorer.
  - **Query syntax**: `"exact phrase"`, `!exclude`, `!node_modules\`, wildcards `*` `?`, `ext:pdf,docx`, `folder\name`, and paths (`D:\My Files\`, `tools/out/`).
- 📄 **Full-text search**
  - **What**: the **Content** scope searches inside txt, md, logs, source code and **Word, Excel, PowerPoint, PDF and WPS** files, showing the page, slide or sheet and row.
  - **How**: ranked by relevance, with a content index for speed; UTF-8, UTF-16 and GBK are detected. Files over the size limit, in system folders or of unlisted text types are skipped; the list says how many of each, and one click searches them too.
  - **Safety**: documents are read by a separate process with restricted rights.
- 🚀 **Apps and settings**
  - **Apps**: everything in the Start menu (Store apps included), by name, pinyin, initials (`vsc`) or program file (`winword`).
  - **Settings**: about 1,200 Windows settings pages and Control Panel items (`ncpa.cpl`, “network adapter”), plus Group Policy, Disk Management, hosts, AppData and more.
- 🌐 **Web search**: `zhihu 显卡`, `baidu 天气`: a keyword and a space search that site. Ten Chinese sites are built in; add your own with `%s` in the URL.
- ⭐ **Favorites**: right-click → Add to favorites. They come first when the box is empty; give one a keyword to open it, and `Tab` searches inside a favorite folder.
- 🧩 **Plugins**
  - **Bundled**: `= 1+2*3`, `12 inch to cm`, `#ff8800`, timestamps, UUID, Base64, hashes; `kill` processes, `bm` browser bookmarks, `tr` translation.
  - **Your own**: zero-code `plugin.json`, scripts in any language, or resident processes, with SDKs for Python, PowerShell and C++. Plugins run with normal rights under a memory limit. See [docs/plugins.en.md](docs/plugins.en.md).
- 📂 **Open / Save dialogs**
  - **Search bar**: a search bar under every file dialog; type a name (pinyin too), press Enter, and the dialog goes there.
  - **`Ctrl+G`**: jumps to the folder open in File Explorer, Total Commander or Directory Opus, like Listary.
  - **Suggestions**: open folders, a path you just copied, favorites and recent folders.
- 📋 **Clipboard history (replaces Win+V)**
  - **What**: text, pictures and files, opened with `Win+V` or your own shortcut, by the text cursor, without taking the focus.
  - **Search**: pinyin, `@app` filters, and **words in copied pictures** (Windows' own OCR, on your PC).
  - **Preview**: full text, pictures, HEX / RGB / HSL color values.
  - **Organize**: pins, groups, multi-paste in order, and `Alt+1`…`Alt+9`.
  - **Privacy**: passwords from password managers are not kept, and the history stays on your PC.
- 🔎 **Taskbar search box (replaces Windows search)**: WinShun's own search box on the Windows 11 taskbar (a pinned button on Windows 10), optionally right of Start where Windows' own box sits; it can take over `Win+S` and typing in the Start menu.
- 🤖 **AI assistants (MCP)**
  - **What**: WinShun is an MCP server. Claude Desktop, Claude Code, Cursor and VS Code connect in one click and search file names, file contents and recent changes.
  - **Safety**: read-only by default, limited to what the AI client may see, with folders kept from AI and a call log.
- 🎨 **Looks and settings**
  - **Looks**: light and dark themes following Windows, Windows 11 Mica and rounded corners, sharp icons at any scaling.
  - **Settings**: searchable (`Ctrl+F`).
  - **Updates**: announced in the tray; WinShun never replaces its own files.

| Key | Action |
|---|---|
| Ctrl twice | Open / close the search bar (another shortcut, such as `Alt+Space`, can be set) |
| `Enter` / `Ctrl+Enter` / `Ctrl+Shift+Enter` | Open / show in folder / run as administrator |
| `Ctrl+C` / `Ctrl+Shift+C` | Copy the file / copy its path |
| `Ctrl+1`…`Ctrl+9` | Open the nth row (the number on its icon) |
| `Tab` / `Shift+Tab` | Switch scope: All / Files / Content; in **Files**, `Tab` steps through the kinds |
| `Ctrl+S` | Best match → newest first → oldest first |
| `Ctrl+G` (in a file dialog) | Go to the folder shown in your file manager |
| `Win+V` / `Win+S` (once turned on) | Clipboard history / search above the taskbar |

### Download and install

1. Download `WinShun-<version>-x64-setup.exe` from the [Releases page](https://github.com/LingCore/WinShun/releases/latest) and run it. If Windows says “Windows protected your PC”, click **More info → Run anyway**: the app is not code-signed yet.
2. When the WinShun icon appears in the notification area, it is running; the first index takes a few seconds. Press Ctrl twice and type.
3. To upgrade, run a newer installer: your settings and index are kept. Uninstall from Windows Settings → Apps.

Requires Windows 10 or 11, 64-bit.

### Why administrator rights

- **Why**: reading the NTFS master file table (MFT) and change journal (USN) indexes millions of files in seconds and follows changes live. A scheduled task with highest privileges starts it at login without a UAC prompt.
- **What you open is not elevated**: files and apps you open from WinShun run with your normal rights; only `Ctrl+Shift+Enter` runs one as administrator.

### FAQ

**How is it different from Everything or Listary?**
WinShun builds its own index, so Everything is not needed. File names, pinyin, apps and file contents are searched from one box, tuned for Chinese. File dialogs get Listary's `Ctrl+G` and a search bar, but WinShun does not embed itself in Explorer windows.

**Ctrl twice clashes with another program?**
Listary and PowerToys' Find My Mouse use it too: turn it off on one side, or give WinShun another shortcut. Full-screen games and games that hide the mouse are ignored by default.

**AutoHotkey remaps don't work in WinShun?**
WinShun runs as administrator, and Windows keeps normal-rights programs from intercepting keys sent to it. Run the script with UI Access, or as administrator.

**Memory? Index location?**
- **Memory**: about 30 MB of index per million files; the whole app is usually 120–140 MB.
- **Index location**: the index can move to another built-in drive under Settings → Advanced.

**Is it free? Does it collect data?**
Free and MIT-licensed. It goes online only to check GitHub for new versions (you can turn that off) and to fetch a web shortcut's icon once. The index and history stay on your PC (`%LOCALAPPDATA%\WinShun`).

### Feedback

Bug reports and suggestions are welcome in [Issues](https://github.com/LingCore/WinShun/issues).

### Build from source

Needs Visual Studio 2022 (Desktop development with C++), Qt 6.8 or later (6.12 recommended, MSVC 2022 64-bit kit), CMake 3.25+ and Ninja.

```powershell
./scripts/build.ps1                                # build and run the unit tests
./scripts/build.ps1 -QtDir C:\Qt\6.12.0\msvc2022_64 # pick a Qt kit (default: QTDIR or the newest under C:\Qt)
./scripts/build.ps1 -Deploy                        # make the distributable folder dist\WinShun
./scripts/release.ps1                              # package the installer and SHA256SUMS.txt (needs Inno Setup 6.5+); -Publish makes the GitHub release
```

Architecture, design notes and benchmarks (in Chinese) are in [docs/architecture.md](docs/architecture.md).

---

## LINUX DO

本项目积极参与并认可 [LINUX DO 社区](https://linux.do)。

WinShun is proud to be part of the [LINUX DO community](https://linux.do).

## 许可证 · License

Win顺 以 [MIT 许可证](LICENSE) 发布。Copyright © 2026 LingCore.

WinShun is released under the [MIT License](LICENSE). Copyright © 2026 LingCore.

随附的第三方内容按各自的许可使用，不在 MIT 许可范围内 · Bundled third-party content keeps its own license:

- `resources/fonts/` 阿里巴巴普惠体 3.0 · Alibaba PuHuiTi 3.0 — © Alibaba Group，按其免费商用授权随程序附带 · redistributed under its free commercial-use terms.
- 拼音读音数据 · Pinyin data — [pinyin-data](https://github.com/mozillazg/pinyin-data)，MIT，见 · see `tools/data/pinyin-data-LICENSE.txt`。常用字的读音以其中的《通用规范汉字字典》（2013）数据为准。
