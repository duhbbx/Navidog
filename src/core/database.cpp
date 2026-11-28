#include "navidog/driver_packages.hpp"
#include "navidog/database_sql.hpp"
#include "navidog/database.hpp"
#include "navidog/credentials.hpp"
#include "navidog/ssh_tunnel.hpp"
#include "navidog/http_tunnel.hpp"
#include "navidog/sql.hpp"
#include "client_codec.hpp"
#include "parameter_markers.hpp"
#ifdef NAVIDOG_HAS_JDBC
#include "../drivers/jdbc_bridge.hpp"
#endif
#include <QElapsedTimer>
#include <QThread>
#include <QFile>
#include <QHostInfo>
#include <QHostAddress>
#include <sqlite3.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <mutex>
#include <QHash>
#ifdef NAVIDOG_HAS_POSTGRES
#include <libpq-fe.h>
#endif
#ifdef NAVIDOG_HAS_MYSQL
#include "socket_options.hpp"
#include <mysql.h>
#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#endif

namespace navidog {
#ifdef NAVIDOG_HAS_SQLSERVER
std::unique_ptr<Database> connectSqlServer(const ConnectionProfile&);
QString microsoftOdbcDriver();
#endif
#ifdef NAVIDOG_HAS_ORACLE
std::unique_ptr<Database> connectOracle(const ConnectionProfile&);
#endif
QueryResult Database::transaction(TransactionAction action,const Cancellation& cancel){return execute(action==TransactionAction::Begin?"BEGIN":action==TransactionAction::Commit?"COMMIT":"ROLLBACK",{},cancel);}
QueryResult Database::executeDefinition(const QString& query,QueryLimits limits,const Cancellation& cancel) {
    const bool previous=definitionMode_;definitionMode_=true;
    try{auto result=execute(query,limits,cancel);definitionMode_=previous;return result;}catch(...){definitionMode_=previous;throw;}
}
namespace {
[[noreturn]] void fail(const QString& message) { throw std::runtime_error(message.toUtf8().constData()); }
struct Collector {
    QueryResult result;
    QueryLimits limits;
    const ClientCodec* codec=nullptr;
    explicit Collector(QueryLimits l,const ClientCodec* encoding=nullptr) : limits(l),codec(encoding) {
        if (l.maxRows < 1 || l.maxRows > 100000 || l.maxBytes < 128 || l.maxCellBytes < 1 || l.maxCellBytes > 1048576)
            fail("结果缓存限制无效");
    }
    QVariant text(const char* data, qsizetype size) {
        if (!data) return {};
        const auto count = std::min<qsizetype>(size, limits.maxCellBytes);
        if (limits.rejectInvalidTextEncoding && count == size) {
            static const ClientCodec utf8;
            return (codec ? *codec : utf8).decodeStrict(data, count);
        }
        auto value = codec?codec->decode(data,count):QString::fromUtf8(data,count);
        if (count < size) { value += QStringLiteral("… [已截断]"); result.cellTruncated = true; }
        return value;
    }
    bool wantsRow() const { return !result.truncated; }
    bool add(QVector<QVariant> row) {
        qint64 bytes = 64 + row.size() * 48;
        for (auto& value : row) bytes += value.userType() == QMetaType::QByteArray ? value.toByteArray().size() : value.toString().size() * 2;
        if (result.rows.size() >= limits.maxRows || result.cachedBytes + bytes > limits.maxBytes) { result.truncated = true; return false; }
        result.cachedBytes += bytes; result.rows.push_back(std::move(row)); return true;
    }
};
void validate(const QString& query, sql::Dialect dialect,bool definitionMode=false) {
    auto bytes = query.toUtf8(); std::string error;
    if (bytes.contains('\0')) fail("SQL 包含无效的空字符");
    if (definitionMode && sql::isCompoundDefinition(std::string_view(bytes.constData(),bytes.size()),dialect)) return;
    if (!sql::isSingleStatement(std::string_view(bytes.constData(), bytes.size()), dialect, error)) fail(QString::fromStdString(error));
}
class SQLiteDatabase final : public Database {
    sqlite3* db_ = nullptr;
public:
    explicit SQLiteDatabase(const ConnectionProfile& p) {
        // Existing connections never silently create a mistyped path.
        const int flags = (p.readOnly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE) | SQLITE_OPEN_NOMUTEX;
        if (sqlite3_open_v2(p.database.toUtf8().constData(), &db_, flags, nullptr) != SQLITE_OK) {
            const auto error = QString::fromUtf8(sqlite3_errmsg(db_)); sqlite3_close(db_); db_ = nullptr; fail(error);
        }
        sqlite3_busy_timeout(db_, 1000);
        sqlite3_exec(db_, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr);
    }
    TransactionState transactionState()const override{return sqlite3_get_autocommit(db_)?TransactionState::Idle:TransactionState::Active;}
    QString serverVersion() override { return QString::fromUtf8(sqlite3_libversion()); }
    ~SQLiteDatabase() override { if (db_) sqlite3_close(db_); }
    QueryResult execute(const QString& query, QueryLimits limits, const Cancellation& cancel) override { return executeBound(query,{},limits,cancel); }
    QueryResult executeBound(const QString& query,const QVector<QVariant>& parameters,QueryLimits limits,const Cancellation& cancel) override {
        validate(query, sql::Dialect::SQLite,definitionMode_); Collector c(limits);
        const auto bytes = query.toUtf8(); sqlite3_stmt* raw = nullptr;const char* tail=nullptr;
        if (sqlite3_prepare_v2(db_, bytes.constData(), bytes.size(), &raw, &tail) != SQLITE_OK) fail(QString::fromUtf8(sqlite3_errmsg(db_)));
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw, sqlite3_finalize);
        if (!stmt) fail("SQL 没有可运行的内容");
        if(tail){auto remainder=QByteArray(tail);auto parsed=sql::lex(std::string_view(remainder.constData(),remainder.size()),sql::Dialect::SQLite);for(auto token:parsed.tokens)if(token.kind!=sql::TokenKind::Comment)fail("一次只能执行一个完整定义");}
        if(sqlite3_bind_parameter_count(stmt.get())!=parameters.size())fail("绑定参数数量不匹配");
        for(int i=0;i<parameters.size();++i) {
            const auto& value=parameters[i];int rc=SQLITE_OK;
            if(!value.isValid() || value.isNull())rc=sqlite3_bind_null(stmt.get(),i+1);
            else if(value.userType()==QMetaType::QByteArray){auto b=value.toByteArray();rc=sqlite3_bind_blob(stmt.get(),i+1,b.constData(),b.size(),SQLITE_TRANSIENT);}
            else if(value.userType()==QMetaType::Double)rc=sqlite3_bind_double(stmt.get(),i+1,value.toDouble());
            else if(value.userType()==QMetaType::Int || value.userType()==QMetaType::LongLong || value.userType()==QMetaType::Bool)rc=sqlite3_bind_int64(stmt.get(),i+1,value.toLongLong());
            else {auto b=value.toString().toUtf8();rc=sqlite3_bind_text(stmt.get(),i+1,b.constData(),b.size(),SQLITE_TRANSIENT);}
            if(rc!=SQLITE_OK)fail(QString::fromUtf8(sqlite3_errmsg(db_)));
        }
        const int count = sqlite3_column_count(stmt.get());
        for (int i = 0; i < count; ++i){c.result.columns << QString::fromUtf8(sqlite3_column_name(stmt.get(), i));const auto* type=sqlite3_column_decltype(stmt.get(),i);c.result.columnTypes<< (type?QString::fromUtf8(type):QString{});}
        sqlite3_progress_handler(db_, 1000, [](void* ptr) { return static_cast<std::atomic_bool*>(ptr)->load() ? 1 : 0; }, cancel.get());
        struct Reset { sqlite3* db; ~Reset() { sqlite3_progress_handler(db, 0, nullptr, nullptr); } } reset{db_};
        int rc = SQLITE_DONE;
        while (!cancel->load() && (rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
            QVector<QVariant> row; row.reserve(count);
            for (int col = 0; col < count; ++col) {
                switch (sqlite3_column_type(stmt.get(), col)) {
                case SQLITE_NULL: row.push_back({}); break;
                case SQLITE_INTEGER: row.push_back(QVariant::fromValue<qlonglong>(sqlite3_column_int64(stmt.get(), col))); break;
                case SQLITE_FLOAT: row.push_back(sqlite3_column_double(stmt.get(), col)); break;
                case SQLITE_BLOB: {
                    const auto size = sqlite3_column_bytes(stmt.get(), col);
                    auto take = std::min(size, limits.maxCellBytes);
                    row.push_back(QByteArray(static_cast<const char*>(sqlite3_column_blob(stmt.get(), col)), take));
                    if (take < size) c.result.cellTruncated = true;
                    break;
                }
                default: row.push_back(c.text(reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), col)), sqlite3_column_bytes(stmt.get(), col)));
                }
            }
            if (!c.add(std::move(row))) break;
        }
        c.result.cancelled = cancel->load() || rc == SQLITE_INTERRUPT;
        if (!c.result.cancelled && !c.result.truncated && rc != SQLITE_DONE) c.result.error = QString::fromUtf8(sqlite3_errmsg(db_));
        if (count == 0 && rc == SQLITE_DONE) c.result.affectedRows = sqlite3_changes64(db_);
        if (c.result.cancelled || !c.result.error.isEmpty()) c.result.rows.clear();
        return c.result;
    }
    QStringList tables() override {
        auto result = execute("SELECT name FROM sqlite_schema WHERE type IN ('table','view') AND name NOT LIKE 'sqlite_%' AND name <> 'harbor_meta' ORDER BY name LIMIT 1000", {}, cancellation());
        QStringList out; for (const auto& row : result.rows) out << row[0].toString(); return out;
    }
};
#ifdef NAVIDOG_HAS_POSTGRES
QString postgresResultType(Oid oid,int modifier) {
    // Built-in OIDs are stable. Extension/domain/composite types keep their OID
    // instead of guessing a name or issuing extra SQL inside the user's transaction.
    switch(oid){
    case 16:return "boolean";case 17:return "bytea";case 18:return "\"char\"";case 19:return "name";
    case 20:return "bigint";case 21:return "smallint";case 23:return "integer";case 25:return "text";case 26:return "oid";
    case 114:return "json";case 142:return "xml";case 700:return "real";case 701:return "double precision";case 790:return "money";
    case 1042:return modifier>=4?QString("character(%1)").arg(modifier-4):"character";
    case 1043:return modifier>=4?QString("varchar(%1)").arg(modifier-4):"varchar";
    case 1082:return "date";case 1083:return "time";case 1114:return "timestamp";case 1184:return "timestamptz";case 1186:return "interval";case 1266:return "timetz";
    case 1560:return "bit";case 1562:return "bit varying";
    case 1700:{
        if (modifier<4) return "numeric";
        const auto packed=modifier-4;
        const auto precision=(packed>>16)&0xffff;
        const auto scale=((packed&0x7ff)^1024)-1024;
        return QStringLiteral("numeric(%1,%2)").arg(precision).arg(scale);
    }
    case 2950:return "uuid";case 3802:return "jsonb";
    case 1000:return "boolean[]";case 1005:return "smallint[]";case 1007:return "integer[]";case 1009:return "text[]";case 1015:return "varchar[]";case 1016:return "bigint[]";case 1021:return "real[]";case 1022:return "double precision[]";case 1231:return "numeric[]";case 2951:return "uuid[]";case 3807:return "jsonb[]";
    default:return QString("OID %1").arg(oid);
    }
}
class PostgresDatabase final : public Database {
    PGconn* db_ = nullptr;
    ClientCodec codec_;
    int readTimeout_=0,writeTimeout_=0;
    void requestCancel() {
        if (auto* request = PQgetCancel(db_)) { char error[256]; PQcancel(request, error, sizeof error); PQfreeCancel(request); }
    }
public:
    explicit PostgresDatabase(const ConnectionProfile& p):codec_(connectionCharset(p).compare("LATIN1",Qt::CaseInsensitive)==0?"ISO-8859-1":connectionCharset(p)),readTimeout_(p.advanced.readTimeout),writeTimeout_(p.advanced.writeTimeout) {
        QVector<QByteArray> keys,values;
        auto add=[&](QByteArray key,QByteArray value){keys<<key;values<<value;};
        add("host",p.ssl.serverName.isEmpty()?p.host.toUtf8():p.ssl.serverName.toUtf8());
        if(!p.transportHost.isEmpty())add("hostaddr",p.transportHost.toUtf8());else if(!p.ssl.serverName.isEmpty()){auto host=p.host;if(QHostAddress(host).isNull()){auto addresses=QHostInfo::fromName(host).addresses();if(addresses.isEmpty())fail("无法解析数据库主机");host=addresses.first().toString();}add("hostaddr",host.toUtf8());}
        add("port",QByteArray::number(p.transportPort?p.transportPort:p.port));add("dbname",p.database.toUtf8());add("user",p.user.toUtf8());add("password",p.password.toUtf8());add("connect_timeout",QByteArray::number(p.advanced.connectTimeout));add("application_name",p.advanced.applicationName.toUtf8());
        add("sslmode",!p.tls?"disable":p.ssl.verification==SslVerification::Identity?"verify-full":p.ssl.verification==SslVerification::CertificateAuthority?"verify-ca":"require");
        if(!p.ssl.caFile.isEmpty())add("sslrootcert",QFile::encodeName(p.ssl.caFile));if(!p.ssl.certificateFile.isEmpty())add("sslcert",QFile::encodeName(p.ssl.certificateFile));if(!p.ssl.keyFile.isEmpty())add("sslkey",QFile::encodeName(p.ssl.keyFile));
        if(!p.ssl.tlsVersions.isEmpty()){auto versions=p.ssl.tlsVersions.split(',',Qt::SkipEmptyParts);for(auto v:versions)if(v.trimmed()!="TLSv1.2" && v.trimmed()!="TLSv1.3")fail("PostgreSQL TLS 版本仅支持 TLSv1.2 / TLSv1.3");add("ssl_min_protocol_version",versions.contains("TLSv1.2")?"TLSv1.2":"TLSv1.3");add("ssl_max_protocol_version",versions.contains("TLSv1.3")?"TLSv1.3":"TLSv1.2");}
        if(p.advanced.keepAlive){add("keepalives","1");add("keepalives_idle",QByteArray::number(p.advanced.keepAliveSeconds));add("keepalives_interval","10");add("keepalives_count","3");}
        if(p.advanced.readTimeout)add("options",("-c statement_timeout="+QString::number(p.advanced.readTimeout*1000)).toUtf8());
        QVector<const char*> keyPointers,valuePointers;for(int i=0;i<keys.size();++i){keyPointers<<keys[i].constData();valuePointers<<values[i].constData();}keyPointers<<nullptr;valuePointers<<nullptr;
        db_=PQconnectdbParams(keyPointers.constData(),valuePointers.constData(),0);
        if(!db_ || PQstatus(db_)!=CONNECTION_OK){auto error=db_?codec_.decode(PQerrorMessage(db_)):QString("无法创建 PostgreSQL 连接");if(db_)PQfinish(db_);db_=nullptr;fail(error);}
        auto encoding=connectionCharset(p).toUtf8();if(PQsetClientEncoding(db_,encoding.constData())!=0){PQfinish(db_);db_=nullptr;fail("无法设置 PostgreSQL 客户端字符集");}
        if (p.readOnly) {
            auto* r = PQexec(db_, "SET default_transaction_read_only = on");
            const bool ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
            if (r) PQclear(r);
            if (!ok) { PQfinish(db_); db_ = nullptr; fail("无法设置只读会话"); }
        }
        PQsetnonblocking(db_, 1);
    }
    bool healthy()const override{auto state=PQtransactionStatus(db_);return PQstatus(db_)==CONNECTION_OK && (state==PQTRANS_IDLE || state==PQTRANS_INTRANS || state==PQTRANS_INERROR);}
    TransactionState transactionState()const override{switch(PQtransactionStatus(db_)){case PQTRANS_IDLE:return TransactionState::Idle;case PQTRANS_INTRANS:return TransactionState::Active;case PQTRANS_INERROR:return TransactionState::Failed;default:return TransactionState::Unknown;}}
    QString serverVersion() override { return QString::fromUtf8(PQparameterStatus(db_, "server_version")); }
    ~PostgresDatabase() override { if (db_) PQfinish(db_); }
    QueryResult execute(const QString& query, QueryLimits limits, const Cancellation& cancel) override { return executeBound(query,{},limits,cancel); }
    QueryResult executeBound(const QString& query,const QVector<QVariant>& parameters,QueryLimits limits,const Cancellation& cancel) override {
        validate(query, sql::Dialect::PostgreSQL,definitionMode_); Collector c(limits,&codec_); auto bytes = codec_.encode(query);
        if(parameters.size()>32767)fail("绑定参数过多");
        QVector<QByteArray> storage;QVector<const char*> values;QVector<int> lengths,formats;QVector<Oid> types;
        storage.reserve(parameters.size());
        for(const auto& value:parameters)storage<< (value.userType()==QMetaType::QByteArray?value.toByteArray():codec_.encode(value.toString()));
        for(int i=0;i<parameters.size();++i) {
            const auto& value=parameters[i];bool binary=value.userType()==QMetaType::QByteArray;
            if(!binary && storage[i].contains('\0'))fail("PostgreSQL 文本值不能包含空字符");
            values<<(!value.isValid() || value.isNull()?nullptr:storage[i].constData());lengths<<storage[i].size();formats<<(binary?1:0);types<<(binary?17:0);
        }
        if (!PQsendQueryParams(db_, bytes.constData(), parameters.size(), types.constData(), values.constData(), lengths.constData(), formats.constData(), 0)) fail(codec_.decode(PQerrorMessage(db_)));
        if (!PQsetSingleRowMode(db_)) fail("无法启用 PostgreSQL 流式结果模式");
        QElapsedTimer timer; timer.start();
        for (;;) {
            if (cancel->load()) { requestCancel(); c.result.cancelled = true; c.result.rows.clear(); return c.result; }
            if (readTimeout_>0 && timer.elapsed()>readTimeout_*1000LL+2000) { requestCancel(); fail("查询超过配置的读取超时"); }
            const int flush = PQflush(db_);
            if(flush==1 && writeTimeout_>0 && timer.elapsed()>writeTimeout_*1000LL){requestCancel();fail("发送查询超过配置的写入超时");}
            if (flush < 0 || !PQconsumeInput(db_)) fail(codec_.decode(PQerrorMessage(db_)));
            if (flush == 1 || PQisBusy(db_)) { QThread::msleep(5); continue; }
            auto* raw = PQgetResult(db_); if (!raw) break;
            std::unique_ptr<PGresult, decltype(&PQclear)> result(raw, PQclear);
            const auto status = PQresultStatus(raw);
            if (status != PGRES_SINGLE_TUPLE && status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
                c.result.rows.clear();auto message=codec_.decode(PQresultErrorMessage(raw));
                if(status==PGRES_FATAL_ERROR || status==PGRES_NONFATAL_ERROR) {
                    // Finish consuming the command so a caller can issue ROLLBACK on this session.
                    while(auto* remaining=PQgetResult(db_))PQclear(remaining);
                }
                fail(message.isEmpty()?"当前版本不支持 COPY 或此类结果":message);
            }
            if (c.result.columns.isEmpty()) for (int i = 0; i < PQnfields(raw); ++i){c.result.columns << codec_.decode(PQfname(raw, i));c.result.columnTypes<<postgresResultType(PQftype(raw,i),PQfmod(raw,i));}
            if (status == PGRES_COMMAND_OK) c.result.affectedRows = QString::fromUtf8(PQcmdTuples(raw)).toLongLong();
            if (status == PGRES_SINGLE_TUPLE && c.wantsRow()) {
                QVector<QVariant> row;
                for (int i = 0; i < PQnfields(raw); ++i) {
                    if(PQgetisnull(raw,0,i)){row.push_back({});continue;}
                    const auto* value=PQgetvalue(raw,0,i);const auto length=PQgetlength(raw,0,i);
                    if(PQftype(raw,i)==17) {
                        if(length>=2 && value[0]=='\\' && value[1]=='x') {
                            int take=std::min(length-2,limits.maxCellBytes*2);row<<QByteArray::fromHex(QByteArray(value+2,take));if(take<length-2)c.result.cellTruncated=true;
                        } else if(length<=limits.maxCellBytes) {size_t n=0;auto* decoded=PQunescapeBytea(reinterpret_cast<const unsigned char*>(value),&n);if(!decoded)fail("无法读取二进制字段");row<<QByteArray(reinterpret_cast<const char*>(decoded),n);PQfreemem(decoded);}
                        else {row<<QByteArray(value,limits.maxCellBytes);c.result.cellTruncated=true;}
                    } else row<<c.text(value,length);
                }
                c.add(std::move(row));
            }
        }
        return c.result;
    }
    QStringList tables() override {
        auto result = execute("SELECT tablename FROM pg_catalog.pg_tables WHERE schemaname = current_schema() UNION SELECT viewname FROM pg_catalog.pg_views WHERE schemaname = current_schema() ORDER BY 1 LIMIT 1000", {}, cancellation());
        QStringList out; for (const auto& row : result.rows) out << row[0].toString(); return out;
    }
};
#endif
#ifdef NAVIDOG_HAS_MYSQL
QString mysqlResultType(const MYSQL_FIELD& field) {
    QString type;bool integer=false;
    switch(field.type){
    case MYSQL_TYPE_TINY:type="TINYINT";integer=true;break;case MYSQL_TYPE_SHORT:type="SMALLINT";integer=true;break;
    case MYSQL_TYPE_LONG:type="INT";integer=true;break;case MYSQL_TYPE_INT24:type="MEDIUMINT";integer=true;break;case MYSQL_TYPE_LONGLONG:type="BIGINT";integer=true;break;
    case MYSQL_TYPE_DECIMAL:case MYSQL_TYPE_NEWDECIMAL:type="DECIMAL";break;case MYSQL_TYPE_FLOAT:type="FLOAT";break;case MYSQL_TYPE_DOUBLE:type="DOUBLE";break;
    case MYSQL_TYPE_NULL:type="NULL";break;case MYSQL_TYPE_TIMESTAMP:case MYSQL_TYPE_TIMESTAMP2:type="TIMESTAMP";break;case MYSQL_TYPE_DATE:case MYSQL_TYPE_NEWDATE:type="DATE";break;case MYSQL_TYPE_TIME:case MYSQL_TYPE_TIME2:type="TIME";break;case MYSQL_TYPE_DATETIME:case MYSQL_TYPE_DATETIME2:type="DATETIME";break;case MYSQL_TYPE_YEAR:type="YEAR";break;
    case MYSQL_TYPE_VARCHAR:case MYSQL_TYPE_VAR_STRING:type=field.charsetnr==63?"VARBINARY":"VARCHAR";break;case MYSQL_TYPE_STRING:type=field.flags&ENUM_FLAG?"ENUM":field.flags&SET_FLAG?"SET":field.charsetnr==63?"BINARY":"CHAR";break;
    case MYSQL_TYPE_BIT:type=QString("BIT(%1)").arg(field.length);break;case MYSQL_TYPE_JSON:type="JSON";break;case MYSQL_TYPE_ENUM:type="ENUM";break;case MYSQL_TYPE_SET:type="SET";break;
    case MYSQL_TYPE_TINY_BLOB:type=field.charsetnr==63?"TINYBLOB":"TINYTEXT";break;case MYSQL_TYPE_MEDIUM_BLOB:type=field.charsetnr==63?"MEDIUMBLOB":"MEDIUMTEXT";break;case MYSQL_TYPE_LONG_BLOB:type=field.charsetnr==63?"LONGBLOB":"LONGTEXT";break;case MYSQL_TYPE_BLOB:type=field.charsetnr==63?"BLOB":"TEXT";break;case MYSQL_TYPE_GEOMETRY:type="GEOMETRY";break;
    default:type=QString("TYPE %1").arg(int(field.type));break;
    }
    if(integer && (field.flags&UNSIGNED_FLAG))type+=" UNSIGNED";
    // MYSQL_FIELD.length is bytes, not character count: do not label UTF-8
    // VARCHAR lengths as if they were declared character limits.
    return type;
}
class MySqlDatabase final : public Database {
    MYSQL* db_ = nullptr;
    ClientCodec codec_;
    QString version_;
    bool broken_=false;
    int readTimeout_=0;
    void interrupted(){broken_=true;abortSocket(db_->net.fd);}
public:
    explicit MySqlDatabase(const ConnectionProfile& p):codec_(connectionCharset(p)),readTimeout_(p.advanced.readTimeout) {
        static std::once_flag init;
        std::call_once(init, [] { if (mysql_library_init(0, nullptr, nullptr)) fail("MySQL 客户端初始化失败"); });
        struct ThreadRuntime {ThreadRuntime(){if(mysql_thread_init())fail("MySQL 线程初始化失败");}~ThreadRuntime(){mysql_thread_end();}};
        thread_local ThreadRuntime runtime;
        db_ = mysql_init(nullptr); if (!db_) fail("无法创建 MySQL 连接");
        unsigned connectTimeout=p.advanced.connectTimeout,readTimeout=p.advanced.readTimeout,writeTimeout=p.advanced.writeTimeout;
        auto option=[&](mysql_option key,const void* value){if(mysql_options(db_,key,value))fail("MySQL 客户端选项不被当前驱动支持");};
        try {
            option(MYSQL_OPT_CONNECT_TIMEOUT,&connectTimeout);option(MYSQL_OPT_READ_TIMEOUT,&readTimeout);option(MYSQL_OPT_WRITE_TIMEOUT,&writeTimeout);auto charset=connectionCharset(p).toUtf8();option(MYSQL_SET_CHARSET_NAME,charset.constData());
            unsigned protocol=p.advanced.socketFile.isEmpty()?MYSQL_PROTOCOL_TCP:MYSQL_PROTOCOL_SOCKET;option(MYSQL_OPT_PROTOCOL,&protocol);
            auto ssl=!p.tls?SSL_MODE_DISABLED:p.ssl.verification==SslVerification::Identity && p.ssl.serverName.isEmpty() && p.transportHost.isEmpty()?SSL_MODE_VERIFY_IDENTITY:p.ssl.verification!=SslVerification::Encryption?SSL_MODE_VERIFY_CA:SSL_MODE_REQUIRED;option(MYSQL_OPT_SSL_MODE,&ssl);
            auto textOption=[&](mysql_option key,const QString& text){if(!text.isEmpty()){auto value=text.toUtf8();option(key,value.constData());}};
            textOption(MYSQL_OPT_SSL_CA,p.ssl.caFile);textOption(MYSQL_OPT_SSL_CERT,p.ssl.certificateFile);textOption(MYSQL_OPT_SSL_KEY,p.ssl.keyFile);textOption(MYSQL_OPT_SSL_CIPHER,p.ssl.ciphers);textOption(MYSQL_OPT_TLS_VERSION,p.ssl.tlsVersions);
#ifdef NAVIDOG_MYSQL_HAS_SNI
            if(p.tls){auto server=p.ssl.serverName.isEmpty()?p.host:p.ssl.serverName;if(QHostAddress(server).isNull())textOption(MYSQL_OPT_TLS_SNI_SERVERNAME,server);}
#else
            if (p.tls && !p.ssl.serverName.isEmpty())
                fail("当前 MySQL 客户端不支持设置 TLS 服务器名称");
#endif
            if(p.advanced.compression){bool compressed=true;option(MYSQL_OPT_COMPRESS,&compressed);}
            auto host=(p.transportHost.isEmpty()?p.host:p.transportHost).toUtf8(),name=p.database.toUtf8(),user=p.user.toUtf8(),password=p.password.toUtf8(),socket=QFile::encodeName(p.advanced.socketFile);
            if(!mysql_real_connect(db_,host.constData(),user.constData(),password.constData(),name.isEmpty()?nullptr:name.constData(),p.transportPort?p.transportPort:p.port,socket.isEmpty()?nullptr:socket.constData(),CLIENT_FOUND_ROWS))fail(codec_.decode(mysql_error(db_)));
            if(p.advanced.keepAlive && p.advanced.socketFile.isEmpty())configureKeepAlive(db_->net.fd,p.advanced.keepAliveSeconds);
            if(p.tls && p.ssl.verification==SslVerification::Identity && (!p.ssl.serverName.isEmpty() || !p.transportHost.isEmpty())){
#if MYSQL_VERSION_ID >= 80029
                unsigned int length=0;void* pem=mysql_get_ssl_session_data(db_,0,&length);if(!pem)fail("当前 MySQL TLS 会话未提供可验证的证书");
                struct SessionData {MYSQL* mysql;void* data;~SessionData(){mysql_free_ssl_session_data(mysql,data);}} sessionData{db_,pem};
                std::unique_ptr<BIO,decltype(&BIO_free)> bio(BIO_new_mem_buf(pem,length),BIO_free);std::unique_ptr<SSL_SESSION,decltype(&SSL_SESSION_free)> session(PEM_read_bio_SSL_SESSION(bio.get(),nullptr,nullptr,nullptr),SSL_SESSION_free);auto* certificate=session?SSL_SESSION_get0_peer(session.get()):nullptr;
                auto server=(p.ssl.serverName.isEmpty()?p.host:p.ssl.serverName).toUtf8();bool ip=!QHostAddress(QString::fromUtf8(server)).isNull();if(!certificate || (ip?X509_check_ip_asc(certificate,server.constData(),0):X509_check_host(certificate,server.constData(),server.size(),0,nullptr))!=1)fail("MySQL 服务器证书与主机身份不匹配："+QString::fromUtf8(server));
#else
                fail("使用隧道和服务器身份验证需要 MySQL 客户端 8.0.29 或更新版本");
#endif
            }
        }catch(...){mysql_close(db_);db_=nullptr;throw;}
        if (p.readOnly && mysql_query(db_, "SET SESSION TRANSACTION READ ONLY")) {
            auto error = codec_.decode(mysql_error(db_)); mysql_close(db_); db_ = nullptr; fail(error);
        }
        if(p.driver==DatabaseKind::OceanBase) {
            try { auto r=execute("SELECT OB_VERSION()",{},cancellation()); if(!r.rows.isEmpty())version_=r.rows[0][0].toString(); }
            catch(...) { mysql_close(db_);db_=nullptr;throw; }
        }
    }
    QString serverVersion() override {
        if(version_.isEmpty()) version_=QString::fromUtf8(mysql_get_server_info(db_));
        return version_;
    }
    bool healthy()const override{auto code=mysql_errno(db_);return !broken_ && code!=2006 && code!=2013 && code!=2014 && code!=2055;}
    TransactionState transactionState()const override{return (db_->server_status & SERVER_STATUS_IN_TRANS)?TransactionState::Active:TransactionState::Idle;}
    ~MySqlDatabase() override { if (db_) mysql_close(db_); }
    QueryResult execute(const QString& query,QueryLimits limits,const Cancellation& cancel)override{
        validate(query,sql::Dialect::MySQL,definitionMode_);Collector guard(limits,&codec_);auto bytes=codec_.encode(query);if(cancel->load()){guard.result.cancelled=true;return guard.result;}
#if MYSQL_VERSION_ID >= 80016
        QElapsedTimer waiting;waiting.start();net_async_status status;
        while((status=mysql_real_query_nonblocking(db_,bytes.constData(),bytes.size()))==NET_ASYNC_NOT_READY){if(cancel->load()){interrupted();guard.result.cancelled=true;return guard.result;}if(readTimeout_>0 && waiting.elapsed()>readTimeout_*1000LL){interrupted();fail("MySQL 查询超过读取超时");}QThread::msleep(2);}if(status!=NET_ASYNC_COMPLETE)fail(codec_.decode(mysql_error(db_)));
#else
        if(mysql_real_query(db_,bytes.constData(),bytes.size()))fail(codec_.decode(mysql_error(db_)));
#endif
        QueryResult first;qint64 remaining=limits.maxBytes;int set=0;
        for(;;){auto current=limits;current.maxBytes=qMax<qint64>(128,remaining);Collector c(current,&codec_);auto* raw=mysql_use_result(db_);
            if(!raw){if(mysql_field_count(db_))fail(codec_.decode(mysql_error(db_)));c.result.affectedRows=static_cast<qint64>(mysql_affected_rows(db_));}
            else {std::unique_ptr<MYSQL_RES,decltype(&mysql_free_result)> owned(raw,mysql_free_result);auto count=mysql_num_fields(raw);auto* fields=mysql_fetch_fields(raw);if(count>4096)fail("结果列数超出上限");for(unsigned i=0;i<count;++i){c.result.columns<<codec_.decode(fields[i].name);c.result.columnTypes<<mysqlResultType(fields[i]);}MYSQL_ROW row=nullptr;
                for(;;){
#if MYSQL_VERSION_ID >= 80016
                    QElapsedTimer receiving;receiving.start();net_async_status fetch;
                    while((fetch=mysql_fetch_row_nonblocking(raw,&row))==NET_ASYNC_NOT_READY){if(cancel->load()){interrupted();c.result.cancelled=true;break;}if(readTimeout_>0 && receiving.elapsed()>readTimeout_*1000LL){interrupted();fail("MySQL 结果读取超时");}QThread::msleep(2);}if(c.result.cancelled)break;if(fetch==NET_ASYNC_ERROR)fail(codec_.decode(mysql_error(db_)));
#else
                    row=mysql_fetch_row(raw);
#endif
                    if(!row)break;if(cancel->load()){interrupted();c.result.cancelled=true;break;}if(!c.wantsRow() || set>=32 || remaining<128){c.result.truncated=true;continue;}auto* lengths=mysql_fetch_lengths(raw);QVector<QVariant> values;
                    for(unsigned i=0;i<count;++i){if(!row[i])values<<QVariant{};else if(fields[i].charsetnr==63 && (fields[i].type==MYSQL_TYPE_BLOB || fields[i].type==MYSQL_TYPE_STRING || fields[i].type==MYSQL_TYPE_VAR_STRING || fields[i].type==MYSQL_TYPE_BIT || fields[i].type==MYSQL_TYPE_GEOMETRY)){auto take=std::min<unsigned long>(lengths[i],limits.maxCellBytes);values<<QByteArray(row[i],take);c.result.cellTruncated|=take<lengths[i];}else values<<c.text(row[i],lengths[i]);}c.add(std::move(values));
                }if(!c.result.cancelled && mysql_errno(db_))fail(codec_.decode(mysql_error(db_)));
            }
            remaining-=c.result.cachedBytes;if(set==0)first=std::move(c.result);else if(set<32)first.moreResults<<std::move(c.result);else first.truncated=true;++set;
            if(first.cancelled || cancel->load()){first.cancelled=true;first.rows.clear();interrupted();return first;}
#if MYSQL_VERSION_ID >= 80016
            QElapsedTimer moving;moving.start();net_async_status transition;
            while((transition=mysql_next_result_nonblocking(db_))==NET_ASYNC_NOT_READY){if(cancel->load()){interrupted();first.cancelled=true;return first;}if(readTimeout_>0 && moving.elapsed()>readTimeout_*1000LL){interrupted();fail("MySQL 后续结果读取超时");}QThread::msleep(2);}if(transition==NET_ASYNC_ERROR)fail(codec_.decode(mysql_error(db_)));if(transition==NET_ASYNC_COMPLETE_NO_MORE_RESULTS)break;
#else
            auto next=mysql_next_result(db_);if(next>0)fail(codec_.decode(mysql_error(db_)));if(next<0)break;
#endif
        }return first;
    }
    QueryResult executeBound(const QString& query,const QVector<QVariant>& parameters,QueryLimits limits,const Cancellation& cancel)override{
        if(parameters.isEmpty())return execute(query,limits,cancel);
        validate(query,sql::Dialect::MySQL);Collector c(limits,&codec_);auto rewritten=mysqlParameters(query,parameters);auto bytes=codec_.encode(QString::fromUtf8(rewritten.sql));auto* statement=mysql_stmt_init(db_);if(!statement)fail("无法创建 MySQL 参数语句");struct Release{MYSQL_STMT* statement;~Release(){mysql_stmt_close(statement);}}release{statement};auto check=[&](int status){if(status){auto code=mysql_stmt_errno(statement);if(code==2006 || code==2013 || code==2014 || code==2055)broken_=true;fail(codec_.decode(mysql_stmt_error(statement)));}};check(mysql_stmt_prepare(statement,bytes.constData(),bytes.size()));if(mysql_stmt_param_count(statement)!=static_cast<unsigned long>(rewritten.values.size()))fail("绑定参数数量不匹配");
        struct Value{QByteArray bytes;long long integer=0;double real=0;};std::vector<Value> storage(rewritten.values.size());std::vector<MYSQL_BIND> bindings(rewritten.values.size());std::vector<unsigned long> lengths(rewritten.values.size());
        for(int i=0;i<rewritten.values.size();++i){const auto& value=rewritten.values[i];auto& bound=bindings[i];auto& saved=storage[i];if(!value.isValid() || value.isNull())bound.buffer_type=MYSQL_TYPE_NULL;else if(value.userType()==QMetaType::Int || value.userType()==QMetaType::LongLong || value.userType()==QMetaType::Bool){saved.integer=value.toLongLong();bound.buffer_type=MYSQL_TYPE_LONGLONG;bound.buffer=&saved.integer;}else if(value.userType()==QMetaType::Double){saved.real=value.toDouble();bound.buffer_type=MYSQL_TYPE_DOUBLE;bound.buffer=&saved.real;}else{bool binary=value.userType()==QMetaType::QByteArray;saved.bytes=binary?value.toByteArray():codec_.encode(value.toString());lengths[i]=saved.bytes.size();bound.buffer_type=binary?MYSQL_TYPE_BLOB:MYSQL_TYPE_STRING;bound.buffer=saved.bytes.data();bound.buffer_length=saved.bytes.size();bound.length=&lengths[i];}}
        if(!bindings.empty())check(mysql_stmt_bind_param(statement,bindings.data()));if(cancel->load()){c.result.cancelled=true;return c.result;}check(mysql_stmt_execute(statement));auto* raw=mysql_stmt_result_metadata(statement);if(!raw){if(mysql_stmt_field_count(statement))check(1);c.result.affectedRows=mysql_stmt_affected_rows(statement);return c.result;}std::unique_ptr<MYSQL_RES,decltype(&mysql_free_result)> metadata(raw,mysql_free_result);auto count=mysql_num_fields(raw);auto* fields=mysql_fetch_fields(raw);if(count>4096)fail("结果列数超出上限");std::vector<QByteArray> buffers(count);std::vector<MYSQL_BIND> outputs(count);std::vector<unsigned long> sizes(count);auto nulls=std::make_unique<bool[]>(count);auto errors=std::make_unique<bool[]>(count);
        for(unsigned i=0;i<count;++i){c.result.columns<<codec_.decode(fields[i].name);c.result.columnTypes<<mysqlResultType(fields[i]);buffers[i].resize(qMin(256,limits.maxCellBytes));auto& output=outputs[i];output.buffer_type=MYSQL_TYPE_STRING;output.buffer=buffers[i].data();output.buffer_length=buffers[i].size();output.length=&sizes[i];output.is_null=&nulls[i];output.error=&errors[i];}check(mysql_stmt_bind_result(statement,outputs.data()));
        for(;;){if(cancel->load()){interrupted();c.result.cancelled=true;c.result.rows.clear();break;}auto status=mysql_stmt_fetch(statement);if(status==MYSQL_NO_DATA)break;if(status!=0 && status!=MYSQL_DATA_TRUNCATED)check(status);if(!c.wantsRow())continue;QVector<QVariant> values;
            for(unsigned i=0;i<count;++i){if(nulls[i]){values<<QVariant{};continue;}auto take=std::min<unsigned long>(sizes[i],limits.maxCellBytes);QByteArray value;if(take<=static_cast<unsigned long>(buffers[i].size()))value=QByteArray(buffers[i].constData(),take);else{value.resize(take);auto column=outputs[i];column.buffer=value.data();column.buffer_length=take;check(mysql_stmt_fetch_column(statement,&column,i,0));}c.result.cellTruncated|=take<sizes[i];bool binary=fields[i].charsetnr==63 && (fields[i].type==MYSQL_TYPE_BLOB || fields[i].type==MYSQL_TYPE_VAR_STRING || fields[i].type==MYSQL_TYPE_STRING || fields[i].type==MYSQL_TYPE_BIT || fields[i].type==MYSQL_TYPE_GEOMETRY);if(binary)values<<value;else values<<codec_.decode(value.constData(),value.size());}c.add(std::move(values));
        }mysql_stmt_free_result(statement);return c.result;
    }
    QStringList tables() override {
        auto result = execute("SELECT table_name FROM information_schema.tables WHERE table_schema = DATABASE() ORDER BY table_name LIMIT 1000", {}, cancellation());
        QStringList out; for (const auto& row : result.rows) out << row[0].toString(); return out;
    }
};
#endif
}
#ifdef NAVIDOG_HAS_OCEANBASE_ORACLE
std::unique_ptr<Database> connectOceanBaseOracle(const ConnectionProfile&);
bool oceanBaseOracleClientAvailable();
#endif
#ifdef NAVIDOG_HAS_REDIS
std::unique_ptr<Database> connectRedis(const ConnectionProfile&);
bool redisDriverAvailable();
#endif
QVector<DatabaseKind> availableDrivers() {
    QVector<DatabaseKind> out{DatabaseKind::SQLite};
#ifdef NAVIDOG_HAS_REDIS
    if (redisDriverAvailable()) out << DatabaseKind::Redis;
#endif
#ifdef NAVIDOG_HAS_POSTGRES
    out << DatabaseKind::PostgreSQL << DatabaseKind::PolarDbPostgres;
#endif
#ifdef NAVIDOG_HAS_MYSQL
    out << DatabaseKind::MySQL << DatabaseKind::OceanBase << DatabaseKind::MariaDB << DatabaseKind::TiDB << DatabaseKind::PolarDbMySql;
#endif
#ifdef NAVIDOG_HAS_SQLSERVER
    out << DatabaseKind::SQLServer;
#endif
#ifdef NAVIDOG_HAS_ORACLE
    out << DatabaseKind::Oracle;
#endif
#ifdef NAVIDOG_HAS_OCEANBASE_ORACLE
    if (oceanBaseOracleClientAvailable()) out << DatabaseKind::OceanBaseOracle;
#endif
#ifdef NAVIDOG_HAS_JDBC
    if (bundledJdbcAvailable())
        out << DatabaseKind::Dameng << DatabaseKind::H2 << DatabaseKind::Kingbase
            << DatabaseKind::UxDb << DatabaseKind::GaussDb << DatabaseKind::Db2;
#endif
    return out;
}
QVector<DriverInfo> driverRegistry() {
    auto compiled = availableDrivers();
    QVector<DriverInfo> drivers{
        {DatabaseKind::SQLite, "SQLite", "SQLite 官方 C API", {}, false, 0},
        {DatabaseKind::MySQL, "MySQL", "Oracle MySQL 官方 libmysqlclient", {}, false, 3306},
        {DatabaseKind::MariaDB, "MariaDB", "原生 libmysqlclient · MariaDB 兼容协议", {}, false, 3306},
        {DatabaseKind::PostgreSQL, "PostgreSQL", "PostgreSQL 官方 libpq", {}, false, 5432},
        {DatabaseKind::Oracle, "Oracle", "Oracle ODPI-C / OCI", {}, false, 1521},
        {DatabaseKind::TiDB, "TiDB", "原生 libmysqlclient · TiDB MySQL 兼容协议", {}, false, 4000},
        {DatabaseKind::OceanBaseOracle, "OceanBase（Oracle 模式）", "OceanBase 官方 Connector/C", {}, false, 2881},
        {DatabaseKind::OceanBase, "OceanBase（MySQL 模式）", "MySQL 官方 C API · OceanBase 文档认可", {}, false, 2881},
        {DatabaseKind::SQLServer, "SQL Server", "Microsoft ODBC Driver 18 / 17", {}, false, 1433},
        {DatabaseKind::Dameng, "达梦 DM8", "达梦官方 JDBC · 内置 OpenJDK 25", {}, false, 5236},
        {DatabaseKind::Redis, "Redis", "Redis 官方 hiredis C API", {}, false, 6379},
        {DatabaseKind::H2, "H2", "H2 官方 JDBC", {}, false, 9092},
        {DatabaseKind::Kingbase, "金仓 KingbaseES", "金仓官方 JDBC", {}, false, 54321},
        {DatabaseKind::UxDb, "优炫 UXDB", "优炫官方 JDBC", {}, false, 5432},
        {DatabaseKind::GaussDb, "华为 GaussDB", "华为官方 JDBC", {}, false, 8000},
        {DatabaseKind::Db2, "IBM Db2", "IBM JCC Type 4 JDBC", {}, false, 50000},
        {DatabaseKind::PolarDbMySql, "PolarDB MySQL", "Oracle MySQL 官方 libmysqlclient", {}, false, 3306},
        {DatabaseKind::PolarDbPostgres, "PolarDB PostgreSQL", "PostgreSQL 官方 libpq", {}, false, 5432}};
    for (auto& driver : drivers) {
        driver.ready = compiled.contains(driver.id);
        driver.status = driver.ready ? "适配器已编译" : "此构建未包含适配器";
        if (usesJdbcDriver(driver.id))
            driver.status = driver.ready
                ? (managedJdbcJar(driver.id).isEmpty() ? "可下载安装或选择官方 JDBC JAR" : "托管 JDBC 已安装，可自选覆盖")
                : (jdbcBridgeSupported() ? "请在驱动与运行时安装 Java 25" : "此构建未包含 JDBC 连接组件");
        if (driver.id == DatabaseKind::Redis && managedRedisSupported())
            driver.status = driver.ready ? "原生驱动已就绪" : "通过驱动与运行时下载安装";
        if (driver.id == DatabaseKind::Oracle && driver.ready) driver.status = "需要 Oracle Instant Client（运行时检查）";
#ifdef NAVIDOG_HAS_SQLSERVER
        if (driver.id == DatabaseKind::SQLServer && driver.ready) {
            auto installed = microsoftOdbcDriver(); driver.ready = !installed.isEmpty();
            driver.status = driver.ready ? installed : "未安装 Microsoft ODBC Driver 18 / 17";
        }
#endif
    }
    return drivers;
}
namespace {
std::unique_ptr<Database> connectDirect(const ConnectionProfile& p) {
    if (p.driver == navidog::DatabaseKind::SQLite) return std::make_unique<SQLiteDatabase>(p);
#ifdef NAVIDOG_HAS_POSTGRES
    if (usesNativePostgres(p.driver)) return std::make_unique<PostgresDatabase>(p);
#endif
#ifdef NAVIDOG_HAS_MYSQL
    if (navidog::isMySqlFamily(p.driver)) return std::make_unique<MySqlDatabase>(p);
#endif
#ifdef NAVIDOG_HAS_SQLSERVER
    if (p.driver == navidog::DatabaseKind::SQLServer) return connectSqlServer(p);
#endif
#ifdef NAVIDOG_HAS_ORACLE
    if (p.driver == navidog::DatabaseKind::Oracle) return connectOracle(p);
#endif
#ifdef NAVIDOG_HAS_REDIS
    if (p.driver == DatabaseKind::Redis) return connectRedis(p);
#endif
    if (usesJdbcDriver(p.driver)) {
#ifdef NAVIDOG_HAS_JDBC
        return p.driver == DatabaseKind::Dameng ? connectDameng(p) : connectJdbcProduct(p);
#else
        fail("此构建未包含 JDBC 组件");
#endif
    }
    if (p.driver == DatabaseKind::OceanBaseOracle) {
#ifdef NAVIDOG_HAS_OCEANBASE_ORACLE
        return connectOceanBaseOracle(p);
#else
        fail("此构建未包含 OceanBase Oracle 模式所需的 Connector/C");
#endif
    }
    fail("此构建未包含所选数据库驱动");
}
// One lease covers the whole transport and native session lifetime, including
// short-lived metadata/design connections opened outside QueryService's cache.
class SessionLease final {
    QString key_;
    static std::mutex& mutex(){static std::mutex value;return value;}
    static QHash<QString,int>& counts(){static QHash<QString,int> value;return value;}
public:
    explicit SessionLease(const ConnectionProfile& p):key_(p.id.isEmpty()?databaseKindKey(p.driver)+"\n"+p.host+"\n"+QString::number(p.port)+"\n"+p.user+"\n"+p.database:p.id){
        std::lock_guard<std::mutex> lock(mutex());const int count=counts().value(key_);
        if(p.advanced.maxSessions>0 && count>=p.advanced.maxSessions)fail("已达到此连接的最大会话数，请关闭不用的查询会话后重试");
        counts()[key_]=count+1;
    }
    ~SessionLease(){std::lock_guard<std::mutex> lock(mutex());auto it=counts().find(key_);if(it!=counts().end() && --it.value()==0)counts().erase(it);}
    SessionLease(const SessionLease&)=delete;
    SessionLease& operator=(const SessionLease&)=delete;
};
class LeasedDatabase final:public Database {
    // Reverse member destruction closes the DB before releasing its slot.
    std::unique_ptr<SessionLease> lease_;
    std::unique_ptr<Database> database_;
public:
    LeasedDatabase(std::unique_ptr<SessionLease> lease,std::unique_ptr<Database> db):lease_(std::move(lease)),database_(std::move(db)){}
    QueryResult execute(const QString& sql,QueryLimits limits,const Cancellation& cancel)override{return definitionMode_?database_->executeDefinition(sql,limits,cancel):database_->execute(sql,limits,cancel);}
    QueryResult executeBound(const QString& sql,const QVector<QVariant>& values,QueryLimits limits,const Cancellation& cancel)override{return database_->executeBound(sql,values,limits,cancel);}
    TransactionState transactionState()const override{return database_->transactionState();}
    bool healthy()const override{return database_->healthy();}
    void setAutoCommit(bool value)override{database_->setAutoCommit(value);}
    QueryResult transaction(TransactionAction action,const Cancellation& cancel)override{return database_->transaction(action,cancel);}
    QueryResult jdbcMetadata(JdbcMetadata kind,const QString& schema,const QString& object,QueryLimits limits,const Cancellation& cancel)override{
        return database_->jdbcMetadata(kind,schema,object,limits,cancel);
    }
    QueryResult redisKeys(QueryLimits limits,const Cancellation& cancel)override{
        return database_->redisKeys(limits,cancel);
    }
    QueryResult redisPreview(const QByteArray& key,QueryLimits limits,const Cancellation& cancel)override{
        return database_->redisPreview(key,limits,cancel);
    }
    QStringList tables()override{return database_->tables();}
    QString serverVersion()override{return database_->serverVersion();}
};
class TunnelDatabase final:public Database {
    std::unique_ptr<SshTunnel> tunnel_;
    std::unique_ptr<Database> database_;
public:
    TunnelDatabase(std::unique_ptr<SshTunnel> tunnel,std::unique_ptr<Database> database):tunnel_(std::move(tunnel)),database_(std::move(database)){}
    QueryResult execute(const QString& sql,QueryLimits limits,const Cancellation& cancel)override{return definitionMode_?database_->executeDefinition(sql,limits,cancel):database_->execute(sql,limits,cancel);}
    QueryResult executeBound(const QString& sql,const QVector<QVariant>& values,QueryLimits limits,const Cancellation& cancel)override{return database_->executeBound(sql,values,limits,cancel);}
    TransactionState transactionState()const override{return database_->transactionState();}
    bool healthy()const override{return database_->healthy();}
    void setAutoCommit(bool value)override{database_->setAutoCommit(value);}
    QueryResult transaction(TransactionAction action,const Cancellation& cancel)override{return database_->transaction(action,cancel);}
    QueryResult jdbcMetadata(JdbcMetadata kind,const QString& schema,const QString& object,QueryLimits limits,const Cancellation& cancel)override{
        return database_->jdbcMetadata(kind,schema,object,limits,cancel);
    }
    QueryResult redisKeys(QueryLimits limits,const Cancellation& cancel)override{
        return database_->redisKeys(limits,cancel);
    }
    QueryResult redisPreview(const QByteArray& key,QueryLimits limits,const Cancellation& cancel)override{
        return database_->redisPreview(key,limits,cancel);
    }
    QStringList tables()override{return database_->tables();}
    QString serverVersion()override{return database_->serverVersion();}
};
}
std::unique_ptr<Database> connectDatabase(const ConnectionProfile& original){
    if(!versionBelongsTo(original.version,original.driver))fail("数据库版本与类型不匹配");auto p=resolveCredentials(original);validateConnectionOptions(p);
    auto lease=std::make_unique<SessionLease>(p);
    if(p.http.enabled){if(!p.advanced.initialSql.trimmed().isEmpty())fail("HTTP 隧道不支持跨请求保存初始会话设置");return std::make_unique<LeasedDatabase>(std::move(lease),connectHttpTunnel(p)); }
    std::unique_ptr<SshTunnel> tunnel;if(p.ssh.enabled){tunnel=openSshTunnel(p);p.transportHost="127.0.0.1";p.transportPort=tunnel->port();}
    auto database=connectDirect(p);
    if(p.driver==DatabaseKind::Redis){
        for(const auto& line:p.advanced.initialSql.split('\n')){
            const auto result=database->execute(line,{},cancellation());
            if(!result.error.isEmpty())fail("Redis 初始命令失败："+result.error);
        }
    }else if(!p.advanced.initialSql.trimmed().isEmpty()){
        const auto dialect=scriptDialect(p.driver);
        const auto bytes=p.advanced.initialSql.toUtf8();const auto spans=sql::splitStatements(std::string_view(bytes.constData(),bytes.size()),dialect);
        int index=0;for(const auto& span:spans){++index;const auto original=sql::statementSource(std::string_view(bytes.constData(),bytes.size()),span,dialect);const auto statement=QString::fromUtf8(original.data(),int(original.size()));auto result=span.compound?database->executeDefinition(statement,{},cancellation()):database->execute(statement,{},cancellation());if(!result.error.isEmpty() || result.cancelled)fail(QString("初始查询第 %1 条失败：").arg(index)+result.error);}
    }
    if(tunnel)database=std::make_unique<TunnelDatabase>(std::move(tunnel),std::move(database));
    return std::make_unique<LeasedDatabase>(std::move(lease),std::move(database));
}

}
