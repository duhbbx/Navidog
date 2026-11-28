#include "navidog/document_commands.hpp"
#include <QJsonDocument>
#include <QJsonArray>
#include <QRegularExpression>

namespace navidog {
namespace {
QString jsonValue(const QJsonValue& value) {
    const auto encoded = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(encoded.mid(1, encoded.size() - 2));
}
}
DocumentCommand parseDocumentCommand(const QString& text) {
    DocumentCommand out;
    if (text.trimmed().isEmpty()) return out;
    QJsonParseError error;
    const auto bytes = text.toUtf8();
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError) {
        out.error = "MongoDB 命令 JSON 无效 · " + error.errorString();
        out.position = QString::fromUtf8(bytes.left(error.offset)).size();
        return out;
    }
    if (!document.isObject() || document.object().isEmpty()) {
        out.error = "请输入一个包含 MongoDB 命令名的 JSON 对象";
        return out;
    }
    static const QRegularExpression first(R"re(^\s*\{\s*("(?:[^"\\]|\\.)*")\s*:)re");
    const auto match = first.match(text);
    if (!match.hasMatch()) {
        out.error = "无法读取 MongoDB 命令名";
        return out;
    }
    const auto decoded = QJsonDocument::fromJson(QString("{\"key\":%1}").arg(match.captured(1)).toUtf8());
    out.name = decoded.object().value("key").toString();
    out.position = match.capturedStart(1);
    out.options = document.object();
    const auto command = out.options.value(out.name);
    const auto collectionCommands = QStringList{"find", "insert", "update", "delete", "count", "distinct",
                                               "listIndexes", "create", "drop", "createIndexes", "dropIndexes"};
    if (out.name.isEmpty()) out.error = "MongoDB 命令名不能为空";
    else if (collectionCommands.contains(out.name) && (!command.isString() || command.toString().isEmpty()))
        out.error = out.name + " 需要集合名";
    else if (out.name == "aggregate" && !out.options.value("pipeline").isArray())
        out.error = "aggregate 的 pipeline 需要 JSON 数组";
    else if (out.name == "find" && out.options.contains("filter") && !out.options.value("filter").isObject())
        out.error = "find 的 filter 需要 JSON 对象";
    else if (out.name == "insert" && !out.options.value("documents").isArray())
        out.error = "insert 的 documents 需要 JSON 数组";
    else if (out.name == "update" && !out.options.value("updates").isArray())
        out.error = "update 的 updates 需要 JSON 数组";
    else if (out.name == "delete" && !out.options.value("deletes").isArray())
        out.error = "delete 的 deletes 需要 JSON 数组";
    else if (out.name == "createIndexes" && !out.options.value("indexes").isArray())
        out.error = "createIndexes 的 indexes 需要 JSON 数组";
    return out;
}
QStringList mongoCommandNames() {
    return {"aggregate", "count", "create", "createIndexes", "delete", "distinct", "drop",
            "dropIndexes", "explain", "find", "insert", "listCollections", "listDatabases",
            "listIndexes", "ping", "update"};
}
QStringList mongoCommandOptions(const QString& command) {
    if (command == "find") return {"filter", "projection", "sort", "limit", "skip", "hint", "batchSize", "maxTimeMS"};
    if (command == "aggregate") return {"pipeline", "cursor", "allowDiskUse", "hint", "maxTimeMS"};
    if (command == "insert") return {"documents", "ordered", "writeConcern"};
    if (command == "update") return {"updates", "ordered", "writeConcern"};
    if (command == "delete") return {"deletes", "ordered", "writeConcern"};
    if (command == "createIndexes") return {"indexes", "commitQuorum", "writeConcern"};
    if (command == "dropIndexes") return {"index", "writeConcern"};
    if (command == "listCollections") return {"filter", "nameOnly", "authorizedCollections", "cursor"};
    if (command == "explain") return {"verbosity", "comment"};
    return {};
}
QString mongoCatalogCommand(const QString& command, const QString& collection) {
    // MongoDB takes the first field as the command name. Encode the value separately.
    const auto quoted = jsonValue(collection);
    if (command == "find") return QString("{\"find\":%1,\"limit\":100}").arg(quoted);
    if (command == "listIndexes") return QString("{\"listIndexes\":%1}").arg(quoted);
    if (command == "listDatabases") return "{\"listDatabases\":1,\"nameOnly\":true}";
    if (command == "listCollections") return "{\"listCollections\":1,\"nameOnly\":true,\"authorizedCollections\":true}";
    if (command == "collectionInfo") return QString("{\"listCollections\":1,\"filter\":{\"name\":%1}}").arg(quoted);
    return "{\"ping\":1}";
}
QString mongoCommandDraft(const QString& command, const QString& collection, const QString& index) {
    const auto target = jsonValue(collection);
    if (command == "create") return QString("{\"create\": %1}").arg(target);
    if (command == "insert") return QString("{\n  \"insert\": %1,\n  \"documents\": [{}]\n}").arg(target);
    if (command == "createIndexes")
        return QString("{\n  \"createIndexes\": %1,\n  \"indexes\": [{\"key\": {\"field\": 1}, \"name\": \"field_1\"}]\n}").arg(target);
    if (command == "dropIndexes")
        return QString("{\"dropIndexes\": %1, \"index\": %2}").arg(target, jsonValue(index));
    if (command == "drop") return QString("{\"drop\": %1}").arg(target);
    return {};
}
}
