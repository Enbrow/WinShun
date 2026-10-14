#include "Settings.h"

#include "Unc.h"
#include "Win32Util.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

#include <windows.h>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

QString g_profile; // see Settings::useProfile()

// Never indexed: system stores and machine-generated caches nobody searches.
// Forward slashes keep the INI file readable; %VARS% are expanded.
const QStringList kDefaultExcludedPaths {
    u"%WINDIR%/WinSxS"_s,
    u"%WINDIR%/Installer"_s,
    u"%WINDIR%/servicing"_s,
    u"%WINDIR%/SoftwareDistribution"_s,
    u"%WINDIR%/assembly"_s,
    u"%WINDIR%/System32/DriverStore"_s,
    u"%WINDIR%/Prefetch"_s,
    u"%SystemDrive%/$WinREAgent"_s,
    u"%TEMP%"_s,
};

// A folder name at any depth, or a tail such as ".svn/pristine".
const QStringList kDefaultExcludedNames {
    u"$Recycle.Bin"_s,
    u"System Volume Information"_s,
    u"$SysReset"_s,
    u"Config.Msi"_s,
    u".git/objects"_s,
    u".svn/pristine"_s,
    // Browser / Electron caches
    u"Code Cache"_s,
    u"GPUCache"_s,
    u"CacheStorage"_s,
    u"ScriptCache"_s,
    u"DawnCache"_s,
    u"DawnGraphiteCache"_s,
    u"DawnWebGPUCache"_s,
    u"GrShaderCache"_s,
    u"ShaderCache"_s,
    u"Crashpad"_s,
    u"blob_storage"_s,
};

// Indexed, but ranked below your own files. Dot-folders (.git, .vscode, ...)
// are always ranked lower as well.
const QStringList kLowPriorityPaths {
    u"%WINDIR%"_s,
    u"%ProgramFiles%"_s,
    u"%ProgramFiles(x86)%"_s,
    u"%ProgramData%"_s,
    u"%SystemDrive%/Recovery"_s,
    u"%SystemDrive%/PerfLogs"_s,
};

const QStringList kLowPriorityNames {
    u"AppData"_s,
    u"node_modules"_s,
    u"__pycache__"_s,
    u"site-packages"_s,
    u"Cache"_s,
    u"Caches"_s,
    u"Temp"_s,
    u"Logs"_s,
    u"vendor"_s,
    u"bower_components"_s,
};

// Plain-text formats: the scanner reads their raw bytes. Documents (docx,
// xlsx, pdf...) are read by WinShunExtract.exe instead, under their own
// setting (Content/Documents). The first three name the scope in the
// launcher ("搜索 .txt / .md / .log 等文件中的文字").
const QStringList kDefaultContentExtensions {
    // Documents and logs
    u"txt"_s, u"md"_s, u"log"_s, u"csv"_s, u"tsv"_s, u"ini"_s, u"cfg"_s, u"conf"_s,
    // Source code and scripts
    u"c"_s, u"cpp"_s, u"h"_s, u"hpp"_s, u"py"_s, u"js"_s, u"ts"_s, u"java"_s, u"cs"_s, u"go"_s, u"rs"_s,
    u"qml"_s, u"cmake"_s, u"bat"_s, u"cmd"_s, u"ps1"_s, u"sh"_s, u"sql"_s,
    // Data and markup
    u"json"_s, u"xml"_s, u"yaml"_s, u"yml"_s, u"toml"_s, u"html"_s, u"htm"_s, u"css"_s, u"svg"_s, u"srt"_s,
};

// Password managers: most mark what they copy as not for clipboard history
// (see ClipboardWatcher), these are skipped in any case.
const QStringList kDefaultClipboardExcludedApps {
    u"KeePass.exe"_s,
    u"KeePassXC.exe"_s,
    u"1Password.exe"_s,
    u"Bitwarden.exe"_s,
};

// Folders on network shares that NAS boxes (Synology, QNAP) and Macs keep for
// themselves: recycle bins, thumbnails, snapshots, Spotlight. Shares only:
// [Index] ExcludedNames of earlier versions is in the file already, so its
// defaults growing would reach nobody.
const QStringList kDefaultNetworkExcludedNames {
    u"#recycle"_s,
    u"@eaDir"_s,
    u"@Recycle"_s,
    u".@__thumb"_s,
    u"@Recently-Snapshot"_s,
    u"#snapshot"_s,
    u".snapshot"_s,
    u".recycle"_s,
    u"$RECYCLE.BIN"_s,
    u"System Volume Information"_s,
    u".TemporaryItems"_s,
    u".Trashes"_s,
    u".Spotlight-V100"_s,
    u".fseventsd"_s,
};

// What a Mac leaves next to the files it writes to a share.
const QStringList kDefaultNetworkExcludedFiles {u"._*"_s, u".DS_Store"_s};

// Bumped when kDefaultContentExtensions grows, so lists that still hold an
// older default pick up the new one; 3: smaller default size limits.
constexpr int kContentDefaultsVersion = 3;
// The size limits before version 3, by ContentSizeLimits::Kind: one still
// at its old default takes the new one.
constexpr std::array<double, ContentSizeLimits::kKinds> kOldContentMaxSizeMB {64, 8, 16, 512};

