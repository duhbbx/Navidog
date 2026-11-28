#include "navidog/database.hpp"
#include "navidog/redis_commands.hpp"
#include "navidog/driver_packages.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#ifdef Q_OS_WIN
#include <winsock2.h>
#endif
#if __has_include(<hiredis.h>)
#include <hiredis.h>
#else
#include <hiredis/hiredis.h>
#endif
#include <QSet>
#include <algorithm>

namespace navidog {
namespace {
using Reply = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;

#ifdef NAVIDOG_MANAGED_REDIS
QString redisLibraryPath() {
    const auto name = redisPackageLibraryName();
    const auto installed = QDir(driverPackageDirectory(DriverPackage::Redis)).filePath(name);
    if (QFileInfo(installed).isFile()) return installed;
    const auto app = QDir(QCoreApplication::applicationDirPath());
    const auto adjacent = app.filePath("driver-runtime/redis/" + name);
    if (QFileInfo(adjacent).isFile()) return adjacent;
#ifdef Q_OS_MACOS
    const auto buildTree = app.filePath("../../../driver-runtime/redis/" + name);
    if (QFileInfo(buildTree).isFile()) return buildTree;
#endif
    return {};
}
#endif


[[noreturn]] void fail(const QString& message) {
    throw std::runtime_error(message.toStdString());
}

struct RedisApi {
#ifdef NAVIDOG_MANAGED_REDIS
    QLibrary library{redisLibraryPath()};
    template<class F> F symbol(const char* name) {
        if (library.fileName().isEmpty()) fail("请在「工具 → 驱动与运行时」安装 Redis 原生驱动");
        auto address = library.resolve(name);
        if (!address) fail("无法加载 Redis 原生驱动：" + library.errorString());
        return reinterpret_cast<F>(address);
    }
#define REDIS_FUNCTION(name) decltype(&::name) name = symbol<decltype(&::name)>(#name)
#else
#define REDIS_FUNCTION(name) decltype(&::name) name = &::name
#endif
    REDIS_FUNCTION(redisConnectWithTimeout);
    REDIS_FUNCTION(redisFree);
    REDIS_FUNCTION(redisSetTimeout);
    REDIS_FUNCTION(redisCommandArgv);
    REDIS_FUNCTION(freeReplyObject);
#undef REDIS_FUNCTION
};

RedisApi& redisApi() {
    static RedisApi api;
    return api;
}

QByteArray replyBytes(const redisReply& reply) {
    return reply.str ? QByteArray(reply.str, qsizetype(reply.len)) : QByteArray{};
}

class RedisDatabase final : public Database {
    RedisApi& api_ = redisApi();
    std::unique_ptr<redisContext, decltype(&redisFree)> context_{nullptr, api_.redisFree};
    QString version_;

    Reply command(const QVector<QByteArray>& arguments) {
        QVector<const char*> values;
        QVector<size_t> lengths;
        for (const auto& argument : arguments) {
            values << argument.constData();
            lengths << size_t(argument.size());
        }
        Reply reply(static_cast<redisReply*>(api_.redisCommandArgv(context_.get(), int(values.size()),
                                                            values.data(), lengths.constData())), api_.freeReplyObject);
        if (!reply) fail(QString::fromUtf8(context_->errstr));
        return reply;
    }

    Reply checked(const QVector<QByteArray>& arguments) {
        auto reply = command(arguments);
        if (reply->type == REDIS_REPLY_ERROR) fail(QString::fromUtf8(replyBytes(*reply)));
        return reply;
    }

