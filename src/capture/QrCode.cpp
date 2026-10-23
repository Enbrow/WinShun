#include "QrCode.h"

#include <QUrl>

#include <ReadBarcode.h>

namespace ws::capture {

QStringList readQrCodes(const QImage& image)
{
    QStringList found;
    if (image.isNull())
        return found;
    // Grey is all the reader looks at; Grayscale8 rows are padded to 4 bytes.
    const QImage grey = image.convertToFormat(QImage::Format_Grayscale8);
    const ZXing::ImageView view(grey.constBits(), grey.width(), grey.height(), ZXing::ImageFormat::Lum,
        static_cast<int>(grey.bytesPerLine()));
    ZXing::ReaderOptions options;
    options.setFormats(ZXing::BarcodeFormat::QRCode | ZXing::BarcodeFormat::MicroQRCode);
    options.setTryHarder(true);
    options.setTryRotate(true);
    options.setTryInvert(true);
    options.setMaxNumberOfSymbols(8);
    for (const ZXing::Barcode& code : ZXing::ReadBarcodes(view, options)) {
        if (!code.isValid())
            continue;
        const QString text = QString::fromUtf8(code.text().data(), static_cast<qsizetype>(code.text().size()));
        if (!text.isEmpty() && !found.contains(text))
            found.append(text);
    }
    return found;
}

QString qrLink(const QString& text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > 2048)
        return {};
    for (const QChar c : trimmed) {
        if (c.isSpace() || c.unicode() < 0x20)
            return {};
    }
    const QUrl url(trimmed, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || (scheme != u"http" && scheme != u"https") || url.host().isEmpty())
        return {};
    return trimmed;
}

} // namespace ws::capture
