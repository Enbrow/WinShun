#include "Preview.h"

#include "Backup.h"
#include "AppCatalog.h"
#include "History.h"
#include "IndexService.h"
#include "Launcher.h"
#include "SystemCatalog.h"
#include "FavoritesSettings.h"
#include "FileIconProvider.h"
#include "PluginSettings.h"
#include "ResultModel.h"
#include "Settings.h"
#include "SettingsEditor.h"
#include "SystemTheme.h"
#include "Updater.h"
#include "WindowFrame.h"
#include "AnnotationModel.h"
#include "platform/SiteIcons.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QLibraryInfo>
#include <QLocale>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTimer>
#include <QTime>
#include <QTranslator>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

using namespace Qt::StringLiterals;

namespace ws::preview {

namespace {

constexpr auto kFlag = u"--preview-plugins";
constexpr auto kSettingsFlag = u"--preview-settings";
constexpr auto kResultsFlag = u"--preview-results";
constexpr auto kAiCardsFlag = u"--preview-ai-cards";
constexpr auto kCaptureToolsFlag = u"--preview-capture-tools";

int g_warnings = 0;
bool g_edges = false; // --edges (or --tree): every grab also writes <png>.edges.txt
QtMessageHandler g_previous = nullptr;
QString g_logPath; // the run is elevated, its console cannot be read

void say(const QString& line)
{
    std::fprintf(stderr, "%s\n", qUtf8Printable(line));
    if (g_logPath.isEmpty())
        return;
    QFile file(g_logPath);
    if (file.open(QIODevice::Append | QIODevice::Text))
        file.write((line + u'\n').toUtf8());
}

void countWarnings(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg)
        ++g_warnings;
    if (g_previous)
        g_previous(type, context, message);
    say((type == QtDebugMsg || type == QtInfoMsg ? u""_s : u"WARNING: "_s) + message);
}

QString valueAfter(const QStringList& args, QStringView flag, const QString& fallback = {})
{
    const qsizetype at = args.indexOf(flag);
    return at >= 0 && at + 1 < args.size() ? args[at + 1] : fallback;
}

void pump(int ms)
{
    QElapsedTimer timer;
    timer.start();
    do
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    while (timer.elapsed() < ms);
}

// A window off the screen gets no resize or expose from Windows, so its
// content item would stay 0x0 (and every `parent.width` with it): sized
// here, more than once, as the height follows the width through the
// wrapping. Items are laid out (polished) only for a frame, and a frame
// comes only with a grab here: a grab per pass, thrown away, until the
// height the page asks for is the window's.
void settle(QQuickWindow* window, int passes = 4)
{
    for (int pass = 0; pass < passes; ++pass) {
        window->contentItem()->setSize(QSizeF(window->width(), window->height()));
        pump(200);
        (void)window->grabWindow();
        if (window->contentItem()->height() == window->height() && pass > 0)
            break;
    }
}

// The items with their places and sizes, for finding what is laid out
// wrong: "<png>.tree.txt".
void writeTree(QQuickWindow* window, const QString& path)
{
    QFile tree(path);
    if (!tree.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return;
    std::function<void(QQuickItem*, int)> walk = [&](QQuickItem* item, int depth) {
        if (depth > 14)
            return;
        const QString name = QString::fromLatin1(item->metaObject()->className()).section(u'_', 0, 0);
        tree.write(u"%1%2 x=%3 y=%4 w=%5 h=%6 iw=%7 ih=%8%9\n"_s.arg(QString(depth * 2, u' '), name).arg(item->x()).arg(item->y())
                       .arg(item->width()).arg(item->height()).arg(item->implicitWidth()).arg(item->implicitHeight())
                       .arg(item->isVisible() ? QString() : u" (hidden)"_s)
                       .toUtf8());
        for (QQuickItem* child : item->childItems())
            walk(child, depth + 1);
    };
    walk(window->contentItem(), 0);
}

// Every visible Rectangle with an edge that is not on a whole device pixel
// (what a 1 px line, a border or a child inset by a border looks like at 150 %),
// in device pixels: "<png>.edges.txt". Thin ones (a separator) and ones with a
// border or a radius are the ones that show.
void writeEdges(QQuickWindow* window, const QString& path)
{
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return;
    const qreal dpr = window->effectiveDevicePixelRatio();
    const auto fraction = [](qreal v) { return std::abs(v - std::round(v)); };
    std::function<void(QQuickItem*, int, const QString&)> walk = [&](QQuickItem* item, int depth, const QString& trail) {
        if (!item->isVisible() || item->opacity() <= 0 || depth > 40)
            return;
        const QString name = QString::fromLatin1(item->metaObject()->className()).section(u'_', 0, 0);
        const QString here = trail + u"/"_s + (item->objectName().isEmpty() ? name : item->objectName());
        if (name == u"QQuickRectangle" && item->width() > 0 && item->height() > 0) {
            const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
            const qreal l = r.left() * dpr, t = r.top() * dpr, rt = r.right() * dpr, b = r.bottom() * dpr;
            qreal bw = 0;
            if (QObject* pen = item->property("border").value<QObject*>())
                bw = pen->property("width").toReal();
            const qreal alpha = item->property("color").value<QColor>().alphaF();
            const bool edge = fraction(l) > 0.02 || fraction(t) > 0.02 || fraction(rt) > 0.02 || fraction(b) > 0.02;
            if (edge && (alpha > 0.02 || bw > 0)) {
                out.write(u"l=%1 t=%2 r=%3 b=%4 | %5x%6 dev | border %7 radius %8 alpha %9 | %10 (qml y=%11 h=%12)"_s
                              .arg(l, 0, 'f', 2).arg(t, 0, 'f', 2).arg(rt, 0, 'f', 2).arg(b, 0, 'f', 2)
                              .arg(rt - l, 0, 'f', 2).arg(b - t, 0, 'f', 2).arg(bw, 0, 'f', 2)
                              .arg(item->property("radius").toReal()).arg(alpha, 0, 'f', 2).arg(here)
                              .arg(item->y(), 0, 'f', 3).arg(item->height(), 0, 'f', 3)
                              .toUtf8() + '\n');
            }
        }
        for (QQuickItem* child : item->childItems())
            walk(child, depth + 1, here);
    };
    walk(window->contentItem(), 0, QString());
}

bool saveGrab(QQuickWindow* window, const QString& path)
{
    const QImage image = window->grabWindow();
    if (g_edges)
        writeEdges(window, path + u".edges.txt"_s);
    const bool saved = !image.isNull() && image.save(path, "PNG");
    say(u"%1 %2 %3x%4"_s.arg(saved ? u"saved"_s : u"NOT saved"_s, path).arg(image.width()).arg(image.height()));
    return saved;
}

// Should anything hang (a renderer waiting for a frame that never comes),
// the log says so and the process ends.
void armWatchdog(QTimer& watchdog, int seconds)
{
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, [seconds] {
        say(u"timeout: gave up after %1 s"_s.arg(seconds));
        std::_Exit(5);
    });
    watchdog.start(seconds * 1000);
}

// Off the screen, never activated: the software renderer draws what is
// shown, and the grab takes the frame.
void showOffScreen(QQuickWindow* window)
{
    window->setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    window->setProperty("_q_showWithoutActivating", true);
    window->setMaximumHeight(32000); // taller than the screen, for a whole page at a time
    // Win32 window coordinates are limited to 32767 physical pixels. Keep
    // the offscreen position below that even at 200% and larger scales.
    const int offscreen = static_cast<int>(std::floor(24000 / window->devicePixelRatio()));
    window->setPosition(offscreen, offscreen);
    window->show();
}

int finish(QQuickWindow* window, bool saved)
{
    window->hide();
    delete window;
    QDir(Settings::dataDir()).removeRecursively();
    say(u"%1 warning(s)"_s.arg(g_warnings));
    return saved ? std::min(g_warnings, 100) : 4;
}

// ---- --preview-plugins ------------------------------------------------------------------

int runPlugins(const QStringList& args)
{
    const QString out = valueAfter(args, kFlag);
    const bool dark = args.contains(u"--dark"_s);
    const bool dialog = args.contains(u"--dialog"_s);
    const bool aiPage = valueAfter(args, u"--page") == u"ai"; // the AI 助手 page instead (its plugin switches)

    say(u"starting (dark %1, dialog %2)"_s.arg(dark).arg(dialog));
    QQmlApplicationEngine engine;
    engine.addImageProvider(u"fileicon"_s, new FileIconProvider);
    Settings settings = Settings::defaults();
    if (aiPage) { // every row on show: the switches above the plugins' ones on
        settings.mcp = true;
        settings.mcpPlugins = true;
    }
    auto* editor = new SettingsEditor(settings, u"software"_s, &engine);
    editor->plugins()->setLocale(u"zh"_s);
    editor->plugins()->loadPreview();
    if (aiPage)
        editor->setMcpStatus(u"已连接：Claude Desktop（12 次调用）"_s);
    say(u"loading the page"_s);

    QQmlComponent component(&engine, u"WinShun"_s, u"PluginsPreview"_s);
    QObject* object = component.createWithInitialProperties({
        {u"plugins"_s, QVariant::fromValue(editor->plugins())},
        {u"editor"_s, QVariant::fromValue(editor)},
        {u"dialogShown"_s, dialog},
        {u"aiPage"_s, aiPage},
    });
    auto* window = qobject_cast<QQuickWindow*>(object);
    if (!window) {
        say(component.errorString());
        delete object;
        return 3;
    }
    QTimer watchdog;
    armWatchdog(watchdog, 60);
    showOffScreen(window);
    settle(window);
    say(u"showing %1x%2"_s.arg(window->width()).arg(window->height()));
    pump(500); // the bindings settle (sizes, the asynchronous icons)
    // A menu open (DropMenu): 添加插件's ("add"), or the "…" of the first card
    // ("card"), whose entries run over the cards below it. Each lives on the
    // window's content item (the cards' are not QObject children of the
    // window: a Repeater's delegates have no QObject parent).
    if (const QString menu = valueAfter(args, u"--menu"); !menu.isEmpty()) {
        QObject* chosen = nullptr;
        for (QQuickItem* o : window->contentItem()->childItems()) {
            if (!QString::fromLatin1(o->metaObject()->className()).startsWith(u"DropMenu"))
                continue;
            // 添加插件's drops from a button with that text; a card's from a "…".
            const auto* anchor = o->property("anchor").value<QObject*>();
            const bool add = anchor && !anchor->property("text").toString().isEmpty();
            if (!chosen && (menu == u"add" ? add : !add))
                chosen = o;
        }
        if (!chosen) {
            say(u"no menu %1"_s.arg(menu));
            return finish(window, false);
        }
        // Opened by clicks on its button, as a user does: open, closed again
        // (what a button opened, a second click closes), open for the picture.
        auto* anchor = qobject_cast<QQuickItem*>(chosen->property("anchor").value<QObject*>());
        const QPointF at = anchor->mapToScene(QPointF(anchor->width() / 2, anchor->height() / 2));
        const auto click = [&] {
            QMouseEvent press(QEvent::MouseButtonPress, at, window->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, {});
            QCoreApplication::sendEvent(window, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, at, window->mapToGlobal(at), Qt::LeftButton, {}, {});
            QCoreApplication::sendEvent(window, &release);
            pump(100);
            return chosen->property("open").toBool();
        };
        const bool first = click();
        const bool second = click();
        const bool third = click();
        say(u"clicks on the button: %1, %2, %3"_s.arg(first ? u"open"_s : u"closed"_s,
                                                       second ? u"open"_s : u"closed"_s,
                                                       third ? u"open"_s : u"closed"_s));
        if (!first || second || !third)
            return finish(window, false);
    }
    say(u"grabbing"_s);
    if (args.contains(u"--tree"_s))
        writeTree(window, out + u".tree.txt"_s);
    const bool saved = saveGrab(window, out);
    say(u"dpr %1"_s.arg(window->devicePixelRatio()));
    return finish(window, saved);
}

// ---- --preview-settings -----------------------------------------------------------------

// A share's status as the app would report it (SettingsEditor::setShareStatuses).
ShareIndexer::Status shareStatus(const QString& path, ShareIndexer::State state, share::Problem problem, std::size_t items)
{
    ShareIndexer::Status status;
    status.path = path.toStdWString();
    status.state = state;
    status.problem = problem;
    status.items = items;
    status.lastVerified = state == ShareIndexer::State::Online ? static_cast<FileTime>(QDateTime::currentSecsSinceEpoch() - 1800) : 0;
    status.watching = state == ShareIndexer::State::Online;
    return status;
}

// An id of SettingsWindow.qml's own (flick, searchBox, settingsSearch …).
QObject* named(QQuickWindow* window, const char* id)
{
    QQmlContext* context = qmlContext(window);
    QObject* object = context ? context->objectForName(QString::fromLatin1(id)) : nullptr;
    if (!object)
        qWarning("preview: no item '%s' in SettingsWindow.qml", id);
    return object;
}

// Repeater delegates belong to their model as QObjects; follow visual parents.
QQuickItem* visualNamed(QQuickItem* root, const QString& name)
{
    if (root->objectName() == name)
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = visualNamed(child, name))
            return found;
    }
    return nullptr;
}

