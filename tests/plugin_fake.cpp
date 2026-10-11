// plugin_fake.exe: a plugin of every kind for tst_plugin and wsbench --plugin.
// No Qt, static C runtime (it is started by WinShunPluginHost.exe as any
// plugin is). The first argument is the role, the rest its options:
//
//   --once [--n N] [--text] [--gbk] [--fail] [--slow MS] [--long BYTES] [--echo-env] [--action]
//       A once plugin: reads WS_* and prints JSON items (or plain text,
//       or N items, or a line BYTES long, or exits 3, or the argv and the
//       environment it got, or the effects of WS_ACTION).
//   --serve [--partial] [--slow MS] [--crash N] [--hang] [--tool NAME] [--error] [--chatty]
//       A resident plugin: JSON-RPC on stdio. Answers initialize, query
//       (two query/partial first with --partial; after MS; never with
//       --hang; exits 3 on the Nth query with --crash; with an error with
//       --error; after a winshun/tool call of NAME, whose outcome goes in
//       the subtitle, with --tool; with winshun/log, notify and progress
//       before it with --chatty), cancel (counted), action, settingsChanged
//       (remembered), shutdown.
//   --probe
//       A once plugin whose one item carries the facts about its process
//       (token, job, mitigations, may it start a process, read the
//       clipboard, write where) in `data`.
//   --noop
//       Exits at once: what --probe tries to start.

#include "Json.h"
#include "Lite.h"
#include "Sandbox.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace ws::plugin;

