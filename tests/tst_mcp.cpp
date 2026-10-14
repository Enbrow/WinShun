// The MCP server (src/mcp): JSON-RPC framing, the tools on a hand-made index
// over a temporary folder, the lifecycle through an in-process session,
// the named pipe, and the relay WinShunMcp.exe driven over stdio, with
// Win顺 there and away; and the plugins' commands as tools (plugin::AiBridge).

#include "AiBridge.h"
#include "ContentScanner.h"
#include "FileIndex.h"
#include "Host.h"
#include "IndexService.h"
#include "JsonRpc.h"
#include "McpServer.h"
#include "McpTools.h"
#include "Pipe.h"
#include "Sandbox.h"
#include "SearchEngine.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace ws;
using namespace ws::mcp;
using namespace Qt::StringLiterals;

namespace {

QJsonObject parseObject(const QByteArray& line)
{
    return QJsonDocument::fromJson(line).object();
}

QByteArray requestLine(int id, const QString& method, const QJsonObject& params = {})
{
    return jsonrpc::request(id, method, params);
}

QByteArray callLine(int id, const QString& tool, const QJsonObject& arguments = {})
{
    return jsonrpc::request(id, u"tools/call"_s, {{u"name"_s, tool}, {u"arguments"_s, arguments}});
}

// The structured result of a tools/call response, or the error text.
struct Outcome {
    bool isError = false;
    QString text;
    QJsonObject structured;
    QJsonObject error; // a protocol error
};

Outcome outcomeOf(const QByteArray& line)
{
    Outcome out;
    const QJsonObject message = parseObject(line);
    if (message.contains(u"error"_s)) {
        out.isError = true;
        out.error = message.value(u"error"_s).toObject();
        out.text = out.error.value(u"message"_s).toString();
        return out;
    }
    const QJsonObject result = message.value(u"result"_s).toObject();
    out.isError = result.value(u"isError"_s).toBool();
    const QJsonArray content = result.value(u"content"_s).toArray();
    if (!content.isEmpty())
        out.text = content.first().toObject().value(u"text"_s).toString();
    out.structured = result.value(u"structuredContent"_s).toObject();
    return out;
}

QStringList names(const QJsonObject& structured)
{
    QStringList out;
    for (const QJsonValue& item : structured.value(u"items"_s).toArray())
        out.push_back(item.toObject().value(u"name"_s).toString());
    return out;
}

// A folder on disk and the same in a hand-made index: the service is not
// started (it reads no volume), its index is filled by hand to mirror the
// folder, and the engine, host and server run on it as in the app.
struct Fixture {
    QTemporaryDir dir;
    QString root; // the folder, native
    std::unique_ptr<IndexService> service;
    std::unique_ptr<SearchEngine> engine;
    std::unique_ptr<ToolHost> host;
    std::unique_ptr<McpServer> server;
    EntryId folder = kNoEntry;
    QStringList opened; // what the fake shell opened
    QStringList log;
    std::vector<ClipSummary> clips;

    explicit Fixture(bool withShell = true, bool withClipboard = true)
    {
        root = QDir::toNativeSeparators(dir.path());
        IndexService::Options options;
        options.content.enabled = false;
        service = std::make_unique<IndexService>(dir.path() + u"/index"_s, options);
        folder = addFolder(root);
        engine = std::make_unique<SearchEngine>(service.get());
        Services services;
        services.index = service.get();
        services.engine = engine.get();
        services.version = u"0.0-test"_s;
        services.log = [this](const QString& line) { log.push_back(line); };
        if (withShell) {
            services.open = [this](const QString& path, std::function<void(bool)> done) {
                opened.push_back(u"open "_s + path);
                done(true);
            };
            services.reveal = [this](const QString& path, std::function<void(bool)> done) {
                opened.push_back(u"reveal "_s + path);
                done(true);
            };
        }
        if (withClipboard) {
            services.clipboard = [this](const ClipQuery& query) {
                std::vector<ClipSummary> out;
                for (const ClipSummary& c : clips) {
                    if ((query.kind == u"all" || query.kind == c.kind) && (query.query.isEmpty() || c.text.contains(query.query))
                        && std::ssize(out) < query.limit) {
                        ClipSummary cut = c;
                        cut.text = c.text.left(query.maxChars);
                        out.push_back(cut);
                    }
                }
                return out;
            };
            services.clipboardText = [this](qint64 id) -> std::optional<QString> {
                for (const ClipSummary& c : clips) {
                    if (c.id == id)
                        return c.text;
                }
                return std::nullopt;
            };
        }
        host = std::make_unique<ToolHost>(std::move(services));
        Policy policy;
        policy.contentExtensions = {u"txt"_s, u"md"_s};
        policy.contentDocuments = false;
        host->setPolicy(policy);
        server = std::make_unique<McpServer>(host.get(), u"0.0-test"_s);
    }

    // The entry of a folder on disk, every folder on the way added.
    EntryId addFolder(const QString& path)
    {
        const auto index = service->index();
        const auto lock = index->writeLock();
        const QStringList parts = path.split(u'\\', Qt::SkipEmptyParts);
        EntryId at = kNoEntry;
        for (const QString& part : parts) {
            const std::string name = part.toStdString();
            if (at == kNoEntry) {
                bool found = false;
                for (const EntryId r : index->roots()) {
                    if (QString::fromStdString(std::string(index->name(r))).compare(part, Qt::CaseInsensitive) == 0) {
                        at = r;
                        found = true;
                    }
                }
                if (!found)
                    at = index->addRoot(name);
                continue;
            }
            EntryId child = index->findChild(at, name);
            if (child == kNoEntry)
                child = index->add(at, name, EntryFlag::Directory);
            at = child;
        }
        return at;
    }

    // A file written to disk and put in the index, `modified` seconds since
    // 1970 (the index's; the disk keeps now).
    QString write(const QString& relative, const QByteArray& content, FileTime modified, std::uint8_t flags = 0)
    {
        const QString path = root + u"\\"_s + QDir::toNativeSeparators(relative);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return {};
        file.write(content);
        file.close();
        const EntryId parent = addFolder(QFileInfo(path).absolutePath().replace(u'/', u'\\'));
        const auto index = service->index();
        const auto lock = index->writeLock();
        const EntryId id = index->add(parent, QFileInfo(path).fileName().toStdString(), flags);
        index->setModified(id, modified);
        return path;
    }

    EntryId subfolder(const QString& relative, FileTime modified)
    {
        const QString path = root + u"\\"_s + QDir::toNativeSeparators(relative);
        QDir().mkpath(path);
        const EntryId id = addFolder(path);
        const auto index = service->index();
        const auto lock = index->writeLock();
        index->setModified(id, modified);
        return id;
    }

    // Gives a folder (and what is in it) a DACL for administrators and the
    // system alone: this process, the owner, may still read and change its
    // security, but not list or read it (an elevated test process checks
    // with its linked token, which cannot either). Undone by the destructor.
    QString restricted;
    bool restrict(const QString& relative)
    {
        const QString path = root + u"\\"_s + QDir::toNativeSeparators(relative);
        if (!setDacl(path, L"D:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)", PROTECTED_DACL_SECURITY_INFORMATION))
            return false;
        restricted = path;
        return true;
    }
    void unrestrict()
    {
        if (!restricted.isEmpty())
            setDacl(restricted, L"D:(A;OICI;FA;;;WD)", UNPROTECTED_DACL_SECURITY_INFORMATION);
        restricted.clear();
    }
    ~Fixture() { unrestrict(); }

    static bool setDacl(const QString& path, const wchar_t* sddl, SECURITY_INFORMATION protection)
    {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr))
            return false;
        PACL dacl = nullptr;
        BOOL present = FALSE;
        BOOL defaulted = FALSE;
        ::GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted);
        const DWORD error = ::SetNamedSecurityInfoW(const_cast<wchar_t*>(path.toStdWString().c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | protection, nullptr, nullptr, dacl, nullptr);
        ::LocalFree(descriptor);
        return error == ERROR_SUCCESS;
    }
};

// An in-process session: what the server sends is collected.
struct Session {
    McpServer* server;
    McpServer::SessionId id;
    QList<QByteArray> received;

    explicit Session(McpServer* s)
        : server(s)
    {
        id = server->openSession([this](const QByteArray& line) { received.push_back(line); });
    }
    ~Session() { server->closeSession(id); }

    // Sends a line and waits for the next line back.
    std::optional<QByteArray> exchange(const QByteArray& line, int timeoutMs = 5000)
    {
        const qsizetype before = received.size();
        server->receive(id, line);
        QElapsedTimer timer;
        timer.start();
        while (received.size() <= before && timer.elapsed() < timeoutMs)
            QTest::qWait(5);
        if (received.size() <= before)
            return std::nullopt;
        return received.at(before);
    }

