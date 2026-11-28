#pragma once

#include "navidog/database.hpp"
#include "navidog/sql.hpp"
#include <QMap>

namespace navidog {

enum class JdbcTransportVerification : quint8 { None, DamengTls };

struct JdbcConnectionSpec {
    QString driverJar;
    QString driverClass;
    QString url;
    QMap<QString, QString> properties;
    sql::Dialect scriptDialect;
    int connectTimeout;
    int readTimeout;
    JdbcTransportVerification verification = JdbcTransportVerification::None;
};

bool bundledJdbcAvailable();
std::unique_ptr<Database> connectJdbc(const JdbcConnectionSpec&);
std::unique_ptr<Database> connectJdbcProduct(const ConnectionProfile&);
std::unique_ptr<Database> connectDameng(const ConnectionProfile&);

}