// The size limits of the kinds of files, by ContentSizeLimits::Kind.
const QString kContentSizeKeys[ContentSizeLimits::kKinds] {u"Content/MaxFileSizeMB"_s, u"Content/MaxCodeFileSizeMB"_s,
    u"Content/MaxDataFileSizeMB"_s, u"Content/MaxDocumentFileSizeMB"_s};

std::wstring normalizeName(const QString& name)
{
    return QDir::toNativeSeparators(name.trimmed()).toStdWString();
}

std::wstring normalizePath(const QString& path)
{
    std::wstring p = win32::expandEnvironment(QDir::toNativeSeparators(path.trimmed()).toStdWString());
    while (p.size() > 3 && p.back() == L'\\')
        p.pop_back();
    // %TEMP% can be an 8.3 short path (C:\Users\JOHNDO~1\...); the crawler sees long names.
    if (const DWORD n = ::GetLongPathNameW(p.c_str(), nullptr, 0); n > 0) {
        std::wstring full(n, L'\0');
        if (const DWORD written = ::GetLongPathNameW(p.c_str(), full.data(), n); written > 0 && written < n) {
            full.resize(written);
            p = std::move(full);
        }
    }
    if (p.size() == 3 && p[1] == L':' && p[2] == L'\\')
        p.pop_back(); // "C:\" -> "C:"
    return p;
}

template <typename T> T readOrDefault(QSettings& s, const QString& key, const T& fallback)
{
    if (!s.contains(key)) {
        s.setValue(key, fallback);
        return fallback;
    }
    return s.value(key).value<T>();
}

void writeWebShortcuts(QSettings& s, const WebShortcuts& shortcuts)
{
    s.remove(u"WebSearch"_s); // no entries left over from a longer list
    s.beginWriteArray(u"WebSearch"_s, static_cast<int>(shortcuts.size()));
    for (int i = 0; i < shortcuts.size(); ++i) {
        s.setArrayIndex(i);
        s.setValue(u"Keyword"_s, shortcuts[i].keyword);
        s.setValue(u"Name"_s, shortcuts[i].name);
        s.setValue(u"Url"_s, shortcuts[i].url);
        s.setValue(u"Home"_s, shortcuts[i].home);
        if (!shortcuts[i].icon.isEmpty())
            s.setValue(u"Icon"_s, shortcuts[i].icon);
    }
    s.endArray();
    s.setValue(u"WebSearch/Defaults"_s, kWebDefaultsVersion); // the list as its user left it, new defaults seen
}

// The file may have been edited by hand: addresses are tidied as the settings
// window would ("github.com/..." gets https://), an entry without a name takes
// its keyword, and one that cannot be used (no keyword to type, an address
// Win顺 does not open) is left out.
WebShortcuts readWebShortcuts(QSettings& s, const WebShortcuts& fallback)
{
    if (!s.contains(u"WebSearch/size"_s)) { // never written: the defaults, and into the file with them
        writeWebShortcuts(s, fallback);
        return fallback;
    }
    WebShortcuts shortcuts;
    const int size = s.beginReadArray(u"WebSearch"_s);
    for (int i = 0; i < size; ++i) {
        s.setArrayIndex(i);
        WebShortcut shortcut;
        shortcut.keyword = s.value(u"Keyword"_s).toString().trimmed();
        shortcut.name = s.value(u"Name"_s).toString().trimmed();
        shortcut.url = normalizeWebUrl(s.value(u"Url"_s).toString());
        shortcut.home = normalizeWebUrl(s.value(u"Home"_s).toString());
        if (const QString icon = s.value(u"Icon"_s).toString(); isValidCustomIconName(icon))
            shortcut.icon = icon; // a name of ours, never a path
        if (!isValidWebKeyword(shortcut.keyword) || shortcut.url.isEmpty())
            continue; // nothing to type, or nothing to open
        if (shortcut.name.isEmpty())
            shortcut.name = shortcut.keyword;
        shortcuts.append(shortcut);
    }
    s.endArray();
    // Written by an older Win顺: the defaults added since, once.
    if (s.value(u"WebSearch/Defaults"_s, 1).toInt() < kWebDefaultsVersion) {
        shortcuts = withNewDefaults(shortcuts, fallback);
        writeWebShortcuts(s, shortcuts);
    }
    return shortcuts;
}

// The built-in file types: id, name (to translate), extensions. As
// Everything groups them, less what few people look for; 代码 last, the
// least asked for.
struct BuiltInFileType {
    const char* id;
    const char* name;
    const char* extensions;
};
constexpr BuiltInFileType kBuiltInFileTypes[] {
    {"documents", QT_TRANSLATE_NOOP("ws::Settings", "文档"),
        "doc docx docm dot dotx rtf odt wps wpd txt md pdf xls xlsx xlsm xlsb csv ods et ppt pptx pptm pps ppsx odp dps "
        "vsd vsdx one pages numbers key"},
    {"pictures", QT_TRANSLATE_NOOP("ws::Settings", "图片"),
        "jpg jpeg jfif png gif bmp webp heic heif avif tif tiff ico svg psd ai raw cr2 cr3 nef arw dng orf rw2"},
    {"videos", QT_TRANSLATE_NOOP("ws::Settings", "视频"),
        "mp4 mkv avi mov wmv flv webm m4v mpg mpeg ts m2ts 3gp rm rmvb vob"},
    {"audio", QT_TRANSLATE_NOOP("ws::Settings", "音频"),
        "mp3 wav flac aac m4a ogg opus wma ape aiff alac mid midi amr"},
    {"archives", QT_TRANSLATE_NOOP("ws::Settings", "压缩包"), "zip rar 7z tar gz tgz bz2 xz zst iso cab lzh arj"},
    {"programs", QT_TRANSLATE_NOOP("ws::Settings", "程序"), "exe msi msix appx bat cmd ps1 vbs com lnk"},
    {"ebooks", QT_TRANSLATE_NOOP("ws::Settings", "电子书"), "epub mobi azw azw3 fb2 djvu chm cbz cbr"},
    {"code", QT_TRANSLATE_NOOP("ws::Settings", "代码"),
        "c h cpp hpp cc cxx cs java kt py js mjs ts jsx tsx go rs swift rb php lua html htm css scss json xml yaml "
        "yml toml ini sql sh ps1 qml vue"},
};

