#include "navidog/database.hpp"
#include "collector.hpp"
#include "cancel_watch.hpp"
#include <dpi.h>
#include <QStandardPaths>
#include <QDir>
namespace navidog {
namespace {
QString oracleResultType(const dpiDataTypeInfo& info){
    auto sized=[&](const char* name){return info.sizeInChars?QString("%1(%2)").arg(name).arg(info.sizeInChars):QString(name);};
    switch(info.oracleTypeNum){
    case DPI_ORACLE_TYPE_VARCHAR:return sized("VARCHAR2");case DPI_ORACLE_TYPE_NVARCHAR:return sized("NVARCHAR2");case DPI_ORACLE_TYPE_CHAR:return sized("CHAR");case DPI_ORACLE_TYPE_NCHAR:return sized("NCHAR");
    case DPI_ORACLE_TYPE_NUMBER:return info.precision>0?QString("NUMBER(%1,%2)").arg(int(info.precision)).arg(int(info.scale)):"NUMBER";
    case DPI_ORACLE_TYPE_DATE:return "DATE";case DPI_ORACLE_TYPE_TIMESTAMP:return "TIMESTAMP";case DPI_ORACLE_TYPE_TIMESTAMP_TZ:return "TIMESTAMP WITH TIME ZONE";case DPI_ORACLE_TYPE_TIMESTAMP_LTZ:return "TIMESTAMP WITH LOCAL TIME ZONE";
    case DPI_ORACLE_TYPE_NATIVE_FLOAT:return "BINARY_FLOAT";case DPI_ORACLE_TYPE_NATIVE_DOUBLE:return "BINARY_DOUBLE";case DPI_ORACLE_TYPE_NATIVE_INT:return "PLS_INTEGER";case DPI_ORACLE_TYPE_NATIVE_UINT:return "BINARY_INTEGER";
    case DPI_ORACLE_TYPE_RAW:return QString("RAW(%1)").arg(info.dbSizeInBytes);case DPI_ORACLE_TYPE_LONG_RAW:return "LONG RAW";case DPI_ORACLE_TYPE_LONG_VARCHAR:return "LONG";case DPI_ORACLE_TYPE_LONG_NVARCHAR:return "LONG NVARCHAR";
    case DPI_ORACLE_TYPE_CLOB:return "CLOB";case DPI_ORACLE_TYPE_NCLOB:return "NCLOB";case DPI_ORACLE_TYPE_BLOB:return "BLOB";case DPI_ORACLE_TYPE_BFILE:return "BFILE";
    case DPI_ORACLE_TYPE_INTERVAL_DS:return "INTERVAL DAY TO SECOND";case DPI_ORACLE_TYPE_INTERVAL_YM:return "INTERVAL YEAR TO MONTH";case DPI_ORACLE_TYPE_ROWID:return "ROWID";case DPI_ORACLE_TYPE_UROWID:return "UROWID";
    case DPI_ORACLE_TYPE_BOOLEAN:return "BOOLEAN";case DPI_ORACLE_TYPE_STMT:return "REF CURSOR";case DPI_ORACLE_TYPE_JSON:return "JSON";case DPI_ORACLE_TYPE_XMLTYPE:return "XMLTYPE";case DPI_ORACLE_TYPE_VECTOR:return "VECTOR";
    default:return QString("ORACLE TYPE %1").arg(info.oracleTypeNum);
    }
}
class OracleDatabase final : public Database {
    bool autoCommit_=false;
    int versionMajor_=0;
    bool implicitAvailable_=false;
    dpiContext* context_=nullptr; dpiConn* conn_=nullptr;
    void check(int status) const {
        if (status==DPI_SUCCESS) return;
        dpiErrorInfo error{}; dpiContext_getError(context_,&error);
        native::fail(QString::fromUtf8(error.message,error.messageLength));
    }
    QueryResult readStatement(dpiStmt* stmt,uint32_t count,QueryLimits limits,const Cancellation& cancel){
        native::Collector c(limits);check(dpiStmt_setFetchArraySize(stmt,1));
        QVector<dpiOracleTypeNum> types;
        for(uint32_t col=1;col<=count;++col) {
            dpiQueryInfo info{}; check(dpiStmt_getQueryInfo(stmt,col,&info)); c.result.columns << QString::fromUtf8(info.name,info.nameLength);
            c.result.columnTypes<<oracleResultType(info.typeInfo);auto type=info.typeInfo.oracleTypeNum; types << type;
            if(type==DPI_ORACLE_TYPE_BLOB || type==DPI_ORACLE_TYPE_CLOB || type==DPI_ORACLE_TYPE_NCLOB)
                check(dpiStmt_defineValue(stmt,col,type,DPI_NATIVE_TYPE_LOB,0,1,nullptr));
            else if(type==DPI_ORACLE_TYPE_DATE || type==DPI_ORACLE_TYPE_TIMESTAMP || type==DPI_ORACLE_TYPE_TIMESTAMP_TZ || type==DPI_ORACLE_TYPE_TIMESTAMP_LTZ)
                check(dpiStmt_defineValue(stmt,col,type,DPI_NATIVE_TYPE_TIMESTAMP,0,1,nullptr));
            else {
                // ODPI-C reports an explicit conversion error for unsupported object/vector types.
                auto target=(type==DPI_ORACLE_TYPE_RAW || type==DPI_ORACLE_TYPE_LONG_RAW) ? DPI_ORACLE_TYPE_RAW : DPI_ORACLE_TYPE_VARCHAR;
                check(dpiStmt_defineValue(stmt,col,target,DPI_NATIVE_TYPE_BYTES,limits.maxCellBytes,1,nullptr));
            }
        }
        while(count && !cancel->load()) {
            int found=0; uint32_t index=0; check(dpiStmt_fetch(stmt,&found,&index)); if(!found) break;
            QVector<QVariant> row;
            for(uint32_t col=1;col<=count;++col) {
                dpiNativeTypeNum nativeType; dpiData* data; check(dpiStmt_getQueryValue(stmt,col,&nativeType,&data));
                if(data->isNull) { row << QVariant{}; continue; }
                auto type=types[col-1];
                if(nativeType==DPI_NATIVE_TYPE_TIMESTAMP){
                    const auto* stamp=dpiData_getTimestamp(data);
                    auto value=QString("%1-%2-%3 %4:%5:%6").arg(stamp->year,4,10,QChar('0')).arg(stamp->month,2,10,QChar('0')).arg(stamp->day,2,10,QChar('0')).arg(stamp->hour,2,10,QChar('0')).arg(stamp->minute,2,10,QChar('0')).arg(stamp->second,2,10,QChar('0'));
                    if(type!=DPI_ORACLE_TYPE_DATE)value+="."+QString::number(stamp->fsecond).rightJustified(9,'0');
                    if(type==DPI_ORACLE_TYPE_TIMESTAMP_TZ){const int minutes=stamp->tzHourOffset*60+stamp->tzMinuteOffset;value+=QString(" %1%2:%3").arg(minutes<0?"-":"+").arg(qAbs(minutes)/60,2,10,QChar('0')).arg(qAbs(minutes)%60,2,10,QChar('0'));}
                    row<<value;
                }else if(nativeType==DPI_NATIVE_TYPE_LOB) {
                    auto* lob=data->value.asLOB; uint64_t total=0; check(dpiLob_getSize(lob,&total));
                    // CLOB sizes are characters; UTF-8 can require four bytes per character.
                    const bool binary=type==DPI_ORACLE_TYPE_BLOB; auto amount=std::min<uint64_t>(total,binary ? limits.maxCellBytes : std::max(1,limits.maxCellBytes/4));
                    QByteArray buffer(limits.maxCellBytes,'\0'); uint64_t size=buffer.size();
                    if(amount) check(dpiLob_readBytes(lob,1,amount,buffer.data(),&size)); else size=0;
                    buffer.resize(size); const bool cut=amount<total; c.result.cellTruncated |= cut;
                    if(binary) row << buffer; else row << (QString::fromUtf8(buffer) + (cut ? "… [已截断]" : ""));
                } else {
                    auto value=dpiData_getBytes(data); auto take=std::min<uint32_t>(value->length,limits.maxCellBytes); c.result.cellTruncated |= take<value->length;
                    if(type==DPI_ORACLE_TYPE_RAW || type==DPI_ORACLE_TYPE_LONG_RAW) row << QByteArray(value->ptr,take);
                    else row << (QString::fromUtf8(value->ptr,take) + (take<value->length ? "… [已截断]" : ""));
                }
            }
            if(!c.add(std::move(row))) break;
        }
        if(!count) { uint64_t changed=0; check(dpiStmt_getRowCount(stmt,&changed)); c.result.affectedRows=changed; }
        c.result.cancelled=cancel->load(); if(c.result.cancelled) { dpiConn_breakExecution(conn_); c.result.rows.clear(); }
        return c.result;
    }
public:
    explicit OracleDatabase(const ConnectionProfile& p) {
        dpiErrorInfo error{};
        dpiContextCreateParams contextParams{};
        auto clientPath=p.oracle.clientDirectory.isEmpty()?qEnvironmentVariable("NAVIDOG_ORACLE_CLIENT_DIR"):p.oracle.clientDirectory;
#ifdef Q_OS_MAC
        if(clientPath.isEmpty()) {
            const QDir data(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
            for (const auto directory : {"Navidog/instantclient", "Harbor/instantclient"}) {
                const auto candidate = data.filePath(directory);
                if (QDir(candidate).exists()) {
                    clientPath = candidate;
                    break;
                }
            }
        }
#endif
        auto clientBytes=clientPath.toUtf8(); if(!clientBytes.isEmpty()) contextParams.oracleClientLibDir=clientBytes.constData();
        auto configBytes=p.oracle.configDirectory.toUtf8();if(!configBytes.isEmpty())contextParams.oracleClientConfigDir=configBytes.constData();
        if (dpiContext_createWithParams(DPI_MAJOR_VERSION,DPI_MINOR_VERSION,&contextParams,&context_,&error)!=DPI_SUCCESS)
            native::fail("Oracle 客户端初始化失败：" + QString::fromUtf8(error.message,error.messageLength) + "\n请安装与本机架构一致的 Oracle Instant Client，并配置系统动态库搜索路径。");
        try {
            auto address=oracleConnectDescriptor(p).toUtf8();
            auto user=p.user.toUtf8(), password=p.password.toUtf8(); dpiCommonCreateParams common{};
            check(dpiContext_initCommonCreateParams(context_,&common)); common.encoding="UTF-8";common.nencoding="UTF-8";common.createMode=DPI_MODE_CREATE_THREADED;
            dpiConnCreateParams create{};check(dpiContext_initConnCreateParams(context_,&create));create.externalAuth=p.oracle.externalAuthentication;create.authMode=p.oracle.role==OracleRole::SysDBA?DPI_MODE_AUTH_SYSDBA:p.oracle.role==OracleRole::SysOPER?DPI_MODE_AUTH_SYSOPER:DPI_MODE_AUTH_DEFAULT;
            check(dpiConn_create(context_,user.constData(),user.size(),password.constData(),password.size(),address.constData(),address.size(),&common,&create,&conn_));
            check(dpiConn_setCallTimeout(conn_,p.advanced.readTimeout*1000));
            const char* release=nullptr;uint32_t length=0;dpiVersionInfo version{};check(dpiConn_getServerVersion(conn_,&release,&length,&version));versionMajor_=version.versionNum;dpiVersionInfo clientVersion{};check(dpiContext_getClientVersion(context_,&clientVersion));implicitAvailable_=versionMajor_>=12 && clientVersion.versionNum>=12;
            if (p.readOnly) native::fail("Oracle 只读事务不会自动覆盖后续事务，请使用服务器只读账号并取消只读会话选项。");
        } catch (...) { if (conn_) dpiConn_release(conn_); dpiContext_destroy(context_); context_=nullptr; conn_=nullptr; throw; }
    }
    void setAutoCommit(bool value)override{autoCommit_=value;}
    bool healthy()const override{int value=0;return dpiConn_getIsHealthy(conn_,&value)==DPI_SUCCESS && value;}
    TransactionState transactionState()const override{int active=0;if(dpiConn_getTransactionInProgress(conn_,&active)!=DPI_SUCCESS)return TransactionState::Unknown;return active?TransactionState::Active:TransactionState::Idle;}
    QueryResult transaction(TransactionAction action,const Cancellation& cancel)override{if(cancel->load()){QueryResult r;r.cancelled=true;return r;}if(action==TransactionAction::Begin)autoCommit_=false;else if(action==TransactionAction::Commit)check(dpiConn_commit(conn_));else if(action==TransactionAction::Rollback)check(dpiConn_rollback(conn_));return {};}
    QString serverVersion() override {
        const char* release=nullptr; uint32_t length=0; dpiVersionInfo version{};
        check(dpiConn_getServerVersion(conn_,&release,&length,&version));
        return QString("%1.%2.%3").arg(version.versionNum).arg(version.releaseNum).arg(version.updateNum);
    }
    ~OracleDatabase() override { if(conn_) dpiConn_release(conn_); if(context_) dpiContext_destroy(context_); }
    QueryResult execute(const QString& sql,QueryLimits limits,const Cancellation& cancel)override{return executeBound(sql,{},limits,cancel);}
    QueryResult executeBound(const QString& sql,const QVector<QVariant>& parameters,QueryLimits limits,const Cancellation& cancel)override {
        native::validate(sql,sql::Dialect::Oracle,definitionMode_); native::Collector c(limits);
        if(cancel->load()){c.result.cancelled=true;return c.result;}native::CancelWatch stop(cancel,[this]{dpiConn_breakExecution(conn_);});
        auto bytes=sql.toUtf8();
        const auto tokens=sql::lex(bytes.toStdString(),sql::Dialect::Oracle).tokens;
        // OCI excludes the SQL terminator, even when followed by trailing comments.
        if(!definitionMode_) for (auto it=tokens.rbegin();it!=tokens.rend();++it) if (it->kind==sql::TokenKind::Symbol && bytes[it->begin]==';') { bytes.remove(it->begin,it->end-it->begin); break; }
        dpiStmt* stmt=nullptr;
        check(dpiConn_prepareStmt(conn_,0,bytes.constData(),bytes.size(),nullptr,0,&stmt));
        struct Release { dpiStmt* stmt; ~Release(){dpiStmt_release(stmt);} } release{stmt};
        uint32_t bindCount=0;check(dpiStmt_getBindCount(stmt,&bindCount));if(bindCount!=static_cast<uint32_t>(parameters.size()))native::fail("Oracle 绑定参数数量不匹配");
        struct BindResources{QVector<dpiVar*> vars;QVector<dpiLob*> lobs;~BindResources(){for(auto* var:vars)dpiVar_release(var);for(auto* lob:lobs)dpiLob_release(lob);}}bindings;
        QVector<QByteArray> values;values.reserve(parameters.size());for(auto value:parameters)values<< (value.userType()==QMetaType::QByteArray?value.toByteArray():value.toString().toUtf8());
        for(int i=0;i<parameters.size();++i){const auto& value=parameters[i];dpiData data{};if(!value.isValid() || value.isNull()){dpiData_setNull(&data);check(dpiStmt_bindValueByPos(stmt,i+1,DPI_NATIVE_TYPE_BYTES,&data));}else if(value.userType()==QMetaType::Int || value.userType()==QMetaType::LongLong || value.userType()==QMetaType::Bool){dpiData_setInt64(&data,value.toLongLong());check(dpiStmt_bindValueByPos(stmt,i+1,DPI_NATIVE_TYPE_INT64,&data));}else if(value.userType()==QMetaType::Double){dpiData_setDouble(&data,value.toDouble());check(dpiStmt_bindValueByPos(stmt,i+1,DPI_NATIVE_TYPE_DOUBLE,&data));}else{bool binary=value.userType()==QMetaType::QByteArray;auto& bytes=values[i];if(binary || bytes.size()>32767){auto type=binary?(bytes.size()>32767?DPI_ORACLE_TYPE_BLOB:DPI_ORACLE_TYPE_RAW):DPI_ORACLE_TYPE_CLOB;dpiVar* variable=nullptr;dpiData* target=nullptr;bool lob=type==DPI_ORACLE_TYPE_BLOB || type==DPI_ORACLE_TYPE_CLOB;check(dpiConn_newVar(conn_,type,lob?DPI_NATIVE_TYPE_LOB:DPI_NATIVE_TYPE_BYTES,1,qMax(1,bytes.size()),1,0,nullptr,&variable,&target));bindings.vars<<variable;if(lob){dpiLob* object=nullptr;check(dpiConn_newTempLob(conn_,type,&object));bindings.lobs<<object;check(dpiLob_setFromBytes(object,bytes.constData(),bytes.size()));check(dpiVar_setFromLob(variable,0,object));}else check(dpiVar_setFromBytes(variable,0,bytes.constData(),bytes.size()));check(dpiStmt_bindByPos(stmt,i+1,variable));}else{dpiData_setBytes(&data,bytes.data(),bytes.size());check(dpiStmt_bindValueByPos(stmt,i+1,DPI_NATIVE_TYPE_BYTES,&data));}}}
        if(cancel->load()) { c.result.cancelled=true; return c.result; }
        check(dpiStmt_setFetchArraySize(stmt,1)); check(dpiStmt_setPrefetchRows(stmt,0));
        dpiStmtInfo statementInfo{};check(dpiStmt_getInfo(stmt,&statementInfo));
        uint32_t count=0; check(dpiStmt_execute(stmt,autoCommit_?DPI_MODE_EXEC_COMMIT_ON_SUCCESS:DPI_MODE_EXEC_DEFAULT,&count));
        auto result=readStatement(stmt,count,limits,cancel);qint64 remaining=limits.maxBytes-result.cachedBytes;int sets=0;
        if(implicitAvailable_ && statementInfo.isPLSQL)for(;;){dpiStmt* cursor=nullptr;check(dpiStmt_getImplicitResult(stmt,&cursor));if(!cursor)break;struct Cursor{dpiStmt* value;~Cursor(){dpiStmt_release(value);}}owned{cursor};if(++sets>32 || remaining<128){result.truncated=true;continue;}uint32_t columns=0;check(dpiStmt_getNumQueryColumns(cursor,&columns));auto current=limits;current.maxBytes=remaining;auto extra=readStatement(cursor,columns,current,cancel);remaining-=extra.cachedBytes;result.moreResults<<std::move(extra);if(cancel->load()){result.cancelled=true;break;}}
        return result;
    }
    QStringList tables() override {
        auto r=execute("SELECT table_name FROM user_tables ORDER BY table_name",{},cancellation()); QStringList names; for(const auto& row:r.rows) names << row[0].toString(); return names;
    }
};
}
std::unique_ptr<Database> connectOracle(const ConnectionProfile& p) { return std::make_unique<OracleDatabase>(p); }
}