// Exercise the real add/edit actions and form on the preview's temporary
// profile. In particular, a whole-page screenshot cannot detect a form
// appended below the viewport, or a Loader whose contents failed to appear.
bool previewWebForms(QQuickWindow* window, SettingsEditor* editor, const QDir& out, bool tree)
{
    QMetaObject::invokeMethod(window, "showWebPage");
    settle(window);
    QObject* add = named(window, "webAddButton");
    QObject* loader = named(window, "webDialog");
    QObject* pageFlick = named(window, "flick");
    if (!add || !loader || !pageFlick)
        return false;
    const auto check = [](bool condition, const char* message) {
        if (!condition)
            qWarning("web form: %s", message);
        return condition;
    };
    const auto click = [](QObject* button) {
        return button && QMetaObject::invokeMethod(button, "clicked");
    };
    const auto key = [&](int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QKeyEvent press(QEvent::KeyPress, code, modifiers);
        QCoreApplication::sendEvent(window, &press);
        QKeyEvent release(QEvent::KeyRelease, code, modifiers);
        QCoreApplication::sendEvent(window, &release);
        pump(20);
    };
    const auto form = [&] { return qobject_cast<QQuickItem*>(loader->property("item").value<QObject*>()); };
    const auto child = [&](const char* name) -> QQuickItem* {
        return form() ? form()->findChild<QQuickItem*>(QString::fromLatin1(name)) : nullptr;
    };
    const auto inside = [](QQuickItem* item, QQuickItem* ancestor) {
        for (; item; item = item->parentItem()) {
            if (item == ancestor)
                return true;
        }
        return false;
    };
    bool saved = true;
    const auto grab = [&](const QString& name) {
        settle(window);
        const QString file = out.filePath(name + u".png"_s);
        if (tree)
            writeTree(window, file + u".tree.txt"_s);
        saved = saveGrab(window, file) && saved;
    };
    const auto visibleIn = [](QQuickItem* item, QQuickItem* viewport) {
        if (!item || !viewport || !item->isVisible() || item->width() <= 0 || item->height() <= 0)
            return false;
        const QPointF top = item->mapToItem(viewport, QPointF());
        return QRectF(QPointF(), viewport->size()).contains(QRectF(top, item->size()));
    };
    const qsizetype before = editor->webShortcuts().size();
    click(add);
    settle(window);
    if (!check(form() && child("keywordField") && child("urlField") && child("webFormSave"), "add form did not load"))
        return false;
    check(visibleIn(child("webFormCard"), window->contentItem()), "card extends outside the window");
    check(visibleIn(child("keywordField"), child("webFormViewport")), "first field is outside the viewport");
    check(visibleIn(child("webFormSave"), window->contentItem()), "save button is outside the window");
    check(inside(window->activeFocusItem(), child("keywordField")), "keyword did not receive focus");
    const qreal listHeight = pageFlick->property("contentHeight").toReal();
    grab(u"web-add"_s);

    // Tab through a full cycle in each direction, without entering the
    // settings behind the overlay, and bring lower fields into view.
    for (int direction : {1, -1}) {
        for (int step = 0; step < 12; ++step) {
            key(direction > 0 ? Qt::Key_Tab : Qt::Key_Backtab, direction > 0 ? Qt::NoModifier : Qt::ShiftModifier);
            check(inside(window->activeFocusItem(), form()), "Tab left the modal");
        }
    }
    if (!check(child("webFormSvgInput"), "SVG input did not load"))
        return false;
    QMetaObject::invokeMethod(child("webFormSvgInput"), "takeFocus");
    settle(window);
    check(visibleIn(window->activeFocusItem(), child("webFormViewport")), "focused input was not scrolled into view");
    grab(u"web-scrolled"_s);
    click(child("webFormSave"));
    settle(window);
    check(form() && !form()->property("error").toString().isEmpty(), "empty form did not report validation error");
    check(visibleIn(child("webFormError"), window->contentItem()), "validation error is outside the window");
    check(editor->webShortcuts().size() == before, "invalid form changed settings");
    grab(u"web-error"_s);
    key(Qt::Key_Escape);
    check(!loader->property("active").toBool(), "Escape did not close the form");
    check(window->activeFocusItem() == add, "focus did not return to Add");

    // Reopening must create a fresh draft. Enter saves through the actual
    // TextInput, including a model reset when the shortcut is appended.
    click(add);
    settle(window);
    if (!check(child("keywordField") && child("urlField"), "reopened form is blank"))
        return false;
    check(form()->property("error").toString().isEmpty(), "old error survived reopening");
    child("keywordField")->setProperty("text", u"previewweb"_s);
    child("nameField")->setProperty("text", u"表单验证"_s);
    child("urlField")->setProperty("text", u"https://example.com/search?q=%s"_s);
    QMetaObject::invokeMethod(child("urlField"), "takeFocus");
    key(Qt::Key_Return);
    settle(window);
    check(!loader->property("active").toBool() && editor->webShortcuts().size() == before + 1, "Enter did not add the shortcut");

    QList<QQuickItem*> edits;
    std::function<void(QQuickItem*)> collectEdits = [&](QQuickItem* item) {
        if (item->objectName() == u"webEditButton")
            edits.append(item);
        for (auto* child : item->childItems())
            collectEdits(child);
    };
    collectEdits(window->contentItem()); // Repeater delegates have visual, not QObject, parents
    if (!check(!edits.isEmpty(), "no edit button in the list"))
        return false;
    // The last row is below the initial viewport: its editor must still
    // appear in the center, without changing the page's scroll position.
    pageFlick->setProperty("contentY", std::max(0.0, pageFlick->property("contentHeight").toDouble() - pageFlick->property("height").toDouble()));
    const qreal scrollY = pageFlick->property("contentY").toReal();
    click(edits.constLast());
    settle(window);
    if (!check(child("urlField"), "edit form is blank"))
        return false;
    check(child("keywordField")->property("text").toString() == u"previewweb", "edit did not load the selected shortcut");
    check(inside(window->activeFocusItem(), child("urlField")), "edit URL did not receive focus");
    check(visibleIn(child("webFormCard"), window->contentItem()), "edit card is off screen");
    check(pageFlick->property("contentY").toReal() == scrollY, "opening the edit form moved the list");
    grab(u"web-edit"_s);
    child("nameField")->setProperty("text", u"已修改"_s);
    click(child("webFormSave"));
    settle(window);
    check(!loader->property("active").toBool() && editor->webShortcuts().constLast().toMap().value(u"name"_s).toString() == u"已修改", "edit did not save");
    click(add);
    settle(window);
    click(child("webFormCancel"));
    settle(window);
    check(!loader->property("active").toBool() && editor->webShortcuts().size() == before + 1, "Cancel changed settings or left the form open");
    check(pageFlick->property("contentHeight").toReal() == listHeight + 58, "form changed the list layout");

    // An empty list must not collapse the dialog, and resizing must retain
    // the unsaved draft while keeping both its first field and actions usable.
    while (!editor->webShortcuts().isEmpty())
        editor->removeWebShortcut(0);
    click(add);
    settle(window);
    if (!check(child("keywordField"), "empty-list add form is blank"))
        return false;
    child("keywordField")->setProperty("text", u"draft"_s);
    const QSize initialSize = window->size();
    window->resize(760, 420);
    settle(window);
    check(child("keywordField")->property("text").toString() == u"draft", "resizing lost the draft");
    check(visibleIn(child("webFormCard"), window->contentItem()) && visibleIn(child("webFormSave"), window->contentItem()), "resized form is off screen");
    grab(u"web-add-empty-compact"_s);
    click(child("webFormCancel"));
    window->resize(initialSize);
    check(editor->webShortcuts().isEmpty(), "cancel saved the draft");
    say(u"web forms checked at %1x%2, DPR %3"_s.arg(window->width()).arg(window->height()).arg(window->devicePixelRatio()));
    return saved;
}