void writeFileTypes(QSettings& s, const FileTypes& types)
{
    s.remove(u"FileTypes"_s); // no entries left over from a longer list
    s.beginWriteArray(u"FileTypes"_s, static_cast<int>(types.size()));
    for (int i = 0; i < types.size(); ++i) {
        s.setArrayIndex(i);
        if (!types[i].id.isEmpty())
            s.setValue(u"Id"_s, types[i].id);
        if (!types[i].name.isEmpty())
            s.setValue(u"Name"_s, types[i].name);
        s.setValue(u"Extensions"_s, types[i].extensions.join(u' '));
    }
    s.endArray();
}

// Edited by hand perhaps: extensions tidied (".JPG," is jpg), one with no
// name to show or no extension left out.
FileTypes readFileTypes(QSettings& s, const FileTypes& fallback)
{
    if (!s.contains(u"FileTypes/size"_s)) { // never written: the defaults, and into the file with them
        writeFileTypes(s, fallback);
        return fallback;
    }
    FileTypes types;
    const int size = s.beginReadArray(u"FileTypes"_s);
    for (int i = 0; i < size; ++i) {
        s.setArrayIndex(i);
        FileType type;
        type.id = s.value(u"Id"_s).toString().trimmed();
        type.name = s.value(u"Name"_s).toString().trimmed();
        type.extensions = Settings::fileExtensions(s.value(u"Extensions"_s).toString());
        const bool builtIn = std::ranges::any_of(kBuiltInFileTypes, [&](const BuiltInFileType& b) { return type.id == QLatin1StringView(b.id); });
        if (!builtIn)
            type.id.clear();
        if ((type.name.isEmpty() && !builtIn) || type.extensions.isEmpty())
            continue;
        types.append(type);
    }
    s.endArray();
    return types;
}

void writeShares(QSettings& s, const QList<NetworkShare>& shares)
{
    s.remove(u"Shares"_s);
    s.beginWriteArray(u"Shares"_s, static_cast<int>(shares.size()));
    for (int i = 0; i < shares.size(); ++i) {
        s.setArrayIndex(i);
        s.setValue(u"Path"_s, shares[i].path);
        s.setValue(u"Name"_s, shares[i].name);
    }
    s.endArray();
}

// Paths written by hand are tidied (unc::normalize; a mapped drive becomes
// the share it maps); what is no share at all, or twice, is left out.
QList<NetworkShare> readShares(QSettings& s)
{
    if (!s.contains(u"Shares/size"_s)) {
        writeShares(s, {});
        return {};
    }
    QList<NetworkShare> shares;
    const int size = s.beginReadArray(u"Shares"_s);
    for (int i = 0; i < size; ++i) {
        s.setArrayIndex(i);
        const QString path = Settings::sharePath(s.value(u"Path"_s).toString());
        if (path.isEmpty()
            || std::ranges::any_of(shares, [&](const NetworkShare& n) { return n.path.compare(path, Qt::CaseInsensitive) == 0; }))
            continue;
        shares.append({path, s.value(u"Name"_s).toString().trimmed()});
    }
    s.endArray();
    return shares;
}

constexpr QStringView kPluginGroup = u"Plugin."; // [Plugin.<id>]

// [Plugin.<id>] sections, as they are: nothing is written back that was
// not there (no defaults), and nothing dropped that is.
QList<PluginConfig> readPlugins(QSettings& s)
{
    QList<PluginConfig> plugins;
    for (const QString& group : s.childGroups()) {
        if (!group.startsWith(kPluginGroup) || group.size() <= kPluginGroup.size())
            continue;
        PluginConfig config;
        config.id = group.mid(kPluginGroup.size());
        s.beginGroup(group);
        for (const QString& key : s.childKeys()) {
            const QString value = s.value(key).toString();
            if (key == u"Enabled")
                config.enabled = s.value(key).toBool();
            else if (key == u"Confirmed")
                config.confirmed = s.value(key).toStringList();
            else if (key == u"Ai")
                config.ai = s.value(key).toBool();
            else if (key.startsWith(u"Keyword."))
                config.keywords.append({key.mid(8), value.trimmed()});
            else if (key.startsWith(u"Setting."))
                config.settings.append({key.mid(8), value});
        }
        s.endGroup();
        plugins.append(std::move(config));
    }
    return plugins;
}

