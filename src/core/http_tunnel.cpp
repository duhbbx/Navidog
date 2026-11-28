#include "navidog/http_tunnel.hpp"
#include "navidog/sql.hpp"
#include "../drivers/collector.hpp"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QAuthenticator>
#include <QEventLoop>
#include <QTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QUrl>
static void initTunnelResource(){Q_INIT_RESOURCE(tunnel);}
namespace navidog {
namespace {
using native::fail;
class HttpDatabase final:public Database {
    ConnectionProfile profile_;
    QNetworkAccessManager manager_;
    QString version_;
    QJsonObject request(QJsonObject body,const Cancellation& cancel){
        const auto& p=profile_;body["protocol"]="navidog-1";body["connection"]=QJsonObject{{"kind",databaseKindKey(p.driver)},{"host",p.host},{"port",p.port},{"database",p.database},{"user",p.user},{"password",p.password},{"readOnly",p.readOnly}};
        auto bytes=QJsonDocument(body).toJson(QJsonDocument::Compact);if(bytes.size()>4*1024*1024)fail("HTTP 隧道请求超过 4 MB");
        QNetworkRequest request(QUrl(p.http.url));request.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");request.setRawHeader("Accept","application/json");
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
        if(p.http.authentication)request.setRawHeader("Authorization","Basic "+(p.http.user+":"+p.http.password).toUtf8().toBase64());
        if(cancel->load()){fail("操作已停止");}
        auto* reply=manager_.post(request,bytes);bytes.fill('\0');QEventLoop loop;QTimer poll,timeout;poll.setInterval(20);timeout.setSingleShot(true);QByteArray response;bool oversized=false,timedOut=false;
        QObject::connect(reply,&QNetworkReply::readyRead,&loop,[&]{response+=reply->readAll();if(response.size()>48*1024*1024){oversized=true;reply->abort();}});
        QObject::connect(reply,&QNetworkReply::finished,&loop,&QEventLoop::quit);QObject::connect(&poll,&QTimer::timeout,&loop,[&]{if(cancel->load())reply->abort();});
        QObject::connect(&timeout,&QTimer::timeout,&loop,[&]{timedOut=true;reply->abort();});poll.start();timeout.start((p.advanced.readTimeout?p.advanced.readTimeout:300)*1000);if(!reply->isFinished())loop.exec();response+=reply->readAll();
        const auto error=reply->error();const auto errorText=reply->errorString();const int status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();delete reply;
        if(cancel->load())fail("操作已停止");if(timedOut)fail("HTTP 隧道请求超时；写操作是否提交需重新查询确认");if(oversized || response.size()>48*1024*1024)fail("HTTP 隧道响应超出缓存上限");
        QJsonParseError parse;auto document=QJsonDocument::fromJson(response,&parse);if(parse.error!=QJsonParseError::NoError || !document.isObject())fail("隧道未返回有效的 Navidog JSON 响应（HTTP "+QString::number(status)+"）");auto object=document.object();
        if(!object["error"].toString().isEmpty())fail(object["error"].toString());if(error!=QNetworkReply::NoError)fail("HTTP 隧道失败："+errorText);if(object["protocol"].toString()!="navidog-1")fail("服务器脚本与 Navidog 隧道协议不兼容");return object;
    }
public:
    explicit HttpDatabase(const ConnectionProfile& p):profile_(p){
        if(p.http.proxyEnabled){QNetworkProxy proxy(QNetworkProxy::HttpProxy,p.http.proxyHost,p.http.proxyPort,p.http.proxyUser,p.http.proxyPassword);manager_.setProxy(proxy);}else manager_.setProxy(QNetworkProxy::NoProxy);
        version_=request({{"operation","version"}},cancellation())["version"].toString();
    }
    TransactionState transactionState()const override{return TransactionState::Unavailable;}
    QueryResult transaction(TransactionAction,const Cancellation&)override{fail("HTTP 隧道不支持交互式事务，请使用直连或 SSH");}
    QString serverVersion()override{return version_;}
    QueryResult execute(const QString& sql,QueryLimits limits,const Cancellation& cancel)override{return executeBound(sql,{},limits,cancel);}
    QueryResult executeBound(const QString& query,const QVector<QVariant>& parameters,QueryLimits limits,const Cancellation& cancel)override{
        const auto dialect=profile_.driver==DatabaseKind::PostgreSQL?sql::Dialect::PostgreSQL:profile_.driver==DatabaseKind::SQLite?sql::Dialect::SQLite:sql::Dialect::MySQL;native::validate(query,dialect,definitionMode_);native::Collector collector(limits);
        auto input=query.toUtf8();auto lexed=sql::lex(input.toStdString(),dialect);QString first;for(const auto& token:lexed.tokens)if(token.kind!=sql::TokenKind::Comment){first=QString::fromUtf8(input.mid(token.begin,token.end-token.begin)).toUpper();break;}
        if(QStringList{"BEGIN","START","COMMIT","ROLLBACK","SAVEPOINT","RELEASE","SET","USE"}.contains(first))fail("HTTP 隧道每次请求使用独立数据库会话，不支持会话设置或交互式事务；请使用直连或 SSH");
        // Convert numbered native markers to PDO positional markers outside quoted tokens.
        QByteArray rewritten;QVector<QVariant> values;qsizetype end=0;for(std::size_t i=0;i<lexed.tokens.size();++i){auto t=lexed.tokens[i];if(t.kind!=sql::TokenKind::Symbol || (input[int(t.begin)]!='$' && input[int(t.begin)]!='?'))continue;auto marker=char(input[int(t.begin)]);if(marker=='$' && dialect!=sql::Dialect::PostgreSQL)continue;qsizetype finish=t.end;while(finish<input.size() && input[int(finish)]>='0' && input[int(finish)]<='9')++finish;bool ok=false;int index=input.mid(t.end,finish-t.end).toInt(&ok);if(!ok){if(marker=='?')index=values.size()+1;else continue;}if(index<1 || index>parameters.size())fail("HTTP 绑定参数数量不匹配");rewritten+=input.mid(end,t.begin-end);rewritten+='?';end=finish;values<<parameters[index-1];while(i+1<lexed.tokens.size() && lexed.tokens[i+1].begin<std::size_t(finish))++i;}
        rewritten+=input.mid(end);if(values.isEmpty() && !parameters.isEmpty())fail("HTTP 绑定参数数量不匹配");QJsonArray args;for(auto value:values){QString type="text";QJsonValue encoded;if(!value.isValid() || value.isNull())type="null";else if(value.userType()==QMetaType::QByteArray){type="binary";encoded=QString::fromLatin1(value.toByteArray().toBase64());}else if(value.userType()==QMetaType::Bool){type="bool";encoded=value.toBool();}else if(value.userType()==QMetaType::Int || value.userType()==QMetaType::LongLong){type="integer";encoded=value.toString();}else encoded=value.toString();args<<QJsonObject{{"type",type},{"value",encoded}};}
        auto object=request({{"operation","execute"},{"sql",profile_.http.base64?QString::fromLatin1(rewritten.toBase64()):QString::fromUtf8(rewritten)},{"base64",profile_.http.base64},{"parameters",args},{"maxRows",limits.maxRows},{"maxBytes",double(limits.maxBytes)},{"maxCellBytes",limits.maxCellBytes}},cancel);
        auto& result=collector.result;for(auto column:object["columns"].toArray())result.columns<<column.toString();for(auto row:object["rows"].toArray()){auto array=row.toArray();if(array.size()!=result.columns.size())fail("HTTP 结果列数不一致");QVector<QVariant> values;for(auto cell:array){auto value=cell.toObject();auto type=value["type"].toString();auto text=value["value"].toString();if(type=="null")values<<QVariant{};else if(type=="integer")values<<QVariant::fromValue<qlonglong>(text.toLongLong());else if(type=="float")values<<text.toDouble();else {auto bytes=QByteArray::fromBase64(text.toLatin1());if(bytes.size()>limits.maxCellBytes){bytes.truncate(limits.maxCellBytes);result.cellTruncated=true;}if(type=="binary")values<<bytes;else values<<QString::fromUtf8(bytes);}}if(!collector.add(std::move(values)))break;}
        result.affectedRows=object["affected"].toString().toLongLong();result.truncated|=object["truncated"].toBool();result.cellTruncated|=object["cellTruncated"].toBool();return result;
    }
    QStringList tables()override{auto query=profile_.driver==DatabaseKind::SQLite?QString("SELECT name FROM sqlite_schema WHERE type='table' ORDER BY name LIMIT 1000"):profile_.driver==DatabaseKind::PostgreSQL?QString("SELECT tablename FROM pg_tables WHERE schemaname=current_schema() ORDER BY tablename LIMIT 1000"):QString("SELECT table_name FROM information_schema.tables WHERE table_schema=DATABASE() ORDER BY table_name LIMIT 1000");auto r=execute(query,{},cancellation());QStringList out;for(auto row:r.rows)out<<row[0].toString();return out;}
};
}
std::unique_ptr<Database> connectHttpTunnel(const ConnectionProfile& p){return std::make_unique<HttpDatabase>(p);}
QByteArray httpTunnelScript(){initTunnelResource();QFile file(":/navidog/navidog-tunnel.php");if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("无法读取 HTTP 隧道脚本");return file.readAll();}
}