bool previewCommandIcons(QQuickWindow* window, SettingsEditor* editor, Favorites& favorites, QObject* section, const QDir& out)
{
    const auto check = [](bool condition, const char* message) {
        if (!condition)
            qWarning("command icons: %s", message);
        return condition;
    };
    const auto form = [&] { return visualNamed(qobject_cast<QQuickItem*>(section), u"favoriteForm"_s); };
    const auto load = [&](const QString& path) {
        QPointer<QObject> current = form();
        if (!current)
            return false;
        current->setProperty("iconRequest", editor->favorites()->loadCommandIcon(path));
        QElapsedTimer wait;
        wait.start();
        while (current && current->property("iconLoading").toBool() && wait.elapsed() < 5000)
            pump(10);
        return check(current && !current->property("iconLoading").toBool(), "icon worker did not finish");
    };
    QTemporaryDir pictures;
    const QString path = pictures.filePath(u"icon.svg"_s);
    QFile svg(path);
    if (!check(svg.open(QIODevice::WriteOnly), "cannot write the SVG fixture"))
        return false;
    svg.write("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'>"
              "<rect x='8' y='8' width='48' height='48' rx='10' fill='#1268b9'/>"
              "<path d='M22 17h20v30H22z' fill='white'/></svg>");
    svg.close();
    if (!load(path) || !check(form()->property("error").toString().isEmpty(), "valid SVG was rejected"))
        return false;
    const QString icon = form()->property("iconData").toString();
    check(!favoriteIconPng(icon).isEmpty(), "SVG was not normalized to a bounded PNG");
    check(favorites.items().size() == 4, "picking an icon saved the draft");
    QFile::remove(path); // the draft must already own the picture
    settle(window, 3);
    if (auto* flick = named(window, "flick"))
        flick->setProperty("contentY", std::max(0.0, flick->property("contentHeight").toDouble() - flick->property("height").toDouble()));
    settle(window, 3);
    auto* preview = form()->findChild<QQuickItem*>(u"favoriteIconPreview"_s);
    check(preview && preview->property("status").toInt() == 1 && preview->property("implicitWidth").toReal() > 0,
        "custom preview did not load after its original file was deleted");
    bool saved = saveGrab(window, out.filePath(u"command-custom-icon.png"_s));
    QMetaObject::invokeMethod(form(), "save");
    if (!check(favorites.items().size() == 5 && favorites.items().constLast().icon == icon, "form did not save the icon"))
        return false;
    const Favorite command = favorites.items().constLast();
    const auto exported = readFavoritesFile(writeFavoritesFile(favorites.items()));
    check(exported && exported->favorites.constLast().icon == icon, "export lost the custom icon");

    SearchResult row;
    row.path = command.path;
    row.app = AppKind::Tool;
    row.target = command.command;
    row.workingDirectory = command.workingDirectory;
    row.icon = command.icon;
    ResultModel results;
    results.assign({row}, {});
    check(results.data(results.index(0), ResultModel::IconRole).toString()
            == editor->favorites()->items().constLast().toMap().value(u"icon"_s).toString(),
        "search results and settings disagree on the command icon");

    const auto reopen = [&] {
        QMetaObject::invokeMethod(window, "showFavoritesPage", Q_ARG(QVariant, QVariant(command.path)));
        settle(window, 3);
        return check(form() && form()->property("iconData").toString() == icon, "editing did not restore the saved icon");
    };
    if (!reopen())
        return false;
    // A failed import preserves the previous custom picture and reports why.
    if (!load(pictures.filePath(u"missing.png"_s)))
        return false;
    check(!form()->property("error").toString().isEmpty() && form()->property("iconData").toString() == icon,
        "failed selection discarded the current icon or hid the error");
    auto* reset = form()->findChild<QObject*>(u"favoriteResetIcon"_s);
    if (!check(reset, "reset button is missing"))
        return false;
    QMetaObject::invokeMethod(reset, "clicked");
    check(form()->property("iconData").toString().isEmpty(), "reset did not select automatic extraction");
    QMetaObject::invokeMethod(form(), "finished");
    check(favorites.items().constLast().icon == icon, "cancelling reset changed the saved icon");
    if (!reopen())
        return false;
    reset = form()->findChild<QObject*>(u"favoriteResetIcon"_s);
    QMetaObject::invokeMethod(reset, "clicked");
    QMetaObject::invokeMethod(form(), "save");
    check(favorites.items().constLast().icon.isEmpty(), "saving reset kept the custom icon");
    favorites.remove(command.path);
    say(u"command icons: real form, asynchronous SVG load, source deletion, search model, export, reopen, cancel and reset checked"_s);
    return saved;
}