void writePlugins(QSettings& s, const QList<PluginConfig>& plugins)
{
    for (const QString& group : s.childGroups()) {
        if (group.startsWith(kPluginGroup))
            s.remove(group); // sections of plugins the list no longer has
    }
    for (const PluginConfig& config : plugins) {
        s.beginGroup(kPluginGroup + config.id);
        s.setValue(u"Enabled"_s, config.enabled);
        if (!config.confirmed.isEmpty())
            s.setValue(u"Confirmed"_s, config.confirmed);
        if (!config.ai)
            s.setValue(u"Ai"_s, false);
        for (const auto& [command, keyword] : config.keywords)
            s.setValue(u"Keyword."_s + command, keyword);
        for (const auto& [key, value] : config.settings)
            s.setValue(u"Setting."_s + key, value);
        s.endGroup();
    }
}

} // namespace

const PluginConfig* Settings::plugin(QStringView id) const
{
    const auto it = std::find_if(plugins.cbegin(), plugins.cend(), [&](const PluginConfig& p) { return p.id == id; });
    return it == plugins.cend() ? nullptr : &*it;
}

PluginConfig& Settings::pluginConfig(const QString& id)
{
    const auto it = std::find_if(plugins.begin(), plugins.end(), [&](const PluginConfig& p) { return p.id == id; });
    if (it != plugins.end())
        return *it;
    PluginConfig config;
    config.id = id;
    plugins.append(std::move(config));
    return plugins.last();
}

QString Settings::sharePath(const QString& text)
{
    std::wstring path = QDir::toNativeSeparators(text.trimmed()).toStdWString();
    if (path.size() >= 2 && path[1] == L':')
        path = unc::fromMappedDrive(path);
    return QString::fromStdWString(unc::normalize(path));
}

QString Settings::filePath()
{
    return QSettings(QSettings::IniFormat, QSettings::UserScope, u"WinShun"_s, u"WinShun"_s).fileName();
}

QString Settings::storedRenderer()
{
    QSettings s(QSettings::IniFormat, QSettings::UserScope, u"WinShun"_s, u"WinShun"_s);
    return resolveRenderer(s.value(u"Launcher/Renderer"_s).toString().trimmed().toLower());
}

QString Settings::resolveRenderer(const QString& renderer)
{
    if (renderer == u"software" || renderer == u"d3d11")
        return renderer;
    return defaultRenderer();
}

QString Settings::defaultRenderer()
{
    // Only machines with little memory save the ~50 MB that D3D11 costs. The
    // RAM fitted, not what Windows has left of it: an 8 GB PC reports ~7.8 GB.
    ULONGLONG installedKB = 0;
    if (!::GetPhysicallyInstalledSystemMemory(&installedKB)) {
        MEMORYSTATUSEX status {sizeof(status)};
        if (::GlobalMemoryStatusEx(&status))
            installedKB = (status.ullTotalPhys >> 10) + (512ull << 10); // round up what the firmware keeps
    }
    return installedKB >= 8ull << 20 ? u"d3d11"_s : u"software"_s;
}

bool Settings::lowMemory()
{
    MEMORYSTATUSEX status {sizeof(status)};
    return ::GlobalMemoryStatusEx(&status) && status.ullTotalPhys <= 16ull << 30;
}

void Settings::useProfile(const QString& folder)
{
    g_profile = QDir::toNativeSeparators(QDir(folder).absolutePath());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, g_profile); // <folder>\WinShun\WinShun.ini
}

QString Settings::profile()
{
    return g_profile;
}

QString Settings::portableFolder()
{
    wchar_t exe[MAX_PATH * 2] = {};
    const DWORD length = ::GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (length == 0 || length >= std::size(exe))
        return {};
    const QString folder = QFileInfo(QString::fromWCharArray(exe, static_cast<qsizetype>(length))).absolutePath()
        + u"/data"_s;
    return QFileInfo(folder).isDir() ? QDir::toNativeSeparators(folder) : QString();
}

