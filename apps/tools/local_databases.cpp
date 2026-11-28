#include "navidog/credentials.hpp"
#include "navidog/database.hpp"
#include "navidog/catalog.hpp"
#include "navidog/profile_store.hpp"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTextStream>
#include <QSet>
#include <algorithm>
using namespace navidog;
namespace {
QString env(const QJsonObject& container,const QString& key) {
    for(auto value:container["Config"].toObject()["Env"].toArray()) {
        auto entry=value.toString(); if(entry.startsWith(key+"="))return entry.mid(key.size()+1);
    }
    return {};
}
QString port(const QJsonObject& container,const QString& internal) {
    const auto bindings=container["NetworkSettings"].toObject()["Ports"].toObject()[internal+"/tcp"].toArray();
    for(auto value:bindings) { auto b=value.toObject(); if(b["HostIp"].toString()=="127.0.0.1" || b["HostIp"].toString()=="0.0.0.0" || b["HostIp"].toString().isEmpty())return b["HostPort"].toString(); }
    return {};
}
QJsonObject verify(ConnectionProfile& p) {
    QJsonObject report{{"name",p.name},{"host",p.host},{"port",p.port},{"driver",databaseKindKey(p.driver)},{"container",p.credentialContainer}};
    try {
        auto db=connectDatabase(p); auto token=cancellation();
        p.serverVersion=db->serverVersion(); p.detectedVersion=detectVersion(p.driver,p.serverVersion);
        auto ping=db->execute(p.driver==DatabaseKind::Oracle ? "SELECT 1 FROM dual" : "SELECT 1",{},token);
        if(!ping.error.isEmpty())throw std::runtime_error(ping.error.toStdString());
        auto root=loadCatalog(*db,p,{},token); report["databasesOrSchemas"]=root.nodes.size();
        CatalogNode sample;
        if(p.driver==DatabaseKind::Oracle) {
            auto target=p.user.toUpper();
            auto it=std::find_if(root.nodes.begin(),root.nodes.end(),[&](auto& n){return n.schema==target;});
            if(it!=root.nodes.end())sample=*it;
        } else if(p.driver==DatabaseKind::PostgreSQL) {
            CatalogNode database;database.kind="database";database.database=p.database;
            auto schemas=loadCatalog(*db,p,database,token);
            auto it=std::find_if(schemas.nodes.begin(),schemas.nodes.end(),[](auto& n){return n.schema=="public";});
            if(it!=schemas.nodes.end())sample=*it;
        } else {
            auto it=std::find_if(root.nodes.begin(),root.nodes.end(),[](auto& n){return !QSet<QString>{"information_schema","mysql","performance_schema","sys","oceanbase"}.contains(n.database);});
            if(it!=root.nodes.end())sample=*it;
        }
        if(!sample.name.isEmpty()) {
            auto folders=loadCatalog(*db,p,sample,token);
            auto tableFolder=std::find_if(folders.nodes.begin(),folders.nodes.end(),[](auto& n){return n.kind=="tables";});
            if(tableFolder!=folders.nodes.end()) {
                auto tables=loadCatalog(*db,p,*tableFolder,token); report["tables"]=tables.nodes.size(); report["inspectedDatabase"]=sample.database; report["inspectedSchema"]=sample.schema;
                if(!tables.nodes.isEmpty()) {
                    auto n=tables.nodes[0]; auto folders=loadCatalog(*db,p,n,token);
                    for(auto folder:folders.nodes) { auto contents=loadCatalog(*db,p,folder,token);report[folder.kind]=contents.nodes.size(); }
                    auto rows=db->execute(previewSql(p,n),{},token);
                    if(!rows.error.isEmpty())throw std::runtime_error(rows.error.toStdString());
                    report["previewRows"]=rows.rows.size();report["sampleTable"]=n.name;
                }
            }
        }
        if(p.driver==DatabaseKind::Oracle && report["tables"].toInt()==0) {
            CatalogNode dual;dual.kind="table";dual.schema="SYS";dual.object="DUAL";dual.name="DUAL";
            auto children=loadCatalog(*db,p,dual,token);
            for(auto folder:children.nodes) report[folder.kind]=loadCatalog(*db,p,folder,token).nodes.size();
            auto rows=db->execute(previewSql(p,dual),{},token);if(!rows.error.isEmpty())throw std::runtime_error(rows.error.toStdString());
            report["previewRows"]=rows.rows.size();report["sampleTable"]="SYS.DUAL";
        }
        report["ok"]=true;report["version"]=p.serverVersion;report["versionEnum"]=versionKey(p.detectedVersion);
    }catch(const std::exception& e) {report["ok"]=false;report["error"]=QString::fromUtf8(e.what());}
    return report;
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv); QCommandLineParser parser;parser.addHelpOption();
    parser.addOption({"import","Merge verified local Docker connections into this profile file","path"});
    parser.addOption({"report","Save non-secret verification results","path"});parser.process(app);
    QVector<ConnectionProfile> profiles;QJsonArray reports;
    try {
        for(auto value:localDockerContainers()) {
            auto container=value.toObject();auto image=container["Config"].toObject()["Image"].toString().toLower();
            ConnectionProfile p;p.host="127.0.0.1";p.tls=false;p.groupId="local-databases";auto name=container["Name"].toString().mid(1);
            p.id="local-docker-"+name;p.credentialContainer=name;p.credentialSource=CredentialSource::LocalDocker;
            QString internal;
            if(image.startsWith("mysql:")) {p.driver=DatabaseKind::MySQL;internal="3306";p.database="information_schema";p.user="root";p.credentialVariable="MYSQL_ROOT_PASSWORD";p.tls=true;}
            else if(image.startsWith("postgres:")) {p.driver=DatabaseKind::PostgreSQL;internal="5432";p.user=env(container,"POSTGRES_USER");if(p.user.isEmpty())p.user="postgres";p.database=env(container,"POSTGRES_DB");if(p.database.isEmpty())p.database=p.user;p.credentialVariable="POSTGRES_PASSWORD";}
            else if(image.contains("oracle-free") || image.contains("oracle/database")) {p.driver=DatabaseKind::Oracle;internal="1521";p.database="FREEPDB1";p.user=env(container,"APP_USER");p.credentialVariable="APP_USER_PASSWORD";if(p.user.isEmpty() || env(container,p.credentialVariable).isEmpty()){p.user="system";p.credentialVariable="ORACLE_PASSWORD";}}
            else if(image.contains("oceanbase-ce")) {p.driver=DatabaseKind::OceanBase;internal="2881";p.database="oceanbase";p.user="root@sys";p.credentialSource=CredentialSource::Passwordless;}
            else continue;
            auto mapped=port(container,internal);if(mapped.isEmpty())continue;p.port=mapped.toInt();
            p.name=QString("本机 %1 · %2 (%3)").arg(databaseKindName(p.driver),name,mapped);
            auto report=verify(p);reports.append(report);if(report["ok"].toBool())profiles<<p;
        }
        if(parser.isSet("import")) {
            ProfileStore store(parser.value("import"));auto workspace=store.loadWorkspace();
            if(std::none_of(workspace.groups.begin(),workspace.groups.end(),[](auto& g){return g.id=="local-databases";}))workspace.groups.push_back({"local-databases","本机数据库"});
            for(auto p:profiles) {
                auto it=std::find_if(workspace.connections.begin(),workspace.connections.end(),[&](auto& old){return old.id==p.id || (old.driver==p.driver && old.port==p.port && (old.host=="localhost" || old.host=="127.0.0.1"));});
                if(it==workspace.connections.end())workspace.connections<<p;
                else {p.id=it->id;*it=p;}
            }
            store.saveWorkspace(workspace);
        }
        auto bytes=QJsonDocument(reports).toJson();
        if(parser.isSet("report")) {QSaveFile file(parser.value("report"));if(!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit())return 2;}
        QTextStream(stdout)<<bytes;
        return std::all_of(reports.begin(),reports.end(),[](auto r){return r.toObject()["ok"].toBool();}) ? 0 : 1;
    }catch(const std::exception& e){QTextStream(stderr)<<e.what()<<'\n';return 2;}
}