int runSettings(const QStringList& args)
{
    const QDir out(valueAfter(args, kSettingsFlag));
    siteicons::setFolder(Settings::dataDir() + u"/icons/web"_s);
    const bool dark = args.contains(u"--dark"_s);
    const bool tree = args.contains(u"--tree"_s);
    QTranslator translator;
    if (args.contains(u"--english"_s) && translator.load(u":/i18n/winshun_en.qm"_s))
        QCoreApplication::installTranslator(&translator);
    say(u"starting (dark %1, scale %2)"_s.arg(dark).arg(qEnvironmentVariable("QT_SCALE_FACTOR")));
    // Where Qt came from and what the text is set in: two runs of the same
    // build can differ in line heights when they resolve these differently.
    const QFont font = QGuiApplication::font();
    const QFontMetricsF metrics(font);
    say(u"platform %1, plugins %2, qml %3, library paths %4"_s.arg(QGuiApplication::platformName(), QLibraryInfo::path(QLibraryInfo::PluginsPath),
            QLibraryInfo::path(QLibraryInfo::QmlImportsPath), QCoreApplication::libraryPaths().join(u"; "_s)));
    say(u"font '%1' -> '%2' %3px: ascent %4, descent %5, height %6, line spacing %7, hinting %8"_s.arg(font.family(), QFontInfo(font).family())
            .arg(QFontInfo(font).pixelSize()).arg(metrics.ascent()).arg(metrics.descent()).arg(metrics.height()).arg(metrics.lineSpacing())
            .arg(static_cast<int>(font.hintingPreference())));

    QQmlApplicationEngine engine;
    engine.addImageProvider(u"fileicon"_s, new FileIconProvider);
    // The settings as the window gets them: the defaults, plus what makes
    // every row show (the AI switches on, two network shares).
    Settings settings = Settings::defaults();
    const QString corners = valueAfter(args, u"--corners", settings.corners);
    if (corners == u"round" || corners == u"square")
        settings.corners = corners;
    SystemTheme::setRoundedCorners(settings.corners == u"round");
    settings.mcp = true;
    settings.mcpPlugins = true;
    settings.taskbarSearchBox = true; // its style row
    settings.captureEnabled = true; // show the screenshot's options too; no Capture is created in this preview
    settings.pasteFileEnabled = true; // its card's two ways in
    settings.dialogBar = true; // off for a new install: the rows they keep away
    settings.folderMenu = u"auto"_s;
    const QString shareOnline = u"\\\\192.168.1.10\\资料"_s;
    const QString shareSignIn = u"\\\\nas\\photos"_s;
    const QString shareWsl = u"\\\\wsl.localhost\\Ubuntu-24.04\\home\\me\\项目"_s;
    settings.shares = {{shareOnline, u"公司资料"_s}, {shareSignIn, {}}, {shareWsl, {}}};
    settings.hotkey = u"Alt+Space"_s;
    settings.doubleCtrlExcludedApps = {u"TheFinals.exe"_s};
    settings.clipboardHotkey = u"Win+Alt+V"_s;
    if (args.contains(u"--clipboard-disabled"_s))
        settings.clipboard = false;
    settings.mcpExcludedPaths = {u"%USERPROFILE%\\Documents\\私人"_s};
    // The real view-model and its real neighbours, as App::showSettings makes
    // them; the renderer "running" is the one set, so that no restart is asked for.
    auto* editor = new SettingsEditor(settings, Settings::resolveRenderer(settings.renderer), &engine);
    QObject::connect(editor, &SettingsEditor::changed, editor, [editor] {
        SystemTheme::setRoundedCorners(editor->corners() == u"round");
    });
    auto* frame = new WindowFrame(true, editor);
    auto* updater = new Updater(editor); // never setAutomatic() or check(): no network
    editor->plugins()->setLocale(u"zh"_s);
    editor->plugins()->loadPreview(); // a plugin::Host would start WinShunPluginHost.exe: made-up cards instead
    editor->setMcpStatus(u"已连接：Claude Desktop（12 次调用）"_s);
    editor->setShareStatuses({
        shareStatus(shareOnline, ShareIndexer::State::Online, share::Problem::None, 48213),
        shareStatus(shareSignIn, ShareIndexer::State::Problem, share::Problem::NeedsSignIn, 0),
        shareStatus(shareWsl, ShareIndexer::State::Problem, share::Problem::WslStopped, 1230),
    });
    SettingsEditor::IndexFolderState indexState;
    indexState.folder = u"C:\\Users\\me\\AppData\\Local\\WinShun"_s;
    indexState.size = u"112 MB"_s;
    editor->setIndexFolderState(indexState);
    editor->setContentIndexStatus(u"已收录 337,215 个文件，其中 5,815 个文档"_s);
    editor->setHistoryCount(128);
    editor->setClipboardCount(args.contains(u"--clipboard-disabled"_s) ? -1 : 57);
    editor->setHotkeyError({});
    // What the app reads off this PC, read-only: the file managers
    // installed, the taskbar, the AI clients and their configurations.
    // Not refreshNumberKeys(): it probes with RegisterHotKey.
    editor->refreshFileManagers();
    editor->refreshTaskbarState();
    editor->refreshMcpClients();
    // A few favorites, in a file of their own that goes with the preview:
    // a folder renamed and with a keyword, a file, a folder that is gone.
    QTemporaryDir favoritesDir;
    Favorites favorites(favoritesDir.filePath(u"favorites.json"_s));
    favorites.add({Favorite::Kind::Folder, QDir::toNativeSeparators(QDir::tempPath()), u"临时文件"_s, u"tmp"_s});
    favorites.add({Favorite::Kind::File, QDir::toNativeSeparators(QCoreApplication::applicationFilePath()), {}, {}});
    favorites.add({Favorite::Kind::Folder, u"Z:\\项目\\已删除的文件夹"_s, {}, u"xm"_s});
    editor->favorites()->setStore(&favorites);
    const QString command = u"\"C:\\Tools\\Code.exe\" {query}"_s;
    if (editor->favorites()->saveCommand(-1, u"编辑器"_s, u"tmp"_s, command, {}).isEmpty()
        || editor->favorites()->saveCommand(-1, {}, u"code"_s, command, {}).isEmpty()
        || editor->favorites()->saveCommand(-1, u"编辑器"_s, u"code"_s, u"{query}"_s, {}).isEmpty()
        || favorites.items().size() != 3)
        qWarning("preview: invalid custom command changed favorites");
    if (!editor->favorites()->saveCommand(-1, u"用编辑器打开"_s, u"code"_s, command, u"D:\\Projects"_s).isEmpty()
        || favorites.items().size() != 4)
        qWarning("preview: custom command was not saved");
    say(u"loading SettingsWindow.qml"_s);

    Backup backup; // no folder: never backs anything up
    QQmlComponent component(&engine, u"WinShun"_s, u"SettingsWindow"_s);
    QObject* object = component.createWithInitialProperties({
        {u"editor"_s, QVariant::fromValue(editor)},
        {u"frame"_s, QVariant::fromValue(frame)},
        {u"updater"_s, QVariant::fromValue(updater)},
        {u"backup"_s, QVariant::fromValue(&backup)},
    });
    auto* window = qobject_cast<QQuickWindow*>(object);
    if (!window) {
        say(component.errorString());
        delete object;
        return 3;
    }
    editor->setWindow(window);
    QTimer watchdog;
    armWatchdog(watchdog, 600);
    if (args.contains(u"--command-form"_s) && args.contains(u"--compact"_s))
        window->resize(760, 420);
    showOffScreen(window);
    const int baseHeight = window->height(); // 720: what the page column gets on a screen
    settle(window);
    say(u"showing %1x%2 (dpr %3), active %4"_s.arg(window->width()).arg(window->height()).arg(window->devicePixelRatio()).arg(window->isActive()));

    // The three tables, side by side: a page without a keyword entry or a
    // tabs entry would read undefined.
    const QVariantList pages = window->property("pages").toList();
    const QVariantList keywords = window->property("pageKeywords").toList();
    const QVariantList tabs = window->property("pageTabs").toList();
    const int gleaningPage = window->property("gleaningPage").toInt();
    const int aiPage = window->property("aiPage").toInt();
    const int pluginsPage = window->property("pluginsPage").toInt();
    const int favoritesPage = window->property("favoritesPage").toInt();
    say(u"pages %1, pageKeywords %2, pageTabs %3"_s.arg(pages.size()).arg(keywords.size()).arg(tabs.size()));
    if (pages.size() != keywords.size() || pages.size() != tabs.size())
        qWarning("preview: pages, pageKeywords and pageTabs differ in length");
    if (pages.value(aiPage).toMap().value(u"title"_s).toString() != QCoreApplication::translate("SettingsWindow", "AI 助手"))
        qWarning("preview: aiPage is not the AI 助手 page");
    if (pages.value(pluginsPage).toMap().value(u"title"_s).toString() != QCoreApplication::translate("SettingsWindow", "插件"))
        qWarning("preview: pluginsPage is not the 插件 page");
    if (pages.value(favoritesPage).toMap().value(u"title"_s).toString() != QCoreApplication::translate("SettingsWindow", "收藏夹"))
        qWarning("preview: favoritesPage is not the 收藏夹 page");
    if (!pages.value(gleaningPage).toMap().value(u"gleaning"_s).toBool() || gleaningPage != pages.size() - 1)
        qWarning("preview: gleaningPage is not the last page, or not marked gleaning");

    QObject* flick = named(window, "flick");
    QObject* searchBox = named(window, "searchBox");
    QObject* settingsSearch = named(window, "settingsSearch");
    if (!flick || !searchBox || !settingsSearch)
        return finish(window, false);
    QQuickItem* input = nullptr; // the box's TextInput, what the user types into
    for (QQuickItem* item : searchBox->findChildren<QQuickItem*>()) {
        if (item->inherits("QQuickTextInput"))
            input = item;
    }
    if (!input) {
        qWarning("preview: no TextInput in the search box");
        return finish(window, false);
    }

    if (args.contains(u"--web-form"_s)) {
        if (args.contains(u"--compact"_s))
            window->resize(760, 420); // the supported minimum, also used on high-DPI screens
        return finish(window, previewWebForms(window, editor, out, tree));
    }

    if (args.contains(u"--backup-settings"_s)) {
        bool ok = true;
        const auto check = [&](bool condition, const char* message) {
            if (!condition) {
                qWarning("backup settings: %s", message);
                ok = false;
            }
        };
        auto* row = window->findChild<QQuickItem*>(u"backupExportRow"_s);
        auto* button = window->findChild<QQuickItem*>(u"backupExportButton"_s);
        auto* status = window->findChild<QQuickItem*>(u"backupOperationStatus"_s);
        if (!row || !button || !status)
            return finish(window, false);
        window->resize(args.contains(u"--compact"_s) ? QSize(760, 420) : QSize(920, 720));
        QMetaObject::invokeMethod(window, "goTo", Q_ARG(QVariant, QVariant::fromValue(row)));
        settings.save(); // this preview's isolated profile, never the user's
        const auto waitForBackup = [&] {
            QElapsedTimer timeout;
            timeout.start();
            while (backup.busy() && timeout.elapsed() < 10000)
                pump(20);
            check(!backup.busy(), "operation did not finish");
            settle(window, 3);
            QMetaObject::invokeMethod(window, "revealRow", Q_ARG(QVariant, QVariant::fromValue(row)));
            settle(window, 3);
            auto* viewport = qobject_cast<QQuickItem*>(flick);
            const auto inView = [&](QQuickItem* item) {
                const QPointF top = item->mapToItem(viewport, QPointF());
                return top.x() >= 0 && top.y() >= 0 && top.x() + item->width() <= viewport->width() + 1
                    && top.y() + item->height() <= viewport->height() + 1;
            };
            check(inView(button) && inView(status), "export or its feedback is outside the viewport");
        };
        backup.backUpNow();
        check(!button->isEnabled(), "export stayed enabled during a backup");
        waitForBackup();
        check(backup.problem().isEmpty() && !backup.message().isEmpty(), "success was not reported");
        check(!QDir(Settings::dataDir() + u"/backups"_s).entryList({u"*.zip"_s}, QDir::Files).isEmpty(),
            "worker did not write a backup");
        check(status->property("text").toString() == backup.message(), "success did not reach the view");
        bool saved = saveGrab(window, out.filePath(u"backup-success.png"_s));
        const QString blocked = Settings::dataDir() + u"/blocked"_s;
        QFile file(blocked);
        check(file.open(QIODevice::WriteOnly), "cannot create the failure fixture");
        file.close();
        backup.setFolder(blocked); // a file where a directory is needed
        backup.backUpNow();
        waitForBackup();
        backup.setFolder({});
        settle(window, 3);
        check(!backup.problem().isEmpty() && status->property("text").toString().contains(backup.problem()),
            "failure was hidden with automatic backups off");
        check(button->isEnabled(), "export did not become available after failure");
        saved = saveGrab(window, out.filePath(u"backup-failure.png"_s)) && saved;
        if (tree)
            writeTree(window, out.filePath(u"backup.tree.txt"_s));
        say(u"backup worker, success/error feedback and disabled controls checked on an isolated profile"_s);
        return finish(window, ok && saved);
    }

    if (args.contains(u"--plugin-layout"_s)) {
        if (args.contains(u"--compact"_s))
            window->resize(760, 420);
        QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(pluginsPage)), Q_ARG(QVariant, QVariant(0)));
        settle(window);
        auto* card = window->findChild<QQuickItem*>(u"captureSettingsCard"_s);
        auto* expand = window->findChild<QQuickItem*>(u"captureExpandButton"_s);
        if (!card || !expand) {
            qWarning("plugin layout: missing capture card or settings button");
            return finish(window, false);
        }
        bool saved = true;
        const auto grab = [&](const QString& name) {
            settle(window);
            if (tree)
                writeTree(window, out.filePath(name + u".tree.txt"_s));
            saved = saveGrab(window, out.filePath(name + u".png"_s)) && saved;
        };
        const auto check = [](bool condition, const char* message) {
            if (!condition)
                qWarning("plugin layout: %s", message);
        };
        const auto key = [&](int code) {
            QKeyEvent press(QEvent::KeyPress, code, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &press);
            QKeyEvent release(QEvent::KeyRelease, code, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &release);
        };
        check(!card->property("expanded").toBool(), "capture options must start folded");
        say(u"plugins folded: card %1, page content %2"_s.arg(card->height()).arg(flick->property("contentHeight").toReal()));
        grab(u"plugins-folded"_s);
        expand->forceActiveFocus(Qt::TabFocusReason);
        key(Qt::Key_Space);
        settle(window);
        check(card->property("expanded").toBool(), "Space did not expand the capture card");
        auto* first = visualNamed(card, u"captureGroup0"_s);
        if (!first) {
            qWarning("plugin layout: missing category buttons");
            return finish(window, false);
        }
        first->forceActiveFocus(Qt::TabFocusReason);
        for (int group = 0; group < 6; ++group) {
            if (group > 0)
                key(Qt::Key_Right);
            settle(window);
            check(card->property("currentGroup").toInt() == group, "arrow key did not switch category");
            int visible = 0;
            for (int i = 0; i < 6; ++i) {
                auto* panel = window->findChild<QQuickItem*>(u"capturePanel%1"_s.arg(i));
                if (panel && panel->isVisible())
                    ++visible;
            }
            check(visible == 1, "more or fewer than one category visible");
            say(u"capture group %1: card %2, page content %3"_s.arg(group).arg(card->height()).arg(flick->property("contentHeight").toReal()));
            // Full content for inspecting every control; folded.png above stays at the actual viewport.
            const int viewport = window->height();
            window->setHeight(std::max(viewport, static_cast<int>(std::ceil(flick->property("contentHeight").toReal() + flick->property("y").toReal()))));
            grab(u"capture-group-%1"_s.arg(group));
            window->setHeight(viewport);
            settle(window);
        }
        key(Qt::Key_Right);
        check(card->property("currentGroup").toInt() == 0, "category navigation did not wrap");
        expand->forceActiveFocus(Qt::TabFocusReason);
        key(Qt::Key_Return);
        check(!card->property("expanded").toBool(), "Enter did not fold the capture card");
        QMetaObject::invokeMethod(card, "revealGroup", Q_ARG(QVariant, QVariant(4)));
        settle(window);
        auto* words = visualNamed(card, u"captureSmartWords"_s);
        auto* screenshotGroup = visualNamed(card, u"captureGroup1"_s);
        auto* delay = visualNamed(card, u"captureDelay3"_s);
        if (!words || !screenshotGroup || !delay) {
            qWarning("plugin layout: missing screenshot settings controls");
            return finish(window, false);
        }
        // Switching groups must commit a focused text editor and preserve changed values.
        QMetaObject::invokeMethod(words, "takeFocus");
        words->setProperty("text", u"preview-project,测试客户"_s);
        QMetaObject::invokeMethod(screenshotGroup, "clicked");
        settle(window);
        check(editor->captureSmartWords() == u"preview-project,测试客户", "switching category lost the text edit");
        QMetaObject::invokeMethod(delay, "clicked");
        QMetaObject::invokeMethod(expand, "clicked");
        QMetaObject::invokeMethod(expand, "clicked");
        check(editor->captureDelay() == 3 && card->property("currentGroup").toInt() == 1,
              "folding lost the setting or selected category");
        // Search must reveal the category, and Enter must retain it after clearing the query.
        const char* queries[] = {"PrtScn", "复制截图时同时贴图", "中键", "双击贴图", "智能打码", "工具条"};
        for (int group = 0; group < 6; ++group) {
            const QString query = QCoreApplication::translate("CaptureCard", queries[group]);
            input->setProperty("text", query);
            settle(window);
            say(u"capture search '%1': group %2 (wanted %3)"_s.arg(query).arg(card->property("currentGroup").toInt()).arg(group));
            check(card->property("expanded").toBool(), "search did not expand the capture card");
            check(card->property("currentGroup").toInt() == group, "search showed the wrong category");
            QMetaObject::invokeMethod(window, "goToCurrent");
            settle(window);
            check(window->property("currentPage").toInt() == pluginsPage && window->property("currentTab").toInt() == 0,
                  "search did not land on installed plugins");
            check(!window->property("searching").toBool(), "Enter did not leave search");
            check(card->property("currentGroup").toInt() == group, "leaving search lost the selected category");
        }
        grab(u"capture-search-landed"_s);
        return finish(window, saved);
    }

    if (args.contains(u"--wsl-folders"_s)) {
        bool saved = true;
        const auto check = [](bool condition, const char* message) {
            if (!condition)
                qWarning("WSL folders: %s", message);
        };
        const int count = editor->shares().size();
        check(!editor->addShare(u"\"\\\\wsl$\\Ubuntu-24.04\\home\\me\\项目\""_s).isEmpty(),
            "the second WSL host alias was not recognized as the same folder");
        check(editor->shares().size() == count, "a duplicate WSL folder was added");
        check(editor->addShare(u"\\\\wsl.localhost\\Ubuntu-24.04\\home\\me\\Case"_s).isEmpty(), "adding Case failed");
        check(editor->addShare(u"\\\\wsl.localhost\\Ubuntu-24.04\\home\\me\\case"_s).isEmpty(), "adding case failed");
        check(editor->shares().size() == count + 2, "case-sensitive folders were merged");
        editor->removeShare(count + 1);
        editor->removeShare(count);
        const auto stopped = editor->shares().last().toMap();
        check(stopped.value(u"startable"_s).toBool() && !stopped.value(u"signIn"_s).toBool(), "a stopped WSL folder must offer Start, never Sign in");
        QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(2)), Q_ARG(QVariant, QVariant(3)));
        window->setWidth(args.contains(u"--compact"_s) ? 760 : 920);
        window->setHeight(args.contains(u"--compact"_s) ? 420 : 780);
        settle(window, 3);
        saved = saveGrab(window, out.filePath(u"wsl-stopped.png"_s)) && saved;
        editor->setShareStatuses({
            shareStatus(shareOnline, ShareIndexer::State::Online, share::Problem::None, 48213),
            shareStatus(shareSignIn, ShareIndexer::State::Problem, share::Problem::NeedsSignIn, 0),
            shareStatus(shareWsl, ShareIndexer::State::Online, share::Problem::None, 1234),
        });
        check(!editor->shares().last().toMap().value(u"startable"_s).toBool(), "a running WSL folder still offers Start");
        settle(window, 3);
        saved = saveGrab(window, out.filePath(u"wsl-online.png"_s)) && saved;
        if (args.contains(u"--compact"_s)) {
            flick->setProperty("contentY", std::max(0.0, flick->property("contentHeight").toDouble() - flick->property("height").toDouble()));
            settle(window, 3);
            saved = saveGrab(window, out.filePath(u"wsl-bottom.png"_s)) && saved;
        }
        return finish(window, saved);
    }

    if (args.contains(u"--indexing"_s)) {
        const auto check = [](bool ok, const char* message) {
            if (!ok)
                qWarning("indexing: %s", message);
        };
        SettingsEditor::IndexAccessState direct;
        direct.effective = u"direct"_s;
        direct.elevated = direct.administrator = true;
        direct.foldersPossible = false;
        editor->setIndexAccessState(direct);
        QTemporaryDir folder;
        check(folder.isValid(), "temporary folder was not created");
        check(editor->addFolder(folder.path()).isEmpty(), "adding a folder while elevated was rejected");
        window->resize(args.contains(u"--compact"_s) ? QSize(760, 420) : QSize(920, 720));
        bool saved = true;
        const auto grab = [&](int tab, const QString& name) {
            QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(2)), Q_ARG(QVariant, QVariant(tab)));
            flick->setProperty("contentY", 0);
            settle(window, 3);
            saved = saveGrab(window, out.filePath(name + u"-top.png"_s)) && saved;
            flick->setProperty("contentY", std::max(0.0, flick->property("contentHeight").toDouble() - flick->property("height").toDouble()));
            settle(window, 3);
            saved = saveGrab(window, out.filePath(name + u"-bottom.png"_s)) && saved;
        };
        grab(0, u"folders-admin"_s);
        editor->setAllDrives(false);
        for (const auto& value : editor->drives())
            editor->setDriveSelected(value.toMap().value(u"root"_s).toString(), false);
        check(!editor->allDrives(), "empty drive selection was treated as all drives");
        for (const auto& value : editor->drives())
            check(!value.toMap().value(u"selected"_s).toBool(), "drive remained selected");
        grab(2, u"drives-empty"_s);
        const auto drives = editor->drives();
        if (!drives.isEmpty())
            editor->setDriveSelected(drives.front().toMap().value(u"root"_s).toString(), true);
        editor->setDriveSelected(u"Z:"_s, true); // disconnected selections stay visible and can be removed
        grab(2, u"drives-selected"_s);
        editor->setAllDrives(true);
        check(editor->allDrives(), "restoring all drives failed");
        return finish(window, saved);
    }

    if (args.contains(u"--file-types"_s)) {
        const auto check = [](bool condition, const char* message) {
            if (!condition)
                qWarning("file types: %s", message);
        };
        check(!editor->setFileTypeKeyword(0, u"folder"_s).isEmpty(), "folder prefix collision was accepted");
        check(!editor->setFileTypeKeyword(0, u"two words"_s).isEmpty(), "invalid prefix was accepted");
        check(editor->setFileTypeKeyword(0, u":DOC"_s).isEmpty(), "custom prefix was rejected");
        check(editor->fileTypes().value(0).toMap().value(u"keyword"_s).toString() == u"doc", "prefix was not normalized");
        check(!editor->setFileTypeKeyword(-1, u"doc"_s).isEmpty(), "type prefix collision was accepted");
        check(editor->setFileTypeKeyword(0, {}).isEmpty(), "disabling a prefix failed");
        editor->resetFileTypes();
        QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(3)), Q_ARG(QVariant, QVariant(3)));
        window->resize(args.contains(u"--compact"_s) ? QSize(760, 420) : QSize(920, 720));
        settle(window, 3);
        bool saved = saveGrab(window, out.filePath(u"file-types-top.png"_s));
        flick->setProperty("contentY", std::max(0.0, flick->property("contentHeight").toDouble() - flick->property("height").toDouble()));
        settle(window, 3);
        saved = saveGrab(window, out.filePath(u"file-types-bottom.png"_s)) && saved;
        return finish(window, saved);
    }

    // Every page, every tab: the window as tall as the page, so the
    // picture has all of it.
    bool saved = true;
    bool onePage = false;
    const int wantedPage = valueAfter(args, u"--page").toInt(&onePage);
    for (int page = 0; page < pages.size(); ++page) {
        if (args.contains(u"--command-form"_s))
            break;
        if (onePage && page != wantedPage)
            continue;
        const QVariantList pageTabs = tabs.value(page).toList();
        const int count = std::max<int>(1, static_cast<int>(pageTabs.size()));
        for (int tab = 0; tab < count; ++tab) {
            const QString title = pages[page].toMap().value(u"title"_s).toString() + (pageTabs.isEmpty() ? QString() : u" / "_s + pageTabs[tab].toString());
            QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(page)), Q_ARG(QVariant, QVariant(tab)));
            if (page == pluginsPage && tab == 0) { // the cards come folded: one unfolded too (翻译: settings, permissions)
                if (QObject* section = named(window, "pluginsSection"))
                    QMetaObject::invokeMethod(section, "expandAt", Q_ARG(QVariant, QVariant(1)));
                if (auto* paste = window->findChild<QQuickItem*>(u"pasteFileSettingsCard"_s))
                    paste->setProperty("expanded", true);
                else
                    qWarning("preview: no 粘贴为文件 card on the 插件 page");
            }
            window->setHeight(baseHeight);
            settle(window, 3);
            int height = baseHeight;
            if (page != gleaningPage) { // the warm page is one screen of its own
                const int needed = static_cast<int>(std::ceil(flick->property("contentHeight").toReal() + flick->property("y").toReal()));
                height = std::max(baseHeight, needed);
                window->setHeight(height);
                settle(window, 3);
            } else {
                pump(3600); // the avatar's greeting (AuthorClock) has played out
            }
            pump(400); // the sliding marker, the asynchronous icons
            const QString file = out.filePath(u"p%1-t%2.png"_s.arg(page, 2, 10, u'0').arg(tab));
            say(u"page %1 tab %2 (%3): window %4x%5 asked %6, current %7/%8, content %9"_s.arg(page).arg(tab).arg(title)
                    .arg(window->width()).arg(window->height()).arg(height).arg(window->property("currentPage").toInt())
                    .arg(window->property("currentTab").toInt()).arg(flick->property("contentHeight").toReal()));
            if (window->property("currentPage").toInt() != page || window->property("currentTab").toInt() != tab)
                qWarning("preview: showPage(%d, %d) did not land there", page, tab);
            if (window->height() != height)
                qWarning("preview: the window is %d high, not %d", window->height(), height);
            if (tree)
                writeTree(window, file + u".tree.txt"_s);
            saved = saveGrab(window, file) && saved;
            if (page == 3 && tab == 1) {
                QObject* choice = visualNamed(window->contentItem(), u"favoriteFolderTabChoice"_s);
                if (!choice) {
                    qWarning("preview: favorite folder Tab choice missing");
                } else {
                    for (const bool enabled : {true, false}) {
                        if (!QMetaObject::invokeMethod(choice, "activated", Q_ARG(int, enabled ? 1 : 0)))
                            qWarning("preview: favorite folder Tab choice could not be activated");
                        settle(window, 2);
                        Settings read;
                        read.load();
                        if (editor->tabEntersFavoriteFolder() != enabled || read.tabEntersFavoriteFolder != enabled
                            || choice->property("current").toInt() != int(enabled))
                            qWarning("preview: favorite folder Tab choice did not save or update its binding");
                    }
                    say(u"favorite folder Tab setting: real QML control, editor and isolated settings round trip checked"_s);
                }
                QObject* sizeToggle = visualNamed(window->contentItem(), u"showFileSizeToggle"_s);
                if (!sizeToggle) {
                    qWarning("preview: file size toggle missing");
                } else {
                    for (const bool enabled : {true, false}) {
                        if (!QMetaObject::invokeMethod(sizeToggle, "toggled", Q_ARG(bool, enabled)))
                            qWarning("preview: file size toggle could not be activated");
                        settle(window, 2);
                        Settings read;
                        read.load();
                        if (editor->showFileSize() != enabled || read.showFileSize != enabled
                            || sizeToggle->property("checked").toBool() != enabled)
                            qWarning("preview: file size toggle did not save or update its binding");
                    }
                    say(u"file size setting: real QML control, editor and isolated settings round trip checked"_s);
                }
            }
        }
    }

    // A favorite's form open, as the launcher's 编辑收藏 opens it.
    if (!onePage) {
        QMetaObject::invokeMethod(window, "showFavoritesPage", Q_ARG(QVariant, QVariant(favorites.items().constFirst().path)));
        window->setHeight(baseHeight);
        settle(window, 3);
        pump(300);
        if (window->property("currentPage").toInt() != favoritesPage)
            qWarning("preview: showFavoritesPage() did not land on the 收藏夹 page");
        saved = saveGrab(window, out.filePath(u"favorite-form.png"_s)) && saved;
        QMetaObject::invokeMethod(window, "showFavoritesPage", Q_ARG(QVariant, QVariant(favorites.items().constLast().path)));
        settle(window, 3);
        pump(300);
        saved = saveGrab(window, out.filePath(u"command-form.png"_s)) && saved;
        flick->setProperty("contentY", std::max(0.0, flick->property("contentHeight").toDouble()
            - flick->property("height").toDouble()));
        settle(window, 3);
        saved = saveGrab(window, out.filePath(u"command-form-bottom.png"_s)) && saved;
        if (args.contains(u"--command-form"_s)) {
            if (QObject* section = named(window, "favoritesSection")) {
                section->setProperty("editing", -1);
                section->setProperty("addingCommand", true);
                flick->setProperty("contentY", 0);
                settle(window, 3);
                saved = saveGrab(window, out.filePath(u"command-new.png"_s)) && saved;
                QObject* form = visualNamed(qobject_cast<QQuickItem*>(section), u"favoriteForm"_s);
                QObject* name = form ? form->findChild<QObject*>(u"favoriteName"_s) : nullptr;
                QObject* keyword = form ? form->findChild<QObject*>(u"favoriteKeyword"_s) : nullptr;
                QObject* commandBox = form ? form->findChild<QObject*>(u"favoriteCommand"_s) : nullptr;
                if (name && keyword && commandBox) {
                    name->setProperty("text", u"Preview command"_s);
                    keyword->setProperty("text", u"preview-command"_s);
                    commandBox->setProperty("text", u"notepad.exe {query}"_s);
                    saved = previewCommandIcons(window, editor, favorites, section, out) && saved;
                    section->setProperty("addingCommand", true);
                    settle(window, 3);
                    form = visualNamed(qobject_cast<QQuickItem*>(section), u"favoriteForm"_s);
                    name = form ? form->findChild<QObject*>(u"favoriteName"_s) : nullptr;
                    keyword = form ? form->findChild<QObject*>(u"favoriteKeyword"_s) : nullptr;
                    commandBox = form ? form->findChild<QObject*>(u"favoriteCommand"_s) : nullptr;
                    if (!name || !keyword || !commandBox)
                        return finish(window, false);
                    name->setProperty("text", u"Preview command"_s);
                    keyword->setProperty("text", u"preview-command"_s);
                    commandBox->setProperty("text", u"notepad.exe {query}"_s);
                    QMetaObject::invokeMethod(form, "save");
                    if (favorites.items().size() != 5 || section->property("addingCommand").toBool())
                        qWarning("preview: form did not save and close");
                    else
                        favorites.remove(favorites.items().constLast().path);
                } else {
                    qWarning("preview: command form fields missing");
                }
                section->setProperty("addingCommand", false);
                if (favorites.items().size() != 4)
                    qWarning("preview: cancelling the form added a command");
            } else {
                qWarning("preview: favorites section missing");
            }
            return finish(window, saved);
        }
    }

    // The settings search: a word typed, the page it narrows to pictured,
    // then Enter (goToCurrent), which must land on the right page and tab.
    struct Probe {
        QString word;
        int page;
        int tab;
    };
    const QString quickPasteProbe = args.contains(u"--english"_s)
        ? QCoreApplication::translate("SettingsWindow", "不打开面板直接粘贴") : u"无需面板"_s;
    const Probe probes[] = {
        {quickPasteProbe, window->property("clipboardPage").toInt(), 1},
        {u"插件"_s, pluginsPage, 0},
        {u"插件库"_s, pluginsPage, 1},
        {u"消息记录"_s, pluginsPage, 2}, // the 开发者 tab (developer mode is on in the preview)
        {u"API 密钥"_s, pluginsPage, 0}, // a plugin's own setting: its card unfolds (search-API 密钥.png)
        {u"粘贴为文件"_s, pluginsPage, 0}, // a built-in card
        // Shown as 内置功能 cards too, the rows of their own page first (open_settings searches them by title).
        {u"双击空白处弹出文件夹菜单"_s, 0, 2},
        {u"对话框旁的搜索框"_s, 0, 2},
        {u"AI"_s, aiPage, 0},
        {u"收藏"_s, favoritesPage, 0},
        {QCoreApplication::translate("SettingsWindow", "网络与 WSL 文件夹"), 2, 3},
        {QCoreApplication::translate("SettingsWindow", "排序"), 3, 0},
        {QCoreApplication::translate("SettingsWindow", "显示修改日期"), 3, 1},
        {QCoreApplication::translate("SettingsWindow", "显示文件大小"), 3, 1},
        {QCoreApplication::translate("SettingsWindow", "选中收藏文件夹时按 Tab"), 3, 1},
        {QCoreApplication::translate("SettingsWindow", "每次打开时清空搜索"), 3, 2},
        // "File types" also matches preferred types in English; its visible prefix is unambiguous.
        {args.contains(u"--english"_s) ? u":folder"_s : u"文件类型"_s, 3, 3},
    };
    QMetaObject::invokeMethod(window, "showPage", Q_ARG(QVariant, QVariant(0)), Q_ARG(QVariant, QVariant(0)));
    window->setHeight(baseHeight);
    settle(window, 2);
    for (const Probe& probe : probes) {
        if (onePage && probe.page != wantedPage)
            continue;
        input->setProperty("text", probe.word);
        settle(window, 3);
        pump(300);
        QObject* current = settingsSearch->property("current").value<QObject*>();
        const int total = settingsSearch->property("total").toInt();
        const QString rowTitle = current ? current->property("title").toString() : u"(none)"_s;
        QObject* section = current ? current->property("section").value<QObject*>() : nullptr;
        const int sectionPage = section ? section->property("page").toInt() : -1;
        const int sectionTab = section ? section->property("tab").toInt() : -1;
        say(u"search '%1': %2 row(s), best '%3' on page %4 tab %5, searching %6"_s.arg(probe.word).arg(total).arg(rowTitle)
                .arg(sectionPage).arg(sectionTab).arg(window->property("searching").toBool()));
        saved = saveGrab(window, out.filePath(u"search-%1.png"_s.arg(probe.word))) && saved;
        QMetaObject::invokeMethod(window, "goToCurrent");
        pump(100);
        const int landedPage = window->property("currentPage").toInt();
        const int landedTab = window->property("currentTab").toInt();
        say(u"search '%1': Enter went to page %2 tab %3 (%4), searching %5"_s.arg(probe.word).arg(landedPage).arg(landedTab)
                .arg(pages.value(landedPage).toMap().value(u"title"_s).toString()).arg(window->property("searching").toBool()));
        if (total == 0 || !current)
            qWarning("preview: the search for '%s' found nothing", qUtf8Printable(probe.word));
        else if (landedPage != probe.page || landedTab != probe.tab)
            qWarning("preview: '%s' went to page %d tab %d, not %d/%d", qUtf8Printable(probe.word), landedPage, landedTab, probe.page, probe.tab);
        if (window->property("searching").toBool())
            qWarning("preview: still searching after Enter on '%s'", qUtf8Printable(probe.word));
        input->setProperty("text", QString());
        settle(window, 2);
    }
    return finish(window, saved);
}

