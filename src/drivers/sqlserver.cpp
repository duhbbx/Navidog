#include "navidog/database.hpp"
#include "collector.hpp"
#include "cancel_watch.hpp"
#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif
#include <sql.h>
#include <sqlext.h>
#include <QElapsedTimer>
#include <algorithm>
#include <array>
#include <vector>
namespace navidog {
QString microsoftOdbcDriver();
namespace {
QString diagnostic(SQLSMALLINT type, SQLHANDLE handle) {
    QStringList messages; SQLWCHAR state[6], message[2048]; SQLINTEGER code; SQLSMALLINT len;
    for (SQLSMALLINT i=1; SQL_SUCCEEDED(SQLGetDiagRecW(type, handle, i, state, &code, message, 2048, &len)); ++i)
        messages << QString::fromUtf16(reinterpret_cast<const char16_t*>(message), std::min<int>(len,2047));
    return messages.join('\n');
}
struct Handles {
    SQLHENV env = SQL_NULL_HENV; SQLHDBC dbc = SQL_NULL_HDBC;
    Handles() {
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env))) native::fail("无法创建 ODBC 环境");
        if (!SQL_SUCCEEDED(SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0))) {
            SQLFreeHandle(SQL_HANDLE_ENV,env); env=SQL_NULL_HENV; native::fail("无法设置 ODBC 版本");
        }
    }
    ~Handles() { if (dbc != SQL_NULL_HDBC) { SQLDisconnect(dbc); SQLFreeHandle(SQL_HANDLE_DBC,dbc); } if (env != SQL_NULL_HENV) SQLFreeHandle(SQL_HANDLE_ENV,env); }
};
SQLWCHAR* wide(QString& s) { static_assert(sizeof(SQLWCHAR)==sizeof(char16_t)); return reinterpret_cast<SQLWCHAR*>(s.data()); }
QString escaped(QString s) { s.replace("}", "}}"); return "{" + s + "}"; }
class SqlServerDatabase final : public Database {
    Handles handles_;
    int queryTimeout_=0;
public:
    explicit SqlServerDatabase(const ConnectionProfile& p):queryTimeout_(p.advanced.readTimeout) {
        auto driver = microsoftOdbcDriver();
        if (driver.isEmpty()) native::fail("未检测到 Microsoft ODBC Driver 18 / 17 for SQL Server。请安装微软官方驱动后重试。");
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, handles_.env, &handles_.dbc))) native::fail("无法分配 ODBC 连接");
        SQLSetConnectAttr(handles_.dbc,SQL_LOGIN_TIMEOUT,reinterpret_cast<SQLPOINTER>(qintptr(p.advanced.connectTimeout)),0);
        // ReadOnly is enforced with server transactions, not merely ODBC's advisory access mode.
        if (p.readOnly) native::fail("SQL Server 尚未实现可靠的只读会话，请使用服务器只读账号并取消此选项。");
        if (p.host.contains(';') || p.host.contains('{') || p.host.contains('}')) native::fail("主机名不能包含 ODBC 分隔符");
        auto text = "DRIVER=" + escaped(driver) + ";SERVER=" + escaped((p.transportHost.isEmpty()?p.host:p.transportHost) + "," + QString::number(p.transportPort?p.transportPort:p.port)) + (p.database.isEmpty()?QString{}:";DATABASE=" + escaped(p.database))
            + ";UID=" + escaped(p.user) + ";PWD=" + escaped(p.password) + ";Encrypt=" + (p.tls ? "yes" : "no") + ";TrustServerCertificate="+QString(p.ssl.verification==SslVerification::Encryption?"yes":"no")+";APP="+escaped(p.advanced.applicationName)+";";
        if(p.tls && !p.ssl.serverName.isEmpty()){if(!driver.contains("18"))native::fail("服务器主机名验证需要 Microsoft ODBC Driver 18");text+="HostNameInCertificate="+escaped(p.ssl.serverName)+";";}
        auto rc = SQLDriverConnectW(handles_.dbc, nullptr, wide(text), SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
        text.fill(QChar(0));
        if (!SQL_SUCCEEDED(rc)) native::fail(diagnostic(SQL_HANDLE_DBC,handles_.dbc));
    }
    bool healthy()const override{SQLUINTEGER dead=SQL_CD_TRUE;return SQL_SUCCEEDED(SQLGetConnectAttr(handles_.dbc,SQL_ATTR_CONNECTION_DEAD,&dead,0,nullptr)) && dead==SQL_CD_FALSE;}
    TransactionState transactionState()const override{SQLHSTMT stmt=SQL_NULL_HSTMT;if(!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT,handles_.dbc,&stmt)))return TransactionState::Unknown;struct Release{SQLHSTMT s;~Release(){SQLFreeHandle(SQL_HANDLE_STMT,s);}}release{stmt};SQLSetStmtAttr(stmt,SQL_ATTR_QUERY_TIMEOUT,reinterpret_cast<SQLPOINTER>(5),0);QString sql="SELECT XACT_STATE()";if(!SQL_SUCCEEDED(SQLExecDirectW(stmt,wide(sql),SQL_NTS)) || !SQL_SUCCEEDED(SQLFetch(stmt)))return TransactionState::Unknown;SQLINTEGER state=0;SQLLEN size=0;if(!SQL_SUCCEEDED(SQLGetData(stmt,1,SQL_C_SLONG,&state,sizeof(state),&size)))return TransactionState::Unknown;return state<0?TransactionState::Failed:state>0?TransactionState::Active:TransactionState::Idle;}
    QueryResult transaction(TransactionAction action,const Cancellation& cancel)override{return execute(action==TransactionAction::Begin?"BEGIN TRANSACTION":action==TransactionAction::Commit?"COMMIT TRANSACTION":"ROLLBACK TRANSACTION",{},cancel);}
    QString serverVersion() override {
        SQLWCHAR buffer[256]; SQLSMALLINT bytes=0;
        if(!SQL_SUCCEEDED(SQLGetInfoW(handles_.dbc,SQL_DBMS_VER,buffer,sizeof(buffer),&bytes))) native::fail(diagnostic(SQL_HANDLE_DBC,handles_.dbc));
        return QString::fromUtf16(reinterpret_cast<char16_t*>(buffer),std::min<int>(bytes/sizeof(SQLWCHAR),255));
    }
    QueryResult execute(const QString& sql, QueryLimits limits, const Cancellation& cancel) override {
        return executeBound(sql, {}, limits, cancel);
    }
    QueryResult executeBound(const QString& sql, const QVector<QVariant>& parameters, QueryLimits limits,
                             const Cancellation& cancel) override {
        native::validate(sql, sql::Dialect::SQLServer,definitionMode_); native::Collector c(limits);
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT,handles_.dbc,&stmt))) native::fail("无法创建 ODBC 语句");
        struct Release { SQLHSTMT s; ~Release(){SQLFreeHandle(SQL_HANDLE_STMT,s);} } release{stmt};
        // Bounded network calls; cooperative cancellation between calls.
        SQLSetStmtAttr(stmt,SQL_ATTR_QUERY_TIMEOUT,reinterpret_cast<SQLPOINTER>(qintptr(queryTimeout_)),0);
        if (cancel->load()) { c.result.cancelled=true; return c.result; }
        struct Parameter {
            QString text;
            QByteArray bytes;
            SQLBIGINT integer = 0;
            double real = 0;
            unsigned char boolean = 0;
            SQLLEN indicator = 0;
        };
        std::vector<Parameter> storage(static_cast<std::size_t>(parameters.size()));
        native::CancelWatch stop(cancel,[stmt]{SQLCancel(stmt);});
        QString text=sql;
        const auto check = [stmt](SQLRETURN status) {
            if (!SQL_SUCCEEDED(status)) native::fail(diagnostic(SQL_HANDLE_STMT,stmt));
        };
        if (parameters.isEmpty()) check(SQLExecDirectW(stmt,wide(text),SQL_NTS));
        else {
            if (parameters.size() > 2100) native::fail("SQL Server 单条语句最多支持 2100 个参数");
            check(SQLPrepareW(stmt, wide(text), SQL_NTS));
            SQLSMALLINT count = 0;
            check(SQLNumParams(stmt, &count));
            if (count != parameters.size()) native::fail("SQL Server 参数数量不匹配");
            for (int i = 0; i < count; ++i) {
                auto& saved = storage[i];
                const auto& value = parameters[i];
                const auto position = SQLUSMALLINT(i + 1);
                SQLSMALLINT cType = SQL_C_WCHAR, sqlType = SQL_WVARCHAR, scale = 0;
                SQLULEN size = 1;
                SQLLEN bufferLength = 0;
                SQLPOINTER data = nullptr;
                if (!value.isValid() || value.isNull()) {
                    saved.indicator = SQL_NULL_DATA;
                    SQLSMALLINT nullable = 0;
                    if (!SQL_SUCCEEDED(SQLDescribeParam(stmt, position, &sqlType, &size, &scale, &nullable))) {
                        sqlType = SQL_WVARCHAR;
                        size = 1;
                        scale = 0;
                    }
                } else if (value.userType() == QMetaType::Bool) {
                    cType = SQL_C_BIT;
                    sqlType = SQL_BIT;
                    saved.boolean = value.toBool();
                    data = &saved.boolean;
                } else if (value.userType() == QMetaType::Int || value.userType() == QMetaType::LongLong) {
                    cType = SQL_C_SBIGINT;
                    sqlType = SQL_BIGINT;
                    size = 19;
                    saved.integer = value.toLongLong();
                    data = &saved.integer;
                } else if (value.userType() == QMetaType::Double) {
                    cType = SQL_C_DOUBLE;
                    sqlType = SQL_DOUBLE;
                    size = 15;
                    saved.real = value.toDouble();
                    data = &saved.real;
                } else if (value.userType() == QMetaType::QByteArray) {
                    cType = SQL_C_BINARY;
                    saved.bytes = value.toByteArray();
                    size = SQLULEN(qMax<qsizetype>(1, saved.bytes.size()));
                    sqlType = size > 8000 ? SQL_LONGVARBINARY : SQL_VARBINARY;
                    bufferLength = saved.indicator = SQLLEN(saved.bytes.size());
                    data = saved.bytes.data();
                } else {
                    saved.text = value.toString();
                    size = SQLULEN(qMax<qsizetype>(1, saved.text.size()));
                    sqlType = size > 4000 ? SQL_WLONGVARCHAR : SQL_WVARCHAR;
                    saved.indicator = SQLLEN(saved.text.size() * sizeof(SQLWCHAR));
                    bufferLength = saved.indicator + sizeof(SQLWCHAR);
                    data = wide(saved.text);
                }
                check(SQLBindParameter(stmt, position, SQL_PARAM_INPUT, cType, sqlType, size,
                                       scale, data, bufferLength, &saved.indicator));
            }
            if (cancel->load()) { c.result.cancelled = true; return c.result; }
            check(SQLExecute(stmt));
        }
        SQLSMALLINT count=0; SQLNumResultCols(stmt,&count);
        QVector<SQLSMALLINT> types;
        for (SQLUSMALLINT col=1; col<=count; ++col) {
            SQLWCHAR name[1024]; SQLSMALLINT len,type,decimals,nullable; SQLULEN size;
            if (!SQL_SUCCEEDED(SQLDescribeColW(stmt,col,name,1024,&len,&type,&size,&decimals,&nullable))) native::fail(diagnostic(SQL_HANDLE_STMT,stmt));
            c.result.columns << QString::fromUtf16(reinterpret_cast<char16_t*>(name), std::min<int>(len,1023)); types << type;
            SQLWCHAR typeName[128]{};SQLSMALLINT typeLength=0;
            auto described=SQLColAttributeW(stmt,col,SQL_DESC_TYPE_NAME,typeName,sizeof(typeName),&typeLength,nullptr);
            c.result.columnTypes<< (SQL_SUCCEEDED(described)?QString::fromUtf16(reinterpret_cast<char16_t*>(typeName),std::min<int>(127,typeLength/sizeof(SQLWCHAR))):QString("ODBC TYPE %1").arg(type));
        }
        SQLRETURN rc=SQL_SUCCESS;
        if (!count) { SQLLEN changed=0; SQLRowCount(stmt,&changed); c.result.affectedRows=std::max<SQLLEN>(changed,0); }
        while (count && SQL_SUCCEEDED(rc=SQLFetch(stmt))) {
            if (cancel->load()) { SQLCancel(stmt); c.result.cancelled=true; break; }
            QVector<QVariant> row;
            for (SQLUSMALLINT col=1; col<=count; ++col) {
                SQLLEN indicator=0; const bool binary=types[col-1]==SQL_BINARY || types[col-1]==SQL_VARBINARY || types[col-1]==SQL_LONGVARBINARY;
                // A single bounded read per cell. SQLGetData truncation is surfaced explicitly.
                if (binary) {
                    QByteArray buffer(limits.maxCellBytes,'\0'); auto status=SQLGetData(stmt,col,SQL_C_BINARY,buffer.data(),buffer.size(),&indicator);
                    if (indicator==SQL_NULL_DATA) row << QVariant{};
                    else { if (!SQL_SUCCEEDED(status)) native::fail(diagnostic(SQL_HANDLE_STMT,stmt)); const bool cut=indicator==SQL_NO_TOTAL || indicator>buffer.size(); c.result.cellTruncated |= cut; buffer.resize(cut ? buffer.size() : indicator); row << buffer; }
                } else {
                    QVector<SQLWCHAR> buffer(std::max(2,limits.maxCellBytes/2+1));
                    auto status=SQLGetData(stmt,col,SQL_C_WCHAR,buffer.data(),buffer.size()*sizeof(SQLWCHAR),&indicator);
                    if (indicator==SQL_NULL_DATA) row << QVariant{};
                    else {
                        if (!SQL_SUCCEEDED(status)) native::fail(diagnostic(SQL_HANDLE_STMT,stmt));
                        const bool cut=indicator==SQL_NO_TOTAL || indicator>=static_cast<SQLLEN>(buffer.size()*sizeof(SQLWCHAR)); c.result.cellTruncated |= cut;
                        auto size=cut ? buffer.size()-1 : indicator/sizeof(SQLWCHAR);
                        QString value=QString::fromUtf16(reinterpret_cast<char16_t*>(buffer.data()),size); if (cut) value+="… [已截断]"; row << value;
                    }
                }
            }
            if (!c.add(std::move(row))) break;
        }
        if (rc!=SQL_NO_DATA && !SQL_SUCCEEDED(rc)) native::fail(diagnostic(SQL_HANDLE_STMT,stmt));
        if (cancel->load()) c.result.cancelled=true;
        if (c.result.cancelled) c.result.rows.clear();
        // Closing the cursor discards uncached rows. Multiple result sets are not exposed yet.
        SQLCloseCursor(stmt); return c.result;
    }
    QStringList tables() override {
        auto r=execute("SELECT name FROM sys.tables ORDER BY name",{},cancellation()); QStringList names;
        for (const auto& row:r.rows) names << row[0].toString(); return names;
    }
};
}
QString microsoftOdbcDriver() {
    Handles handles; SQLWCHAR name[1024],attrs[2048]; SQLSMALLINT n,a; SQLUSMALLINT direction=SQL_FETCH_FIRST; QString fallback;
    while (SQL_SUCCEEDED(SQLDriversW(handles.env,direction,name,1024,&n,attrs,2048,&a))) {
        direction=SQL_FETCH_NEXT; auto driver=QString::fromUtf16(reinterpret_cast<char16_t*>(name),std::min<int>(n,1023));
        if (driver=="ODBC Driver 18 for SQL Server") return driver;
        if (driver=="ODBC Driver 17 for SQL Server") fallback=driver;
    }
    return fallback;
}
std::unique_ptr<Database> connectSqlServer(const ConnectionProfile& p) { return std::make_unique<SqlServerDatabase>(p); }
}
