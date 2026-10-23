#pragma once

#include <QFont>
#include <QObject>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

namespace ws {

// For the settings (收藏夹): how the drop pie will show a folder's name, by
// the same fitter and font as the pie itself (PieLabels), and how many
// folders it has to choose its first eight from. One per QML engine.
class DropPieText : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(int folderCount READ folderCount NOTIFY folderCountChanged FINAL)
    Q_PROPERTY(int maxFolders READ maxFolders CONSTANT FINAL)

public:
    explicit DropPieText(QObject* parent = nullptr);
    ~DropPieText() override;

    static constexpr int kMaxFolders = 8;

    int folderCount() const;
    int maxFolders() const noexcept { return kMaxFolders; }

    // [line1, line2]: line2 empty when it fits on one; a "…" in it when the
    // name is shortened.
    Q_INVOKABLE QStringList fit(const QString& name) const;

    // From DropPie: the folder favorites (and places that are folders) it
    // could show; every instance is told.
    static void setFolderCount(int count);

    // What the pie's folder labels are drawn in (the application's font, at
    // the size for 界面大小), and how wide one may be with `sectors` of them.
    static QFont labelFont();
    static double labelWidth(int sectors);
    static int uiStep(); // 界面大小: 0 compact, 1 standard, 2 roomy (Theme.step)

signals:
    void folderCountChanged();
};

} // namespace ws
