#pragma once

#include <QImage>
#include <QString>
#include <QStringList>

#include <memory>

namespace ws {

// The words in a picture, read by PP-OCRv6 tiny (PaddleOCR: a model that
// finds the lines of text, one that reads each line; Chinese, English and
// the Latin-script languages) run by ONNX Runtime on the processor. Much
// better on screenshots than Windows' own recognizer: small UI text, digits
// with decimal points, mixed Chinese and English.
//
// The files: onnxruntime.dll next to the program, the two models in ocr\.
// Loaded by their full paths (an older onnxruntime.dll is in System32).
// One object reads one picture at a time; its threads run below normal
// priority. Letting it go gives back its memory (~40 MB while reading).
class PaddleOcr {
public:
    // The files are there (cheap: what to use, decided before loading).
    static bool installed(const QString& programFolder);
    // Null when something is missing or will not load; why in `error`.
    static std::unique_ptr<PaddleOcr> load(const QString& programFolder, int threads, QString* error = nullptr);
    ~PaddleOcr();

    // One string per line of text, top to bottom: the pieces found side by
    // side put together, with a space where there is a gap or Latin writing.
    // False when the recognition itself failed (`error`); a picture without
    // words is true with no lines.
    bool read(const QImage& image, QStringList* lines, QString* error = nullptr);

private:
    PaddleOcr();
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace ws