QString Settings::dataDir()
{
    if (!g_profile.isEmpty())
        return g_profile;
    return QDir::toNativeSeparators(
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/WinShun"_s);
}

QString Settings::indexDir() const
{
    if (indexFolder.isEmpty())
        return dataDir();
    const std::wstring expanded = win32::expandEnvironment(indexFolder.toStdWString());
    return QDir::toNativeSeparators(QDir::cleanPath(QString::fromStdWString(expanded)));
}

QString Settings::resolveLanguage(const QString& language)
{
    if (language == u"zh" || language == u"en")
        return language;
    // The language Windows itself is shown in. Not QLocale::uiLanguages(): that
    // follows the preferred-languages list, which can put English first on a
    // Chinese Windows.
    return PRIMARYLANGID(::GetUserDefaultUILanguage()) == LANG_CHINESE ? u"zh"_s : u"en"_s;
}

FileTypes Settings::defaultFileTypes()
{
    FileTypes types;
    for (const BuiltInFileType& b : kBuiltInFileTypes)
        types.append({{}, QString::fromLatin1(b.extensions).split(u' ', Qt::SkipEmptyParts), QString::fromLatin1(b.id)});
    return types;
}

QStringList Settings::fileExtensions(const QString& text)
{
    static const QRegularExpression separators(u"[\\s,;，；、]+"_s);
    QStringList extensions;
    for (QString ext : text.split(separators, Qt::SkipEmptyParts)) {
        while (ext.startsWith(u'*') || ext.startsWith(u'.'))
            ext.remove(0, 1);
        ext = ext.sliced(ext.lastIndexOf(u'.') + 1).toLower(); // "tar.gz": what comes after the last dot
        // What no name ends in (a path, a pattern) is no extension.
        if (ext.isEmpty() || ext.contains(u'\\') || ext.contains(u'/') || ext.contains(u':') || ext.contains(u'*')
            || ext.contains(u'?') || extensions.contains(ext))
            continue;
        extensions.append(ext);
    }
    return extensions;
}

FileTypes Settings::namedFileTypes() const
{
    FileTypes types = fileTypes;
    for (FileType& type : types) {
        if (!type.name.isEmpty())
            continue;
        for (const BuiltInFileType& b : kBuiltInFileTypes) {
            if (type.id == QLatin1StringView(b.id))
                type.name = QCoreApplication::translate("ws::Settings", b.name);
        }
    }
    return types;
}

Settings Settings::defaults()
{
    Settings d;
    d.excludedPaths = kDefaultExcludedPaths;
    d.excludedNames = kDefaultExcludedNames;
    d.contentExtensions = kDefaultContentExtensions;
    d.clipboard = windowsClipboardHistory(); // whoever kept Windows' clipboard history keeps ours
    d.clipboardExcludedApps = kDefaultClipboardExcludedApps;
    d.webShortcuts = defaultWebShortcuts();
    d.fileTypes = defaultFileTypes();
    d.networkExcludedNames = kDefaultNetworkExcludedNames;
    d.networkExcludedFiles = kDefaultNetworkExcludedFiles;
    d.renderer = defaultRenderer();
    d.transparency = lowMemory() ? u"off"_s : u"on"_s;
    d.clipboardMaxItems = lowMemory() ? 100 : 200;
    return d;
}

bool Settings::hasClipboardSettings()
{
    QSettings s(QSettings::IniFormat, QSettings::UserScope, u"WinShun"_s, u"WinShun"_s);
    return s.contains(u"Clipboard/Enabled"_s);
}

bool Settings::windowsClipboardHistory()
{
    DWORD on = 0;
    DWORD size = sizeof on;
    return ::RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Clipboard", L"EnableClipboardHistory",
               RRF_RT_REG_DWORD, nullptr, &on, &size)
            == ERROR_SUCCESS
        && on != 0;
}

bool Settings::exists()
{
    return QFileInfo::exists(filePath());
}

