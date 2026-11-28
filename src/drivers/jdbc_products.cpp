#include "navidog/driver_packages.hpp"
#include "navidog/database_sql.hpp"
#include "jdbc_bridge.hpp"
#include <QFileInfo>
#include <QHostAddress>
#include <QRegularExpression>
#include <QUrl>

namespace navidog {
namespace {
struct VendorDriver {
    DatabaseKind kind;
    const char* name;
    const char* protocol;
};
constexpr VendorDriver vendors[] = {
    {DatabaseKind::OpenGauss, "org.opengauss.Driver", "opengauss"},
    {DatabaseKind::H2, "org.h2.Driver", "h2:tcp"},
    {DatabaseKind::Kingbase, "com.kingbase8.Driver", "kingbase8"},
    {DatabaseKind::UxDb, "com.uxsino.uxdb.Driver", "uxdb"},
    {DatabaseKind::GaussDb, "com.huawei.gaussdb.jdbc.Driver", "gaussdb"},
    {DatabaseKind::Db2, "com.ibm.db2.jcc.DB2Driver", "db2"},
};

QString serverEndpoint(const ConnectionProfile& p) {
    auto host = p.transportHost.isEmpty() ? p.host : p.transportHost;
    if (host.startsWith('[') && host.endsWith(']'))
        host = host.mid(1, host.size() - 2);
    if (QHostAddress(host).protocol() == QAbstractSocket::IPv6Protocol && !host.contains('%'))
        host = '[' + host + ']';
    else if (!QRegularExpression("^[A-Za-z0-9_.-]+$").match(host).hasMatch())
        throw std::runtime_error("数据库主机名或 IP 地址无效");
    return host + ':' + QString::number(p.transportPort ? p.transportPort : p.port);
}
}

std::unique_ptr<Database> connectJdbcProduct(const ConnectionProfile& p) {
    const VendorDriver* vendor = nullptr;
    for (const auto& candidate : vendors)
        if (candidate.kind == p.driver)
            vendor = &candidate;
    if (!vendor)
        throw std::runtime_error("此数据库未配置 JDBC 驱动");
    JdbcConnectionSpec spec;
    spec.driverJar = QFileInfo(jdbcJarPath(p.driver,p.jdbcDriverJar)).absoluteFilePath();
    const bool gaussOpenDriver = p.driver == DatabaseKind::GaussDb &&
        QFileInfo(jdbcJarPath(p.driver,p.jdbcDriverJar)).fileName().startsWith("opengaussjdbc", Qt::CaseInsensitive);
    spec.driverClass = QString::fromLatin1(gaussOpenDriver ? "com.huawei.opengauss.jdbc.Driver" : vendor->name);
    spec.properties = {{"user", p.user}, {"password", p.password}};
    spec.scriptDialect = scriptDialect(p.driver);
    spec.connectTimeout = p.advanced.connectTimeout;
    spec.readTimeout = p.advanced.readTimeout;

    if (p.driver == DatabaseKind::H2 && p.h2File) {
        const auto path = QFileInfo(p.database).absoluteFilePath();
        if (path.contains(QChar(0)) || path.contains(';') || path.contains('\\'))
            throw std::runtime_error("H2 文件路径不能包含 JDBC 选项分隔符");
        spec.url = "jdbc:h2:file:" + path.left(path.size() - 6) + ";IFEXISTS=TRUE;AUTO_SERVER=TRUE";
    } else {
        const auto endpoint = serverEndpoint(p);
        if (p.driver == DatabaseKind::H2) {
            const auto name = p.database;
            if (name.contains(QChar(0)) || name.contains(';') || name.contains('\\'))
                throw std::runtime_error("H2 数据库路径不能包含 JDBC 选项分隔符");
            spec.url = "jdbc:h2:tcp://" + endpoint + '/' + name + ";IFEXISTS=TRUE";
        } else {
            const auto encoded = QUrl::toPercentEncoding(p.database);
            if (p.driver == DatabaseKind::Db2 && encoded != p.database.toUtf8())
                throw std::runtime_error("Db2 数据库名称包含不支持的字符");
            spec.url = QString("jdbc:%1://%2/%3").arg(QString::fromLatin1(gaussOpenDriver ? "opengauss" : vendor->protocol), endpoint,
                                                       QString::fromLatin1(encoded));
            if (usesPostgresCompatibleSql(p.driver)) {
                spec.properties.insert("connectTimeout", QString::number(p.advanced.connectTimeout));
                spec.properties.insert("socketTimeout", QString::number(p.advanced.readTimeout));
                spec.properties.insert("sslmode", "disable");
            }
        }
    }
    return connectJdbc(spec);
}

}
