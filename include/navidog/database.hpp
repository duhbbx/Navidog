#pragma once
#include "types.hpp"
#include <functional>
#include <stdexcept>

namespace navidog {
enum class JdbcMetadata : quint8 { Schemas, Tables, Columns, PrimaryKeys, Functions, Procedures };

class Database {
protected:
    bool definitionMode_=false;
public:
    virtual ~Database() = default;
    virtual QueryResult execute(const QString&, QueryLimits, const Cancellation&) = 0;
    virtual QueryResult executeBound(const QString&,const QVector<QVariant>&,QueryLimits,const Cancellation&) {
        throw std::runtime_error("此驱动尚未实现参数绑定");
    }
    // Schema editors may submit one compound CREATE TRIGGER to the native driver.
    QueryResult executeDefinition(const QString&,QueryLimits,const Cancellation&,
                                  const QVector<QVariant>& values = {});
    virtual TransactionState transactionState()const{return TransactionState::Unknown;}
    virtual bool healthy()const{return true;}
    virtual void setAutoCommit(bool){}
    virtual QueryResult transaction(TransactionAction,const Cancellation&);
    virtual QueryResult jdbcMetadata(JdbcMetadata, const QString&, const QString&, QueryLimits, const Cancellation&) {
        throw std::runtime_error("此驱动未提供 JDBC 元数据");
    }
    virtual QueryResult redisKeys(QueryLimits, const Cancellation&) {
        throw std::runtime_error("此驱动未提供 Redis 键列表");
    }
    virtual QueryResult redisPreview(const QByteArray&, QueryLimits, const Cancellation&) {
        throw std::runtime_error("此驱动未提供 Redis 键预览");
    }
    virtual QStringList tables() = 0;
    virtual QString serverVersion() = 0;
};
struct DriverInfo { DatabaseKind id; QString name, provider, status; bool ready; int defaultPort; };
QVector<DriverInfo> driverRegistry();
QVector<DatabaseKind> availableDrivers();
std::unique_ptr<Database> connectDatabase(const ConnectionProfile&);
// Creates only project-owned demo data. Existing arbitrary files are never overwritten.
QString ensureDemoDatabase(const QString& directory);
}