void Settings::load()
{
    QSettings s(QSettings::IniFormat, QSettings::UserScope, u"WinShun"_s, u"WinShun"_s);
    const Settings d = defaults();

    doubleCtrl = readOrDefault(s, u"Launcher/DoubleCtrl"_s, d.doubleCtrl);
    doubleCtrlPauseInGames = readOrDefault(s, u"Launcher/DoubleCtrlPauseInGames"_s, d.doubleCtrlPauseInGames);
    doubleCtrlPauseInFullScreen
        = readOrDefault(s, u"Launcher/DoubleCtrlPauseInFullScreen"_s, d.doubleCtrlPauseInFullScreen);
    doubleCtrlExcludedApps = readOrDefault(s, u"Launcher/DoubleCtrlExcludedApps"_s, d.doubleCtrlExcludedApps);
    hotkey =readOrDefault(s, u"Launcher/Hotkey"_s, d.hotkey);
    renderer = resolveRenderer(readOrDefault(s, u"Launcher/Renderer"_s, d.renderer).trimmed().toLower());
    recordHistory = readOrDefault(s, u"Launcher/History"_s, d.recordHistory);
    dialogJump = readOrDefault(s, u"Launcher/DialogJump"_s, d.dialogJump);
    dialogBar = readOrDefault(s, u"Launcher/DialogBar"_s, d.dialogBar);
    dialogBarPlace = readOrDefault(s, u"Launcher/DialogBarPlace"_s, d.dialogBarPlace).trimmed().toLower();
    if (dialogBarPlace != u"below" && dialogBarPlace != u"left" && dialogBarPlace != u"right")
        dialogBarPlace = d.dialogBarPlace;
    dialogAutoJump = readOrDefault(s, u"Launcher/DialogAutoJump"_s, d.dialogAutoJump);
    dialogAutoJumpWhen = readOrDefault(s, u"Launcher/DialogAutoJumpWhen"_s, d.dialogAutoJumpWhen).trimmed().toLower();
    if (dialogAutoJumpWhen != u"back")
        dialogAutoJumpWhen = d.dialogAutoJumpWhen;
    // Turned on before there was the question: no need to ask.
    dialogAutoJumpAsked = readOrDefault(s, u"Launcher/DialogAutoJumpAsked"_s, dialogAutoJump);
    dialogBarExcludedApps = readOrDefault(s, u"Launcher/DialogBarExcludedApps"_s, d.dialogBarExcludedApps);
    fileManager = readOrDefault(s, u"Launcher/FileManager"_s, d.fileManager).trimmed().toLower();
    if (fileManager != u"totalcmd" && fileManager != u"dopus")
        fileManager = d.fileManager;

    foldersFirst = readOrDefault(s, u"Results/FoldersFirst"_s, d.foldersFirst);
    showModified = readOrDefault(s, u"Results/ShowModified"_s, d.showModified);
    if (!s.contains(u"Results/Sort"_s) && s.value(u"Results/SortByModified"_s).toBool())
        s.setValue(u"Results/Sort"_s, u"newest"_s); // 0.6.0's switch: newest first, or best match
    s.remove(u"Results/SortByModified"_s);
    sort = readOrDefault(s, u"Results/Sort"_s, d.sort).trimmed().toLower();
    if (sort != u"newest" && sort != u"oldest")
        sort = d.sort;
    startList = readOrDefault(s, u"Results/StartList"_s, d.startList).trimmed().toLower();
    if (startList != u"modified")
        startList = d.startList;
    numberKeys = readOrDefault(s, u"Results/NumberKeys"_s, d.numberKeys).trimmed().toLower();
    if (numberKeys != u"alt" && numberKeys != u"off")
        numberKeys = d.numberKeys;

    theme = readOrDefault(s, u"Appearance/Theme"_s, d.theme).trimmed().toLower();
    if (theme != u"light" && theme != u"dark")
        theme = d.theme;
    language = readOrDefault(s, u"Appearance/Language"_s, d.language).trimmed().toLower();
    if (language != u"zh" && language != u"en")
        language = d.language;
    transparency = readOrDefault(s, u"Appearance/Transparency"_s, d.transparency).trimmed().toLower();
    // It used to be a switch: "false" was turned off by hand, "true" was mostly
    // the default written back.
    taskbarIcon = readOrDefault(s, u"Appearance/TaskbarIcon"_s, d.taskbarIcon).trimmed().toLower();
    if (taskbarIcon != u"mono")
        taskbarIcon = d.taskbarIcon;
    uiSize = readOrDefault(s, u"Appearance/UiSize"_s, d.uiSize).trimmed().toLower();
    if (uiSize != u"compact" && uiSize != u"roomy")
        uiSize = d.uiSize;
    if (transparency == u"false" || transparency == u"true") {
        transparency = transparency == u"false" ? u"off"_s : d.transparency;
        s.setValue(u"Appearance/Transparency"_s, transparency);
    } else if (transparency != u"on" && transparency != u"off") { // also an old "auto"
        transparency = d.transparency;
    }

    autoUpdate = readOrDefault(s, u"Update/Automatic"_s, d.autoUpdate);
    backupFolder = readOrDefault(s, u"Backup/Folder"_s, d.backupFolder);

    excludedPaths = readOrDefault(s, u"Index/ExcludedPaths"_s, d.excludedPaths);
    excludedNames = readOrDefault(s, u"Index/ExcludedNames"_s, d.excludedNames);
    includeRemovableDrives = readOrDefault(s, u"Index/IncludeRemovableDrives"_s, d.includeRemovableDrives);
    rescanOnStartup = readOrDefault(s, u"Index/RescanOnStartup"_s, d.rescanOnStartup);
    indexFolder = readOrDefault(s, u"Index/Folder"_s, d.indexFolder).trimmed();

    contentSearch = readOrDefault(s, u"Content/Enabled"_s, d.contentSearch);
    contentExtensions = readOrDefault(s, u"Content/Extensions"_s, d.contentExtensions);
    const int contentDefaults = s.value(u"Content/DefaultsVersion"_s, 1).toInt();
    if (contentDefaults < kContentDefaultsVersion) {
        // Version 1 defaulted to just "txt". A list the user edited is left alone.
        if (contentExtensions == QStringList {u"txt"_s}) {
            contentExtensions = d.contentExtensions;
            s.setValue(u"Content/Extensions"_s, contentExtensions);
        }
        s.setValue(u"Content/DefaultsVersion"_s, kContentDefaultsVersion);
    }
    // MaxFileSizeMB once held for every kind of text file: a size set there
    // holds for code and data too, until they have their own.
    for (std::size_t k = 0; k < ContentSizeLimits::kKinds; ++k) {
        double fallback = d.contentMaxSizeMB[k];
        if ((k == ContentSizeLimits::Code || k == ContentSizeLimits::Data) && !s.contains(kContentSizeKeys[k])
            && contentMaxSizeMB[0] != d.contentMaxSizeMB[0])
            fallback = contentMaxSizeMB[0];
        double mb = readOrDefault(s, kContentSizeKeys[k], fallback);
        if (contentDefaults < 3 && mb == kOldContentMaxSizeMB[k]) { // never changed: the new default
            mb = d.contentMaxSizeMB[k];
            s.setValue(kContentSizeKeys[k], mb);
        }
        contentMaxSizeMB[k] = roundedSizeMB(mb);
    }
    contentIndex = readOrDefault(s, u"Content/Index"_s, d.contentIndex);
    contentInLowPriority = readOrDefault(s, u"Content/IncludeSystemFolders"_s, d.contentInLowPriority);
    contentDocuments = readOrDefault(s, u"Content/Documents"_s, d.contentDocuments);

    clipboard = readOrDefault(s, u"Clipboard/Enabled"_s, d.clipboard);
    clipboardWinV = readOrDefault(s, u"Clipboard/WinV"_s, d.clipboardWinV);
    clipboardHotkey = readOrDefault(s, u"Clipboard/Hotkey"_s, d.clipboardHotkey);
    clipboardMaxItems = std::clamp(readOrDefault(s, u"Clipboard/MaxItems"_s, d.clipboardMaxItems), 10, 300);
    clipboardMaxDays = std::clamp(readOrDefault(s, u"Clipboard/MaxDays"_s, d.clipboardMaxDays), 0, 3650);
    clipboardImages = readOrDefault(s, u"Clipboard/Images"_s, d.clipboardImages);
    clipboardExcludedApps = readOrDefault(s, u"Clipboard/ExcludedApps"_s, d.clipboardExcludedApps);
    clipboardNumberKeys = readOrDefault(s, u"Clipboard/NumberKeys"_s, d.clipboardNumberKeys).trimmed().toLower();
    if (clipboardNumberKeys != u"ctrl" && clipboardNumberKeys != u"off")
        clipboardNumberKeys = d.clipboardNumberKeys;

    taskbarWinS = readOrDefault(s, u"Taskbar/WinS"_s, d.taskbarWinS);
    taskbarStartTyping = readOrDefault(s, u"Taskbar/StartMenuTyping"_s, d.taskbarStartTyping);
    taskbarSearchBox = readOrDefault(s, u"Taskbar/SearchBox"_s, d.taskbarSearchBox);
    taskbarStartGap = readOrDefault(s, u"Taskbar/StartGap"_s, d.taskbarStartGap);

    webShortcuts = readWebShortcuts(s, d.webShortcuts);
    fileTypes = readFileTypes(s, d.fileTypes);

    shares = readShares(s);
    networkVerifyHours = std::clamp(readOrDefault(s, u"Network/VerifyHours"_s, d.networkVerifyHours), 0, 24 * 7);
    networkVerifyHoursUnwatched
        = std::clamp(readOrDefault(s, u"Network/VerifyHoursUnwatched"_s, d.networkVerifyHoursUnwatched), 0, 24 * 7);
    networkThreads = std::clamp(readOrDefault(s, u"Network/Threads"_s, d.networkThreads), 1, 64);
    networkExcludedNames = readOrDefault(s, u"Network/ExcludedNames"_s, d.networkExcludedNames);
    networkExcludedFiles = readOrDefault(s, u"Network/ExcludedFiles"_s, d.networkExcludedFiles);

    mcp = readOrDefault(s, u"Mcp/Enabled"_s, d.mcp);
    mcpAllowOpen = readOrDefault(s, u"Mcp/AllowOpen"_s, d.mcpAllowOpen);
    mcpClipboard = readOrDefault(s, u"Mcp/Clipboard"_s, d.mcpClipboard);
    mcpExcludedPaths = readOrDefault(s, u"Mcp/ExcludedPaths"_s, d.mcpExcludedPaths);
    mcpPlugins = readOrDefault(s, u"Mcp/Plugins"_s, d.mcpPlugins);
    mcpPluginActions = readOrDefault(s, u"Mcp/PluginActions"_s, d.mcpPluginActions);

    pluginDeveloperMode = readOrDefault(s, u"Plugins/DeveloperMode"_s, d.pluginDeveloperMode);
    plugins = readPlugins(s);
}

