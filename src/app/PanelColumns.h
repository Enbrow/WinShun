#pragma once

#include <QObject>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

namespace ws {

class PanelColumns : public QObject {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(QVariantList visibleColumns READ visibleColumns NOTIFY changed FINAL)
    Q_PROPERTY(QVariantList availableColumns READ availableColumns NOTIFY changed FINAL)

public:
    explicit PanelColumns(QObject* parent = nullptr);
    void load(const QString& settingsFile);
    QVariantList visibleColumns() const;
    QVariantList availableColumns() const;
    Q_INVOKABLE void toggle(const QString& id);
    Q_INVOKABLE void move(int from, int to);
    Q_INVOKABLE void resize(const QString& id, int width);
    Q_INVOKABLE void reset();
    void retranslate() { emit changed(); }

signals:
    void changed();

private:
    struct Column {
        QString id;
        int width;
    };
    QList<Column> m_columns;
    QString m_settingsFile;
    void save();
};

} // namespace ws
