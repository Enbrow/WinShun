#include "Embedder.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace semantic {

namespace {

constexpr int kQueryTimeoutMs = 8000;
constexpr int kBatchTimeoutMs = 60000; // a local model on the CPU takes its time

struct Preset {
    const char* id;
    const char* zh;
    const char* en;
    const char* endpoint;
    const char* model;
    bool local;
    int batch;
};
constexpr Preset kPresets[] = {
    {"ollama", "Ollama（本机）", "Ollama (this PC)", "http://127.0.0.1:11434/v1", "qwen3-embedding:0.6b", true, 10},
    {"lmstudio", "LM Studio（本机）", "LM Studio (this PC)", "http://127.0.0.1:1234/v1", "", true, 10},
    {"siliconflow", "硅基流动", "SiliconFlow", "https://api.siliconflow.cn/v1", "BAAI/bge-m3", false, 10},
    {"bailian", "阿里云百炼", "Alibaba Cloud Model Studio", "https://dashscope.aliyuncs.com/compatible-mode/v1", "text-embedding-v4", false, 10},
    {"openai", "OpenAI", "OpenAI", "https://api.openai.com/v1", "text-embedding-3-small", false, 32},
    {"custom", "OpenAI 兼容接口", "OpenAI-compatible", "", "", false, 10},
};

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// What the model families want in front of a query and of a passage.
void prefixesFor(Provider& p)
{
    const std::string m = lower(p.model);
    if (m.find("qwen3-embedding") != std::string::npos) {
        p.queryPrefix = "Instruct: Given a description of a file, retrieve passages of the files it describes\nQuery: ";
    } else if (m.find("embeddinggemma") != std::string::npos) {
        p.queryPrefix = "task: search result | query: ";
        p.docPrefix = "title: none | text: ";
    } else if (m.find("e5") != std::string::npos && m.find("multilingual") != std::string::npos) {
        p.queryPrefix = "query: ";
        p.docPrefix = "passage: ";
    } else if (m.find("bge") != std::string::npos && m.find("zh") != std::string::npos) {
        p.queryPrefix = "为这个句子生成表示以用于检索相关文章：";
    }
}

struct Url {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
};

std::optional<Url> crack(const std::string& text)
{
    const std::wstring wide = winshun::wide(text);
    URL_COMPONENTS parts {};
    parts.dwStructSize = sizeof parts;
    wchar_t host[256] {};
    wchar_t path[2048] {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    if (!::WinHttpCrackUrl(wide.c_str(), 0, 0, &parts))
        return std::nullopt;
    Url url;
    url.host.assign(host, parts.dwHostNameLength);
    url.path.assign(path, parts.dwUrlPathLength);
    url.port = parts.nPort;
    url.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    return url;
}

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET handle) : h(handle) {}
    ~Handle()
    {
        if (h)
            ::WinHttpCloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

struct Response {
    DWORD status = 0;
    std::string body;
    DWORD error = 0; // WinHTTP's, when no answer came
};

HINTERNET session()
{
    // The system's proxy settings (WPAD and the user's), as a browser would.
    static Handle s(::WinHttpOpen(L"WinShun-Semantic/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0));
    return s.h;
}

Response post(const Url& url, const std::string& body, const std::string& key, int timeoutMs)
{
    Response r;
    if (!session()) {
        r.error = ::GetLastError();
        return r;
    }
    Handle connection(::WinHttpConnect(session(), url.host.c_str(), url.port, 0));
    if (!connection) {
        r.error = ::GetLastError();
        return r;
    }
    Handle request(::WinHttpOpenRequest(connection.h, L"POST", url.path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, url.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) {
        r.error = ::GetLastError();
        return r;
    }
    ::WinHttpSetTimeouts(request.h, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!key.empty())
        headers += L"Authorization: Bearer " + winshun::wide(key) + L"\r\n";
    if (!::WinHttpSendRequest(request.h, headers.c_str(), static_cast<DWORD>(headers.size()), const_cast<char*>(body.data()),
            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0)
        || !::WinHttpReceiveResponse(request.h, nullptr)) {
        r.error = ::GetLastError();
        return r;
    }
    DWORD size = sizeof r.status;
    ::WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
        &r.status, &size, WINHTTP_NO_HEADER_INDEX);
    for (;;) {
        DWORD available = 0;
        if (!::WinHttpQueryDataAvailable(request.h, &available)) {
            r.error = ::GetLastError();
            return r;
        }
        if (available == 0)
            break;
        const std::size_t at = r.body.size();
        r.body.resize(at + available);
        DWORD read = 0;
        if (!::WinHttpReadData(request.h, r.body.data() + at, available, &read)) {
            r.error = ::GetLastError();
            return r;
        }
        r.body.resize(at + read);
        if (r.body.size() > (64u << 20))
            break; // no embedding answer is this big
    }
    return r;
}

std::string networkError(DWORD error, const Provider& p, bool chinese)
{
    switch (error) {
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
        if (p.local)
            return chinese ? "连不上 " + p.name + "：它在运行吗？" : "Cannot reach " + p.name + ": is it running?";
        return chinese ? "连不上 " + p.endpoint + "：检查网络或代理" : "Cannot reach " + p.endpoint + ": check the network or proxy";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        return chinese ? "找不到服务器：检查接口地址和网络" : "Server not found: check the endpoint and the network";
    case ERROR_WINHTTP_TIMEOUT:
        return chinese ? p.name + " 没有及时回答" : p.name + " did not answer in time";
    case ERROR_WINHTTP_SECURE_FAILURE:
        return chinese ? "HTTPS 证书有问题" : "The HTTPS certificate is not valid";
    default:
        return (chinese ? "网络错误 " : "Network error ") + std::to_string(error);
    }
}

// The service's own words, when it gave any.
std::string serviceMessage(const std::string& body)
{
    const auto json = winshun::Json::parse(body);
    if (!json)
        return body.substr(0, 200);
    const winshun::Json& error = (*json)["error"];
    if (error.isString())
        return error.toString();
    if (error["message"].isString())
        return error["message"].toString();
    if ((*json)["message"].isString())
        return (*json)["message"].toString();
    return body.substr(0, 200);
}

void normalize(std::vector<float>& v)
{
    double sum = 0;
    for (const float x : v)
        sum += static_cast<double>(x) * x;
    if (sum <= 0)
        return;
    const auto scale = static_cast<float>(1.0 / std::sqrt(sum));
    for (float& x : v)
        x *= scale;
}

} // namespace

std::string Provider::signature() const
{
    return id + "|" + endpoint + "|" + model + "|" + queryPrefix + "|" + docPrefix;
}

Provider providerFrom(const winshun::Json& settings, bool chinese)
{
    Provider p;
    p.id = settings["provider"].isString() ? settings["provider"].toString() : "none";
    for (const Preset& preset : kPresets) {
        if (p.id != preset.id)
            continue;
        p.name = chinese ? preset.zh : preset.en;
        p.endpoint = preset.endpoint;
        p.model = preset.model;
        p.local = preset.local;
        p.batch = preset.batch;
    }
    if (p.name.empty())
        p.id = "none";
    const std::string endpoint = winshun::trim(settings["endpoint"].toString());
    const std::string model = winshun::trim(settings["model"].toString());
    if (!endpoint.empty())
        p.endpoint = endpoint;
    if (!model.empty())
        p.model = model;
    while (!p.endpoint.empty() && p.endpoint.back() == '/')
        p.endpoint.pop_back();
    if (p.endpoint.size() > 11 && p.endpoint.ends_with("/embeddings"))
        p.endpoint.resize(p.endpoint.size() - 11);
    if (p.id == "custom") {
        // A server on this computer needs no key.
        const std::string e = lower(p.endpoint);
        p.local = e.find("://127.0.0.1") != std::string::npos || e.find("://localhost") != std::string::npos;
    }
    p.key = winshun::trim(settings["apiKey"].toString());
    prefixesFor(p);
    return p;
}

Embedded embed(const Provider& provider, const std::vector<std::string>& texts, bool query, bool chinese)
{
    Embedded out;
    const auto url = crack(provider.endpoint + "/embeddings");
    if (!url) {
        out.error = chinese ? "接口地址不对：" + provider.endpoint : "Not a valid endpoint: " + provider.endpoint;
        return out;
    }
    const std::string& prefix = query ? provider.queryPrefix : provider.docPrefix;
    for (std::size_t from = 0; from < texts.size(); from += static_cast<std::size_t>(provider.batch)) {
        const std::size_t to = std::min(texts.size(), from + static_cast<std::size_t>(provider.batch));
        winshun::Json::Array input;
        for (std::size_t i = from; i < to; ++i)
            input.push_back(prefix + texts[i]);
        const winshun::Json request = winshun::Json::Object {{"model", provider.model}, {"input", winshun::Json(std::move(input))},
            {"encoding_format", "float"}};
        const Response r = post(*url, request.dump(), provider.key, query ? kQueryTimeoutMs : kBatchTimeoutMs);
        if (r.error) {
            out.error = networkError(r.error, provider, chinese);
            out.retryLater = true;
            return out;
        }
        if (r.status != 200) {
            const std::string said = serviceMessage(r.body);
            if (r.status == 401 || r.status == 403)
                out.error = (chinese ? "密钥不对或没有权限：" : "The key was refused: ") + said;
            else if (r.status == 404)
                out.error = (chinese ? "接口地址或模型名不对：" : "Wrong endpoint or model: ") + said;
            else if (r.status == 429 || r.status >= 500)
                out.error = (chinese ? "服务忙，稍后再试：" : "The service is busy: ") + said;
            else
                out.error = (chinese ? "服务拒绝了：" : "The service refused: ") + said;
            out.retryLater = r.status == 429 || r.status >= 500;
            out.badInput = r.status == 400 || r.status == 413 || r.status == 422;
            return out;
        }
        const auto json = winshun::Json::parse(r.body);
        const winshun::Json::Array& data = json ? (*json)["data"].toArray() : winshun::Json::Array {};
        if (data.size() != to - from) {
            out.error = chinese ? "服务的回答看不懂（不是 OpenAI 兼容的 embeddings？）" : "Unexpected answer (not OpenAI-compatible embeddings?)";
            return out;
        }
        std::vector<std::vector<float>> batch(data.size());
        for (std::size_t i = 0; i < data.size(); ++i) {
            const std::size_t at = data[i]["index"].isNumber() ? static_cast<std::size_t>(data[i]["index"].toInt()) : i;
            const winshun::Json::Array& values = data[i]["embedding"].toArray();
            if (at >= batch.size() || values.empty()) {
                out.error = chinese ? "服务回答的向量不完整" : "The service's vectors are incomplete";
                return out;
            }
            std::vector<float>& v = batch[at];
            v.reserve(values.size());
            for (const winshun::Json& x : values)
                v.push_back(static_cast<float>(x.toNumber()));
            normalize(v);
        }
        for (auto& v : batch)
            out.vectors.push_back(std::move(v));
    }
    return out;
}

} // namespace semantic