// The production delegates and model, with made-up folders and local Qt key
// events: no global shortcuts, file opening, or user's profile involved.
void checkLauncherFilters(QQuickWindow* window)
{
    QTemporaryDir temporary;
    IndexService index(temporary.path(), IndexService::Options{});
    AppCatalog apps;
    SystemCatalog places;
    apps.setApps(std::make_shared<const AppList>());
    places.setPlaces(std::make_shared<const PlaceList>());
    History history(temporary.filePath(u"history.tsv"_s));
    SearchEngine engine(&index, &apps, &places);
    Launcher launcher(&index, &apps, &places, &engine, &history);
    launcher.setWindow(window);
    Settings settings;
    settings.fileTypes = Settings::defaultFileTypes();
    launcher.setFileTypes(settings.namedFileTypes());
    launcher.setContentOptions(false, {u"txt"_s}, ContentSizeLimits{}, false, false);
    {
        const auto files = index.index();
        const auto lock = files->writeLock();
        const auto root = files->addRoot("T:");
        files->add(root, "报告资料", EntryFlag::Directory);
        const auto report = files->add(root, "报告图片.png", 0);
        files->setSize(report, packSize(1234567));
        const auto empty = files->add(root, "照片.png", 0);
        files->setSize(empty, packSize(0));
        files->add(root, "报告.txt", 0);
    }
    const auto check = [](bool condition, const char* reason) {
        if (!condition)
            qWarning("launcher regression: %s", reason);
    };
    // #80: applying a preference updates the existing controller immediately,
    // independently of dates and without resetting the result model.
    check(!launcher.showFileSize(), "file size should be off by default");
    int optionChanges = 0;
    QObject::connect(&launcher, &Launcher::resultOptionsChanged, &launcher, [&] { ++optionChanges; });
    for (const bool enabled : {true, false}) {
        launcher.setResultOptions(KindOrder::FilesFirst, true, enabled, true, u"ctrl"_s);
        check(launcher.showFileSize() == enabled && launcher.showModified(), "file size preference did not apply independently");
        const int before = optionChanges;
        launcher.setResultOptions(KindOrder::FilesFirst, true, enabled, true, u"ctrl"_s);
        check(optionChanges == before, "unchanged preferences signalled again");
    }
    check(optionChanges == 2, "file size changes did not notify existing delegates");
    const auto query = [&](const QString& text) {
        launcher.setQuery(text);
        QElapsedTimer timeout;
        timeout.start();
        while (launcher.searching() && timeout.elapsed() < 2000)
            pump(10);
        check(!launcher.searching(), "search did not settle");
    };
    query(u":folder 报告"_s);
    check(launcher.scope() == Launcher::Folders && launcher.results()->count() == 1
        && launcher.results()->at(0)->isDir, "folder prefix did not reach the search engine");
    query(u":pictures 报告"_s);
    check(launcher.scope() == Launcher::Files && launcher.fileType() == 1 && launcher.results()->count() == 1
        && launcher.results()->at(0)->name.endsWith(u".png"), "type prefix did not filter extensions");
    check(launcher.results()->at(0) && launcher.results()->at(0)->size == 1234567
        && !launcher.results()->data(launcher.results()->index(0), ResultModel::SizeRole).toString().isEmpty(),
        "ordinary search lost the indexed file size");
    check(launcher.query() == u":pictures 报告", "typed prefix disappeared from the field");
    query(u":pictures"_s);
    check(launcher.results()->count() == 2, "prefix alone showed history instead of browsing the type");
    const int emptyRow = launcher.results()->indexOf(u"T:\\照片.png"_s);
    check(emptyRow >= 0 && launcher.results()->at(emptyRow)->size == 0
        && !launcher.results()->data(launcher.results()->index(emptyRow), ResultModel::SizeRole).toString().isEmpty(),
        "zero bytes were treated as unknown");
    query({});
    check(launcher.scope() == Launcher::All, "removing a prefix did not restore the original scope");
    query(u":folder 报告"_s);
    launcher.chooseFileType(0);
    check(launcher.query() == u"报告" && launcher.fileType() == 0 && launcher.scope() == Launcher::Files,
        "a manual type choice did not replace the prefix");
    struct SortCase { Launcher::Sort sort; SortKey key; bool descending; };
    const SortCase sorts[]{{Launcher::BestMatch, SortKey::Rank, false}, {Launcher::NewestFirst, SortKey::Modified, true},
        {Launcher::OldestFirst, SortKey::Modified, false}, {Launcher::ByName, SortKey::Name, false},
        {Launcher::ByFolder, SortKey::Path, false}, {Launcher::ByType, SortKey::Type, false},
        {Launcher::LargestFirst, SortKey::Size, true}, {Launcher::SmallestFirst, SortKey::Size, false}};
    for (const auto& test : sorts) {
        launcher.setPanel(false);
        launcher.setSort(test.sort);
        launcher.setPanel(true);
        check(launcher.panelSortKey() == int(test.key) && launcher.panelSortDescending() == test.descending,
            "expanding the panel lost the compact list's sort");
    }
    // #81: the actual controller, hint, completion and menu paths, without
    // touching the running user's launcher or opening anything in the shell.
    launcher.setPanel(false);
    QDir(temporary.path()).mkpath(u"favorite"_s);
    QDir(temporary.path()).mkpath(u"ordinary"_s);
    const QString favoritePath = QDir::toNativeSeparators(temporary.filePath(u"favorite"_s));
    const QString ordinaryPath = QDir::toNativeSeparators(temporary.filePath(u"ordinary"_s));
    SearchResult folder {.name = u"Favorite"_s, .path = favoritePath, .isDir = true, .favorite = true};
    const auto showRow = [&](const SearchResult& row) {
        launcher.setScope(Launcher::All);
        query({});
        launcher.results()->assign({row}, {});
        launcher.setCurrentRow(-1);
        launcher.setCurrentRow(0);
    };
    const auto tabHint = [&] { return launcher.keyHints().value(u"tab"_s).toString(); };
    const QString scopeHint = QCoreApplication::translate("ws::Launcher", "切换范围");
    const QString folderHint = QCoreApplication::translate("ws::Launcher", "进入文件夹");
    const auto hasBrowseAction = [&] {
        const QVariantList menu = launcher.menuItems(0);
        return std::ranges::any_of(menu, [](const QVariant& entry) {
            const QVariantMap item = entry.toMap();
            return item.contains(u"action"_s) && item.value(u"action"_s).toInt() == Launcher::BrowseFolder;
        });
    };
    showRow(folder);
    check(tabHint() == scopeHint && !launcher.autocomplete(0) && launcher.query().isEmpty(),
        "favorite folder stole Tab with the default setting");
    launcher.cycleScope(1);
    check(launcher.scope() == Launcher::Files, "Tab did not advance the scope on a favorite folder");
    launcher.cycleScope(-1);
    check(launcher.scope() == Launcher::All, "Shift+Tab did not reverse the scope");
    showRow(folder);
    launcher.setTabEntersFavoriteFolder(true);
    check(tabHint() == folderHint, "changing the Tab setting did not update the footer immediately");
    check(launcher.autocomplete(0) && launcher.query() == favoritePath + u'\\',
        "opted-in Tab did not browse the favorite folder");
    folder.favorite = false;
    folder.path = ordinaryPath;
    showRow(folder);
    check(tabHint() == scopeHint && !launcher.autocomplete(0) && hasBrowseAction(),
        "an ordinary folder stole Tab or lost its independent browse action");
    launcher.setScope(Launcher::Folders);
    query({});
    launcher.results()->assign({folder}, {});
    launcher.trigger(0, Launcher::BrowseFolder);
    check(launcher.scope() == Launcher::All && launcher.query() == ordinaryPath + u'\\',
        "the folder menu did not browse inside WinShun or retained a folders-only filter");
    folder.favorite = true;
    folder.offline = true;
    showRow(folder);
    check(!launcher.autocomplete(0) && !hasBrowseAction(), "an offline folder can be browsed");
    folder.offline = false;
    folder.missing = true;
    showRow(folder);
    check(!launcher.autocomplete(0) && !hasBrowseAction(), "a missing folder can be browsed");
    folder.missing = false;
    showRow(folder);
    launcher.setTabEntersFavoriteFolder(false);
    check(tabHint() == scopeHint && !launcher.autocomplete(0), "turning the Tab setting off left stale behavior or hints");
    SearchResult suggestion {.name = u"Search suggestion"_s, .app = AppKind::Web,
        .keyword = u"example"_s, .words = u"report"_s, .suggestion = true};
    showRow(suggestion);
    check(launcher.autocomplete(0) && launcher.query() == u"example report", "web suggestion completion changed");
    query({}); // let the last search settle before its controller and engine go away
    say(u"launcher prefix, panel-sort and favorite Tab regressions: real controllers/engine over isolated fixtures"_s);
}

