#include "navidog/driver_packages.hpp"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>

namespace navidog {
namespace {
constexpr auto releaseRepository = "https://github.com/duhbbx/Navidog/releases/download/";
constexpr auto maven = "https://repo.maven.apache.org/maven2/";
constexpr qint64 mib = 1024 * 1024;

QString packageKey(DriverPackage package) {
    switch (package) {
    case DriverPackage::Java: return "java";
    case DriverPackage::Redis: return "redis";
    case DriverPackage::H2: return "h2";
    case DriverPackage::Dameng: return "dameng";
    case DriverPackage::Db2: return "db2";
    case DriverPackage::Kingbase: return "kingbase";
    case DriverPackage::GaussDb: return "gaussdb";
    }
    return {};
}
}

QString packagePlatform() {
    auto arch = QSysInfo::buildCpuArchitecture();
#if defined(Q_OS_MACOS)
    // A Universal executable must use the architecture of the active slice.
# if defined(__aarch64__) || defined(__arm64__)
    arch = "arm64";
# elif defined(__x86_64__)
    arch = "x86_64";
# endif
    const QString os = "macos";
#elif defined(Q_OS_WIN)
    const QString os = "windows";
#elif defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    const QString os = "linux";
#else
    const QString os;
    return {};
#endif
    if (arch == "aarch64") arch = "arm64";
    if (arch != "arm64" && arch != "x86_64") return {};
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    if (arch != "x86_64") return {}; // Hosted release builds currently produce x64 packages.
#endif
    return os + '-' + arch;
}

QString driverPackageDirectory(DriverPackage package) {
    const auto data = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (data.isEmpty()) return {};
    QString key = "components/" + packageKey(package);
    // These runtime ABIs are unchanged in 0.54.24. Keep otherwise identical
    // components installed by 0.54.23 usable without downloading again.
    if (package == DriverPackage::Java || package == DriverPackage::Redis)
        key += '/' + packagePlatform() + "/0.54.23";
    return QDir(data).filePath(key);
}

QString javaPackageExecutable() {
#ifdef Q_OS_WIN
    return QDir(driverPackageDirectory(DriverPackage::Java)).filePath("bin/java.exe");
#else
    return QDir(driverPackageDirectory(DriverPackage::Java)).filePath("bin/java");
#endif
}

QString redisPackageLibraryName() {
#ifdef Q_OS_WIN
    return "navidog-hiredis.dll";
#elif defined(Q_OS_MACOS)
    return "libnavidog-hiredis.dylib";
#else
    return "libnavidog-hiredis.so";
#endif
}

QString managedJdbcJar(DatabaseKind kind) {
    DriverPackage package;
    switch (kind) {
    case DatabaseKind::H2: package = DriverPackage::H2; break;
    case DatabaseKind::Dameng: package = DriverPackage::Dameng; break;
    case DatabaseKind::Db2: package = DriverPackage::Db2; break;
    case DatabaseKind::Kingbase: package = DriverPackage::Kingbase; break;
    case DatabaseKind::GaussDb: package = DriverPackage::GaussDb; break;
    default: return {};
    }
    const auto jar = QFileInfo(QDir(driverPackageDirectory(package)).filePath(driverDownload(package).asset));
    return jar.isFile() && jar.isReadable() ? jar.absoluteFilePath() : QString{};
}

QString jdbcJarPath(DatabaseKind kind, const QString& selected) {
    return selected.isEmpty() ? managedJdbcJar(kind) : selected;
}

DriverDownload driverDownload(DriverPackage package) {
    auto jar = [](const QString& path, const char* digest) -> DriverDownload {
        return {path.section('/', -1), QUrl(maven + path), QByteArray::fromHex(digest), 24 * mib, false};
    };
    switch (package) {
    case DriverPackage::H2:
        return jar("com/h2database/h2/2.3.232/h2-2.3.232.jar",
                   "8dae62d22db8982c3dcb3826edb9c727c5d302063a67eef7d63d82de401f07d3");
    case DriverPackage::Dameng:
        return jar("com/dameng/DmJdbcDriver18/8.1.3.140/DmJdbcDriver18-8.1.3.140.jar",
                   "9af4ff4d6ed15948507f528a18ab9b7196b3600d9169ad7998c19869031a3c6f");
    case DriverPackage::Db2:
        return jar("com/ibm/db2/jcc/12.1.3.0/jcc-12.1.3.0.jar",
                   "6380b30ba7ce3262c775720066dbd2a3abc14dd0212619068e589866894029c7");
    case DriverPackage::Kingbase:
        return jar("cn/com/kingbase/kingbase8/9.0.1/kingbase8-9.0.1.jar",
                   "662ad6f0646cc3fb9b552c9fff93e09bf9eb12e4a9f33f0dec18a624cb48104c");
    case DriverPackage::GaussDb:
        return jar("com/huaweicloud/gaussdb/gaussdbjdbc/506.0.0.b058/gaussdbjdbc-506.0.0.b058.jar",
                   "f7cbac0cb554a408bbe683167f9b25e61fc2b21552098ac12e925a205e342765");
    case DriverPackage::Java:
    case DriverPackage::Redis: {
        const auto platform = packagePlatform();
        if (platform.isEmpty()) return {};
        const auto prefix = package == DriverPackage::Java ? "JRE" : "Driver-Redis";
        return {QString("Navidog-%1-%2-%3.tar.gz").arg(NAVIDOG_VERSION, prefix, platform), {},
                {}, package == DriverPackage::Java ? 180 * mib : 8 * mib, true};
    }
    }
    return {};
}

DriverDownload githubDriverAsset(DriverDownload download, const QByteArray& releaseJson) {
    const auto release = QJsonDocument::fromJson(releaseJson).object();
    const auto tag = QStringLiteral("v" NAVIDOG_VERSION);
    if (release["tag_name"].toString() != tag || release["draft"].toBool()) return {};
    const auto expectedUrl = QUrl(releaseRepository + tag + '/' + download.asset);
    static const QRegularExpression hashPattern("^sha256:[0-9a-f]{64}$");
    for (const auto& value : release["assets"].toArray()) {
        const auto entry = value.toObject();
        if (entry["name"].toString() != download.asset) continue;
        const auto digest = entry["digest"].toString();
        const auto size = entry["size"].toDouble();
        if (!hashPattern.match(digest).hasMatch() || size <= 0 || size > download.maxBytes ||
            entry["state"].toString() != "uploaded" ||
            QUrl(entry["browser_download_url"].toString()) != expectedUrl) return {};
        download.url = expectedUrl;
        download.maxBytes = qint64(size);
        download.sha256 = QByteArray::fromHex(digest.mid(7).toLatin1());
        return download;
    }
    return {};
}

bool jdbcBridgeSupported() {
#ifdef NAVIDOG_HAS_JDBC
    return true;
#else
    return false;
#endif
}
bool managedRedisSupported() {
#ifdef NAVIDOG_MANAGED_REDIS
    return true;
#else
    return false;
#endif
}
}
