#include "navidog/credentials.hpp"
#include "navidog/secret_store.hpp"
#include <QProcess>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QRegularExpression>
#include <stdexcept>
namespace navidog {
namespace {
[[noreturn]] void fail(const QString& message) { throw std::runtime_error(message.toUtf8().constData()); }
QByteArray docker(const QStringList& arguments) {
    auto executable=QStandardPaths::findExecutable("docker");
    if(executable.isEmpty()) for(const auto& path:{"/usr/local/bin/docker","/opt/homebrew/bin/docker"}) if(QFileInfo::exists(path)){executable=path;break;}
    if(executable.isEmpty()) fail("无法找到本机 Docker，请启动容器运行环境或改用手动密码。");
    QProcess process; process.start(executable,arguments);
    if(!process.waitForFinished(5000)) { process.kill(); process.waitForFinished(1000); fail("本机 Docker 请求超时，请确认容器运行环境已启动。"); }
    if(process.exitCode()!=0 || process.exitStatus()!=QProcess::NormalExit) fail("无法读取本机 Docker 容器，请确认容器存在并且正在运行。");
    return process.readAllStandardOutput();
}
void requireLocalDocker() {
    auto contexts=QJsonDocument::fromJson(docker({"context","inspect"})).array();
    if(contexts.isEmpty() || !contexts[0].toObject()["Endpoints"].toObject()["docker"].toObject()["Host"].toString().startsWith("unix://")) fail("仅支持本机 Unix socket Docker context，不能从远程 Docker 读取密码。");
    // Environment overrides could otherwise route inspect to an unrelated daemon.
    if(!qEnvironmentVariableIsEmpty("DOCKER_HOST") && !qEnvironmentVariable("DOCKER_HOST").startsWith("unix://")) fail("当前 DOCKER_HOST 指向远程端点，不读取凭据。");
}
}
QJsonArray localDockerContainers() {
    requireLocalDocker(); auto ids=QString::fromUtf8(docker({"ps","-q"})).split('\n',Qt::SkipEmptyParts);
    if(ids.isEmpty()) return {};
    QStringList args{"inspect"}; args.append(ids); return QJsonDocument::fromJson(docker(args)).array();
}
bool useSessionCredentialsOnly(ConnectionProfile& p) {
    if(hasRememberedSecrets(p))return false;
    const bool changed=p.savePassword || p.ssh.savePassword || p.ssh.savePassphrase || p.http.savePassword || p.ssl.saveDamengKeystorePassword || p.credentialSource==CredentialSource::SystemStore;
    p.savePassword=p.ssh.savePassword=p.ssh.savePassphrase=p.http.savePassword=p.ssl.saveDamengKeystorePassword=false;
    if(p.credentialSource==CredentialSource::SystemStore)p.credentialSource=CredentialSource::Prompt;
    return changed;
}
ConnectionProfile resolveCredentials(const ConnectionProfile& original) {
    auto profile=original;
    const bool legacy=profile.credentialSource==CredentialSource::SystemStore;
    useSessionCredentialsOnly(profile);
    if(legacy && profile.password.isEmpty())fail("系统钥匙串已停用，请重新输入数据库密码");
    if(hasRememberedSecrets(profile) && !profile.secretsLoaded &&
       ((profile.savePassword && profile.password.isEmpty()) || (profile.ssh.savePassword && profile.ssh.password.isEmpty()) || (profile.ssh.savePassphrase && profile.ssh.passphrase.isEmpty()) || (profile.http.savePassword && profile.http.password.isEmpty()) || (profile.ssl.saveDamengKeystorePassword && profile.ssl.damengKeystorePassword.isEmpty())))loadConnectionSecrets(profile);
    if(profile.credentialSource==CredentialSource::Passwordless) profile.password.clear();
    if(profile.credentialSource!=CredentialSource::LocalDocker) return profile;
    if(profile.host!="127.0.0.1" && profile.host!="::1" && profile.host!="localhost") fail("Docker 密码来源仅可用于本机数据库连接。");
    if(!QRegularExpression("^[A-Za-z0-9][A-Za-z0-9_.-]*$").match(profile.credentialContainer).hasMatch() || profile.credentialVariable.isEmpty()) fail("本机容器密码来源配置不完整。");
    requireLocalDocker(); auto list=QJsonDocument::fromJson(docker({"inspect",profile.credentialContainer})).array();
    if(list.isEmpty() || !list[0].toObject()["State"].toObject()["Running"].toBool()) fail("对应数据库容器未运行，请先启动容器。");
    for(auto value:list[0].toObject()["Config"].toObject()["Env"].toArray()) {
        auto entry=value.toString(); const auto prefix=profile.credentialVariable+"=";
        if(entry.startsWith(prefix)) { profile.password=entry.mid(prefix.size()); return profile; }
    }
    fail("容器中找不到已配置的密码环境变量，请改用手动密码。");
}
}
