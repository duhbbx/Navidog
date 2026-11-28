#include "navidog/driver_packages.hpp"
#include "jdbc_bridge.hpp"
#include <QFileInfo>
#include <QHostAddress>
#include <QRegularExpression>

namespace navidog {

std::unique_ptr<Database> connectDameng(const ConnectionProfile& profile) {
    auto host = profile.transportHost.isEmpty() ? profile.host : profile.transportHost;
    const auto port = profile.transportPort ? profile.transportPort : profile.port;
    if (host.startsWith('[') && host.endsWith(']'))
        host = host.mid(1, host.size() - 2);
    if (QHostAddress(host).protocol() == QAbstractSocket::IPv6Protocol && !host.contains('%')) {
        host = "[" + host + "]";
    } else {
        static const QRegularExpression hostName(QStringLiteral("^[A-Za-z0-9_.-]+$"));
        if (!hostName.match(host).hasMatch())
            throw std::runtime_error("达梦主机名或 IP 地址无效");
    }
    JdbcConnectionSpec spec;
    spec.driverJar = QFileInfo(jdbcJarPath(profile.driver, profile.jdbcDriverJar)).absoluteFilePath();
    spec.driverClass = "dm.jdbc.driver.DmDriver";
    spec.url = QString("jdbc:dm://%1:%2").arg(host).arg(port);
    spec.properties = {{"user", profile.user}, {"password", profile.password},
                       {"connectTimeout", QString::number(profile.advanced.connectTimeout * 1000)},
                       {"socketTimeout", QString::number(profile.advanced.readTimeout * 1000)},
                       {"appName", "Navidog"},
                       {"autoReconnect", "0"}};
    if (profile.tls) {
        spec.properties.insert("sslFilesPath", QFileInfo(profile.ssl.damengKeystoreDirectory).absoluteFilePath());
        spec.properties.insert("sslKeystorePass", profile.ssl.damengKeystorePassword.isEmpty()
            ? QStringLiteral("changeit") : profile.ssl.damengKeystorePassword);
        spec.verification = JdbcTransportVerification::DamengTls;
    }
    spec.scriptDialect = sql::Dialect::Oracle;
    spec.connectTimeout = profile.advanced.connectTimeout;
    spec.readTimeout = profile.advanced.readTimeout;
    return connectJdbc(spec);
}

}
