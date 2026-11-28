#include "navidog/database.hpp"
#include "cancel_watch.hpp"
#include "collector.hpp"
#include <algorithm>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include NAVIDOG_OCEANBASE_MYSQL_HEADER
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

namespace navidog {
namespace {
QString libraryPath() {
    const QDir application(QCoreApplication::applicationDirPath());
    const auto name = QStringLiteral(NAVIDOG_OCEANBASE_LIBRARY_NAME);
#ifdef Q_OS_MAC
    const auto bundled = application.filePath("../Frameworks/" + name);
    if (QFileInfo::exists(bundled)) return bundled;
#endif
    return application.filePath(name);
}

// Connector/C and libmysqlclient export identical C symbols with different
// ABI layouts. Resolve every call from this library's handle.
class Client {
    QLibrary library_{libraryPath()};
    template <class Function>
    Function symbol(const char* name) {
        const auto address = library_.resolve(name);
        if (!address) native::fail(QString("OceanBase Connector/C 缺少 %1：%2")
                                    .arg(QString::fromLatin1(name), library_.errorString()));
        return reinterpret_cast<Function>(address);
    }
public:
#define OB_LOAD(name) decltype(&::name) name = symbol<decltype(&::name)>(#name)
    OB_LOAD(mysql_server_init);
    OB_LOAD(mysql_server_end);
    OB_LOAD(mysql_init);
    OB_LOAD(mysql_close);
    OB_LOAD(mysql_options);
    OB_LOAD(mysql_real_connect);
    OB_LOAD(mysql_real_query);
    OB_LOAD(mysql_errno);
    OB_LOAD(mysql_error);
    OB_LOAD(mysql_get_socket);
    OB_LOAD(mysql_get_ssl_cipher);
    OB_LOAD(mysql_use_result);
    OB_LOAD(mysql_free_result);
    OB_LOAD(mysql_fetch_row);
    OB_LOAD(mysql_fetch_lengths);
    OB_LOAD(mysql_fetch_fields);
    OB_LOAD(mysql_num_fields);
    OB_LOAD(mysql_affected_rows);
    OB_LOAD(mysql_field_count);
    OB_LOAD(mysql_next_result);
    OB_LOAD(mysql_autocommit);
#undef OB_LOAD

    Client() {
        if (mysql_server_init(0, nullptr, nullptr))
            native::fail("OceanBase Connector/C 初始化失败");
    }
    ~Client() { mysql_server_end(); }
};

constexpr auto oracle = sql::Dialect::Oracle;
QString columnType(enum_field_types type) {
    switch (type) {
    case MYSQL_TYPE_DECIMAL: case MYSQL_TYPE_NEWDECIMAL: return "NUMBER";
    case MYSQL_TYPE_FLOAT: return "BINARY_FLOAT";
    case MYSQL_TYPE_DOUBLE: return "BINARY_DOUBLE";
    case MYSQL_TYPE_OB_NUMBER_FLOAT: return "FLOAT";
    case MYSQL_TYPE_TINY: case MYSQL_TYPE_SHORT: case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG: case MYSQL_TYPE_INT24: return "INTEGER";
    case MYSQL_TYPE_OB_RAW: return "RAW";
    case MYSQL_TYPE_OB_NCHAR: return "NCHAR";
    case MYSQL_TYPE_OB_NVARCHAR2: return "NVARCHAR2";
    case MYSQL_TYPE_OB_TIMESTAMP_WITH_TIME_ZONE: return "TIMESTAMP WITH TIME ZONE";
    case MYSQL_TYPE_OB_TIMESTAMP_WITH_LOCAL_TIME_ZONE: return "TIMESTAMP WITH LOCAL TIME ZONE";
    case MYSQL_TYPE_OB_TIMESTAMP_NANO: case MYSQL_TYPE_TIMESTAMP: return "TIMESTAMP";
    case MYSQL_TYPE_DATE: case MYSQL_TYPE_DATETIME: return "DATE";
    case MYSQL_TYPE_OB_INTERVAL_YM: return "INTERVAL YEAR TO MONTH";
    case MYSQL_TYPE_OB_INTERVAL_DS: return "INTERVAL DAY TO SECOND";
    case MYSQL_TYPE_OB_UROWID: return "UROWID";
    default: return {};
    }
}

QByteArray preparedSql(const QString& statement, bool definitionMode) {
    native::validate(statement, oracle, definitionMode);
    auto bytes = statement.toUtf8();
    if (!definitionMode) {
        const auto tokens = sql::lex(bytes.toStdString(), oracle).tokens;
        const auto last = std::find_if(tokens.rbegin(), tokens.rend(),
            [](const auto& token) { return token.kind != sql::TokenKind::Comment; });
        if (last != tokens.rend() && last->end == last->begin + 1 &&
                bytes[int(last->begin)] == ';')
            bytes.remove(int(last->begin), 1);
    }
    return bytes;
}

class OceanBaseOracleDatabase final : public Database {
    Client& api_;
    MYSQL* connection_ = nullptr;
    bool broken_ = false;
    QString version_;

