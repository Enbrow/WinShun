#include "PinManager.h"

#include "platform/Shell.h"

#include <QDateTime>
#include <QDebug>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws::capture {

namespace {

enum MenuCommand {
    CopyCommand = 1,
    SaveCommand,
    SaveAsCommand,
    EditCommand,
    OcrCommand,
    TopmostCommand,
    ClickThroughCommand,
    CloseCommand,
    CloseAllCommand
};

} // namespace

PinManager::PinManager(Callbacks callbacks, QObject* parent)
    : QObject(parent)
    , m_callbacks(std::move(callbacks))
{
}

PinManager::~PinManager()
{
    m_pins.clear(); // their windows destroyed without telling anyone
    m_closing.clear();
}

void PinManager::pin(const QImage& image, QPoint origin, bool screenshot, QByteArray project)
{
    if (image.isNull())
        return;
    PinWindow::Host host;
    host.closed = [this](PinWindow* pin) { closed(pin); };
    host.menu = [this](PinWindow* pin, POINT at) { showMenu(pin, at); };
    host.copy = [this](PinWindow* pin) {
        if (m_callbacks.copy)
            m_callbacks.copy(pin->image(), pin->screenshot());
    };
    host.save = [this](PinWindow* pin) {
        if (m_callbacks.save)
            m_callbacks.save(pin->image(), {});
    };
    host.dark = m_callbacks.dark;
    auto pin = std::make_unique<PinWindow>(image, origin, std::move(host));
    pin->setScreenshot(screenshot);
    pin->setProject(std::move(project));
    if (pin->hwnd())
        m_pins.push_back(std::move(pin));
}

bool PinManager::contains(const PinWindow* pin) const
{
    return std::ranges::any_of(m_pins, [pin](const auto& p) { return p.get() == pin; });
}

bool PinManager::anyClickThrough() const
{
    return std::ranges::any_of(m_pins, [](const auto& p) { return p->clickThrough(); });
}

void PinManager::clearClickThrough()
{
    for (const auto& p : m_pins)
        p->setClickThrough(false);
}

void PinManager::closeAll()
{
    // Each close takes it out of m_pins (closed()): from a copy of the list.
    std::vector<PinWindow*> all;
    for (const auto& p : m_pins)
        all.push_back(p.get());
    for (PinWindow* p : all)
        p->close();
}

void PinManager::closed(PinWindow* pin)
{
    // Called from inside its window procedure: deleted once that returns.
    const auto found = std::ranges::find(m_pins, pin, &std::unique_ptr<PinWindow>::get);
    if (found == m_pins.end())
        return;
    m_closing.push_back(std::move(*found));
    m_pins.erase(found);
    QMetaObject::invokeMethod(this, [this] {
        m_closing.clear();
        if (m_callbacks.closed)
            m_callbacks.closed();
    }, Qt::QueuedConnection);
}

void PinManager::showMenu(PinWindow* pin, POINT at)
{
    const QString ocr = m_callbacks.ocrAvailable ? tr("识别文字") : tr("识别文字（没有安装文字识别模型）");
    const std::vector<shell::MenuItem> items {
        {CopyCommand, tr("复制") + u"\tCtrl+C"_s},
        {SaveCommand, tr("保存到“屏幕截图”") + u"\tCtrl+S"_s},
        {SaveAsCommand, tr("另存为…")},
        shell::MenuItem::separator(),
        {EditCommand, tr("编辑")},
        {OcrCommand, ocr, false, m_callbacks.ocrAvailable},
        shell::MenuItem::separator(),
        {TopmostCommand, tr("置顶"), pin->topmost()},
        {ClickThroughCommand, tr("鼠标穿透"), pin->clickThrough()},
        shell::MenuItem::separator(),
        {CloseCommand, tr("关闭") + u"\tEsc"_s},
        {CloseAllCommand, tr("关闭全部贴图")},
    };
    const HWND hwnd = pin->hwnd();
    const int chosen = shell::popupMenu(hwnd, items, at);
    // The pin may have gone while the menu was up (the tray closed them all).
    if (std::ranges::find(m_pins, pin, &std::unique_ptr<PinWindow>::get) == m_pins.end())
        return;
    switch (chosen) {
    case CopyCommand:
        if (m_callbacks.copy)
            m_callbacks.copy(pin->image(), pin->screenshot());
        break;
    case SaveCommand:
        if (m_callbacks.save)
            m_callbacks.save(pin->image(), {});
        break;
    case SaveAsCommand: {
        const QString name = screenshotFileName(QDateTime::currentDateTime(), {});
        const QString path = shell::pickSaveFile(hwnd, tr("另存为"), name, tr("PNG 图片"), u"png"_s);
        if (!path.isEmpty() && m_callbacks.save && std::ranges::find(m_pins, pin, &std::unique_ptr<PinWindow>::get) != m_pins.end())
            m_callbacks.save(pin->image(), path);
        break;
    }
    case EditCommand:
        if (m_callbacks.edit)
            m_callbacks.edit(pin);
        break;
    case OcrCommand:
        if (m_callbacks.ocr)
            m_callbacks.ocr(pin->image(), pin->dpr(), pin->pictureRect());
        break;
    case TopmostCommand:
        pin->setTopmost(!pin->topmost());
        break;
    case ClickThroughCommand:
        pin->setClickThrough(!pin->clickThrough());
        break;
    case CloseCommand:
        pin->close();
        break;
    case CloseAllCommand:
        closeAll();
        break;
    default:
        break;
    }
}

} // namespace ws::capture