    static void append(QueryResult& result, const QueryLimits& limits,
                       const redisReply& reply, const QString& path = {}, int depth = 0) {
        if (result.rows.size() >= limits.maxRows || depth > 32) { result.truncated = true; return; }
        if (reply.type == REDIS_REPLY_ARRAY && reply.elements) {
            for (size_t i = 0; i < reply.elements && !result.truncated; ++i)
                append(result, limits, *reply.element[i], path + "/" + QString::number(i + 1), depth + 1);
            return;
        }
        QVariant value;
        if (reply.type == REDIS_REPLY_INTEGER) value = qlonglong(reply.integer);
        else if (reply.type == REDIS_REPLY_ARRAY) value = "[]";
        else if (reply.type != REDIS_REPLY_NIL) {
            const auto limit = size_t(qMax(0, limits.maxCellBytes));
            result.cellTruncated |= reply.len > limit;
            const QByteArray bytes(reply.str ? reply.str : "", qsizetype(std::min(reply.len, limit)));
            const auto decoded = QString::fromUtf8(bytes);
            value = decoded.toUtf8() == bytes ? decoded : quoteRedisArgument(bytes);
        }
        const auto bytes = qint64(path.size() + value.toString().size()) * 2 + 64;
        if (result.cachedBytes + bytes > limits.maxBytes) { result.truncated = true; return; }
        result.cachedBytes += bytes;
        result.rows << QVector<QVariant>{path, value};
    }

public:
    explicit RedisDatabase(const ConnectionProfile& profile) {
        const auto host = (profile.transportHost.isEmpty() ? profile.host : profile.transportHost).toUtf8();
        const auto port = profile.transportPort ? profile.transportPort : profile.port;
        const timeval connectTimeout{profile.advanced.connectTimeout, 0};
        context_.reset(api_.redisConnectWithTimeout(host.constData(), port, connectTimeout));
        if (!context_) fail("无法分配 Redis 会话");
        if (context_->err) fail(QString::fromUtf8(context_->errstr));
        const timeval readTimeout{qBound(1, profile.advanced.readTimeout ? profile.advanced.readTimeout : 5, 30), 0};
        if (api_.redisSetTimeout(context_.get(), readTimeout) != REDIS_OK) fail(QString::fromUtf8(context_->errstr));
        if (!profile.password.isEmpty()) {
            QVector<QByteArray> credentials{"AUTH"};
            if (!profile.user.isEmpty()) credentials << profile.user.toUtf8();
            credentials << profile.password.toUtf8();
            auto reply = command(credentials);
            if (reply->type == REDIS_REPLY_ERROR) fail("Redis 认证失败，请检查 ACL 用户和密码");
        }
        if (!profile.database.isEmpty() && profile.database.toInt() != 0)
            checked({"SELECT", profile.database.toUtf8()});
        auto info = command({"INFO", "SERVER"});
        if (info->type == REDIS_REPLY_STRING) {
            for (const auto& line : replyBytes(*info).split('\n'))
                if (line.startsWith("redis_version:")) version_ = QString::fromLatin1(line.mid(14).trimmed());
        }
    }

    QueryResult execute(const QString& text, QueryLimits limits, const Cancellation& cancel) override {
        QueryResult result;
        const auto parsed = parseRedisCommand(text);
        if (cancel->load()) { result.cancelled = true; return result; }
        if (!parsed.error.isEmpty()) { result.error = parsed.error; return result; }
        if (parsed.arguments.isEmpty()) return result;
        const auto verb = parsed.arguments.first().toUpper();
        static const QSet<QByteArray> streaming{"MONITOR", "SUBSCRIBE", "PSUBSCRIBE", "SSUBSCRIBE",
                                                 "HELLO", "SYNC", "PSYNC", "AUTH"};
        if (streaming.contains(verb)) {
            result.error = verb == "AUTH" ? "请在连接设置中提供 Redis 凭据" : "命令控制台仅支持普通 Redis 请求和响应";
            return result;
        }
        auto reply = command(parsed.arguments);
        result.cancelled = cancel->load();
        if (result.cancelled) return result;
        if (reply->type == REDIS_REPLY_ERROR) { result.error = QString::fromUtf8(replyBytes(*reply)); return result; }
        result.columns = QStringList{"位置", "返回值"};
        append(result, limits, *reply);
        return result;
    }

