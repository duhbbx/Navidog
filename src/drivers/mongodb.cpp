#include "navidog/database.hpp"
#include "navidog/document_commands.hpp"
#include "collector.hpp"
#include <mongoc/mongoc.h>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QRegularExpression>

namespace navidog {
namespace {
using Client = std::unique_ptr<mongoc_client_t, decltype(&mongoc_client_destroy)>;
using Cursor = std::unique_ptr<mongoc_cursor_t, decltype(&mongoc_cursor_destroy)>;
using Bson = std::unique_ptr<bson_t, decltype(&bson_destroy)>;
using Uri = std::unique_ptr<mongoc_uri_t, decltype(&mongoc_uri_destroy)>;

struct MongoRuntime {
    MongoRuntime() { mongoc_init(); }
    ~MongoRuntime() { mongoc_cleanup(); }
};

bool addDocument(native::Collector& collector, const bson_t* doc) {
    size_t size = 0;
    std::unique_ptr<char, decltype(&bson_free)> json(bson_as_relaxed_extended_json(doc, &size), &bson_free);
    if (!json) native::fail("无法读取 BSON 文档");
    const auto length = size_t(collector.limits.maxCellBytes);
    collector.result.cellTruncated |= size > length;
    return collector.add({QString::fromUtf8(json.get(), int(qMin(size, length)))});
}
QString writeError(const bson_t* reply) {
    bson_iter_t field, errors, first;
    if (bson_iter_init_find(&field, reply, "writeErrors") && BSON_ITER_HOLDS_ARRAY(&field) &&
        bson_iter_recurse(&field, &errors) && bson_iter_next(&errors) &&
        BSON_ITER_HOLDS_DOCUMENT(&errors) && bson_iter_recurse(&errors, &first) &&
        bson_iter_find(&first, "errmsg") && BSON_ITER_HOLDS_UTF8(&first))
        return QString::fromUtf8(bson_iter_utf8(&first, nullptr)).left(2048);
    if (bson_iter_init(&field, reply) && bson_iter_find_descendant(&field, "writeConcernError.errmsg", &first) &&
        BSON_ITER_HOLDS_UTF8(&first))
        return QString::fromUtf8(bson_iter_utf8(&first, nullptr)).left(2048);
    return {};
}

class MongoDatabase final : public Database {
    Client client_{nullptr, &mongoc_client_destroy};
    QByteArray current_;
    QString version_;