void Settings::save() const
{
    QSettings s(QSettings::IniFormat, QSettings::UserScope, u"WinShun"_s, u"WinShun"_s);
    s.setValue(u"Launcher/DoubleCtrl"_s, doubleCtrl);
    s.setValue(u"Launcher/DoubleCtrlPauseInGames"_s, doubleCtrlPauseInGames);
    s.setValue(u"Launcher/DoubleCtrlPauseInFullScreen"_s, doubleCtrlPauseInFullScreen);
    s.setValue(u"Launcher/DoubleCtrlExcludedApps"_s, doubleCtrlExcludedApps);
    s.setValue(u"Launcher/Hotkey"_s, hotkey);
    s.setValue(u"Launcher/Renderer"_s, renderer);
    s.setValue(u"Launcher/History"_s, recordHistory);
    s.setValue(u"Launcher/DialogJump"_s, dialogJump);
    s.setValue(u"Launcher/DialogBar"_s, dialogBar);
    s.setValue(u"Launcher/DialogBarPlace"_s, dialogBarPlace);
    s.setValue(u"Launcher/DialogAutoJump"_s, dialogAutoJump);
    s.setValue(u"Launcher/DialogAutoJumpWhen"_s, dialogAutoJumpWhen);
    s.setValue(u"Launcher/DialogAutoJumpAsked"_s, dialogAutoJumpAsked);
    s.setValue(u"Launcher/DialogBarExcludedApps"_s, dialogBarExcludedApps);
    s.setValue(u"Launcher/FileManager"_s, fileManager);
    s.setValue(u"Results/FoldersFirst"_s, foldersFirst);
    s.setValue(u"Results/ShowModified"_s, showModified);
    s.setValue(u"Results/Sort"_s, sort);
    s.setValue(u"Results/StartList"_s, startList);
    s.setValue(u"Results/NumberKeys"_s, numberKeys);
    s.setValue(u"Appearance/Theme"_s, theme);
    s.setValue(u"Appearance/Language"_s, language);
    s.setValue(u"Appearance/Transparency"_s, transparency);
    s.setValue(u"Appearance/TaskbarIcon"_s, taskbarIcon);
    s.setValue(u"Appearance/UiSize"_s, uiSize);
    s.setValue(u"Update/Automatic"_s, autoUpdate);
    s.setValue(u"Backup/Folder"_s, backupFolder);
    s.setValue(u"Index/ExcludedPaths"_s, excludedPaths);
    s.setValue(u"Index/ExcludedNames"_s, excludedNames);
    s.setValue(u"Index/IncludeRemovableDrives"_s, includeRemovableDrives);
    s.setValue(u"Index/RescanOnStartup"_s, rescanOnStartup);
    s.setValue(u"Index/Folder"_s, indexFolder);
    s.setValue(u"Content/Enabled"_s, contentSearch);
    s.setValue(u"Content/Extensions"_s, contentExtensions);
    for (std::size_t k = 0; k < ContentSizeLimits::kKinds; ++k)
        s.setValue(kContentSizeKeys[k], contentMaxSizeMB[k]);
    s.setValue(u"Content/Index"_s, contentIndex);
    s.setValue(u"Content/IncludeSystemFolders"_s, contentInLowPriority);
    s.setValue(u"Content/Documents"_s, contentDocuments);
    s.setValue(u"Clipboard/Enabled"_s, clipboard);
    s.setValue(u"Clipboard/WinV"_s, clipboardWinV);
    s.setValue(u"Clipboard/Hotkey"_s, clipboardHotkey);
    s.setValue(u"Clipboard/MaxItems"_s, clipboardMaxItems);
    s.setValue(u"Clipboard/MaxDays"_s, clipboardMaxDays);
    s.setValue(u"Clipboard/Images"_s, clipboardImages);
    s.setValue(u"Clipboard/ExcludedApps"_s, clipboardExcludedApps);
    s.setValue(u"Clipboard/NumberKeys"_s, clipboardNumberKeys);
    s.setValue(u"Taskbar/WinS"_s, taskbarWinS);
    s.setValue(u"Taskbar/StartMenuTyping"_s, taskbarStartTyping);
    s.setValue(u"Taskbar/SearchBox"_s, taskbarSearchBox);
    s.setValue(u"Taskbar/StartGap"_s, taskbarStartGap);
    writeWebShortcuts(s, webShortcuts);
    writeFileTypes(s, fileTypes);
    writeShares(s, shares);
    s.setValue(u"Network/VerifyHours"_s, networkVerifyHours);
    s.setValue(u"Network/VerifyHoursUnwatched"_s, networkVerifyHoursUnwatched);
    s.setValue(u"Network/Threads"_s, networkThreads);
    s.setValue(u"Network/ExcludedNames"_s, networkExcludedNames);
    s.setValue(u"Network/ExcludedFiles"_s, networkExcludedFiles);
    s.setValue(u"Mcp/Enabled"_s, mcp);
    s.setValue(u"Mcp/AllowOpen"_s, mcpAllowOpen);
    s.setValue(u"Mcp/Clipboard"_s, mcpClipboard);
    s.setValue(u"Mcp/ExcludedPaths"_s, mcpExcludedPaths);
    s.setValue(u"Mcp/Plugins"_s, mcpPlugins);
    s.setValue(u"Mcp/PluginActions"_s, mcpPluginActions);
    s.setValue(u"Plugins/DeveloperMode"_s, pluginDeveloperMode);
    writePlugins(s, plugins);
}