int runResults(const QStringList& args)
{
    const QDir out(valueAfter(args, kResultsFlag));
    QDir().mkpath(out.path());
    const QString baseline = valueAfter(args, u"--rows-source");
    SystemTheme::setUiSize(valueAfter(args, u"--size", u"standard"_s));
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);
    QFont font = QGuiApplication::font();
    font.setStyleStrategy(QFont::NoSubpixelAntialias);
    QGuiApplication::setFont(font);
    QQmlApplicationEngine engine;
    engine.addImageProvider(u"fileicon"_s, new FileIconProvider);
    ResultModel results;
    results.setThumbnails(false); // synthetic paths: no thumbnail handler or real file access
    SearchResults rows;
    for (const QString& name : {u"设计"_s, u"图纸"_s, u"照片"_s, u"合同"_s, u"资料"_s, u"归档"_s, u"参考"_s, u"备份"_s}) {
        SearchResult row;
        row.name = name;
        row.path = u"F:\\项目资料\\2026\\客户项目\\工程设计与交付文档\\机械设备\\最终确认版本\\"_s + name;
        row.isDir = true;
        row.recent = true;
        row.modified = static_cast<std::uint32_t>(QDateTime::currentSecsSinceEpoch() - 86400);
        rows.push_back(row);
    }
    results.assign(std::move(rows), {u"项目资料"_s});
    engine.rootContext()->setContextProperty(u"previewResults"_s, &results);
    QString qml = uR"(
        import QtQuick
        import WinShun
        %1
        Window {
            id: window
            width: 800; height: 60 + 8 * Theme.rowHeight
            color: Theme.background
            property alias hints: hints
            property alias field: field
            property alias resultList: resultList
            property bool sizeOrder: false
            property bool showFileSize: false
            property bool showDate: true
            Binding { target: previewResults; property: "highlightColor"; value: Theme.accent }
            NumberHints { id: hints; key: Qt.Key_Control; listening: true }
            TextInput {
                id: field
                x: 20; y: 16; width: parent.width - 40
                text: "文件夹 · 可读性预览"; color: Theme.text; font.pixelSize: Theme.uiTitle
                focus: true; cursorVisible: false
                Keys.onPressed: event => hints.press(event)
                Keys.onReleased: event => hints.release(event)
            }
            ListView {
                id: resultList
                y: 60; width: parent.width; height: 8 * Theme.rowHeight
                model: previewResults; currentIndex: -1
                delegate: %2ResultRow {
                    objectName: "metadataRow-" + index
                    width: resultList.width; height: Theme.rowHeight
                    hint: index + 1; hintTaken: index < 3
                    %3
                }
            }
        }
    )"_s.arg(baseline.isEmpty() ? QString() : u"import \"%1\" as Baseline"_s.arg(QUrl::fromLocalFile(baseline).toString()),
             baseline.isEmpty() ? QString() : u"Baseline."_s,
             baseline.isEmpty() ? u"hintOpacity: hints.hintOpacity; showSize: window.showFileSize || window.sizeOrder; showModified: window.showDate"_s : QString());
    QQmlComponent component(&engine);
    component.setData(qml.toUtf8(), QUrl(u"qrc:/results-preview.qml"_s));
    auto* window = qobject_cast<QQuickWindow*>(component.create());
    if (!window) {
        say(component.errorString());
        return 3;
    }
    QTimer watchdog;
    armWatchdog(watchdog, 45);
    showOffScreen(window);
    settle(window);
    pump(500);
    auto* hints = window->property("hints").value<QObject*>();
    auto* field = window->property("field").value<QObject*>();
    bool saved = true;
    const auto grab = [&](const QString& name) {
        saved = saveGrab(window, out.filePath(name + u".png"_s)) && saved;
    };
    const auto key = [&](QEvent::Type type) {
        QKeyEvent event(type, Qt::Key_Control, Qt::ControlModifier);
        QCoreApplication::sendEvent(field, &event);
    };
    hints->setProperty("mode", u"always"_s);
    pump(150);
    grab(u"always"_s);
    if (!baseline.isEmpty())
        return finish(window, saved);
    hints->setProperty("mode", u"held"_s);
    pump(100);
    grab(u"held-idle"_s);
    key(QEvent::KeyPress);
    pump(100);
    grab(u"held-100ms"_s);
    pump(200);
    grab(u"held-300ms"_s);
    if (!hints->property("shown").toBool())
        qWarning("preview: held Ctrl did not show hints");
    key(QEvent::KeyRelease);
    // Off-screen windows do not get every display frame: finish the fade
    // with explicit frames before recording its settled state.
    QElapsedTimer fade;
    fade.start();
    do {
        pump(20);
        (void)window->grabWindow();
    } while (hints->property("hintOpacity").toReal() > 0 && fade.elapsed() < 500);
    settle(window, 2); // the GPU grab can still hold the preceding frame
    grab(u"held-released"_s);
    for (int tap = 0; tap < 2; ++tap) {
        key(QEvent::KeyPress);
        pump(40);
        key(QEvent::KeyRelease);
        pump(40);
        if (hints->property("hintOpacity").toReal() != 0)
            qWarning("preview: quick Ctrl tap flashed hints");
    }
    grab(u"quick-taps"_s);
    hints->setProperty("mode", u"off"_s);
    key(QEvent::KeyPress);
    pump(300);
    grab(u"off"_s);
    key(QEvent::KeyRelease);
    if (args.contains(u"--file-metadata"_s)) {
        checkLauncherFilters(window);
        SearchResults files;
        const QDateTime when(QDate(2024, 3, 5), QTime(14, 32));
        const QDateTime now(QDate(2026, 10, 25), QTime(15, 0));
        const auto stamp = static_cast<std::uint32_t>(when.toSecsSinceEpoch());
        const QString withTime = ResultModel::modifiedText(stamp, now, true);
        const QString withoutTime = ResultModel::modifiedText(stamp, now, false);
        if (withTime == withoutTime || !withTime.contains(QLocale().toString(when.time(), QLocale::ShortFormat)))
            qWarning("preview: older dates lost their modification time");
        for (int i = 0; i < 8; ++i) {
            SearchResult file;
            file.name = u"项目交付与验收报告-%1.pdf"_s.arg(i + 1);
            file.path = u"F:\\项目资料\\2026\\"_s + file.name;
            file.modified = stamp;
            file.size = (8 - i) * 1234567;
            files.append(file);
        }
        results.assign(std::move(files), {});
        results.setShowModifiedTime(true);
        field->setProperty("text", u"文件修改时间"_s);
        settle(window);
        grab(u"date-time"_s);
        window->setProperty("showFileSize", true);
        field->setProperty("text", u"文件大小与修改时间"_s);
        settle(window);
        grab(u"size-and-date"_s);
        window->setProperty("showDate", false);
        field->setProperty("text", u"只显示文件大小"_s);
        settle(window);
        grab(u"size-only"_s);
        window->setProperty("showFileSize", false);
        window->setProperty("sizeOrder", true);
        field->setProperty("text", u"按大小排序"_s);
        settle(window);
        grab(u"size-order"_s);

        // Real delegates/model over edge fixtures: unknown is blank, zero is
        // not, and folders/apps never pretend to have an aggregate size.
        SearchResults edges;
        for (int i = 0; i < 8; ++i)
            edges.append(*results.at(i));
        edges[1].size = 0;
        edges[2].size = -1;
        edges[3].isDir = true;
        edges[4].app = AppKind::Desktop;
        edges[5].modified = 0;
        edges[6].line = 12;
        edges[6].snippet = u"在文档中找到的内容"_s;
        edges[7].name = u"名称很长的项目交付与验收报告-最终确认版本-v3.pdf"_s;
        results.assign(std::move(edges), {});
        auto* list = window->property("resultList").value<QObject*>();
        struct MetadataMode { bool size; bool date; bool sort; };
        const MetadataMode modes[]{{false, true, false}, {true, true, false}, {true, false, false},
            {false, false, false}, {false, false, true}, {false, true, true}};
        for (const int width : {640, 760}) {
            window->setWidth(width);
            for (const auto mode : modes) {
                window->setProperty("showFileSize", mode.size);
                window->setProperty("showDate", mode.date);
                window->setProperty("sizeOrder", mode.sort);
                for (const int current : {-1, 0}) {
                    list->setProperty("currentIndex", current);
                    settle(window, 2);
                    for (int i = 0; i < results.count(); ++i) {
                        QQuickItem* row = visualNamed(window->contentItem(), u"metadataRow-%1"_s.arg(i));
                        QQuickItem* size = row ? visualNamed(row, u"fileSizeLabel"_s) : nullptr;
                        QQuickItem* date = row ? visualNamed(row, u"modifiedLabel"_s) : nullptr;
                        QQuickItem* meta = row ? visualNamed(row, u"resultMetadata"_s) : nullptr;
                        QQuickItem* text = row ? visualNamed(row, u"resultText"_s) : nullptr;
                        if (!size || !date || !meta || !text) {
                            qWarning("preview: missing metadata delegate %d", i);
                            continue;
                        }
                        const QString sizeText = results.data(results.index(i), ResultModel::SizeRole).toString();
                        const QString dateText = results.data(results.index(i), ResultModel::ModifiedRole).toString();
                        const bool sizeVisible = (mode.size || mode.sort) && !sizeText.isEmpty();
                        const bool dateVisible = mode.date && !dateText.isEmpty();
                        if (size->isVisible() != sizeVisible || date->isVisible() != dateVisible
                            || size->property("text").toString() != sizeText || date->property("text").toString() != dateText)
                            qWarning("preview: wrong metadata for row %d", i);
                        if (meta->isVisible() && (text->width() < 0 || text->x() + text->width() + 15 > meta->x()
                            || meta->x() + meta->width() > row->width()))
                            qWarning("preview: metadata overlaps text or leaves row %d", i);
                        if (sizeVisible && dateVisible && size->y() + size->height() > date->y())
                            qWarning("preview: size and date overlap in row %d", i);
                    }
                }
            }
            grab(u"metadata-edges-%1"_s.arg(width));
        }
        say(u"file metadata: real delegates/model, six display combinations at 640/760px, selected/idle, zero/unknown/folder/app/content checked"_s);
    }
    say(u"renderer D3D11, DPR %1; Qt events only, not a global double-Ctrl gesture"_s.arg(window->devicePixelRatio()));
    return finish(window, saved);
}