    static Client connect(const ConnectionProfile& p) {
        static MongoRuntime runtime;
        auto host = p.transportHost.isEmpty() ? p.host : p.transportHost;
        if (host.startsWith('[') && host.endsWith(']')) host = host.mid(1, host.size() - 2);
        if (QHostAddress(host).protocol() == QAbstractSocket::IPv6Protocol && !host.contains('%'))
            host = '[' + host + ']';
        else if (!QRegularExpression("^[A-Za-z0-9_.-]+$").match(host).hasMatch())
            native::fail("MongoDB 主机名或 IP 地址无效");
        const auto address = QString("mongodb://%1:%2").arg(host).arg(p.transportPort ? p.transportPort : p.port).toUtf8();
        Uri uri(mongoc_uri_new(address.constData()), &mongoc_uri_destroy);
        if (!uri) native::fail("无法读取 MongoDB 连接地址");
        if (!p.user.isEmpty()) {
            const auto user = p.user.toUtf8(), password = p.password.toUtf8();
            const auto auth = p.mongoAuthDatabase.isEmpty() ? QByteArray("admin") : p.mongoAuthDatabase.toUtf8();
            if (!mongoc_uri_set_username(uri.get(), user.constData()) ||
                !mongoc_uri_set_password(uri.get(), password.constData()) ||
                !mongoc_uri_set_auth_source(uri.get(), auth.constData()))
                native::fail("MongoDB 凭据配置无效");
        }
        mongoc_uri_set_option_as_int32(uri.get(), MONGOC_URI_CONNECTTIMEOUTMS, p.advanced.connectTimeout * 1000);
        mongoc_uri_set_option_as_int32(uri.get(), MONGOC_URI_SERVERSELECTIONTIMEOUTMS, p.advanced.connectTimeout * 1000);
        mongoc_uri_set_option_as_int32(uri.get(), MONGOC_URI_SOCKETTIMEOUTMS, qBound(1, p.advanced.readTimeout ? p.advanced.readTimeout : 10, 60) * 1000);
        mongoc_uri_set_option_as_bool(uri.get(), MONGOC_URI_TLS, p.tls);
        Client client(mongoc_client_new_from_uri(uri.get()), &mongoc_client_destroy);
        if (!client) native::fail("无法初始化 MongoDB 客户端");
        mongoc_client_set_appname(client.get(), "Navidog");
        if (p.tls && !p.ssl.caFile.isEmpty()) {
            auto opts = *mongoc_ssl_opt_get_default();
            const auto ca = QFile::encodeName(p.ssl.caFile);
            opts.ca_file = ca.constData();
            mongoc_client_set_ssl_opts(client.get(), &opts);
        }
        return client;
    }

public:
    explicit MongoDatabase(const ConnectionProfile& p)
        : client_(connect(p)), current_(p.database.isEmpty() ? QByteArray("admin") : p.database.toUtf8()) {
        const auto ping = execute("{\"ping\":1}", {}, cancellation());
        if (!ping.error.isEmpty()) native::fail(ping.error);
        const auto info = execute("{\"buildInfo\":1}", {}, cancellation());
        if (!info.rows.isEmpty())
            version_ = QJsonDocument::fromJson(info.rows.first().first().toString().toUtf8()).object().value("version").toString();
    }
    QueryResult execute(const QString& source, QueryLimits limits, const Cancellation& cancel) override {
        native::Collector output(limits);
        auto& result = output.result;
        if (cancel->load()) { result.cancelled = true; return result; }
        const auto parsed = parseDocumentCommand(source);
        if (!parsed.error.isEmpty()) { result.error = parsed.error; return result; }
        if (parsed.name.isEmpty()) return result;
        if (parsed.options.contains("$db") || parsed.options.contains("lsid")) {
            result.error = "请用查询页的数据库选择器和连接会话执行命令";
            return result;
        }
        bson_error_t error{};
        const auto bytes = source.toUtf8();
        Bson command(bson_new_from_json(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size(), &error), &bson_destroy);
        if (!command) { result.error = QString::fromUtf8(error.message); return result; }
        const auto database = parsed.name == "listDatabases" ? QByteArray("admin") : current_;
        Bson reply(bson_new(), &bson_destroy);
        const bool read = QStringList{"find","aggregate","listDatabases","listCollections","listIndexes","count","distinct","explain"}.contains(parsed.name);
        std::unique_ptr<mongoc_server_description_t,decltype(&mongoc_server_description_destroy)> server(
            mongoc_client_select_server(client_.get(), !read, nullptr, &error), &mongoc_server_description_destroy);
        if (!server) { result.error = QString::fromUtf8(error.message); return result; }
        Bson opts(bson_new(), &bson_destroy);
        BSON_APPEND_INT32(opts.get(), "serverId", mongoc_server_description_id(server.get()));
        std::unique_ptr<mongoc_client_session_t,decltype(&mongoc_client_session_destroy)> session(
            mongoc_client_start_session(client_.get(), nullptr, &error), &mongoc_client_session_destroy);
        if (session && !mongoc_client_session_append(session.get(),opts.get(),&error)) {
            result.error = QString::fromUtf8(error.message);
            return result;
        }
        if (!mongoc_client_command_with_opts(client_.get(), database.constData(), command.get(), nullptr,
                                             opts.get(), reply.get(), &error)) {
            result.error = QString::fromUtf8(error.message);
            return result;
        }
        result.columns = QStringList{"文档"};
        result.columnTypes = QStringList{"Extended JSON"};
        if (bson_has_field(reply.get(), "cursor")) {
            Cursor cursor(mongoc_cursor_new_from_command_reply_with_opts(client_.get(), reply.release(), opts.get()), &mongoc_cursor_destroy);
            const bson_t* entry = nullptr;
            while (!cancel->load() && mongoc_cursor_next(cursor.get(), &entry))
                if (!addDocument(output, entry)) break;
            if (mongoc_cursor_error(cursor.get(), &error)) result.error = QString::fromUtf8(error.message);
        } else if (parsed.name == "listDatabases") {
            bson_iter_t databases{}, items{};
            if (bson_iter_init_find(&databases, reply.get(), "databases") && bson_iter_recurse(&databases, &items)) {
                while (!cancel->load() && bson_iter_next(&items)) {
                    if (!BSON_ITER_HOLDS_DOCUMENT(&items)) continue;
                    const uint8_t* data = nullptr;
                    uint32_t size = 0;
                    bson_iter_document(&items, &size, &data);
                    bson_t entry;
                    if (bson_init_static(&entry, data, size) && !addDocument(output, &entry)) break;
                }
            }
        } else {
            addDocument(output, reply.get());
            if (QStringList{"insert", "update", "delete"}.contains(parsed.name)) {
                bson_iter_t count;
                if (bson_iter_init_find(&count, reply.get(), parsed.name == "update" ? "nModified" : "n") &&
                    BSON_ITER_HOLDS_NUMBER(&count))
                    result.affectedRows = bson_iter_as_int64(&count);
            }
            result.error = writeError(reply.get());
        }
        result.cancelled = cancel->load();
        return result;
    }
    TransactionState transactionState() const override { return TransactionState::Unavailable; }
    QStringList tables() override { return {}; }
    QString serverVersion() override { return version_.isEmpty() ? QString("MongoDB") : version_; }
};
}
std::unique_ptr<Database> connectMongoDb(const ConnectionProfile& p) {
    return std::make_unique<MongoDatabase>(p);
}
}
