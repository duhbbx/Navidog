#pragma once
#include <QString>
#include <QJsonObject>
#include <tuple>
namespace navidog {
enum class SslVerification { Encryption, CertificateAuthority, Identity };
enum class SshAuthentication { Password, PublicKey, PasswordAndKey, Agent };
enum class OracleAddressKind { ServiceName, SID, TNS };
enum class OracleRole { Normal, SysDBA, SysOPER };
struct AdvancedConnectionOptions {
    int connectTimeout=5,readTimeout=0,writeTimeout=30,keepAliveSeconds=240,maxSessions=0;
    bool keepAlive=false,compression=false,autoConnect=false;
    QString charset,socketFile,initialSql,applicationName="Navidog";
    bool operator==(const AdvancedConnectionOptions& p)const{return std::tie(connectTimeout,readTimeout,writeTimeout,keepAliveSeconds,maxSessions,keepAlive,compression,autoConnect,charset,socketFile,initialSql,applicationName)==std::tie(p.connectTimeout,p.readTimeout,p.writeTimeout,p.keepAliveSeconds,p.maxSessions,p.keepAlive,p.compression,p.autoConnect,p.charset,p.socketFile,p.initialSql,p.applicationName);}
};
struct SslConnectionOptions {
    SslVerification verification=SslVerification::Encryption;
    QString caFile,certificateFile,keyFile,ciphers,tlsVersions,serverName,walletDirectory,serverDN;
    QString damengKeystoreDirectory,damengKeystorePassword;
    bool saveDamengKeystorePassword=false;
    bool operator==(const SslConnectionOptions& p)const{return std::tie(verification,caFile,certificateFile,keyFile,ciphers,tlsVersions,serverName,walletDirectory,serverDN,damengKeystoreDirectory,damengKeystorePassword,saveDamengKeystorePassword)==std::tie(p.verification,p.caFile,p.certificateFile,p.keyFile,p.ciphers,p.tlsVersions,p.serverName,p.walletDirectory,p.serverDN,p.damengKeystoreDirectory,p.damengKeystorePassword,p.saveDamengKeystorePassword);}
};
struct SshConnectionOptions {
    bool enabled=false,savePassword=false,savePassphrase=false;
    QString host,user,password,keyFile,passphrase,knownHostsFile,hostKey;
    int port=22;
    SshAuthentication authentication=SshAuthentication::Password;
    bool operator==(const SshConnectionOptions& p)const{return std::tie(enabled,savePassword,savePassphrase,host,user,password,keyFile,passphrase,knownHostsFile,hostKey,port,authentication)==std::tie(p.enabled,p.savePassword,p.savePassphrase,p.host,p.user,p.password,p.keyFile,p.passphrase,p.knownHostsFile,p.hostKey,p.port,p.authentication);}
};
struct HttpConnectionOptions {
    bool enabled=false,authentication=false,savePassword=false,base64=false,proxyEnabled=false;
    QString url,user,password,proxyHost,proxyUser,proxyPassword;
    int proxyPort=8080;
    bool operator==(const HttpConnectionOptions& p)const{return std::tie(enabled,authentication,savePassword,base64,proxyEnabled,url,user,password,proxyHost,proxyUser,proxyPassword,proxyPort)==std::tie(p.enabled,p.authentication,p.savePassword,p.base64,p.proxyEnabled,p.url,p.user,p.password,p.proxyHost,p.proxyUser,p.proxyPassword,p.proxyPort);}
};
struct OracleConnectionOptions {
    OracleAddressKind addressKind=OracleAddressKind::ServiceName;
    OracleRole role=OracleRole::Normal;
    QString clientDirectory,configDirectory;
    bool externalAuthentication=false;
    bool operator==(const OracleConnectionOptions& p)const{return std::tie(addressKind,role,clientDirectory,configDirectory,externalAuthentication)==std::tie(p.addressKind,p.role,p.clientDirectory,p.configDirectory,p.externalAuthentication);}
};
struct ConnectionProfile;
QJsonObject connectionOptionsJson(const ConnectionProfile&);
void readConnectionOptions(ConnectionProfile&,const QJsonObject&);
void validateConnectionOptions(const ConnectionProfile&);
QString connectionCharset(const ConnectionProfile&);
QString oracleConnectDescriptor(const ConnectionProfile&);
}
