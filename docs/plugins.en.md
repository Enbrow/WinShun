# Writing plugins for Win顺 (WinShun)

Plugins add keyword commands to the search box and mix their rows into the list. They **always run with your normal rights** (Win顺 itself is administrator; its plugins are not), and a slow or crashing plugin never holds the launcher up. Three ways to write one, a single protocol:

| Kind | What you write | For |
|---|---|---|
| **Zero-code** | `plugin.json` only | a keyword that opens a URL or a program, jumps to a folder, runs a saved search, or lists fixed items |
| **Once** (a process per query) | any language; the query comes in environment variables, the rows go to stdout | small tools, used now and then |
| **Resident** | any language; JSON-RPC lines on stdio | anything queried on every key, anything with state, PowerShell (slow to start) |

The plugins that come with Win顺 are in `<install dir>\plugins\` (all off until you turn them on under Settings → Plugins): `winshun.tools` (calculator, units, colours, timestamps, UUIDs, Base64, hashes; C++), `winshun.kill` (end a process; PowerShell), `winshun.bookmarks` (browser bookmarks; Python, once), `winshun.translate` (translation with your own API key; Python, resident), `winshun.web` and `winshun.go` (zero-code). Each is a working example with a README.

The Chinese version, [plugins.md](plugins.md), is the reference; this one follows it.

## Quick start

A plugin is a folder under `%LOCALAPPDATA%\WinShun\plugins\<id>\` with a `plugin.json`. Fastest: Settings → Plugins → Advanced → Developer mode → **New plugin…**, pick a language, and a template is written with its SDK file; turn the switch on and type its keyword. Edits to the script reload it; the message log shows every line that goes back and forth.

### 1. Zero-code: a keyword that opens a URL

`%LOCALAPPDATA%\WinShun\plugins\me.npm\plugin.json`:

```json
{
  "manifest": 1,
  "id": "me.npm",
  "name": "npm",
  "version": "1.0.0",
  "commands": [
    {"id": "npm", "keyword": "npm", "title": "Search npm", "run": {"url": "https://www.npmjs.com/search?q=%s", "home": "https://www.npmjs.com/"}}
  ]
}
```

`npm lodash` and Enter opens the search; `npm ` alone (with the space) opens the home page. `run` may also be `{"program": "C:\\…\\code.exe", "args": "%s"}`, `{"folder": "%USERPROFILE%\\Downloads"}`, `{"search": {"query": "ext:pdf %s", "scope": "files"}}` (as if typed into the box) or `{"items": [{"id": …, "title": …, "open": {"path": …}}]}` (a fixed list the words filter).

### 2. Python, once: a few lines

`me.hello\plugin.json`:

```json
{
  "manifest": 1,
  "id": "me.hello",
  "name": "Hello",
  "version": "1.0.0",
  "runtime": {"mode": "once", "program": "python", "script": "main.py"},
  "commands": [{"id": "hi", "keyword": "hi", "title": "Say hello", "hint": "a name"}]
}
```

`me.hello\main.py` (copy `winshun.py` from `src/plugins-sdk/` or from any bundled plugin's folder):

```python
import winshun