    bool initialize(const QString& version = u"2025-11-25"_s, const QString& client = u"tst_mcp"_s)
    {
        const auto reply = exchange(requestLine(1, u"initialize"_s,
            {{u"protocolVersion"_s, version}, {u"capabilities"_s, QJsonObject()},
                {u"clientInfo"_s, QJsonObject {{u"name"_s, client}, {u"version"_s, u"1.0"_s}}}}));
        if (!reply)
            return false;
        server->receive(id, jsonrpc::notification(u"notifications/initialized"_s));
        return parseObject(*reply).value(u"result"_s).toObject().value(u"protocolVersion"_s).toString() == version;
    }
};

// The relay, driven over its stdio as an AI client drives it: started with
// pipes for its stdin and stdout (Win32, as the clients do; its stderr is
// the test's), lines written, lines read on a thread of their own.
struct Relay {
    HANDLE process = nullptr;
    HANDLE toRelay = nullptr; // its stdin
    HANDLE fromRelay = nullptr; // its stdout
    std::thread reader;
    std::mutex mutex;
    QByteArray pending;
    bool eof = false;
    DWORD exitCode = 0xFFFF'FFFF;

    ~Relay()
    {
        if (toRelay)
            ::CloseHandle(toRelay);
        if (process) {
            if (::WaitForSingleObject(process, 2000) != WAIT_OBJECT_0)
                ::TerminateProcess(process, 9);
            ::CloseHandle(process);
        }
        if (reader.joinable())
            reader.join();
        if (fromRelay)
            ::CloseHandle(fromRelay);
    }

    bool start(const std::wstring& pipeName)
    {
        SECURITY_ATTRIBUTES inheritable {sizeof inheritable, nullptr, TRUE};
        HANDLE inRead = nullptr;
        HANDLE outWrite = nullptr;
        if (!::CreatePipe(&inRead, &toRelay, &inheritable, 0) || !::CreatePipe(&fromRelay, &outWrite, &inheritable, 0))
            return false;
        ::SetHandleInformation(toRelay, HANDLE_FLAG_INHERIT, 0);
        ::SetHandleInformation(fromRelay, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW startup {};
        startup.cb = sizeof startup;
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = inRead;
        startup.hStdOutput = outWrite;
        startup.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
        std::wstring command = L"\"" + QString::fromUtf8(WINSHUN_MCP_RELAY).toStdWString() + L"\" --pipe " + pipeName;
        PROCESS_INFORMATION info {};
        const bool started = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &info);
        ::CloseHandle(inRead);
        ::CloseHandle(outWrite);
        if (!started)
            return false;
        ::CloseHandle(info.hThread);
        process = info.hProcess;
        reader = std::thread([this] {
            char chunk[64 * 1024];
            DWORD got = 0;
            while (::ReadFile(fromRelay, chunk, sizeof chunk, &got, nullptr) && got > 0) {
                const std::lock_guard lock(mutex);
                pending.append(chunk, static_cast<qsizetype>(got));
            }
            const std::lock_guard lock(mutex);
            eof = true;
        });
        return true;
    }

    void send(const QByteArray& line)
    {
        DWORD put = 0;
        ::WriteFile(toRelay, line.constData(), static_cast<DWORD>(line.size()), &put, nullptr);
    }

    // The event loop runs meanwhile: the server (on this thread) must get
    // to answer the relay.
    std::optional<QByteArray> readLine(int timeoutMs = 10000)
    {
        QElapsedTimer timer;
        timer.start();
        for (;;) {
            {
                const std::lock_guard lock(mutex);
                if (const qsizetype at = pending.indexOf('\n'); at >= 0) {
                    QByteArray line = pending.left(at);
                    pending.remove(0, at + 1);
                    return line;
                }
                if (eof)
                    return std::nullopt;
            }
            if (timer.elapsed() >= timeoutMs)
                return std::nullopt;
            QTest::qWait(5);
        }
    }

    std::optional<QByteArray> exchange(const QByteArray& line, int timeoutMs = 10000)
    {
        send(line);
        return readLine(timeoutMs);
    }

    // Closes its stdin, as a client shutting down does, and waits for it to exit.
    void finish()
    {
        ::CloseHandle(toRelay);
        toRelay = nullptr;
        if (::WaitForSingleObject(process, 5000) == WAIT_OBJECT_0)
            ::GetExitCodeProcess(process, &exitCode);
    }
};

std::wstring testPipeName(const char* tag)
{
    return L"WinShun.Mcp.test-" + std::to_wstring(::GetCurrentProcessId()) + L"-" + std::wstring(tag, tag + std::strlen(tag));
}

// This test runs as administrator (ctest started from an elevated shell).
bool elevated()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    const bool result = plugin::sandbox::tokenElevated(token);
    ::CloseHandle(token);
    return result;
}

} // namespace