ContentSizeLimits Settings::contentSizeLimits() const
{
    ContentSizeLimits limits;
    for (std::size_t k = 0; k < ContentSizeLimits::kKinds; ++k)
        limits.bytes[k] = std::llround(contentMaxSizeMB[k] * (1 << 20));
    return limits;
}

CrawlRules Settings::crawlRules() const
{
    CrawlRules rules;
    for (const QString& p : excludedPaths) {
        if (!p.trimmed().isEmpty())
            rules.excludedPaths.push_back(normalizePath(p));
    }
    // Our own files churn constantly. Not when they are a drive's root (an
    // empty drive chosen for the index): that would leave the whole drive out.
    const auto ownFolder = [&](const QString& folder) {
        std::wstring path = normalizePath(folder);
        if (!(path.size() == 2 && path[1] == L':'))
            rules.excludedPaths.push_back(std::move(path));
    };
    ownFolder(dataDir());
    if (!indexFolder.isEmpty())
        ownFolder(indexDir());
    for (const QString& n : excludedNames) {
        if (!n.trimmed().isEmpty())
            rules.excludedNames.push_back(normalizeName(n));
    }
    for (const QString& p : kLowPriorityPaths) {
        const std::wstring path = normalizePath(p);
        if (path.find(L'%') == std::wstring::npos) // skip variables that do not exist here
            rules.lowPriorityPaths.push_back(path);
    }
    for (const QString& n : kLowPriorityNames)
        rules.lowPriorityNames.push_back(normalizeName(n));
    return rules;
}

ShareIndexer::Options Settings::shareOptions() const
{
    ShareIndexer::Options o;
    for (const NetworkShare& n : shares)
        o.shares.push_back({n.path.toStdWString(), n.name.toStdWString()});
    o.rules = crawlRules();
    for (const QString& n : networkExcludedNames) {
        if (!n.trimmed().isEmpty())
            o.rules.excludedNames.push_back(normalizeName(n));
    }
    for (const QString& n : networkExcludedFiles) {
        if (!n.trimmed().isEmpty())
            o.excludedFiles.push_back(n.trimmed().toStdWString());
    }
    o.verifyEvery = std::chrono::hours(networkVerifyHours);
    o.verifyEveryUnwatched = std::chrono::hours(networkVerifyHoursUnwatched);
    o.threads = networkThreads;
    return o;
}

} // namespace ws