q = winshun.query()                      # q.text: the words after the keyword
winshun.reply([
    winshun.item("hi", f"Hello, {q.text}", "Enter copies", copy=q.text),
    winshun.item("len", f"{len(q.text)} characters", valid=False),
])
```

A Python process per query (tens of milliseconds). Without the SDK: read `WS_QUERY`, print `{"items": [...]}`, a bare array, or plain text lines (one row each; Enter copies).

### 3. PowerShell, resident: one process that stays

`me.ps\plugin.json`:

```json
{
  "manifest": 1,
  "id": "me.ps",
  "name": "PS",
  "version": "1.0.0",
  "runtime": {"mode": "resident", "program": "powershell", "script": "main.ps1"},
  "commands": [{"id": "ps", "keyword": "ps", "title": "PowerShell"}]
}
```

`me.ps\main.ps1` (**UTF-8 with a BOM**; `WinShun.psm1` beside it):

```powershell
Import-Module "$PSScriptRoot\WinShun.psm1"
Start-WinShunPlugin -Name 'me.ps' -OnQuery {
    param($q, $plugin)
    @(New-WinShunItem -Id 'hi' -Title "Hello, $($q.text)" -Copy $q.text)
}
```

PowerShell takes a few hundred milliseconds to start the first time, then a few milliseconds per query; it exits after 10 idle minutes. For C++ see the head of `src/plugins-sdk/winshun.h` and `src/tools-plugin/`.

## The SDK

Three files in `src/plugins-sdk/`; "New plugin…" copies the right one into the plugin's folder, and every bundled plugin carries its own. They only wrap the protocol: using them is optional.

| File | Once | Resident |
|---|---|---|
| `winshun.py` | `query()`, `settings()`, `item(...)`, `reply(items)`, `is_action()` / `action_request()` / `effects(...)` | `serve(on_query, on_action=None, on_settings=None)`; handlers get `(q, plugin)` with `plugin.partial()`, `plugin.log()`, `plugin.notify_user()`, `plugin.progress()`, `plugin.tool(name, **args)`, `plugin.settings` |
| `WinShun.psm1` | `Get-WinShunQuery`, `Get-WinShunSettings`, `New-WinShunItem`, `Send-WinShunReply`, `Send-WinShunEffects` | `Start-WinShunPlugin -OnQuery {…} -OnAction {…}`, `Send-WinShunPartial`, `Write-WinShunLog`, `Send-WinShunNotify`, `Send-WinShunProgress`, `Invoke-WinShunTool` |
| `winshun.h` | `winshun::onceQuery()`, `onceSettings()`, `once(items)`, `onceAction()` / `onceEffects()` | derive from `winshun::Plugin`, override `query()` (and `action()`, `settingsChanged()`), call `serve()`; members `partial()`, `log()`, `notifyUser()`, `progress()`, `tool()`, `settings()` |

Effect helpers: Python `winshun.hide() / keep() / notify(text) / copy(text) / set_query(text) / open_path(path) / open_url(url) / replace(items)`; PowerShell `New-WinShunEffect -Hide / -Keep / -Notify / -Copy / -SetQuery / -OpenPath / -OpenUrl / -Replace`; C++ `winshun::effect::hide()` and the rest.

## The manifest (plugin.json)

Top level:

| Field | Required | Meaning |
|---|---|---|
| `manifest` | yes | schema version, `1`; a newer one than Win顺 knows is refused |
| `id` | yes | `author.name`, `[a-z0-9._-]` only, the folder's name |
| `name` | yes | a string, or `{"zh": …, "en": …}` picked by the interface language ("text" below) |
| `version` | yes | e.g. `1.2.0`; installing a lower version over a higher one asks |
| `description` | no | one sentence (text) |
| `author`, `homepage`, `license` | no | shown on the card; `homepage` is a link |
| `icon` | no | a PNG / JPG / ICO in the plugin folder (`name.dark.png` for the dark theme; **no SVG**); else the first command's icon |
| `minWinShun` | no | not enabled below it |
| `runtime` | once / resident | see below |
| `permissions` | no | see "Permissions" |
| `commands` | yes | at least one |
| `settings` | no | see "Settings" |
| `ai` | no | `{"expose": true}`: the commands may become AI tools (the user still has to allow it); see "For AI" |

`runtime`:

| Field | Meaning |
|---|---|
| `mode` | `once` or `resident` |
| `program` | `python`, `pwsh`, `powershell`, `node`, or an exe in the plugin folder (relative path). A missing interpreter puts "needs Python 3" with a download link on the card |
| `script` | the entry script; default `main.py` / `main.ps1` / `main.js` |
| `args` | extra arguments (array) |
| `cwd` | working folder; default the plugin folder |
| `env` | extra environment variables (`WS_*` cannot be overridden) |
| `encoding` | stdout's encoding: `utf-8` (default) or `oem` (Windows PowerShell 5.1 without an encoding set; the templates set UTF-8) |
| `timeoutMs` | one query's budget: resident 1500, once 3000, at most 10000 |
| `debounceMs` | once plugins: default 120 |
| `idleExitSec` | resident: idle seconds before exit, default 600; `0` never |
| `start` | resident: `lazy` (default) or `eager` (started 10 s after Win顺; for push plugins) |
| `memoryMB` | the job's memory limit, default 512, at most 2048 |
| `sandbox` | `normal` (medium integrity, as you) or `strict` (low integrity: cannot write your folders, only its own data folder; for exe plugins that write nothing) |

`commands[]`:

| Field | Meaning |
|---|---|
| `id` | unique within the plugin; the settings keys and the AI tool name use it |
| `title`, `description`, `hint` | the name, one sentence, the placeholder in keyword mode (all text) |
| `icon` | see "Icons" |
| `keyword` | a string or an array. **Keyword + space** enters keyword mode; the keyword alone puts the command's hint row first (Enter or Tab enters, ↓ reaches the names found); two or more characters a keyword begins with (`uu` for `uuid`) add a row after the apps, before the files (the keyboard stays where it is; Tab completes to `uuid ` from any row; two rows at most, none when too many keywords qualify); the user may change it |
| `triggers` | beyond the keyword: `{"regex": "…", "minLength": 1, "maxLength": 64}` (the whole input matches: rows are pushed into 全部; `match` carries the groups), `{"push": {"minLength": 2, "maxItems": 3}}` (every key; resident + eager only) |
| `mode` | `list` (default) or `instant` (the keyword alone runs it) |
| `actions` | `[{"id": "kill", "title": "Kill", "key": "Enter"}, …]`: the order gives Enter / Ctrl+Enter / Shift+Enter; the menu lists all; none means one `default` |
| `run` | zero-code only: `url` (`%s` is the text, URL-encoded; `home` for the keyword alone), `program` (`%VAR%` expanded; `%s` in `args`), `folder`, `search` (`{"query": "ext:pdf %s", "scope": "all | files | folders | content"}`), `items` (item objects; `%VAR%` in `open.path` expanded) |
| `cache` | `{"seconds": 60}`: how long the same text's answer is reused |
| `ai` | `{"description": "…", "parameters": {JSON Schema}}` for the AI tool; default: `description` plus a `query` string |

Unknown fields are ignored. A validation error is a sentence, shown as is on the settings page.

## The protocol

### Once

A process per query; stdin is closed.

In (environment): `WS_QUERY` (after the keyword), `WS_TEXT` (the whole input), `WS_COMMAND`, `WS_TRIGGER` (`keyword` / `regex` / `push`), `WS_KEYWORD`, `WS_MATCH` (regex groups, a JSON array), `WS_PLUGIN_DIR`, `WS_DATA_DIR` (the plugin's writable `<id>.data`), `WS_SETTINGS` (JSON), `WS_LOCALE` (`zh` / `en`), `WS_THEME` (`light` / `dark`), `WS_VERSION`. For an action on a row without a declarative `open`, the process is started again with `WS_ACTION` (the action id) and `WS_ITEM` (the row as JSON).

Out (stdout, UTF-8): one JSON object `{"items": [...], "rerun": seconds, "cache": seconds}`, a bare array (the items), or **plain text lines** (one row each; Enter copies). For an action, `{"effects": [...]}`; nothing printed means hide. stderr goes to `plugins.log`. A non-zero exit code is a failure (shown in the footer).

### Resident

One process, JSON-RPC 2.0 lines on stdin / stdout, UTF-8. **Only protocol messages may go to stdout**; debug output goes to stderr.

Win顺 → plugin:

```
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"1","winshun":"0.7.0","pluginDir":"…","dataDir":"…","locale":"zh","theme":"dark","settings":{…}}}
← {"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"1","name":"…"}}
{"jsonrpc":"2.0","id":2,"method":"query","params":{"queryId":17,"command":"kill","trigger":"keyword","keyword":"kill","text":"note","raw":"kill note","limit":200}}
← (optional, any number) {"jsonrpc":"2.0","method":"query/partial","params":{"queryId":17,"items":[…],"replace":false}}
← {"jsonrpc":"2.0","id":2,"result":{"items":[…],"rerun":0,"cache":0}}
{"jsonrpc":"2.0","method":"cancel","params":{"queryId":17}}         (a newer key came; may be ignored, Win顺 drops by queryId)
{"jsonrpc":"2.0","id":3,"method":"action","params":{"command":"kill","action":"kill","item":{…as given…},"text":"note"}}
← {"jsonrpc":"2.0","id":3,"result":{"effects":[{"type":"notify","text":"Ended notepad"},{"type":"hide"}]}}
{"jsonrpc":"2.0","method":"settingsChanged","params":{"settings":{…}}}
{"jsonrpc":"2.0","id":5,"method":"shutdown"}  ← {"jsonrpc":"2.0","id":5,"result":{}} then exit
```

Plugin → Win顺:

```
{"jsonrpc":"2.0","id":"p1","method":"winshun/tool","params":{"name":"search_files","arguments":{"query":"*.pdf","limit":20}}}
{"jsonrpc":"2.0","method":"winshun/log","params":{"level":"info","text":"…"}}        plugins.log and the developer log
{"jsonrpc":"2.0","method":"winshun/notify","params":{"text":"…"}}                   the footer
{"jsonrpc":"2.0","method":"winshun/progress","params":{"queryId":17,"text":"Reading bookmarks…"}}   the "waiting…" line
```

`winshun/tool` takes the same names and arguments as Win顺's MCP tools: `search_files`, `search_content`, `recent_files`, `list_folder`, `get_file_info`, `open_item`, `show_in_folder`, `clipboard_history`, `clipboard_get`, `index_status`; gated by the manifest's permissions (`winshun:search`, `winshun:open`, `clipboard:read`; `index_status` always). The result is the MCP `structuredContent`.

Timeouts: one timer per query (`timeoutMs`); a late answer is dropped. A resident plugin that misses three in a row is restarted with backoff (1 → 5 → 30 s); three crashes in five minutes turn it off, with the reason on its card. After `idleExitSec` it gets `shutdown`. At most 200 rows per answer, 16 MB per line.

### Item fields

```json
{
  "id": "pid-1234",                  // stable: in-place updates, the target of actions, what the AI's _run tool names
  "title": "notepad.exe",            // required
  "subtitle": "PID 1234 · 38 MB",
  "icon": "file:C:\\Windows\\notepad.exe",   // see "Icons"
  "score": 80,                       // 0–100; the apps' scale when pushed into 全部; 50 when absent
  "match": "notepad 记事本",          // zero-code items filter on it
  "autocomplete": "kill notepad",    // Tab puts it in the box
  "open": {"path": "C:\\…", "args": "…", "cwd": "…"} or {"url": "https://…"},   // declarative default action: Win顺 opens it (normal rights), no round trip; the url "winshun:settings" opens Win顺's settings at this plugin's card (for a "no API key yet" row)
  "copy": "1234",                    // Ctrl+C; else the title
  "preview": {"text": "…"},          // the hover tip
  "actions": [{"id": "kill", "title": "Kill", "key": "Enter"}],   // overrides the command's
  "valid": true,                     // false: Enter does nothing (a message row)
  "data": {"pid": 1234}              // opaque; comes back with the action; never shown to the AI
}
```

Effects (an action's answer, in order): `hide` (default), `keep`, `setQuery {text}`, `notify {text}`, `copy {text}`, `open {path | url}` (`url` takes `winshun:settings` too), `replace {items}` (a second-level list).

### Icons

`icon.png` (PNG / JPG / ICO in the plugin folder, relative; `icon.dark.png` preferred in the dark theme), `ext:pdf`, `file:C:\…`, `app:<AUMID>`, `glyph:U+E8E5` (Segoe Fluent Icons), `color:#ff8800` (a swatch), `data:image/png;base64,…` (≤ 64 KB). Paths outside the plugin folder and SVG are refused.

## Permissions

`permissions` is an array: `["processes", "clipboard:read", {"files:read": ["%LOCALAPPDATA%\\Google\\Chrome\\User Data\\*\\Bookmarks"]}]`. Turning a plugin on lists them: what Windows enforces is ticked, the rest is marked "declared only". Honestly:

| Permission | Meaning | Enforced | How |
|---|---|---|---|
| (always) not administrator | the plugin runs with your normal rights | **yes** | the token loses the Administrators group and the privileges; `normal` medium integrity, `strict` low; memory limit, a job that ends with Win顺 |
| `processes` | child processes | **yes** | without it the job's `ActiveProcessLimit = 1` (`CreateProcess` fails with 1816); with it the children are in the job too |
| `clipboard:read` / `clipboard:write` | the clipboard directly | **yes** | job UI restrictions; `GetClipboardData` returns nothing without it |
| `ui` | own windows, other windows | partly | without it the job restricts the desktop and USER handles |
| `winshun:search` / `winshun:open` / `winshun:history` | Win顺's tools | **yes** | `winshun/tool` is gated |
| `files:write` | writing your files | `strict` only | `strict` cannot write your folders, only `<id>.data`; `normal` writes as you do: **declared only** |
| `files:read` | which files it reads | **no** | a declaration for the user (medium integrity reads anyway) |
| `network` | going online | **no** | Win顺 cannot stop it; the card says so |

Install plugins only from sources you trust.

## Settings

`settings[]`: `{"key", "type", "title", "hint", "default", "options"}`, `type` ∈ `text | password | bool | number | choice | folder | file | hotkey`. Drawn with Win顺's own controls on the card; stored in `WinShun.ini` as `[Plugin.<id>] Setting.<key>=`; **`password` goes to Windows Credential Manager**, never into the INI or the logs (masked in the message log). `choice` options are strings or `[{"value": "deepl", "title": {"zh": …, "en": …}}]`.

The plugin sees the merged JSON (declared defaults under the user's values): `WS_SETTINGS` for once plugins, `initialize`'s `settings` for resident ones, then `settingsChanged`.

## For AI

With `"ai": {"expose": true}` in the manifest and "Allow AI to use plugins" on under Settings → AI assistants, every command becomes the MCP tool `plugin_<id>_<command>` (dots to underscores: `plugin_winshun_tools_calc`). A call is one `query` (the plugin is started if it is not running) returning `{items: [{id, title, subtitle, url?, path?, copy?}], returned}`, without `data`; `readOnlyHint` is true. `ai.description` (English, for the model) and `ai.parameters` override the description and the schema (default `{query, limit}`).

Actions are **not** exposed unless "Allow AI to run plugin actions" is on; then `plugin_<id>_<command>_run` (`destructiveHint`) takes the same `query`, the row's `item_id` and an optional `action`; Win顺 queries again, finds the row, sends `action`, and answers `{done, message, effects}`. Each card has a "For AI" switch; a plugin that is turned off answers "plugin X is turned off". Connected clients are told when the tool list changes.

## Debugging

- **Developer mode** (Settings → Plugins → Advanced): a changed script restarts its resident plugin; the message log shows every line (time, plugin, direction, method, milliseconds to the response), click one for the whole message; "open plugins.log" (`%LOCALAPPDATA%\WinShun\plugins.log`: stderr, protocol errors, crashes).
- **Command line** (`build\release\wsbench.exe` from a source build; no window):

```powershell
wsbench --plugin D:\my.plugin --query "hi 你好" --runs 20                 # validate the manifest, start through the runner, print the rows and the timings (cold start, first round trip, steady median) and the process's integrity level and job
wsbench --plugin D:\my.plugin --query "hi 你好" --setting greeting=hey    # a declared setting's value
wsbench --plugin D:\my.plugin --query "kill note" --action kill          # run an action on the first row, print the effects
wsbench --launcher-sim --plugins D:\plugins "hi 你好" "#ff8800"          # the launcher's merge, key by key
wsbench --mcp-host --plugins D:\plugins                                 # serve the plugins as AI tools on the test pipe (WinShunMcp.exe --pipe …)
```

## FAQ

- **Garbled Chinese.** UTF-8 on both ends: Python gets `-X utf8`; a PowerShell script **must be saved as UTF-8 with a BOM** (Windows PowerShell 5.1 reads a bare .ps1 in the ANSI code page) and set `[Console]::OutputEncoding = [Text.Encoding]::UTF8` (`WinShun.psm1` does). `"encoding": "oem"` in the manifest rescues old scripts, Latin characters only.
- **Python not found.** Win顺 looks for `python.exe` in the registry (`PythonCore\*\InstallPath`) and on PATH, never `py.exe` (it starts a child, which the job forbids); the Store alias under `WindowsApps` is skipped too. Install one from python.org.
- **Timeouts.** A query's budget is `timeoutMs` (resident 1500, once 3000, at most 10000); send `query/partial` or `winshun/progress` early and do the heavy work on a thread. Starting a process costs about 15 ms (Defender), Python tens of milliseconds, PowerShell hundreds: hence resident PowerShell.
- **A resident plugin prints and nothing happens.** stdout on a pipe is block-buffered: flush after each line (the SDKs do; Python also gets `-u`).
- **Enter does nothing.** The row has `"valid": false`, or the command declares no `actions` and the plugin did not answer `action`. A row with `open` is opened by Win顺 itself; no `action` is sent.
- **Writing files.** Write to `WS_DATA_DIR` (`<id>.data`; uninstalling asks whether to delete it); `strict` plugins can write nowhere else.
- **Icons missing.** PNG / JPG / ICO inside the plugin folder only; no SVG.
- **Keyword taken.** It clashed with a Web search keyword or another plugin's; the user's own wins and the card says so. Pick another.
- **Can the plugin folder be changed?** A `normal` plugin runs as you and can change what you can — that is what developer mode's reload relies on. Use `strict` to keep a plugin from changing itself.
