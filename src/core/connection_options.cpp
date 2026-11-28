#include "navidog/driver_packages.hpp"
#include "navidog/connection_options.hpp"
#include "navidog/types.hpp"
#include <QFileInfo>
#include <QDir>
#include <QUrl>
#include <QRegularExpression>
#include <stdexcept>
namespace navidog {
namespace {[[noreturn]] void fail(const QString& s){throw std::runtime_error(s.toStdString());}}
QString connectionCharset(const ConnectionProfile& p){if(!p.advanced.charset.isEmpty())return p.advanced.charset;return navidog::isMySqlFamily(p.driver)?"utf8mb4":"UTF8";}
QJsonObject connectionOptionsJson(const ConnectionProfile& p){const auto& a=p.advanced;const auto& s=p.ssl;const auto& t=p.ssh;const auto& h=p.http;const auto& o=p.oracle;
    return {{"notes",p.notes},{"jdbcDriverJar",p.jdbcDriverJar},{"h2File",p.h2File},{"savePassword",p.savePassword},{"advanced",QJsonObject{{"connectTimeout",a.connectTimeout},{"readTimeout",a.readTimeout},{"writeTimeout",a.writeTimeout},{"keepAlive",a.keepAlive},{"keepAliveSeconds",a.keepAliveSeconds},{"maxSessions",a.maxSessions},{"compression",a.compression},{"autoConnect",a.autoConnect},{"charset",a.charset},{"socketFile",a.socketFile},{"initialSql",a.initialSql},{"applicationName",a.applicationName}}},
    {"ssl",QJsonObject{{"verification",int(s.verification)},{"caFile",s.caFile},{"certificateFile",s.certificateFile},{"keyFile",s.keyFile},{"ciphers",s.ciphers},{"tlsVersions",s.tlsVersions},{"serverName",s.serverName},{"walletDirectory",s.walletDirectory},{"serverDN",s.serverDN},{"damengKeystoreDirectory",s.damengKeystoreDirectory},{"saveDamengKeystorePassword",s.saveDamengKeystorePassword}}},
    {"ssh",QJsonObject{{"enabled",t.enabled},{"host",t.host},{"port",t.port},{"user",t.user},{"authentication",int(t.authentication)},{"keyFile",t.keyFile},{"knownHostsFile",t.knownHostsFile},{"hostKey",t.hostKey},{"savePassword",t.savePassword},{"savePassphrase",t.savePassphrase}}},
    {"http",QJsonObject{{"enabled",h.enabled},{"url",h.url},{"authentication",h.authentication},{"user",h.user},{"savePassword",h.savePassword},{"base64",h.base64},{"proxyEnabled",h.proxyEnabled},{"proxyHost",h.proxyHost},{"proxyPort",h.proxyPort},{"proxyUser",h.proxyUser}}},
    {"oracle",QJsonObject{{"addressKind",int(o.addressKind)},{"role",int(o.role)},{"clientDirectory",o.clientDirectory},{"configDirectory",o.configDirectory},{"externalAuthentication",o.externalAuthentication}}}};
}
void readConnectionOptions(ConnectionProfile& p,const QJsonObject& value){auto a=value["advanced"].toObject(),s=value["ssl"].toObject(),t=value["ssh"].toObject(),h=value["http"].toObject(),o=value["oracle"].toObject();p.notes=value["notes"].toString();p.jdbcDriverJar=value["jdbcDriverJar"].toString();p.h2File=value["h2File"].toBool();p.savePassword=value["savePassword"].toBool();
    p.advanced.connectTimeout=qBound(1,a["connectTimeout"].toInt(5),600);p.advanced.readTimeout=qBound(0,a["readTimeout"].toInt(0),86400);p.advanced.writeTimeout=qBound(0,a["writeTimeout"].toInt(30),86400);p.advanced.keepAlive=a["keepAlive"].toBool();p.advanced.keepAliveSeconds=qBound(1,a["keepAliveSeconds"].toInt(240),86400);p.advanced.maxSessions=qBound(0,a["maxSessions"].toInt(),16);p.advanced.compression=a["compression"].toBool();p.advanced.autoConnect=a["autoConnect"].toBool();p.advanced.charset=a["charset"].toString();p.advanced.socketFile=a["socketFile"].toString();p.advanced.initialSql=a["initialSql"].toString();p.advanced.applicationName=a["applicationName"].toString("Navidog");
    const int fallback=p.driver==DatabaseKind::SQLServer || p.driver==DatabaseKind::Oracle?int(SslVerification::Identity):0;p.ssl.verification=SslVerification(qBound(0,s["verification"].toInt(fallback),2));p.ssl.caFile=s["caFile"].toString();p.ssl.certificateFile=s["certificateFile"].toString();p.ssl.keyFile=s["keyFile"].toString();p.ssl.ciphers=s["ciphers"].toString();p.ssl.tlsVersions=s["tlsVersions"].toString();p.ssl.serverName=s["serverName"].toString();p.ssl.walletDirectory=s["walletDirectory"].toString();p.ssl.serverDN=s["serverDN"].toString();p.ssl.damengKeystoreDirectory=s["damengKeystoreDirectory"].toString();p.ssl.saveDamengKeystorePassword=s["saveDamengKeystorePassword"].toBool();
    p.ssh.enabled=t["enabled"].toBool();p.ssh.host=t["host"].toString();p.ssh.port=qBound(1,t["port"].toInt(22),65535);p.ssh.user=t["user"].toString();p.ssh.authentication=SshAuthentication(qBound(0,t["authentication"].toInt(),3));p.ssh.keyFile=t["keyFile"].toString();p.ssh.knownHostsFile=t["knownHostsFile"].toString();p.ssh.hostKey=t["hostKey"].toString();p.ssh.savePassword=t["savePassword"].toBool();p.ssh.savePassphrase=t["savePassphrase"].toBool();
    p.http.enabled=h["enabled"].toBool();p.http.url=h["url"].toString();p.http.authentication=h["authentication"].toBool();p.http.user=h["user"].toString();p.http.savePassword=h["savePassword"].toBool();p.http.base64=h["base64"].toBool();p.http.proxyEnabled=h["proxyEnabled"].toBool();p.http.proxyHost=h["proxyHost"].toString();p.http.proxyPort=qBound(1,h["proxyPort"].toInt(8080),65535);p.http.proxyUser=h["proxyUser"].toString();
    p.oracle.addressKind=OracleAddressKind(qBound(0,o["addressKind"].toInt(),2));p.oracle.role=OracleRole(qBound(0,o["role"].toInt(),2));p.oracle.clientDirectory=o["clientDirectory"].toString();p.oracle.configDirectory=o["configDirectory"].toString();p.oracle.externalAuthentication=o["externalAuthentication"].toBool();
}
void validateConnectionOptions(const ConnectionProfile& p){
    if(p.ssh.enabled && p.http.enabled)fail("SSH 和 HTTP 隧道不能同时启用");if(p.driver==DatabaseKind::SQLite && p.ssh.enabled)fail("SQLite 文件不使用 SSH 隧道");
    if(p.driver==DatabaseKind::OceanBaseOracle) {
        static const QRegularExpression login(R"(^[^@#\s\x00]+@[^@#\s\x00]+(?:#[^@#\s\x00]+)?$)");
        if(!login.match(p.user).hasMatch())fail("请填写用户@租户，使用代理集群时可填写用户@租户#集群");
        if(p.host.isEmpty() || p.port<1 || p.port>65535)fail("OceanBase 主机或端口无效");
        if(p.readOnly)fail("此模式请使用服务端只读账号");
        if(!p.advanced.socketFile.isEmpty())fail("OceanBase Oracle 模式使用 TCP 主机与端口");
        if(p.tls && p.ssl.verification!=SslVerification::Encryption && p.ssl.caFile.isEmpty())
            fail("校验证书时需要提供 CA 证书文件");
        if(p.tls && (!p.ssl.serverName.isEmpty() || (p.ssh.enabled && p.ssl.verification==SslVerification::Identity)))
            fail("OceanBase Connector/C 当前仅支持对直连主机验证证书身份");
    }
    if(p.driver==DatabaseKind::Redis) {
        bool numeric=false;
        const auto index=p.database.toInt(&numeric);
        if(p.host.isEmpty() || p.port<1 || p.port>65535)fail("Redis 主机或端口无效");
        if(!p.database.isEmpty() && (!numeric || index<0))fail("Redis 数据库编号须为非负整数");
        if(p.tls)fail("当前 Redis 驱动使用直连 TCP 或 SSH 隧道");
        if(p.readOnly)fail("请使用 Redis ACL 授予只读权限");
        if(!p.advanced.socketFile.isEmpty())fail("当前 Redis 驱动使用 TCP 主机与端口");
    }
    if(usesJdbcDriver(p.driver)) {
        const QFileInfo jar(jdbcJarPath(p.driver,p.jdbcDriverJar));
        if(!jar.isFile() || !jar.isReadable() || jar.suffix().compare("jar",Qt::CaseInsensitive)!=0)
            fail("请选择数据库官方 JDBC JAR 文件");
    }
    if(usesJdbcCatalog(p.driver)) {
        if(p.tls)fail("此 JDBC 类型的图形连接配置暂未提供原生 SSL；可选择 SSH 隧道");
        if(p.driver==DatabaseKind::H2 && p.h2File) {
            if(p.ssh.enabled)fail("H2 本地数据库文件不使用 SSH 隧道");
            const QFileInfo file(p.database);
            if(!file.isFile() || !file.fileName().endsWith(".mv.db",Qt::CaseInsensitive))
                fail("请选择已存在的 H2 .mv.db 文件");
        } else {
            if(p.host.isEmpty() || p.port<1 || p.port>65535)fail("主机或端口无效");
            if(p.database.isEmpty())fail("请填写数据库名称");
        }
        if(p.readOnly)fail("请使用服务器授权的只读账号");
    }
    if(p.driver==DatabaseKind::Dameng) {
        if(p.host.isEmpty() || p.port<1 || p.port>65535)fail("达梦主机或端口无效");
        if(p.user.isEmpty())fail("请填写达梦用户名");
        if(p.tls) {
            const QFileInfo directory(p.ssl.damengKeystoreDirectory);
            const QFileInfo keystore(QDir(directory.absoluteFilePath()).filePath(".keystore"));
            if(!directory.isDir() || !keystore.isFile() || !keystore.isReadable())
                fail("请选择包含 .keystore 的达梦 SSL 证书目录");
        }
        if(p.readOnly)fail("达梦只读连接请使用数据库只读账号");
    }
    if(p.advanced.connectTimeout<1 || p.advanced.connectTimeout>600)fail("连接超时须在 1 至 600 秒之间");
    if(p.ssh.enabled){if(!p.advanced.socketFile.isEmpty())fail("SSH 隧道使用数据库主机和端口，请取消本地 Socket 文件");if(p.ssh.host.isEmpty() || p.ssh.user.isEmpty())fail("请填写 SSH 主机和用户名");if(p.ssh.port<1 || p.ssh.port>65535)fail("SSH 端口无效");if((p.ssh.authentication==SshAuthentication::PublicKey || p.ssh.authentication==SshAuthentication::PasswordAndKey) && !QFileInfo(p.ssh.keyFile).isFile())fail("请选择可读取的 SSH 私钥文件");}
    if(p.http.enabled){if(!navidog::isMySqlFamily(p.driver) && !usesNativePostgres(p.driver) && p.driver!=DatabaseKind::SQLite)fail("该引擎不支持 HTTP 脚本隧道");QUrl url(p.http.url);if(!url.isValid() || url.host().isEmpty() || (url.scheme()!="http" && url.scheme()!="https"))fail("HTTP 隧道需要完整的 http / https 地址");if(!url.userInfo().isEmpty())fail("请在验证区域填写 HTTP 凭据，不要把密码写入网址");}
    if(p.tls && p.driver!=DatabaseKind::Dameng && !p.http.enabled)for(const auto& path:{p.ssl.caFile,p.ssl.certificateFile,p.ssl.keyFile})if(!path.isEmpty() && !QFileInfo(path).isFile())fail("证书或密钥文件不存在："+path);
    if(p.tls && p.driver!=DatabaseKind::Dameng && !p.http.enabled && (!p.ssl.certificateFile.isEmpty() != !p.ssl.keyFile.isEmpty()))fail("客户端证书与私钥需要同时提供");
}
QString oracleConnectDescriptor(const ConnectionProfile& p){
    static const QRegularExpression identifier("^[A-Za-z0-9_.:-]+$");
    if(p.oracle.addressKind==OracleAddressKind::TNS){if(p.ssh.enabled)fail("TNS 别名请在 TNS 配置中设置隧道地址；应用 SSH 隧道请使用基本连接");if(!identifier.match(p.database).hasMatch())fail("TNS 网络服务名包含无效字符");return p.database;}
    auto host=p.transportHost.isEmpty()?p.host:p.transportHost;host.remove('[');host.remove(']');
    if(!identifier.match(host).hasMatch() || !identifier.match(p.database).hasMatch())fail("Oracle 主机或服务名包含无效字符");
    auto text=QString("(DESCRIPTION=(CONNECT_TIMEOUT=%1)(TRANSPORT_CONNECT_TIMEOUT=%1)(ADDRESS=(PROTOCOL=%2)(HOST=%3)(PORT=%4))(CONNECT_DATA=(%5=%6))")
        .arg(p.advanced.connectTimeout).arg(p.tls?"TCPS":"TCP",host).arg(p.transportPort?p.transportPort:p.port).arg(p.oracle.addressKind==OracleAddressKind::SID?"SID":"SERVICE_NAME",p.database);
    if(p.tls){auto quote=[](QString value){if(value.contains('"') || value.contains('\n') || value.contains('\r') || value.contains('(') || value.contains(')'))fail("Oracle SSL 参数包含不支持的字符");return "\""+value+"\"";};
        if(p.ssh.enabled && p.ssl.verification==SslVerification::Identity && p.ssl.serverDN.isEmpty())fail("通过 SSH 验证 Oracle 服务器身份时，请填写服务器证书 DN");
        text+="(SECURITY=(SSL_SERVER_DN_MATCH="+QString(p.ssl.verification==SslVerification::Identity?"YES":"NO")+")";
        if(!p.ssl.walletDirectory.isEmpty())text+="(MY_WALLET_DIRECTORY="+quote(p.ssl.walletDirectory)+")";
        if(!p.ssl.serverDN.isEmpty())text+="(SSL_SERVER_CERT_DN="+quote(p.ssl.serverDN)+")";text+=")";
    }return text+")";
}

}
