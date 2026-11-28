#pragma once
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QVector>

namespace navidog::metadata_detail {
inline constexpr int maxSecondaryKeys = 8;
inline constexpr int maxSecondaryKeyColumns = 32;

inline QStringList decodeKeyColumns(const QJsonArray &fields, int maxColumns = 4096) {
    if (fields.size() > maxColumns)
        return {};
    QStringList columns;
    QSet<QString> seen;
    for (const auto &value : fields) {
        const auto name = value.toString();
        if (name.isEmpty() || name.size() > 4096 || seen.contains(name))
            return {};
        seen.insert(name);
        columns << name;
    }
    return columns;
}

inline QStringList decodePrimaryKey(const QString &payload) {
    if (payload.size() > 65536)
        return {};
    return decodeKeyColumns(QJsonDocument::fromJson(payload.toUtf8()).array());
}

struct TableKeys {
    QStringList primary;
    QVector<QStringList> secondary;
};

inline TableKeys decodeTableKeys(const QString &payload) {
    TableKeys out;
    if (payload.size() > 65536)
        return out;
    const auto document=QJsonDocument::fromJson(payload.toUtf8());
    if (document.isArray()) {
        out.primary=decodeKeyColumns(document.array());
        return out;
    }
    const auto object=document.object();
    out.primary=decodeKeyColumns(object.value("primary").toArray());
    for (const auto &key : object.value("secondary").toArray()) {
        if (out.secondary.size() == maxSecondaryKeys)
            break;
        auto columns = decodeKeyColumns(key.toArray(), maxSecondaryKeyColumns);
        if (!columns.isEmpty() && !out.secondary.contains(columns))
            out.secondary.push_back(std::move(columns));
    }
    return out;
}

inline QString encodeTableKeys(const QStringList &primary, const QVector<QStringList> &secondary) {
    const auto primaryJson = QJsonArray::fromStringList(primary);
    if (secondary.isEmpty())
        return QString::fromUtf8(QJsonDocument(primaryJson).toJson(QJsonDocument::Compact));
    QJsonArray secondaryJson;
    for (const auto &key : secondary) {
        if (secondaryJson.size()>=maxSecondaryKeys)
            break;
        if (!key.isEmpty() && key.size()<=maxSecondaryKeyColumns)
            secondaryJson.push_back(QJsonArray::fromStringList(key));
    }
    const auto document = QJsonDocument(QJsonObject{{"primary",primaryJson},{"secondary",secondaryJson}});
    return QString::fromUtf8(document.toJson(QJsonDocument::Compact));
}
}