    [[noreturn]] void raiseError() {
        const auto code = api_.mysql_errno(connection_);
        broken_ |= code == 2006 || code == 2013 || code == 2014 || code == 2055;
        const auto message = QString::fromUtf8(api_.mysql_error(connection_));
        native::fail(message.isEmpty() ? "OceanBase Connector/C 未返回错误详情" : message);
    }

    void configure(const ConnectionProfile& profile) {
        const auto option = [&](mysql_option key, const void* value) {
            if (api_.mysql_options(connection_, key, value)) raiseError();
        };
        const auto stringOption = [&](mysql_option key, const QString& value) {
            if (!value.isEmpty()) option(key, value.toUtf8().constData());
        };
        const unsigned int connectTimeout = unsigned(profile.advanced.connectTimeout);
        const unsigned int readTimeout = unsigned(profile.advanced.readTimeout);
        const unsigned int writeTimeout = unsigned(profile.advanced.writeTimeout);
        const unsigned int protocol = MYSQL_PROTOCOL_TCP;
        option(MYSQL_OPT_PROTOCOL, &protocol);
        option(MYSQL_OPT_CONNECT_TIMEOUT, &connectTimeout);
        if (readTimeout) option(MYSQL_OPT_READ_TIMEOUT, &readTimeout);
        if (writeTimeout) option(MYSQL_OPT_WRITE_TIMEOUT, &writeTimeout);
        const my_bool off = 0;
        option(MYSQL_OPT_RECONNECT, &off);
        option(MYSQL_SET_CHARSET_NAME, "utf8mb4");

        if (!profile.tls) return;
        const my_bool on = 1;
        option(MYSQL_OPT_SSL_ENFORCE, &on);
        if (profile.ssl.verification == SslVerification::Identity)
            option(MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &on);
        stringOption(MYSQL_OPT_SSL_CA, profile.ssl.caFile);
        stringOption(MYSQL_OPT_SSL_CERT, profile.ssl.certificateFile);
        stringOption(MYSQL_OPT_SSL_KEY, profile.ssl.keyFile);
        stringOption(MYSQL_OPT_SSL_CIPHER, profile.ssl.ciphers);
        stringOption(MYSQL_OPT_TLS_VERSION, profile.ssl.tlsVersions);
    }

    QueryResult readResult(QueryLimits limits, const Cancellation& cancel) {
        native::Collector collector(limits);
        std::unique_ptr<MYSQL_RES, decltype(&::mysql_free_result)>
            result(api_.mysql_use_result(connection_), api_.mysql_free_result);
        const auto finish = [&] { return std::move(collector.result); };
        if (!result) {
            if (api_.mysql_field_count(connection_) && !cancel->load()) raiseError();
            const auto affected = api_.mysql_affected_rows(connection_);
            if (affected != static_cast<my_ulonglong>(-1))
                collector.result.affectedRows = qint64(affected);
            return finish();
        }
        const auto count = api_.mysql_num_fields(result.get());
        if (count > 4096) {
            // Keep draining the protocol through mysql_free_result on unwinding.
            native::fail("结果列数超出显示上限");
        }
        const auto fields = api_.mysql_fetch_fields(result.get());
        for (unsigned int i = 0; i < count; ++i) {
            collector.result.columns << QString::fromUtf8(fields[i].name);
            collector.result.columnTypes << columnType(fields[i].type);
        }
        while (!cancel->load()) {
            const auto record = api_.mysql_fetch_row(result.get());
            if (!record) break;
            const auto lengths = api_.mysql_fetch_lengths(result.get());
            QVector<QVariant> row;
            row.reserve(int(count));
            for (unsigned int i = 0; i < count; ++i) {
                if (!record[i]) { row << QVariant{}; continue; }
                const auto take = std::min(lengths[i], static_cast<unsigned long>(limits.maxCellBytes));
                collector.result.cellTruncated |= take < lengths[i];
                const auto& field = fields[i];
                const bool binary = field.type == MYSQL_TYPE_OB_RAW || field.type == MYSQL_TYPE_BIT ||
                    (field.charsetnr == 63 && (field.type == MYSQL_TYPE_BLOB ||
                    field.type == MYSQL_TYPE_TINY_BLOB || field.type == MYSQL_TYPE_MEDIUM_BLOB ||
                    field.type == MYSQL_TYPE_LONG_BLOB || field.type == MYSQL_TYPE_VAR_STRING ||
                    field.type == MYSQL_TYPE_STRING));
                if (binary) row << QByteArray(record[i], int(take));
                else row << QString::fromUtf8(record[i], int(take));
            }
            if (!collector.add(std::move(row))) break;
        }
        if (!cancel->load() && api_.mysql_errno(connection_)) raiseError();
        return finish();
    }

public:
    explicit OceanBaseOracleDatabase(const ConnectionProfile& profile)
        : api_([]() -> Client& { static Client client; return client; }()) {
        connection_ = api_.mysql_init(nullptr);
        if (!connection_) native::fail("无法初始化 OceanBase Connector/C 会话");
        try {
            configure(profile);
            const auto host = (profile.transportHost.isEmpty() ? profile.host : profile.transportHost).toUtf8();
            const auto user = profile.user.toUtf8();
            const auto password = profile.password.toUtf8();
            const auto port = profile.transportPort ? profile.transportPort : profile.port;
            if (!api_.mysql_real_connect(connection_, host.constData(), user.constData(),
                                         password.constData(), nullptr, unsigned(port), nullptr,
                                         CLIENT_MULTI_RESULTS))
                raiseError();
            if (profile.tls && !api_.mysql_get_ssl_cipher(connection_))
                native::fail("服务端未建立此连接要求的 TLS 会话");
            if (!connection_->oracle_mode)
                native::fail("服务端租户不是 Oracle 模式，请核对用户名中的租户，或在连接设置选择 MySQL 模式。");
        } catch (...) {
            api_.mysql_close(connection_);
            throw;
        }
    }
    ~OceanBaseOracleDatabase() override { api_.mysql_close(connection_); }
    bool healthy() const override { return !broken_; }
    TransactionState transactionState() const override {
        if (broken_) return TransactionState::Unknown;
        return connection_->server_status & SERVER_STATUS_IN_TRANS ? TransactionState::Active : TransactionState::Idle;
    }
    void setAutoCommit(bool value) override {
        if (value == bool(connection_->server_status & SERVER_STATUS_AUTOCOMMIT)) return;
        if (api_.mysql_autocommit(connection_, value ? 1 : 0)) raiseError();
    }
    QueryResult transaction(TransactionAction action, const Cancellation& cancel) override {
        if (cancel->load()) { QueryResult result; result.cancelled = true; return result; }
        if (action == TransactionAction::Begin) setAutoCommit(false);
        else if (action == TransactionAction::Commit) return execute("COMMIT", {}, cancel);
        else if (action == TransactionAction::Rollback) return execute("ROLLBACK", {}, cancel);
        return {};
    }
    QString serverVersion() override {
        if (version_.isEmpty()) {
            const auto response = execute("SELECT OB_VERSION() FROM DUAL", {}, cancellation());
            if (response.rows.isEmpty() || response.rows.first().isEmpty())
                native::fail("无法读取 OceanBase 服务端版本");
            version_ = response.rows.first().first().toString();
        }
        return version_;
    }