    QueryResult redisKeys(QueryLimits limits, const Cancellation& cancel) override {
        QueryResult result;
        QByteArray cursor = "0";
        QSet<QByteArray> seen;
        for (int batch = 0; batch < 24 && !cancel->load(); ++batch) {
            auto reply = checked({"SCAN", cursor, "COUNT", "128"});
            if (reply->type != REDIS_REPLY_ARRAY || reply->elements != 2 ||
                reply->element[0]->type != REDIS_REPLY_STRING || reply->element[1]->type != REDIS_REPLY_ARRAY)
                fail("Redis SCAN 返回了意外的结果");
            cursor = replyBytes(*reply->element[0]);
            const auto& keys = *reply->element[1];
            for (size_t i = 0; i < keys.elements; ++i) {
                const auto& item = *keys.element[i];
                if (item.type != REDIS_REPLY_STRING) continue;
                const auto cost = qint64(item.len) * 3 + 64;
                if (size_t(qMax(0, limits.maxCellBytes)) < item.len) {
                    result.cellTruncated = true;
                    continue;
                }
                const auto key = replyBytes(item);
                if (seen.contains(key)) continue;
                if (result.rows.size() >= limits.maxRows || result.cachedBytes + cost > limits.maxBytes) {
                    result.truncated = true;
                    return result;
                }
                seen.insert(key);
                result.rows << QVector<QVariant>{key};
                result.cachedBytes += cost;
            }
            if (cursor == "0") break;
        }
        result.cancelled = cancel->load();
        result.truncated = cursor != "0" || result.cellTruncated;
        return result;
    }

    QueryResult redisPreview(const QByteArray& key, QueryLimits limits, const Cancellation& cancel) override {
        QueryResult result;
        if (cancel->load()) { result.cancelled = true; return result; }
        const auto type = replyBytes(*checked({"TYPE", key}));
        if (type == "none") { result.error = "此键已不存在，请刷新键列表"; return result; }
        const auto count = QByteArray::number(qBound(1, limits.maxRows, 100));
        QVector<QByteArray> arguments;
        if (type == "string") arguments = {"GETRANGE", key, "0", QByteArray::number(qMax(0, limits.maxCellBytes - 1))};
        if (type == "list") arguments = {"LRANGE", key, "0", QByteArray::number(count.toInt() - 1)};
        if (type == "hash") arguments = {"HSCAN", key, "0", "COUNT", count};
        if (type == "set") arguments = {"SSCAN", key, "0", "COUNT", count};
        if (type == "zset") arguments = {"ZRANGE", key, "0", QByteArray::number(count.toInt() - 1), "WITHSCORES"};
        if (type == "stream") arguments = {"XRANGE", key, "-", "+", "COUNT", count};
        const auto appendValue = [&](const redisReply& reply) {
            if ((type == "hash" || type == "set") && reply.type == REDIS_REPLY_ARRAY &&
                reply.elements == 2 && replyBytes(*reply.element[0]) != "0") {
                append(result, limits, *reply.element[1]);
                result.truncated = true;
            } else if ((type == "hash" || type == "set") && reply.type == REDIS_REPLY_ARRAY && reply.elements == 2)
                append(result, limits, *reply.element[1]);
            else
                append(result, limits, reply);
        };
        if (!arguments.isEmpty() && !cancel->load()) appendValue(*checked(arguments));
        result.columns = QStringList{"位置", QString::fromLatin1(type)};
        result.cancelled = cancel->load();
        return result;
    }

    TransactionState transactionState() const override { return TransactionState::Unavailable; }
    bool healthy() const override { return context_ && !context_->err; }
    QStringList tables() override { return {}; }
    QString serverVersion() override { return version_; }
};
}

bool redisDriverAvailable() {
#ifdef NAVIDOG_MANAGED_REDIS
    return !redisLibraryPath().isEmpty();
#else
    return true;
#endif
}

std::unique_ptr<Database> connectRedis(const ConnectionProfile& profile) {
    return std::make_unique<RedisDatabase>(profile);
}
}