class TestMcp : public QObject {
    Q_OBJECT

private slots:
    void jsonRpcParse()
    {
        QString problem;
        auto m = jsonrpc::parse(R"({"jsonrpc":"2.0","id":1,"method":"ping"})", &problem);
        QVERIFY(m);
        QVERIFY(m->isRequest());
        QCOMPARE(m->method, u"ping"_s);
        QCOMPARE(m->id.toInt(), 1);

        m = jsonrpc::parse(R"({"jsonrpc":"2.0","id":"abc","method":"tools/call","params":{"name":"x"}})");
        QVERIFY(m && m->isRequest());
        QCOMPARE(m->params.value(u"name"_s).toString(), u"x"_s);
        QCOMPARE(jsonrpc::idKey(m->id), u"\"abc\""_s);

        m = jsonrpc::parse(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
        QVERIFY(m && m->isNotification());

        m = jsonrpc::parse(R"({"jsonrpc":"2.0","id":3,"result":{"ok":true}})");
        QVERIFY(m && m->isResponse() && !m->failed);
        m = jsonrpc::parse(R"({"jsonrpc":"2.0","id":3,"error":{"code":-32601,"message":"no"}})");
        QVERIFY(m && m->isResponse() && m->failed);
        QCOMPARE(m->error.value(u"code"_s).toInt(), -32601);

        QVERIFY(!jsonrpc::parse("not json", &problem));
        QVERIFY(problem.contains(u"JSON"));
        QVERIFY(!jsonrpc::parse(R"([{"jsonrpc":"2.0","id":1,"method":"ping"}])", &problem)); // no batches
        QVERIFY(!jsonrpc::parse(R"({"id":1,"method":"ping"})")); // no version
        QVERIFY(!jsonrpc::parse(R"({"jsonrpc":"2.0","id":{"a":1},"method":"ping"})")); // id must be a string or a number
        QVERIFY(!jsonrpc::parse(R"({"jsonrpc":"2.0","id":1,"method":"ping","params":[1]})")); // params an object

        const QByteArray line = jsonrpc::errorResponse(7, jsonrpc::kMethodNotFound, u"Method not found: x"_s);
        QVERIFY(line.endsWith('\n'));
        QVERIFY(!line.chopped(1).contains('\n'));
        const QJsonObject error = parseObject(line).value(u"error"_s).toObject();
        QCOMPARE(error.value(u"code"_s).toInt(), -32601);
    }

    void toolsOnOffer()
    {
        Fixture f;
        QStringList expected {u"search_files"_s, u"search_content"_s, u"recent_files"_s, u"list_folder"_s,
            u"get_file_info"_s, u"index_status"_s, u"read_file_text"_s};
        QCOMPARE(f.host->toolNames(), expected);
        QCOMPARE(f.host->known(u"open_item"_s), ToolHost::Known::Off);
        QCOMPARE(f.host->known(u"nothing"_s), ToolHost::Known::No);
        QCOMPARE(f.host->known(u"search_files"_s), ToolHost::Known::Yes);

        QSignalSpy changed(f.host.get(), &ToolHost::toolsChanged);
        Policy policy = f.host->policy();
        policy.allowOpen = true;
        policy.allowClipboard = true;
        f.host->setPolicy(policy);
        QCOMPARE(changed.count(), 1);
        QVERIFY(f.host->toolNames().contains(u"open_item"_s));
        QVERIFY(f.host->toolNames().contains(u"show_in_folder"_s));
        QVERIFY(f.host->toolNames().contains(u"clipboard_history"_s));
        QVERIFY(f.host->toolNames().contains(u"clipboard_get"_s));
        f.host->setPolicy(policy); // the same again: no change
        QCOMPARE(changed.count(), 1);

        // Every tool has what MCP wants of it.
        for (const QJsonValue& v : f.host->tools()) {
            const QJsonObject tool = v.toObject();
            QVERIFY2(!tool.value(u"name"_s).toString().isEmpty(), "name");
            QVERIFY2(!tool.value(u"description"_s).toString().isEmpty(), "description");
            QCOMPARE(tool.value(u"inputSchema"_s).toObject().value(u"type"_s).toString(), u"object"_s);
            QVERIFY(tool.value(u"annotations"_s).toObject().contains(u"readOnlyHint"_s));
        }

        // Without a shell or a clipboard (wsbench), their tools are not offered even when allowed.
        Fixture bare(false, false);
        bare.host->setPolicy(policy);
        QVERIFY(!bare.host->toolNames().contains(u"open_item"_s));
        QVERIFY(!bare.host->toolNames().contains(u"clipboard_history"_s));
    }

    void lifecycle()
    {
        Fixture f;
        Session s(f.server.get());
        // Before initialize: ping is fine, nothing else.
        auto reply = s.exchange(requestLine(10, u"ping"_s));
        QVERIFY(reply);
        QVERIFY(parseObject(*reply).contains(u"result"_s));
        reply = s.exchange(requestLine(11, u"tools/list"_s));
        QVERIFY(reply);
        QCOMPARE(parseObject(*reply).value(u"error"_s).toObject().value(u"code"_s).toInt(), jsonrpc::kUnavailable);

        // An older revision is echoed; an unknown one gets the newest.
        QVERIFY(s.initialize(u"2025-06-18"_s));
        reply = s.exchange(requestLine(1, u"initialize"_s, {{u"protocolVersion"_s, u"1999-01-01"_s}}));
        QVERIFY(reply);
        const QJsonObject result = parseObject(*reply).value(u"result"_s).toObject();
        QCOMPARE(result.value(u"protocolVersion"_s).toString(), u"2025-11-25"_s);
        QCOMPARE(result.value(u"serverInfo"_s).toObject().value(u"name"_s).toString(), u"winshun"_s);
        QVERIFY(result.value(u"capabilities"_s).toObject().value(u"tools"_s).toObject().value(u"listChanged"_s).toBool());
        QVERIFY(result.value(u"instructions"_s).toString().contains(u"pinyin"));

        reply = s.exchange(requestLine(12, u"tools/list"_s));
        QVERIFY(reply);
        QCOMPARE(parseObject(*reply).value(u"result"_s).toObject().value(u"tools"_s).toArray().size(), 7);
        reply = s.exchange(requestLine(13, u"prompts/list"_s));
        QVERIFY(reply && parseObject(*reply).value(u"result"_s).toObject().value(u"prompts"_s).toArray().isEmpty());
        reply = s.exchange(requestLine(14, u"nothing/here"_s));
        QVERIFY(reply);
        QCOMPARE(parseObject(*reply).value(u"error"_s).toObject().value(u"code"_s).toInt(), jsonrpc::kMethodNotFound);
        reply = s.exchange("{not json");
        QVERIFY(reply);
        QCOMPARE(parseObject(*reply).value(u"error"_s).toObject().value(u"code"_s).toInt(), jsonrpc::kInvalidRequest);
        QVERIFY(parseObject(*reply).value(u"id"_s).isNull());

        // Unknown tool: a protocol error; wrong arguments: a tool error.
        reply = s.exchange(callLine(15, u"no_such_tool"_s));
        QVERIFY(reply);
        QCOMPARE(parseObject(*reply).value(u"error"_s).toObject().value(u"code"_s).toInt(), jsonrpc::kInvalidParams);
        reply = s.exchange(callLine(16, u"search_files"_s, {{u"limit"_s, 5}}));
        QVERIFY(reply);
        Outcome out = outcomeOf(*reply);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"query"), qPrintable(out.text));
        reply = s.exchange(callLine(17, u"search_files"_s, {{u"query"_s, u"x"_s}, {u"kind"_s, u"sideways"_s}}));
        QVERIFY(reply && outcomeOf(*reply).isError);
        // A tool its switch keeps off: a tool error saying so.
        reply = s.exchange(callLine(18, u"open_item"_s, {{u"path"_s, u"C:\\"_s}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"turned off"), qPrintable(out.text));

        // The session is counted, with the client's name.
        const auto sessions = f.server->sessions();
        QCOMPARE(sessions.size(), std::size_t {1});
        QCOMPARE(sessions.front().client, u"tst_mcp"_s);
        QCOMPARE(sessions.front().calls, 2); // the two that reached a tool
    }