int runCaptureTools(const QStringList& args)
{
    using namespace capture;
    using namespace capture::annot;
    const QDir out(valueAfter(args, kCaptureToolsFlag));
    QDir().mkpath(out.path());
    QImage base(900, 400, QImage::Format_RGB32);
    base.fill(Qt::white);
    {
        QPainter painter(&base);
        painter.setPen(QColor(35, 40, 45));
        QFont font = QGuiApplication::font();
        font.setPixelSize(24);
        painter.setFont(font);
        painter.drawText(40, 48, u"截图标注 · 连续折线、贝塞尔曲线与荧光笔"_s);
        font.setPixelSize(18);
        painter.setFont(font);
        painter.drawText(40, 98, u"矩形荧光笔覆盖一行文字，黑色正文仍然清晰可读。"_s);
    }
    AnnotationModel model;
    model.begin(base, 1, StyleMemory{});
    model.setSelection(base.rect());
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(u"previewAnnotation"_s, &model);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import WinShun
        Window {
            id: window
            width: 940; height: 530
            color: Theme.background
            Item {
                x: 20; y: 20; width: 900; height: 400
                AnnotationCanvas { anchors.fill: parent; model: previewAnnotation; sparse: false }
                AnnotateObjects { anchors.fill: parent; annot: previewAnnotation; keyTarget: parent }
            }
            AnnotateStylePanel { x: 20; y: 450; annot: previewAnnotation }
        }
    )", QUrl(u"qrc:/capture-tools-preview.qml"_s));
    auto* window = qobject_cast<QQuickWindow*>(component.create());
    if (!window) {
        say(component.errorString());
        return 3;
    }
    showOffScreen(window);
    const auto tool = [&](Kind kind) {
        for (qsizetype i = 0; i < tools().size(); ++i) {
            if (tools()[i].kind == kind)
                model.setTool(int(i));
        }
    };
    const auto press = [&](QPoint point) {
        if (!model.pressFirst(point, {}))
            model.pressSecond(point, {});
    };
    const auto drag = [&](QPoint from, QPoint to) {
        press(from);
        model.move(to, {});
        model.release(to);
    };
    bool saved = true;
    const auto grab = [&](QStringView name) {
        settle(window);
        pump(150);
        saved = saveGrab(window, out.filePath(name.toString() + u".png"_s)) && saved;
    };
    tool(Kind::Arrow);
    model.setPanel({{u"lineMode"_s, u"bezier"_s}});
    drag({410, 180}, {760, 180});
    grab(u"bezier");
    tool(Kind::Line);
    model.setPanel({{u"lineMode"_s, u"polyline"_s}});
    for (QPoint p : {QPoint(60, 200), QPoint(200, 200), QPoint(200, 300), QPoint(300, 300)}) {
        press(p);
        model.release(p);
    }
    model.finishPath();
    grab(u"polyline");
    tool(Kind::Marker);
    model.setPanel({{u"markerMode"_s, u"rectangle"_s}, {u"color"_s, u"#FFFF00"_s}});
    drag({36, 75}, {755, 108});
    grab(u"marker-rectangle");
    model.setPanel({{u"markerMode"_s, u"line"_s}});
    drag({410, 330}, {760, 330});
    grab(u"marker-line");
    return finish(window, saved);
}