    QueryResult execute(const QString& statement, QueryLimits limits, const Cancellation& cancel) override {
        const auto sql = preparedSql(statement, definitionMode_);
        native::Collector validateLimits(limits);
        const auto cancelled = [] { QueryResult result; result.cancelled = true; return result; };
        if (cancel->load()) return cancelled();
        const auto socket = api_.mysql_get_socket(connection_);
        native::CancelWatch watch(cancel, [socket] {
#ifdef _WIN32
            ::shutdown(SOCKET(socket), SD_BOTH);
#else
            ::shutdown(socket, SHUT_RDWR);
#endif
        });
        const auto abort = [&] { broken_ = true; return cancelled(); };
        if (api_.mysql_real_query(connection_, sql.constData(), static_cast<unsigned long>(sql.size()))) {
            if (cancel->load()) return abort();
            raiseError();
        }
        auto first = readResult(limits, cancel);
        qint64 remaining = limits.maxBytes - first.cachedBytes;
        while (!cancel->load()) {
            const auto next = api_.mysql_next_result(connection_);
            if (next < 0) break;
            if (next > 0) {
                if (cancel->load()) return abort();
                raiseError();
            }
            if (remaining < 128 || first.moreResults.size() >= 31) {
                const auto discarded = api_.mysql_use_result(connection_);
                if (discarded) api_.mysql_free_result(discarded);
                else if (api_.mysql_field_count(connection_) && !cancel->load()) raiseError();
                first.truncated = true;
                continue;
            }
            auto current = limits;
            current.maxBytes = remaining;
            auto result = readResult(current, cancel);
            remaining -= result.cachedBytes;
            first.moreResults << std::move(result);
        }
        return cancel->load() ? abort() : std::move(first);
    }
    QStringList tables() override {
        const auto result = execute("SELECT TABLE_NAME FROM USER_TABLES ORDER BY TABLE_NAME",
                                    {}, cancellation());
        QStringList names;
        for (const auto& row : result.rows) if (!row.isEmpty()) names << row.front().toString();
        return names;
    }
};
} // namespace

bool oceanBaseOracleClientAvailable() {
    return QFileInfo(libraryPath()).isFile();
}

std::unique_ptr<Database> connectOceanBaseOracle(const ConnectionProfile& profile) {
    if (!oceanBaseOracleClientAvailable())
        native::fail("找不到 OceanBase Connector/C，请使用包含 Oracle 租户驱动的 Navidog 构建。");
    return std::make_unique<OceanBaseOracleDatabase>(profile);
}
} // namespace navidog