    void searchFiles()
    {
        Fixture f;
        f.write(u"年度报告.docx"_s, "x", 1'700'000'000);
        f.write(u"报告草稿.txt"_s, "draft", 1'700'000'100);
        f.write(u"notes.md"_s, "notes", 1'700'000'200);
        f.write(u"项目\\readme.md"_s, "readme", 1'700'000'300);
        f.subfolder(u"报告"_s, 1'700'000'400);
        Session s(f.server.get());
        QVERIFY(s.initialize());

        auto reply = s.exchange(callLine(2, u"search_files"_s, {{u"query"_s, u"报告"_s}}));
        QVERIFY(reply);
        Outcome out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QStringList found = names(out.structured);
        QVERIFY2(found.contains(u"年度报告.docx"_s) && found.contains(u"报告草稿.txt"_s) && found.contains(u"报告"_s), qPrintable(found.join(u", "_s)));
        QCOMPARE(out.structured.value(u"returned"_s).toInt(), 3);
        QVERIFY(!out.structured.value(u"truncated"_s).toBool());
        // The text block is the same JSON.
        QCOMPARE(parseObject(out.text.toUtf8()).value(u"returned"_s).toInt(), 3);
        // Items carry a path, a type, the time from the index and the size from the disk.
        const QJsonObject first = out.structured.value(u"items"_s).toArray().first().toObject();
        QVERIFY(first.value(u"path"_s).toString().startsWith(f.root));
        QVERIFY2(first.value(u"modified"_s).toString().startsWith(u"2023-11-1"), qPrintable(first.value(u"modified"_s).toString()));
        for (const QJsonValue& v : out.structured.value(u"items"_s).toArray()) {
            const QJsonObject item = v.toObject();
            if (item.value(u"type"_s).toString() == u"file")
                QVERIFY(item.value(u"size"_s).toInt() > 0);
        }

        // Pinyin, folders only, files only, extensions, a limit.
        reply = s.exchange(callLine(3, u"search_files"_s, {{u"query"_s, u"ndbg"_s}}));
        QVERIFY(reply);
        QCOMPARE(names(outcomeOf(*reply).structured), QStringList {u"年度报告.docx"_s});
        reply = s.exchange(callLine(4, u"search_files"_s, {{u"query"_s, u"报告"_s}, {u"kind"_s, u"folders"_s}}));
        QVERIFY(reply);
        QCOMPARE(names(outcomeOf(*reply).structured), QStringList {u"报告"_s});
        reply = s.exchange(callLine(5, u"search_files"_s, {{u"query"_s, u"报告"_s}, {u"kind"_s, u"files"_s}}));
        QVERIFY(reply);
        found = names(outcomeOf(*reply).structured);
        QCOMPARE(found.size(), 2);
        QVERIFY(!found.contains(u"报告"_s));
        reply = s.exchange(callLine(6, u"search_files"_s, {{u"query"_s, u"ext:md"_s}}));
        QVERIFY(reply);
        found = names(outcomeOf(*reply).structured);
        QVERIFY(found.contains(u"notes.md"_s) && found.contains(u"readme.md"_s));
        reply = s.exchange(callLine(7, u"search_files"_s, {{u"query"_s, u"报告"_s}, {u"limit"_s, 1}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QCOMPARE(out.structured.value(u"returned"_s).toInt(), 1);
        QVERIFY(out.structured.value(u"truncated"_s).toBool());
        // Newest first.
        reply = s.exchange(callLine(8, u"search_files"_s, {{u"query"_s, u"报告"_s}, {u"sort"_s, u"modified"_s}, {u"kind"_s, u"files"_s}}));
        QVERIFY(reply);
        QCOMPARE(names(outcomeOf(*reply).structured), (QStringList {u"报告草稿.txt"_s, u"年度报告.docx"_s}));
        // A folder AI must not see.
        Policy policy = f.host->policy();
        policy.excludedPaths = {f.root + u"\\项目"_s};
        f.host->setPolicy(policy);
        reply = s.exchange(callLine(9, u"search_files"_s, {{u"query"_s, u"ext:md"_s}}));
        QVERIFY(reply);
        QCOMPARE(names(outcomeOf(*reply).structured), QStringList {u"notes.md"_s});
        reply = s.exchange(callLine(10, u"list_folder"_s, {{u"path"_s, f.root + u"\\项目"_s}}));
        QVERIFY(reply && outcomeOf(*reply).isError);
        QVERIFY(f.log.size() >= 9);
        QVERIFY2(f.log.last().contains(u"tst_mcp list_folder"), qPrintable(f.log.last()));
    }

    void searchContent()
    {
        Fixture f;
        f.write(u"销售.txt"_s, QByteArrayLiteral("第一行\n第三季度销售额 123 万\n第三行"), 1'700'000'000);
        f.write(u"other.txt"_s, "nothing here", 1'700'000'000);
        f.write(u"code.md"_s, "x\nconst auto lock = index->readLock();\n", 1'700'000'000);
        Session s(f.server.get());
        QVERIFY(s.initialize());

        auto reply = s.exchange(callLine(2, u"search_content"_s, {{u"query"_s, u"第三季度"_s}}), 20000);
        QVERIFY(reply);
        Outcome out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(names(out.structured), QStringList {u"销售.txt"_s});
        const QJsonObject item = out.structured.value(u"items"_s).toArray().first().toObject();
        QCOMPARE(item.value(u"location"_s).toObject().value(u"line"_s).toInt(), 2);
        QVERIFY(item.value(u"snippet"_s).toString().contains(u"第三季度销售额"));
        QVERIFY(out.structured.value(u"complete"_s).toBool());

        reply = s.exchange(callLine(3, u"search_content"_s, {{u"query"_s, u"const auto lock = index->readLock();"_s}}), 20000);
        QVERIFY(reply);
        QCOMPARE(names(outcomeOf(*reply).structured), QStringList {u"code.md"_s});
        reply = s.exchange(callLine(4, u"search_content"_s, {{u"query"_s, u"zzqqxx"_s}}), 20000);
        QVERIFY(reply);
        QCOMPARE(outcomeOf(*reply).structured.value(u"returned"_s).toInt(), 0);

        // Content search turned off in the settings: no file is read for AI either.
        Policy policy = f.host->policy();
        policy.content = false;
        f.host->setPolicy(policy);
        reply = s.exchange(callLine(5, u"search_content"_s, {{u"query"_s, u"第三季度"_s}}), 20000);
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"turned off"), qPrintable(out.text));
    }

    // Content search in some folders: named among the words ("第三季度 abc/")
    // or given (folders), every folder so named, system folders only when
    // nothing else is so named, and nowhere else.
    void searchContentInFolders()
    {
        Fixture f;
        const QByteArray text = QByteArrayLiteral("第三季度销售额 123 万");
        f.write(u"项目\\abc\\报告.txt"_s, text, 1'700'000'000);
        f.write(u"备份\\abc\\报告.txt"_s, text, 1'700'000'000);
        f.write(u"其他\\报告.txt"_s, text, 1'700'000'000);
        f.write(u"其他\\abcdef\\报告.txt"_s, text, 1'700'000'000);
        f.subfolder(u"tools\\abc"_s, 1'700'000'000);
        f.subfolder(u"tools\\only"_s, 1'700'000'000);
        {
            const auto index = f.service->index();
            const auto lock = index->writeLock();
            for (const QString& folder : {u"tools"_s, u"tools\\abc"_s, u"tools\\only"_s})
                index->setFlags(index->findPath((f.root + u'\\' + folder).toStdWString()),
                    static_cast<std::uint8_t>(EntryFlag::Directory | EntryFlag::LowPriority));
        }
        f.write(u"tools\\abc\\报告.txt"_s, text, 1'700'000'000, EntryFlag::LowPriority);
        f.write(u"tools\\only\\报告.txt"_s, text, 1'700'000'000, EntryFlag::LowPriority);
        const QString sep = u"\\"_s;
        const auto in = [&](const QString& relative) { return f.root + sep + relative + sep + u"报告.txt"_s; };

        // Straight to the engine: where it says it looks, and what it finds.
        const auto search = [&](SearchEngine::Request request) {
            request.scope = Scope::Content;
            request.contentExtensions = {u"txt"_s};
            request.contentDocuments = false;
            QSignalSpy scopes(f.engine.get(), &SearchEngine::contentScope);
            QSignalSpy results(f.engine.get(), &SearchEngine::contentResults);
            QSignalSpy progress(f.engine.get(), &SearchEngine::contentProgress);
            const quint64 id = f.engine->submit(std::move(request));
            const auto finished = [&] {
                return std::ranges::any_of(progress, [&](const QList<QVariant>& p) { return p[0].toULongLong() == id && p[3].toBool(); });
            };
            if (!QTest::qWaitFor(finished, 20000))
                return std::pair {ContentScope {}, QStringList {u"(timed out)"_s}};
            QStringList paths;
            for (const QList<QVariant>& r : results) {
                for (const SearchResult& hit : r[1].value<SearchResults>())
                    paths.append(hit.path);
            }
            paths.sort();
            return std::pair {scopes.isEmpty() ? ContentScope {} : scopes.last()[1].value<ContentScope>(), paths};
        };
        const auto sorted = [](QStringList list) {
            list.sort();
            return list;
        };

        SearchEngine::Request request;
        request.text = u"第三季度 abc/"_s;
        auto [scope, found] = search(request);
        QCOMPARE(found, sorted({in(u"项目\\abc"_s), in(u"备份\\abc"_s)})); // not tools\abc: a program folder
        QCOMPARE(scope.words, u"第三季度"_s);
        QCOMPARE(scope.places.size(), std::size_t {1});
        QCOMPARE(scope.places[0].typed, u"abc/"_s);
        QCOMPARE(scope.places[0].status, ContentScope::Status::Searched);
        QCOMPARE(scope.places[0].count, 2);
        QVERIFY(!scope.places[0].approximate);
        // One of them picked.
        request.contentPick = {u"abc/"_s, scope.places[0].folders[1]};
        std::tie(scope, found) = search(request);
        QCOMPARE(found, QStringList {scope.places[0].folders[1] + u"\\报告.txt"_s});
        QCOMPARE(scope.places[0].chosen, request.contentPick.folder);
        QCOMPARE(scope.places[0].count, 2); // still told of both, for the chips

        // A folder in a folder; "/name" last; only program folders so named: those.
        request = {};
        request.text = u"第三季度 项目/abc/"_s;
        QCOMPARE(search(request).second, QStringList {in(u"项目\\abc"_s)});
        request.text = u"第三季度 /only"_s;
        QCOMPARE(search(request).second, QStringList {in(u"tools\\only"_s)});
        // None named just so: those whose names hold it.
        request.text = u"第三季度 abcd/"_s;
        std::tie(scope, found) = search(request);
        QCOMPARE(found, QStringList {in(u"其他\\abcdef"_s)});
        QVERIFY(scope.places[0].approximate);
        // None at all: it is something to find ("/nosuch" is not in the file).
        request.text = u"第三季度 /nosuch"_s;
        std::tie(scope, found) = search(request);
        QVERIFY(found.isEmpty());
        QCOMPARE(scope.places[0].status, ContentScope::Status::Words);
        QCOMPARE(scope.words, u"第三季度 /nosuch"_s);
        // ... and searched for as such, everywhere: a file that has it is found.
        f.write(u"路由\\接口.txt"_s, QByteArrayLiteral("第三季度 GET nosuchfolder/ 200"), 1'700'000'000);
        request.text = u"第三季度 nosuchfolder/"_s;
        std::tie(scope, found) = search(request);
        QCOMPARE(found, QStringList {f.root + u"\\路由\\接口.txt"_s});
        QCOMPARE(scope.places[0].status, ContentScope::Status::Words);
        // A path: that folder alone; one not in the index: nowhere.
        request.text = f.root + u"\\其他\\ 第三季度"_s;
        QCOMPARE(search(request).second, sorted({in(u"其他"_s), in(u"其他\\abcdef"_s)}));
        request.text = f.root + u"\\不在\\ 第三季度"_s;
        std::tie(scope, found) = search(request);
        QVERIFY(found.isEmpty());
        QCOMPARE(scope.places[0].status, ContentScope::Status::NotFound);
        // Given (a folder chosen in the launcher, or by AI); typed ones go first.
        request.text = u"第三季度"_s;
        request.contentFolders = {f.root + u"\\备份"_s};
        QCOMPARE(search(request).second, QStringList {in(u"备份\\abc"_s)});
        request.text = u"第三季度 项目/"_s;
        QCOMPARE(search(request).second, QStringList {in(u"项目\\abc"_s)});
        // A program folder given: read all the same.
        request.text = u"第三季度"_s;
        request.contentFolders = {f.root + u"\\tools"_s};
        QCOMPARE(search(request).second, sorted({in(u"tools\\abc"_s), in(u"tools\\only"_s)}));

        // Through MCP: folders.
        Session s(f.server.get());
        QVERIFY(s.initialize());
        auto reply = s.exchange(callLine(2, u"search_content"_s,
                                    {{u"query"_s, u"第三季度"_s}, {u"folders"_s, QJsonArray {f.root + u"\\项目"_s, f.root + u"\\备份\\"_s}}}),
            20000);
        QVERIFY(reply);
        Outcome out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(out.structured.value(u"returned"_s).toInt(), 2);
        reply = s.exchange(callLine(3, u"search_content"_s, {{u"query"_s, u"第三季度"_s}, {u"folders"_s, QJsonArray {f.root + u"\\不在"_s}}}), 20000);
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"not in the index"), qPrintable(out.text));
        reply = s.exchange(callLine(4, u"search_content"_s, {{u"query"_s, u"第三季度"_s}, {u"folders"_s, QJsonArray {in(u"其他"_s)}}}), 20000);
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"is a file"), qPrintable(out.text));
    }

    void readFileText()
    {
        Fixture f;
        f.write(u"utf8.txt"_s, QByteArray("\xEF\xBB\xBF") + u"第一行\r\n报告 report\r\n"_s.toUtf8(), 1'700'000'000);
        f.write(u"gbk.txt"_s, QByteArray("\xB1\xA8\xB8\xE6\n"), 1'700'000'000); // 报告 in GBK
        f.write(u"utf16.txt"_s, QByteArray("\xFF\xFE", 2) + QByteArray(reinterpret_cast<const char*>(u"报告"), 4), 1'700'000'000);
        f.write(u"tool.exe"_s, QByteArray("MZ\0\0\x90\0", 6), 1'700'000'000);
        f.write(u"long.txt"_s, QByteArray(2500, 'a'), 1'700'000'000);
        f.write(u"empty.txt"_s, QByteArray(), 1'700'000'000);
        f.write(u"private\\p.txt"_s, "secret", 1'700'000'000);
        Policy policy = f.host->policy();
        policy.excludedPaths = {f.root + u"\\private"_s};
        f.host->setPolicy(policy);
        Session s(f.server.get());
        QVERIFY(s.initialize());

        const auto read = [&](int id, const QString& name, QJsonObject more = {}) {
            more.insert(u"path"_s, f.root + u"\\"_s + name);
            const auto reply = s.exchange(callLine(id, u"read_file_text"_s, more), 20000);
            return reply ? outcomeOf(*reply) : Outcome {};
        };
        Outcome out = read(2, u"utf8.txt"_s);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(out.structured.value(u"text"_s).toString(), u"第一行\n报告 report\n"_s); // no BOM, no CR
        QCOMPARE(out.structured.value(u"kind"_s).toString(), u"text"_s);
        QVERIFY(!out.structured.contains(u"next_offset"_s));
        if (ContentScanner::legacyCodePage() == 936) // as the content search reads it: in the system's code page
            QCOMPARE(read(3, u"gbk.txt"_s).structured.value(u"text"_s).toString(), u"报告\n"_s);
        QCOMPARE(read(4, u"utf16.txt"_s).structured.value(u"text"_s).toString(), u"报告"_s);
        out = read(5, u"tool.exe"_s);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"not a text file"), qPrintable(out.text));

        // A long text in parts.
        out = read(6, u"long.txt"_s, {{u"max_chars"_s, 1000}});
        QCOMPARE(out.structured.value(u"text"_s).toString().size(), 1000);
        QCOMPARE(out.structured.value(u"chars"_s).toInt(), 2500);
        QCOMPARE(out.structured.value(u"next_offset"_s).toInt(), 1000);
        out = read(7, u"long.txt"_s, {{u"max_chars"_s, 1000}, {u"offset"_s, 2000}});
        QCOMPARE(out.structured.value(u"text"_s).toString().size(), 500);
        QVERIFY(!out.structured.contains(u"next_offset"_s));
        out = read(8, u"empty.txt"_s);
        QVERIFY(!out.isError);
        QVERIFY(out.structured.contains(u"note"_s));

        out = read(9, u"missing.txt"_s);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"no such file"), qPrintable(out.text));
        QVERIFY(read(10, u""_s).isError); // the folder itself
        out = read(11, u"private\\p.txt"_s);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"not visible to AI"), qPrintable(out.text));
    }

    void recentAndFolder()
    {
        Fixture f;
        f.write(u"a.txt"_s, "a", 1'700'000'000);
        f.write(u"b.docx"_s, "b", 1'700'000'300);
        f.write(u"sub\\c.txt"_s, "c", 1'700'000'200);
        f.write(u"hidden.txt"_s, "h", 1'700'000'500, EntryFlag::Hidden);
        f.write(u"sys.txt"_s, "s", 1'700'000'600, EntryFlag::LowPriority);
        Session s(f.server.get());
        QVERIFY(s.initialize());

        auto reply = s.exchange(callLine(2, u"recent_files"_s, {{u"folder"_s, f.root}}));
        QVERIFY(reply);
        Outcome out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(names(out.structured), (QStringList {u"b.docx"_s, u"c.txt"_s, u"a.txt"_s}));
        reply = s.exchange(callLine(3, u"recent_files"_s, {{u"folder"_s, f.root}, {u"include_hidden"_s, true}, {u"include_system"_s, true}, {u"limit"_s, 2}}));
        QVERIFY(reply);
        QCOMPARE(names(outcomeOf(*reply).structured), (QStringList {u"sys.txt"_s, u"hidden.txt"_s}));
        reply = s.exchange(callLine(4, u"recent_files"_s, {{u"folder"_s, f.root}, {u"extensions"_s, QJsonArray {u"docx"_s}}}));
        QVERIFY(reply);
        QCOMPARE(names(outcomeOf(*reply).structured), QStringList {u"b.docx"_s});
        reply = s.exchange(callLine(5, u"recent_files"_s, {{u"folder"_s, f.root}, {u"extensions"_s, u".TXT"_s}, {u"days"_s, 1}}));
        QVERIFY(reply);
        QCOMPARE(outcomeOf(*reply).structured.value(u"returned"_s).toInt(), 0); // 2023 is older than a day
        reply = s.exchange(callLine(6, u"recent_files"_s, {{u"folder"_s, f.root + u"\\nowhere"_s}}));
        QVERIFY(reply && outcomeOf(*reply).isError);

        reply = s.exchange(callLine(7, u"list_folder"_s, {{u"path"_s, f.root + u"\\"_s}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(names(out.structured), (QStringList {u"sub"_s, u"a.txt"_s, u"b.docx"_s, u"hidden.txt"_s, u"sys.txt"_s}));
        QCOMPARE(out.structured.value(u"total"_s).toInt(), 5);
        reply = s.exchange(callLine(8, u"list_folder"_s, {{u"path"_s, f.root}, {u"kind"_s, u"folders"_s}}));
        QVERIFY(reply);
        QCOMPARE(names(outcomeOf(*reply).structured), QStringList {u"sub"_s});
        reply = s.exchange(callLine(9, u"list_folder"_s, {{u"path"_s, f.root + u"\\a.txt"_s}}));
        QVERIFY(reply && outcomeOf(*reply).isError);

        reply = s.exchange(callLine(10, u"get_file_info"_s, {{u"path"_s, f.root + u"/sub/c.txt"_s}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QVERIFY(out.structured.value(u"exists"_s).toBool());
        QCOMPARE(out.structured.value(u"size"_s).toInt(), 1);
        QCOMPARE(out.structured.value(u"extension"_s).toString(), u"txt"_s);
        QVERIFY(out.structured.value(u"indexed"_s).toBool());
        QCOMPARE(out.structured.value(u"path"_s).toString(), f.root + u"\\sub\\c.txt"_s);
        reply = s.exchange(callLine(11, u"get_file_info"_s, {{u"path"_s, f.root + u"\\missing.txt"_s}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY(!out.isError);
        QVERIFY(!out.structured.value(u"exists"_s).toBool());

        reply = s.exchange(callLine(12, u"index_status"_s));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY(out.structured.value(u"running"_s).toBool() && out.structured.value(u"connected"_s).toBool());
        QCOMPARE(out.structured.value(u"version"_s).toString(), u"0.0-test"_s);
        QVERIFY(out.structured.value(u"index"_s).toObject().value(u"items"_s).toInt() >= 5);
    }

    void openAndClipboard()
    {
        Fixture f;
        const QString file = f.write(u"open me.txt"_s, "x", 1'700'000'000);
        f.clips.push_back({1, u"text"_s, u"hello world"_s, 11, 1'700'000'000'000, 1'700'000'000'000, u"notepad"_s, {}, false, 0, 0});
        f.clips.push_back({2, u"link"_s, u"https://example.com"_s, 19, 1'700'000'001'000, 1'700'000'001'000, u"msedge"_s, u"固定"_s, true, 0, 0});
        f.clips.push_back({3, u"image"_s, {}, 0, 1'700'000'002'000, 1'700'000'002'000, u"mspaint"_s, {}, false, 640, 480});
        Policy policy = f.host->policy();
        policy.allowOpen = true;
        policy.allowClipboard = true;
        f.host->setPolicy(policy);
        Session s(f.server.get());
        QVERIFY(s.initialize());

        auto reply = s.exchange(callLine(2, u"open_item"_s, {{u"path"_s, file}}));
        QVERIFY(reply);
        Outcome out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QVERIFY(out.structured.value(u"opened"_s).toBool());
        QCOMPARE(f.opened, QStringList {u"open "_s + file});
        reply = s.exchange(callLine(3, u"show_in_folder"_s, {{u"path"_s, file}}));
        QVERIFY(reply && !outcomeOf(*reply).isError);
        QCOMPARE(f.opened.last(), u"reveal "_s + file);
        reply = s.exchange(callLine(4, u"open_item"_s, {{u"path"_s, f.root + u"\\gone.txt"_s}}));
        QVERIFY(reply && outcomeOf(*reply).isError);
        QCOMPARE(f.opened.size(), 2);
        policy.excludedPaths = {f.root};
        f.host->setPolicy(policy);
        reply = s.exchange(callLine(5, u"open_item"_s, {{u"path"_s, file}}));
        QVERIFY(reply && outcomeOf(*reply).isError);
        QCOMPARE(f.opened.size(), 2);

        reply = s.exchange(callLine(6, u"clipboard_history"_s));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(out.structured.value(u"returned"_s).toInt(), 3);
        const QJsonArray items = out.structured.value(u"items"_s).toArray();
        QCOMPARE(items.at(0).toObject().value(u"text"_s).toString(), u"hello world"_s);
        QCOMPARE(items.at(1).toObject().value(u"group"_s).toString(), u"固定"_s);
        QVERIFY(items.at(1).toObject().value(u"pinned"_s).toBool());
        QVERIFY(!items.at(2).toObject().contains(u"text"_s));
        QCOMPARE(items.at(2).toObject().value(u"width"_s).toInt(), 640);
        reply = s.exchange(callLine(7, u"clipboard_history"_s, {{u"kind"_s, u"link"_s}, {u"max_chars"_s, 50}}));
        QVERIFY(reply);
        QCOMPARE(outcomeOf(*reply).structured.value(u"returned"_s).toInt(), 1);
        reply = s.exchange(callLine(8, u"clipboard_get"_s, {{u"id"_s, 1}}));
        QVERIFY(reply);
        QCOMPARE(outcomeOf(*reply).structured.value(u"text"_s).toString(), u"hello world"_s);
        reply = s.exchange(callLine(9, u"clipboard_get"_s, {{u"id"_s, 99}}));
        QVERIFY(reply && outcomeOf(*reply).isError);
        // The log never has the texts.
        for (const QString& line : f.log)
            QVERIFY2(!line.contains(u"hello world"), qPrintable(line));
    }

    // Only what the caller could list or read itself: a folder for
    // administrators alone is, to the viewer, not there, nor is anything in it.
    void visibility()
    {
        Fixture f;
        f.write(u"open.txt"_s, "visible SECRETWORD here", 1'700'000'000);
        f.write(u"secret\\hidden.txt"_s, "SECRETWORD inside", 1'700'000'500);
        f.write(u"secret\\deeper\\more.txt"_s, "SECRETWORD too", 1'700'000'600);
        QVERIFY(f.restrict(u"secret"_s));
        Policy policy = f.host->policy();
        policy.allowOpen = true;
        f.host->setPolicy(policy);
        Session s(f.server.get());
        QVERIFY(s.initialize());

        // The folder's own name is in a folder the viewer can list; its contents are not.
        auto reply = s.exchange(callLine(2, u"list_folder"_s, {{u"path"_s, f.root}}));
        QVERIFY(reply);
        Outcome out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(names(out.structured), (QStringList {u"secret"_s, u"open.txt"_s}));
        reply = s.exchange(callLine(3, u"list_folder"_s, {{u"path"_s, f.root + u"\\secret"_s}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"not in the index"), qPrintable(out.text)); // the same words as for a folder that is not there
        reply = s.exchange(callLine(4, u"search_files"_s, {{u"query"_s, u"ext:txt"_s}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QCOMPARE(names(out.structured), QStringList {u"open.txt"_s});
        QVERIFY(!out.structured.contains(u"total_matches"_s)); // no count of what is hidden either
        reply = s.exchange(callLine(5, u"recent_files"_s, {{u"folder"_s, f.root}}));
        QVERIFY(reply);
        QCOMPARE(names(outcomeOf(*reply).structured), QStringList {u"open.txt"_s});
        reply = s.exchange(callLine(6, u"get_file_info"_s, {{u"path"_s, f.root + u"\\secret\\hidden.txt"_s}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY(!out.isError);
        QVERIFY(!out.structured.value(u"exists"_s).toBool());
        reply = s.exchange(callLine(7, u"get_file_info"_s, {{u"path"_s, f.root + u"\\secret"_s}}));
        QVERIFY(reply);
        QVERIFY(!outcomeOf(*reply).structured.value(u"exists"_s).toBool());
        reply = s.exchange(callLine(8, u"get_file_info"_s, {{u"path"_s, f.root + u"\\open.txt"_s}}));
        QVERIFY(reply);
        QVERIFY(outcomeOf(*reply).structured.value(u"exists"_s).toBool());
        reply = s.exchange(callLine(9, u"search_content"_s, {{u"query"_s, u"SECRETWORD"_s}}), 20000);
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(names(out.structured), QStringList {u"open.txt"_s});
        reply = s.exchange(callLine(10, u"open_item"_s, {{u"path"_s, f.root + u"\\secret\\hidden.txt"_s}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"no such file"), qPrintable(out.text));
        QVERIFY(f.opened.isEmpty());
        reply = s.exchange(callLine(11, u"open_item"_s, {{u"path"_s, f.root + u"\\open.txt"_s}}));
        QVERIFY(reply && !outcomeOf(*reply).isError);
        QCOMPARE(f.opened.size(), 1);
        // The checks are counted in the log.
        QVERIFY2(f.log.last().contains(u"access checks"), qPrintable(f.log.last()));

        // Let go of the restriction: everything is back.
        f.unrestrict();
        reply = s.exchange(callLine(12, u"search_files"_s, {{u"query"_s, u"ext:txt"_s}}));
        QVERIFY(reply);
        QCOMPARE(names(outcomeOf(*reply).structured).size(), 3);
    }

    void cancelAndQueue()
    {
        Fixture f;
        f.write(u"q.txt"_s, "q", 1'700'000'000);
        Session s(f.server.get());
        QVERIFY(s.initialize());
        // Two searches at once: the second waits; cancelled, it never answers.
        const qsizetype before = s.received.size();
        f.server->receive(s.id, callLine(2, u"search_files"_s, {{u"query"_s, u"q"_s}}));
        f.server->receive(s.id, callLine(3, u"search_files"_s, {{u"query"_s, u"q"_s}}));
        f.server->receive(s.id, jsonrpc::notification(u"notifications/cancelled"_s, {{u"requestId"_s, 3}}));
        QTRY_VERIFY(s.received.size() > before);
        QTest::qWait(300);
        QCOMPARE(s.received.size(), before + 1);
        QCOMPARE(parseObject(s.received.last()).value(u"id"_s).toInt(), 2);
        QCOMPARE(f.host->pendingCalls(), 0);

        // Three in a row all answer, in order.
        const qsizetype start = s.received.size();
        for (int i = 4; i <= 6; ++i)
            f.server->receive(s.id, callLine(i, u"search_files"_s, {{u"query"_s, u"q"_s}}));
        QTRY_COMPARE(s.received.size(), start + 3);
        for (int i = 0; i < 3; ++i)
            QCOMPARE(parseObject(s.received.at(start + i)).value(u"id"_s).toInt(), 4 + i);

        // A session that ends takes its calls with it.
        {
            Session other(f.server.get());
            QVERIFY(other.initialize());
            f.server->receive(other.id, callLine(7, u"search_files"_s, {{u"query"_s, u"q"_s}}));
        }
        QTest::qWait(200);
        QCOMPARE(f.host->pendingCalls(), 0);
    }

    void pipeEndToEnd()
    {
        Fixture f;
        f.write(u"管道.txt"_s, "p", 1'700'000'000);
        const std::wstring name = testPipeName("pipe");
        DWORD error = 0;
        QVERIFY2(f.server->listen(name, &error), qPrintable(QString::number(error)));
        // The same name again is refused: it is taken.
        {
            McpServer other(f.host.get(), u"x"_s);
            QVERIFY(!other.listen(name, &error));
            QCOMPARE(error, DWORD {ERROR_ACCESS_DENIED});
        }

        pipe::Client client;
        QVERIFY(client.connect(name, 2000, &error));
        QTRY_COMPARE(f.server->sessionCount(), 1);
        QVERIFY(client.write(requestLine(1, u"initialize"_s, {{u"protocolVersion"_s, u"2025-11-25"_s}, {u"clientInfo"_s, QJsonObject {{u"name"_s, u"pipe-test"_s}}}})));
        QByteArray line;
        // Reading blocks this thread; the server answers on it. So read on another.
        const auto readOne = [&](QByteArray& out) {
            std::optional<QByteArray> got;
            std::thread reader([&] {
                QByteArray l;
                if (client.readLine(l))
                    got = l;
            });
            QElapsedTimer timer;
            timer.start();
            while (!got && timer.elapsed() < 5000)
                QTest::qWait(5);
            reader.join();
            if (!got)
                return false;
            out = *got;
            return true;
        };
        QVERIFY(readOne(line));
        QCOMPARE(parseObject(line).value(u"result"_s).toObject().value(u"protocolVersion"_s).toString(), u"2025-11-25"_s);
        QVERIFY(client.write(jsonrpc::notification(u"notifications/initialized"_s)));
        QVERIFY(client.write(callLine(2, u"search_files"_s, {{u"query"_s, u"管道"_s}})));
        QVERIFY(readOne(line));
        QCOMPARE(names(outcomeOf(line).structured), QStringList {u"管道.txt"_s});
        QCOMPARE(f.server->sessions().front().client, u"pipe-test"_s);
        // A changed policy is announced.
        Policy policy = f.host->policy();
        policy.allowOpen = true;
        f.host->setPolicy(policy);
        QVERIFY(readOne(line));
        QCOMPARE(parseObject(line).value(u"method"_s).toString(), u"notifications/tools/list_changed"_s);
        client.close();
        QTRY_COMPARE(f.server->sessionCount(), 0);
        f.server->stop();
    }

    void relayOnline()
    {
        Fixture f;
        f.write(u"中文 路径.txt"_s, "r", 1'700'000'000);
        const std::wstring name = testPipeName("relay");
        QVERIFY(f.server->listen(name));
        Relay relay;
        QVERIFY2(relay.start(name), WINSHUN_MCP_RELAY);

        auto reply = relay.exchange(requestLine(1, u"initialize"_s,
            {{u"protocolVersion"_s, u"2025-06-18"_s}, {u"clientInfo"_s, QJsonObject {{u"name"_s, u"relay-test"_s}, {u"version"_s, u"1"_s}}}}));
        QVERIFY(reply);
        QCOMPARE(parseObject(*reply).value(u"result"_s).toObject().value(u"protocolVersion"_s).toString(), u"2025-06-18"_s);
        relay.send(jsonrpc::notification(u"notifications/initialized"_s));
        QTRY_COMPARE(f.server->sessionCount(), 1);
        QTRY_COMPARE(f.server->sessions().front().client, u"relay-test"_s);

        reply = relay.exchange(requestLine(2, u"tools/list"_s));
        QVERIFY(reply);
        QCOMPARE(parseObject(*reply).value(u"result"_s).toObject().value(u"tools"_s).toArray().size(), 7);
        // The session has the relay's own token (ImpersonateNamedPipeClient);
        // that what its user cannot list stays out of the results is
        // relayRestricted()'s check.
        QVERIFY(f.server->sessions().front().identified);
        reply = relay.exchange(callLine(3, u"search_files"_s, {{u"query"_s, u"中文"_s}}));
        QVERIFY(reply);
        Outcome out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(names(out.structured), QStringList {u"中文 路径.txt"_s});
        reply = relay.exchange(requestLine(4, u"ping"_s));
        QVERIFY(reply && parseObject(*reply).contains(u"result"_s));

        // Win顺 goes away: the relay answers for it, then finds it again.
        f.server->stop();
        QTRY_COMPARE(f.server->sessionCount(), 0);
        reply = relay.exchange(callLine(5, u"search_files"_s, {{u"query"_s, u"中文"_s}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY(out.isError);
        // "not running", or, with a Win顺 running on this PC that does not
        // serve the pipe (an older one, or its AI connection off), that.
        QVERIFY2(out.text.contains(u"not running") || out.text.contains(u"AI connection is off"), qPrintable(out.text));
        reply = relay.exchange(callLine(6, u"index_status"_s));
        QVERIFY(reply);
        QVERIFY(!outcomeOf(*reply).structured.value(u"connected"_s).toBool()); // running: whatever this PC has
        reply = relay.exchange(requestLine(7, u"tools/list"_s));
        QVERIFY(reply);
        QVERIFY(parseObject(*reply).value(u"result"_s).toObject().value(u"tools"_s).toArray().size() >= 1); // cached, or index_status alone

        QVERIFY(f.server->listen(name));
        reply = relay.exchange(callLine(8, u"search_files"_s, {{u"query"_s, u"中文"_s}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(names(out.structured), QStringList {u"中文 路径.txt"_s});
        QTRY_COMPARE(f.server->sessions().front().client, u"relay-test"_s); // initialized again by the relay

        relay.finish();
        QCOMPARE(relay.exitCode, DWORD {0});
        QTRY_COMPARE(f.server->sessionCount(), 0);
        f.server->stop();
    }

    // The session has the relay's token (ImpersonateNamedPipeClient): what
    // the relay's user cannot list is not among the results. The relay is
    // started here with this process's token, as an AI client would start
    // it with the user's; in an elevated test run it inherits the
    // administrator's, which sees the restricted folder, so the check
    // means nothing there and is skipped (the real relay is never elevated:
    // AI clients run as the user).
    void relayRestricted()
    {
        if (elevated())
            QSKIP("elevated: the relay inherits the administrator token and sees the restricted folder too");
        Fixture f;
        f.write(u"中文 路径.txt"_s, "r", 1'700'000'000);
        f.write(u"secret\\中文 hidden.txt"_s, "s", 1'700'000'100);
        QVERIFY(f.restrict(u"secret"_s)); // administrators alone: not for the relay's user
        const std::wstring name = testPipeName("restricted");
        QVERIFY(f.server->listen(name));
        Relay relay;
        QVERIFY2(relay.start(name), WINSHUN_MCP_RELAY);
        auto reply = relay.exchange(requestLine(1, u"initialize"_s,
            {{u"protocolVersion"_s, u"2025-06-18"_s}, {u"clientInfo"_s, QJsonObject {{u"name"_s, u"relay-test"_s}, {u"version"_s, u"1"_s}}}}));
        QVERIFY(reply);
        relay.send(jsonrpc::notification(u"notifications/initialized"_s));
        QTRY_COMPARE(f.server->sessionCount(), 1);
        QVERIFY(f.server->sessions().front().identified);
        reply = relay.exchange(callLine(2, u"search_files"_s, {{u"query"_s, u"中文"_s}}));
        QVERIFY(reply);
        const Outcome out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(names(out.structured), QStringList {u"中文 路径.txt"_s});
        relay.finish();
        QCOMPARE(relay.exitCode, DWORD {0});
        QTRY_COMPARE(f.server->sessionCount(), 0);
        f.server->stop();
    }

    void relayOffline()
    {
        Relay relay;
        QVERIFY(relay.start(testPipeName("nobody")));
        auto reply = relay.exchange(requestLine(1, u"initialize"_s, {{u"protocolVersion"_s, u"2025-11-25"_s}}));
        QVERIFY(reply);
        const QJsonObject result = parseObject(*reply).value(u"result"_s).toObject();
        QCOMPARE(result.value(u"protocolVersion"_s).toString(), u"2025-11-25"_s);
        QVERIFY(result.value(u"instructions"_s).toString().contains(u"not running"));
        relay.send(jsonrpc::notification(u"notifications/initialized"_s));
        reply = relay.exchange(requestLine(2, u"tools/list"_s));
        QVERIFY(reply);
        bool hasStatus = false;
        for (const QJsonValue& tool : parseObject(*reply).value(u"result"_s).toObject().value(u"tools"_s).toArray())
            hasStatus = hasStatus || tool.toObject().value(u"name"_s).toString() == u"index_status";
        QVERIFY(hasStatus);
        reply = relay.exchange(callLine(3, u"search_files"_s, {{u"query"_s, u"x"_s}}));
        QVERIFY(reply);
        const Outcome out = outcomeOf(*reply);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"Win顺") && (out.text.contains(u"not running") || out.text.contains(u"AI connection is off")),
            qPrintable(out.text));
        reply = relay.exchange("garbage\n");
        QVERIFY(reply);
        QCOMPARE(parseObject(*reply).value(u"error"_s).toObject().value(u"code"_s).toInt(), jsonrpc::kParseError);
        reply = relay.exchange(requestLine(4, u"something/else"_s));
        QVERIFY(reply);
        QCOMPARE(parseObject(*reply).value(u"error"_s).toObject().value(u"code"_s).toInt(), jsonrpc::kMethodNotFound);
        relay.finish();
        QCOMPARE(relay.exitCode, DWORD {0});
    }

    // The plugins' commands as tools (plugin::AiBridge): none while the
    // switch is off; queries once it is on, from a zero-code plugin and from
    // a resident process through the runner; actions only with their own
    // switch; a plugin turned off is known but off, with words that name it.
    void pluginTools()
    {
        Fixture f;
        QTemporaryDir dir;
        plugin::Host::Options options;
        options.userPluginDir = dir.path() + u"/plugins"_s;
        options.dataRoot = dir.path() + u"/data"_s;
        options.runnerExe = QString::fromUtf8(WINSHUN_PLUGIN_HOST);
        options.version = u"0.0-test"_s;
        plugin::Host host(options);
        const auto install = [&](const QString& id, const QByteArray& manifest, bool fake) {
            const QString folder = options.userPluginDir + u'/' + id;
            QDir().mkpath(folder);
            if (fake)
                QVERIFY(QFile::copy(QString::fromUtf8(WINSHUN_PLUGIN_FAKE), folder + u"/fake.exe"_s));
            QFile file(folder + u"/plugin.json"_s);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(manifest);
        };
        // (Addresses spelt with /: a raw string with // in it is a comment to moc.)
        install(u"zero.items"_s,
            R"({"manifest":1,"id":"zero.items","name":"Zero","version":"1.0.0","ai":{"expose":true},
                "commands":[{"id":"list","keyword":"zi","title":"List","run":{"items":[
                  {"id":"a","title":"Alpha","subtitle":"first"},
                  {"id":"b","title":"Beta","subtitle":"second","open":{"url":"https://example.com/b"}}]}}]})",
            false);
        install(u"fake.serve"_s,
            R"({"manifest":1,"id":"fake.serve","name":"Fake","version":"1.0.0","ai":{"expose":true},
                "runtime":{"mode":"resident","program":"fake.exe","args":["--serve"]},
                "commands":[{"id":"main","keyword":"fk","title":"Main","description":"Echoes the words",
                  "actions":[{"id":"open","title":"Open","key":"Enter"},{"id":"do","title":"Do","key":"Ctrl+Enter"}]}]})",
            true);
        install(u"no.ai"_s,
            R"({"manifest":1,"id":"no.ai","name":"Private","version":"1.0.0","commands":[{"id":"main","keyword":"na","run":{"url":"https://example.com/?q=%s"}}]})",
            false);
        host.catalog().scan();
        for (const QString& id : {u"zero.items"_s, u"fake.serve"_s, u"no.ai"_s})
            host.setEnabled(id, true);
        plugin::AiBridge bridge(&host, f.host.get());

        Session s(f.server.get());
        QVERIFY(s.initialize());
        const auto toolNames = [&] {
            QStringList names;
            const auto reply = s.exchange(requestLine(2, u"tools/list"_s));
            if (!reply)
                return names;
            for (const QJsonValue& tool : parseObject(*reply).value(u"result"_s).toObject().value(u"tools"_s).toArray())
                names.push_back(tool.toObject().value(u"name"_s).toString());
            return names;
        };
        const auto pluginToolsOf = [](const QStringList& names) { return names.filter(u"plugin_"_s); };

        // Off: nothing of the plugins'.
        QVERIFY(pluginToolsOf(toolNames()).isEmpty());
        QCOMPARE(f.host->known(u"plugin_zero_items_list"_s), ToolHost::Known::No);

        // On: a tool per command of the plugins that expose themselves, queries only.
        QSignalSpy changed(f.host.get(), &ToolHost::toolsChanged);
        plugin::AiBridge::Options opts;
        opts.enabled = true;
        bridge.setOptions(opts);
        QCOMPARE(changed.count(), 1);
        QVERIFY(std::any_of(s.received.cbegin(), s.received.cend(), [](const QByteArray& line) { return line.contains("notifications/tools/list_changed"); }));
        QStringList names = pluginToolsOf(toolNames());
        names.sort();
        QCOMPARE(names, (QStringList {u"plugin_fake_serve_main"_s, u"plugin_zero_items_list"_s}));
        {
            const auto reply = s.exchange(requestLine(3, u"tools/list"_s));
            QVERIFY(reply);
            for (const QJsonValue& tool : parseObject(*reply).value(u"result"_s).toObject().value(u"tools"_s).toArray()) {
                const QJsonObject t = tool.toObject();
                if (!t.value(u"name"_s).toString().startsWith(u"plugin_"))
                    continue;
                QVERIFY(t.value(u"annotations"_s).toObject().value(u"readOnlyHint"_s).toBool());
                QVERIFY(t.value(u"inputSchema"_s).toObject().value(u"properties"_s).toObject().contains(u"query"_s));
                QVERIFY(t.value(u"description"_s).toString().contains(u"Win顺"));
            }
        }

        // A zero-code command answers in place; a resident one through its process.
        auto reply = s.exchange(callLine(10, u"plugin_zero_items_list"_s, {{u"query"_s, u"beta"_s}}));
        QVERIFY(reply);
        Outcome out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QCOMPARE(out.structured.value(u"returned"_s).toInt(), 1);
        QCOMPARE(out.structured.value(u"items"_s).toArray().first().toObject().value(u"title"_s).toString(), u"Beta"_s);
        QCOMPARE(out.structured.value(u"items"_s).toArray().first().toObject().value(u"url"_s).toString(), u"https://example.com/b"_s);
        reply = s.exchange(callLine(11, u"plugin_fake_serve_main"_s, {{u"query"_s, u"hi 你好"_s}}), 20000);
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        const QJsonObject served = out.structured.value(u"items"_s).toArray().first().toObject();
        QCOMPARE(served.value(u"title"_s).toString(), u"serve: hi 你好"_s);
        QVERIFY(!served.contains(u"data"_s)); // the plugin's own, never the model's
        QVERIFY2(f.log.last().contains(u"plugin_fake_serve_main") && f.log.last().contains(u"1 items"), qPrintable(f.log.last()));

        // Actions: not until their switch; then a _run tool for commands with a process (never zero-code).
        QCOMPARE(f.host->known(u"plugin_fake_serve_main_run"_s), ToolHost::Known::No);
        opts.actions = true;
        bridge.setOptions(opts);
        names = pluginToolsOf(toolNames());
        QVERIFY(names.contains(u"plugin_fake_serve_main_run"_s));
        QVERIFY(!names.contains(u"plugin_zero_items_list_run"_s));
        reply = s.exchange(callLine(12, u"plugin_fake_serve_main_run"_s, {{u"query"_s, u"hi"_s}, {u"item_id"_s, u"nothing"_s}}), 20000);
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY2(out.isError && out.text.contains(u"No row with id"), qPrintable(out.text));
        reply = s.exchange(callLine(13, u"plugin_fake_serve_main_run"_s, {{u"query"_s, u"hi"_s}, {u"item_id"_s, u"q-hi"_s}, {u"action"_s, u"do"_s}}), 20000);
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY2(!out.isError, qPrintable(out.text));
        QVERIFY(out.structured.value(u"done"_s).toBool());
        QVERIFY2(out.structured.value(u"message"_s).toString() == u"did do on serve: hi"_s, qPrintable(out.structured.value(u"message"_s).toString()));

        // A plugin turned off: its tools known but off, the words naming it.
        host.setEnabled(u"fake.serve"_s, false);
        names = pluginToolsOf(toolNames());
        QVERIFY(!names.contains(u"plugin_fake_serve_main"_s));
        QCOMPARE(f.host->known(u"plugin_fake_serve_main"_s), ToolHost::Known::Off);
        reply = s.exchange(callLine(14, u"plugin_fake_serve_main"_s, {{u"query"_s, u"hi"_s}}));
        QVERIFY(reply);
        out = outcomeOf(*reply);
        QVERIFY(out.isError);
        QVERIFY2(out.text.contains(u"'Fake'") && out.text.contains(u"turned off"), qPrintable(out.text));

        // The card's switch: a plugin kept from the AI is not even known.
        opts.excluded.insert(u"zero.items"_s);
        bridge.setOptions(opts);
        QVERIFY(pluginToolsOf(toolNames()).isEmpty());
        QCOMPARE(f.host->known(u"plugin_zero_items_list"_s), ToolHost::Known::No);

        // The switch off again: nothing was on offer any more, so no news.
        const qsizetype before = changed.count();
        bridge.setOptions({});
        QCOMPARE(changed.count(), before);
        host.shutdown();
    }
};

QTEST_GUILESS_MAIN(TestMcp)
#include "tst_mcp.moc"
