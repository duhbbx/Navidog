#pragma once

#include "database_types.hpp"
#include <QByteArray>
#include <QUrl>

namespace navidog {
enum class DriverPackage { Java, Redis, H2, Dameng, Db2, Kingbase, GaussDb, OpenGauss };
struct DriverDownload {
    QString asset;
    QUrl url;
    QByteArray sha256;
    qint64 maxBytes = 0;
    bool archive = false;
};

QString driverPackageDirectory(DriverPackage);
QString packagePlatform();
QString javaPackageExecutable();
QString managedJdbcJar(DatabaseKind);
QString jdbcJarPath(DatabaseKind, const QString& selected);
DriverDownload driverDownload(DriverPackage);
DriverDownload githubDriverAsset(DriverDownload, const QByteArray& releaseJson);
QString redisPackageLibraryName();
bool jdbcBridgeSupported();
bool managedRedisSupported();
}