// ---- --preview-ai-cards -----------------------------------------------------------------

// The AI settings screens (SettingsConfirm.qml, SettingsNotice.qml) over
// made-up changes: the card asking, once with the "from now on" box and once
// without it (a security setting in the batch, a long explanation, a long list),
// the notice after a change, and after 撤销.
int runAiCards(const QStringList& args)
{
    const QDir out(valueAfter(args, kAiCardsFlag));
    QQmlApplicationEngine engine;
    const auto change = [](const QString& title, const QString& oldText, const QString& newText, bool askAlways = false,
                            const QString& diff = {}, const QString& effect = u"now"_s) {
        return QVariantMap{{u"key"_s, u"Preview/"_s + title}, {u"title"_s, title}, {u"old_text"_s, oldText}, {u"new_text"_s, newText},
            {u"diff_text"_s, diff}, {u"is_list"_s, !diff.isEmpty()}, {u"ask_always"_s, askAlways}, {u"changed"_s, true}, {u"effect"_s, effect}};
    };
    const QVariantList few {
        change(u"启动器热键"_s, u"双击 Ctrl"_s, u"Alt+Space"_s),
        change(u"搜索结果排序"_s, u"最匹配"_s, u"最近修改"_s),
        change(u"不索引的文件夹"_s, {}, {}, false, u"+ node_modules, .git / − Downloads"_s),
    };
    QVariantList many = few;
    many << change(u"开机自启"_s, u"关"_s, u"开"_s, true)
         << change(u"用 Win+V 打开剪贴板历史"_s, u"关"_s, u"开"_s, false, {}, u"explorer_restart"_s)
         << change(u"剪贴板保留条数"_s, u"1000"_s, u"5000"_s)
         << change(u"网站搜索"_s, {}, {}, true, u"+ gh（GitHub 搜索）"_s);

    bool saved = true;
    const auto card = [&](const QString& name, const QString& reason, const QVariantList& items, bool askAlways) {
        auto* frame = new WindowFrame(false, &engine);
        QQmlComponent component(&engine, u"WinShun"_s, u"SettingsConfirm"_s);
        QObject* object = component.createWithInitialProperties({{u"frame"_s, QVariant::fromValue(frame)}, {u"client"_s, u"Claude Desktop"_s},
            {u"reason"_s, reason}, {u"items"_s, items}, {u"askAlways"_s, askAlways}});
        auto* window = qobject_cast<QQuickWindow*>(object);
        if (!window) {
            say(component.errorString());
            delete object;
            saved = false;
            return;
        }
        showOffScreen(window);
        settle(window);
        pump(300);
        saved = saveGrab(window, out.filePath(name)) && saved;
        window->hide();
        delete window;
    };
    card(u"confirm.png"_s, u"你说想换一个顺手的热键，并且别让下载文件夹出现在搜索里。"_s, few, false);
    card(u"confirm-always.png"_s,
        u"用户说这台电脑常处理代码，希望开机就能用，剪贴板也想用 Win+V；顺便把常用的几项都调好：排序、热键、不索引的文件夹，以及网站搜索里加 GitHub。这一句故意写得很长，看看超过三行时会不会被截断，而不是把窗口撑得很高。"_s,
        many, true);

    const auto notice = [&](const QString& name, bool undone) {
        auto* frame = new WindowFrame(false, &engine);
        auto* host = new QObject(&engine);
        QQmlComponent component(&engine, u"WinShun"_s, u"SettingsNotice"_s);
        QObject* object = component.createWithInitialProperties({{u"frame"_s, QVariant::fromValue(frame)}, {u"host"_s, QVariant::fromValue(host)}});
        auto* window = qobject_cast<QQuickWindow*>(object);
        if (!window) {
            say(component.errorString());
            delete object;
            saved = false;
            return;
        }
        showOffScreen(window);
        const QVariantMap batch {{u"id"_s, 1}, {u"client"_s, u"Claude Desktop"_s}, {u"count"_s, 6},
            {u"explorerRestart"_s, true}, {u"items"_s, many.mid(0, 6)}};
        QMetaObject::invokeMethod(window, "present", Q_ARG(QVariant, batch));
        if (undone) {
            window->setProperty("outcome", u"已撤销"_s);
            window->setProperty("outcomeOk", true);
        }
        settle(window);
        pump(300);
        saved = saveGrab(window, out.filePath(name)) && saved;
        window->hide();
        delete window;
    };
    notice(u"notice.png"_s, false);
    notice(u"notice-undone.png"_s, true);
    QDir(Settings::dataDir()).removeRecursively();
    say(u"%1 warning(s)"_s.arg(g_warnings));
    return saved ? std::min(g_warnings, 100) : 4;
}

} // namespace

bool wanted(const QStringList& args)
{
    return args.contains(kFlag) || args.contains(kSettingsFlag) || args.contains(kResultsFlag) || args.contains(kAiCardsFlag)
        || args.contains(kCaptureToolsFlag);
}

void note(const QString& line)
{
    say(line);
}

void prepare(const QStringList& args)
{
    const bool resultsMode = args.contains(kResultsFlag);
    const QString folder = valueAfter(args, resultsMode ? kResultsFlag : args.contains(kCaptureToolsFlag) ? kCaptureToolsFlag
        : args.contains(kAiCardsFlag) ? kAiCardsFlag : kSettingsFlag);
    if (!folder.isEmpty()) {
        QDir().mkpath(folder);
        g_logPath = QDir(folder).filePath(u"preview.log"_s);
    } else {
        g_logPath = valueAfter(args, kFlag) + u".log"_s;
    }
    QFile::remove(g_logPath);
    say(u"preparing"_s);
    // The scale asked for, exactly: not on top of the screen's own.
    qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
    qputenv("QT_SCALE_FACTOR", valueAfter(args, u"--scale", u"1"_s).toLatin1());
    const bool webForm = args.contains(u"--web-form"_s);
    // --renderer d3d11: the window as the desktop shows it (for counting edge pixels), not the software renderer's.
    const bool nativeWeb = (webForm || args.contains(QString(kSettingsFlag)) || args.contains(kCaptureToolsFlag))
        && valueAfter(args, u"--renderer") == u"d3d11";
    if (nativeWeb)
        qputenv("QSG_RHI_BACKEND", "d3d11");
    if (resultsMode || nativeWeb)
        qunsetenv("QT_QUICK_BACKEND"); // the same native text path as the launcher
    else
        qputenv("QT_QUICK_BACKEND", "software"); // renders without a window on screen
    qputenv("QT_QPA_PLATFORM", webForm && !nativeWeb ? "windows:fontengine=gdi" : "windows:fontengine=freetype");
    // Nothing of the user's is read or written: a profile folder of its own.
    Settings::useProfile(QDir::temp().filePath(u"WinShun-preview-"_s + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8)));
}

int run(const QStringList& args)
{
    const bool settingsMode = args.contains(kSettingsFlag);
    const bool resultsMode = args.contains(kResultsFlag);
    const bool cardsMode = args.contains(kAiCardsFlag);
    const bool captureMode = args.contains(kCaptureToolsFlag);
    if (valueAfter(args, resultsMode ? kResultsFlag : captureMode ? kCaptureToolsFlag : cardsMode ? kAiCardsFlag : settingsMode ? kSettingsFlag : kFlag).isEmpty()) {
        std::fprintf(stderr, "usage: WinShun --preview-plugins <file.png> [--dark] [--scale 1.5] [--dialog] [--page ai]\n"
                             "       WinShun --preview-settings <folder> [--dark] [--scale 1.5] [--tree] [--edges] [--renderer d3d11]\n");
        return 2;
    }
    g_previous = qInstallMessageHandler(countWarnings);
    g_edges = args.contains(u"--edges"_s) || args.contains(u"--tree"_s);
    QGuiApplication::styleHints()->setColorScheme(args.contains(u"--dark"_s) ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
    SystemTheme::setBackdropAvailable(false); // plain colours, as a window without Mica
    return resultsMode ? runResults(args) : captureMode ? runCaptureTools(args) : cardsMode ? runAiCards(args) : settingsMode ? runSettings(args) : runPlugins(args);
}

} // namespace ws::preview