namespace {

std::vector<std::wstring> g_args;

bool has(const wchar_t* option)
{
    for (const std::wstring& a : g_args)
        if (a == option)
            return true;
    return false;
}

std::wstring value(const wchar_t* option, const wchar_t* fallback = L"")
{
    for (std::size_t i = 0; i + 1 < g_args.size(); ++i)
        if (g_args[i] == option)
            return g_args[i + 1];
    return fallback;
}

std::string env(const wchar_t* name)
{
    return lite::utf8(lite::environmentVariable(name));
}

void out(const std::string& line)
{
    lite::writeAll(::GetStdHandle(STD_OUTPUT_HANDLE), line + '\n');
}

void err(const std::string& line)
{
    lite::writeAll(::GetStdHandle(STD_ERROR_HANDLE), line + '\n');
}

json::Value::Object item(const std::string& id, const std::string& title, const std::string& subtitle)
{
    return {{"id", id}, {"title", title}, {"subtitle", subtitle}};
}

json::Value facts()
{
    sandbox::FactsOptions options;
    options.spawnExe = lite::ownPath();
    options.clipboard = true;
    options.parentMark = L"PLUGIN_TEST_PARENT_MARK";
    const std::wstring dataDir = lite::environmentVariable(L"WS_DATA_DIR");
    if (!dataDir.empty())
        options.writeDirs.push_back(dataDir);
    const std::wstring profile = lite::environmentVariable(L"USERPROFILE");
    if (!profile.empty())
        options.writeDirs.push_back(profile);
    return sandbox::collectFacts(options);
}

int onceMain()
{
    if (has(L"--slow"))
        ::Sleep(static_cast<DWORD>(std::wcstoul(value(L"--slow").c_str(), nullptr, 10)));
    if (has(L"--fail")) {
        err("something went wrong: 出错了");
        return 3;
    }
    if (has(L"--long")) {
        const std::size_t n = std::strtoull(lite::utf8(value(L"--long")).c_str(), nullptr, 10);
        std::string line(n, 'x');
        out(line);
        return 0;
    }
    const std::string query = env(L"WS_QUERY");
    if (has(L"--text")) {
        out("first line " + query);
        out("");
        out("第二行 " + query);
        return 0;
    }
    if (has(L"--gbk")) {
        // What a script that writes in the OEM code page produces (Windows
        // PowerShell 5.1 redirected, say): the bytes of the OEM code page.
        const std::wstring text = L"中文结果 Résumé " + lite::wide(query);
        const int n = ::WideCharToMultiByte(CP_OEMCP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string bytes(static_cast<std::size_t>(n), '\0');
        ::WideCharToMultiByte(CP_OEMCP, 0, text.data(), static_cast<int>(text.size()), bytes.data(), n, nullptr, nullptr);
        out("{\"items\":[{\"title\":" + json::quote(bytes) + "}]}");
        return 0;
    }
    if (has(L"--echo-env")) {
        json::Value::Array argv;
        for (const std::wstring& a : g_args)
            argv.push_back(lite::utf8(a));
        json::Value::Object data;
        data["argv"] = std::move(argv);
        for (const wchar_t* name : {L"WS_QUERY", L"WS_TEXT", L"WS_COMMAND", L"WS_TRIGGER", L"WS_KEYWORD", L"WS_MATCH", L"WS_PLUGIN_DIR",
                 L"WS_DATA_DIR", L"WS_SETTINGS", L"WS_LOCALE", L"WS_THEME", L"WS_VERSION", L"PYTHONIOENCODING", L"FAKE_EXTRA"})
            data[lite::utf8(name)] = env(name);
        json::Value::Object one = item("env", "env", "");
        one["data"] = json::Value(std::move(data));
        out(json::write(json::Value(json::Value::Object {{"items", json::Value::Array {json::Value(std::move(one))}}})));
        return 0;
    }
    if (has(L"--action")) {
        const std::string action = env(L"WS_ACTION");
        const auto item = json::parse(env(L"WS_ITEM"));
        const std::string title = item ? (*item)["title"].toString() : "?";
        out(json::write(json::Value(json::Value::Object {{"effects",
            json::Value::Array {json::Value(json::Value::Object {{"type", "notify"}, {"text", "did " + action + " on " + title}}),
                json::Value(json::Value::Object {{"type", "hide"}})}}})));
        return 0;
    }
    int n = 1;
    if (has(L"--n"))
        n = std::max(1, _wtoi(value(L"--n").c_str()));
    json::Value::Array items;
    for (int i = 0; i < n; ++i) {
        json::Value::Object one = item("once-" + std::to_string(i), "once: " + query + (n > 1 ? " #" + std::to_string(i) : ""),
            env(L"WS_COMMAND") + "/" + env(L"WS_TRIGGER"));
        one["score"] = 70;
        one["open"] = json::Value::Object {{"url", "https://example.com/?q=" + query}};
        items.push_back(json::Value(std::move(one)));
    }
    err("once plugin answered 已回答");
    out(json::write(json::Value(json::Value::Object {{"items", std::move(items)}, {"rerun", 0}})));
    return 0;
}

int probeMain()
{
    json::Value::Object one = item("probe", "probe", "");
    one["data"] = facts();
    out(json::write(json::Value(json::Value::Object {{"items", json::Value::Array {json::Value(std::move(one))}}})));
    return 0;
}

// ---- the resident plugin ---------------------------------------------------

struct Server {
    int queries = 0;
    int cancelled = 0;
    json::Value settings;
    long long nextId = 1;
    lite::LineReader reader {::GetStdHandle(STD_INPUT_HANDLE)};

    void send(const json::Value::Object& message)
    {
        json::Value::Object m = message;
        m["jsonrpc"] = "2.0";
        out(json::write(json::Value(std::move(m))));
    }
    void respond(const json::Value& id, json::Value result) { send({{"id", id}, {"result", std::move(result)}}); }
    void fail(const json::Value& id, int code, const std::string& text)
    {
        send({{"id", id}, {"error", json::Value::Object {{"code", code}, {"message", text}}}});
    }
    void notify(const std::string& method, json::Value::Object params)
    {
        send({{"method", method}, {"params", json::Value(std::move(params))}});
    }

    // A request of ours: waits for its response (other lines meanwhile are
    // handled, but a plugin that simple need not).
    json::Value call(const std::string& method, json::Value::Object params)
    {
        const long long id = nextId++;
        send({{"id", id}, {"method", method}, {"params", json::Value(std::move(params))}});
        std::string line;
        while (reader.next(line)) {
            const auto message = json::parse(line);
            if (!message)
                continue;
            if ((*message)["id"].isNumber() && (*message)["id"].toInt() == id && !(*message).has("method"))
                return *message;
        }
        return json::Value();
    }

    int run()
    {
        std::string line;
        while (reader.next(line)) {
            const auto message = json::parse(line);
            if (!message || !message->isObject()) {
                err("bad line: " + line);
                continue;
            }
            const json::Value& m = *message;
            const std::string method = m["method"].toString();
            const json::Value id = m["id"];
            const json::Value params = m["params"];
            if (method == "initialize") {
                settings = params["settings"];
                respond(id, json::Value::Object {{"protocolVersion", params["protocolVersion"].toString()}, {"name", "fake"}});
            } else if (method == "query") {
                ++queries;
                if (has(L"--crash") && queries == _wtoi(value(L"--crash").c_str())) {
                    err("crashing on purpose 故意崩溃");
                    return 3;
                }
                if (has(L"--hang"))
                    continue;
                if (has(L"--error")) {
                    fail(id, -32000, "cannot do that 做不到");
                    continue;
                }
                const long long queryId = params["queryId"].toInt();
                const std::string text = params["text"].toString();
                if (has(L"--chatty")) {
                    notify("winshun/log", {{"level", "info"}, {"text", "query " + text}});
                    notify("winshun/notify", {{"text", "hello from fake 你好"}});
                    notify("winshun/progress", {{"queryId", queryId}, {"text", "working 正在查"}});
                }
                if (has(L"--partial")) {
                    for (int i = 0; i < 2; ++i) {
                        json::Value::Object partial = item("p" + std::to_string(i), "partial " + std::to_string(i) + ": " + text, "");
                        notify("query/partial", {{"queryId", queryId}, {"items", json::Value::Array {json::Value(std::move(partial))}},
                                                    {"replace", false}});
                    }
                }
                if (has(L"--slow"))
                    ::Sleep(static_cast<DWORD>(std::wcstoul(value(L"--slow").c_str(), nullptr, 10)));
                std::string subtitle = params["command"].toString() + "/" + params["trigger"].toString();
                if (has(L"--tool")) {
                    const std::string tool = lite::utf8(value(L"--tool"));
                    const json::Value reply = call("winshun/tool", {{"name", tool}, {"arguments", json::Value::Object {{"query", text}}}});
                    if (reply.has("error"))
                        subtitle = "tool error " + std::to_string(reply["error"]["code"].toInt());
                    else
                        subtitle = "tool ok " + json::write(reply["result"]);
                }
                // A stable id per text, as a real plugin would give (an AI's
                // _run tool lists again and looks the id up).
                json::Value::Object one = item("q-" + text, "serve: " + text, subtitle);
                one["score"] = 80;
                one["open"] = json::Value::Object {{"url", "https://example.com/?q=" + text}};
                if (!text.empty() && text[0] == '#') // a colour (a regex trigger's push): shown as a swatch
                    one["icon"] = "color:" + text;
                one["data"] = json::Value::Object {{"cancelled", cancelled}, {"settings", settings}, {"pid", static_cast<unsigned long>(::GetCurrentProcessId())}};
                respond(id, json::Value::Object {{"items", json::Value::Array {json::Value(std::move(one))}}});
            } else if (method == "cancel") {
                ++cancelled;
            } else if (method == "action") {
                const std::string action = params["action"].toString();
                const std::string title = params["item"]["title"].toString();
                respond(id, json::Value::Object {{"effects",
                    json::Value::Array {json::Value(json::Value::Object {{"type", "notify"}, {"text", "did " + action + " on " + title}}),
                        json::Value(json::Value::Object {{"type", "hide"}})}}});
            } else if (method == "settingsChanged") {
                settings = params["settings"];
            } else if (method == "shutdown") {
                respond(id, json::Value::Object {});
                return 0;
            } else if (!id.isNull()) {
                fail(id, -32601, "Method not found: " + method);
            }
        }
        return 0; // stdin closed
    }
};

} // namespace

int wmain(int argc, wchar_t** argv)
{
    for (int i = 2; i < argc; ++i)
        g_args.emplace_back(argv[i]);
    const std::wstring role = argc > 1 ? argv[1] : L"";
    if (role == L"--once")
        return onceMain();
    if (role == L"--probe")
        return probeMain();
    if (role == L"--serve")
        return Server().run();
    if (role == L"--noop")
        return 0;
    err("usage: plugin_fake --once | --serve | --probe | --noop [options]");
    return 1;
}
