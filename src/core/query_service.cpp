#include "navidog/database_sql.hpp"
#include "navidog/operation_timing.hpp"
#include "navidog/query_service.hpp"
#include "navidog/database.hpp"
#include "navidog/ssh_tunnel.hpp"
#include "navidog/sql.hpp"
#include "navidog/query_plan.hpp"
#include "navidog/query_context.hpp"
#include "navidog/query_analysis.hpp"
#include "navidog/redis_commands.hpp"
#include <QElapsedTimer>
#include <QHash>
#include <map>
#include <functional>

namespace navidog {
class QueryWorker : public QObject {
public:
    struct Session {
        ConnectionProfile profile;
        std::unique_ptr<Database> db;
        QString schema;
        bool catalogChangesPending=false;
    };
    std::map<QString, Session> sessions;
    Session& session(const ConnectionProfile& profile,const QString& sessionId={}) {
        // One session per saved profile/database context, all owned by this worker.
        const auto key = profile.id + QChar(0x1f) + profile.database + (sessionId.isEmpty()?QString{}:QChar(0x1f)+sessionId);
        auto it = sessions.find(key);
        if (it == sessions.end()) {
            if(profile.advanced.maxSessions>0){int count=0;for(const auto& entry:sessions)if(entry.second.db && entry.second.profile.id==profile.id)++count;if(count>=profile.advanced.maxSessions)throw std::runtime_error("已达到此连接设置的会话数量上限");}
            if (sessions.size() >= 16) throw std::runtime_error("已达到 16 个会话上限，请断开不再使用的连接");
            it = sessions.emplace(key, Session{}).first;
        }
        auto& session = it->second;
        auto comparable=profile; comparable.notes=session.profile.notes; comparable.name=session.profile.name; comparable.groupId=session.profile.groupId; comparable.version=session.profile.version; comparable.detectedVersion=session.profile.detectedVersion; comparable.serverVersion=session.profile.serverVersion;
        if (!session.db || session.profile != comparable) {
            session.db.reset(); session.catalogChangesPending=false;
            session.db = connectDatabase(profile); session.profile = profile;session.schema.clear();
        }
        return session;
    }
    QStringList connectedProfiles() const {
        QStringList ids;
        for (const auto& entry : sessions) if (entry.second.db && !ids.contains(entry.second.profile.id)) ids << entry.second.profile.id;
        return ids;
    }
    void disconnectProfile(const QString& id) {
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (it->second.profile.id == id || it->first.startsWith(id + QChar(0x1f))) it = sessions.erase(it); else ++it;
        }
    }
    QueryOutcome execute(const ConnectionProfile& profile,const QString& script,QueryLimits limits,const Cancellation& cancel,const QueryExecutionOptions& options,const std::function<void()>& connected={},const std::function<void()>& structureChanged={}) {
        const OperationTimer timing(options.analyzeOnly ? TimedOperation::SqlAnalyze
            : options.explain ? TimedOperation::Explain : TimedOperation::QueryExecute);
        QElapsedTimer timer;timer.start();QueryOutcome outcome;outcome.profileId=profile.id;outcome.database=profile.database;outcome.sessionId=options.sessionId;outcome.transactionAction=options.transaction;const auto key=profile.id+QChar(0x1f)+profile.database+(options.sessionId.isEmpty()?QString{}:QChar(0x1f)+options.sessionId);
        auto cancelledBeforeExecution=[&]{outcome.result.cancelled=true;outcome.result.elapsedMs=timer.elapsed();auto existing=sessions.find(key);if(existing!=sessions.end() && existing->second.db)outcome.transactionState=existing->second.db->transactionState();return outcome;};
        if(profile.driver==DatabaseKind::Redis) {
            outcome.transactionState=TransactionState::Unavailable;
            try {
                if (options.explain || options.analyzeOnly || options.transaction!=TransactionAction::None)
                    throw std::runtime_error("Redis 使用命令控制台");
                if (cancel->load()) return cancelledBeforeExecution();
                auto& active=session(profile,options.sessionId);
                if(connected)connected();
                outcome.serverVersion=active.db->serverVersion();
                outcome.detectedVersion=detectVersion(profile.driver,outcome.serverVersion);
                if(options.redisKey) {
                    outcome.result=active.db->redisPreview(*options.redisKey,limits,cancel);
                    outcome.results << outcome.result;
                } else {
                    for(const auto& line:script.split('\n')) {
                        if(cancel->load())break;
                        const auto parsed=parseRedisCommand(line);
                        if(parsed.arguments.isEmpty() && parsed.error.isEmpty())continue;
                        if(outcome.results.size()>=256 || limits.maxBytes<128) {
                            outcome.result.error="已达到 Redis 批处理结果上限，请分批执行";
                            break;
                        }
                        ++outcome.attemptedStatements;
                        outcome.result=active.db->execute(line,limits,cancel);
                        limits.maxBytes-=outcome.result.cachedBytes;
                        outcome.results << outcome.result;
                        ++outcome.executedStatements;
                        if(!outcome.result.error.isEmpty() || outcome.result.cancelled)break;
                    }
                }
            } catch(const std::exception& error) {
                outcome.result.error=QString::fromUtf8(error.what());
                sessions.erase(key);
                outcome.sessionClosed=true;
            }
            outcome.result.cancelled|=cancel->load();
            if(cancel->load() && sessions.erase(key))outcome.sessionClosed=true;
            outcome.result.elapsedMs=timer.elapsed();
            return outcome;
        }
        if(options.analyzeOnly) {
            outcome.analyzedOnly=true;
            try {
                const auto d=scriptDialect(profile.driver);
                const auto bytes=script.toUtf8();const auto spans=sql::splitStatements(bytes.toStdString(),d);
                outcome.analysis=analyzeQueryScript(profile,options.schema,std::string_view(bytes.constData(),bytes.size()),spans,cancel);
            }catch(const std::exception& e){outcome.result.error=QString::fromUtf8(e.what());}
            outcome.result.cancelled=cancel->load();outcome.result.elapsedMs=timer.elapsed();
            const auto existing=sessions.find(key);if(existing!=sessions.end() && existing->second.db)outcome.transactionState=existing->second.db->transactionState();
            return outcome; // Deliberately before session(), authentication and all database calls.
        }
        if(options.explain){
            outcome.explained=true;outcome.estimatedPlan.driver=profile.driver;try{auto bytes=script.toUtf8();auto d=scriptDialect(profile.driver);auto spans=sql::splitStatements(bytes.toStdString(),d);outcome.analysis=analyzeQueryScript(profile,options.schema,std::string_view(bytes.constData(),bytes.size()),spans,cancel);if(cancel->load())return cancelledBeforeExecution();outcome.result=readQueryPlan(profile,script,limits,cancel,connected,options.schema,&outcome.estimatedPlan.request);auto trace=std::move(outcome.estimatedPlan.request);outcome.estimatedPlan=decodeEstimatedPlan(profile.driver,outcome.result);outcome.estimatedPlan.request=std::move(trace);outcome.results<<outcome.result;}catch(const std::exception& e){outcome.result.error=QString::fromUtf8(e.what());outcome.estimatedPlan.warning=outcome.result.error;}
            outcome.result.cancelled|=cancel->load();auto existing=sessions.find(key);if(existing!=sessions.end() && existing->second.db)outcome.transactionState=existing->second.db->transactionState();outcome.result.elapsedMs=timer.elapsed();return outcome;
        }
        try {
            if(cancel->load())return cancelledBeforeExecution();
            const auto dialect=scriptDialect(profile.driver);
            auto bytes=script.toUtf8();std::vector<sql::StatementSpan> statements;if(options.transaction==TransactionAction::None)statements=sql::splitStatements(bytes.toStdString(),dialect);
            if(options.transaction==TransactionAction::None)outcome.analysis=analyzeQueryScript(profile,options.schema,std::string_view(bytes.constData(),bytes.size()),statements,cancel);
            if(cancel->load())return cancelledBeforeExecution();
            auto& active=session(profile,options.sessionId);
            if(active.schema!=options.schema){
                if(active.db->transactionState()!=TransactionState::Idle)throw std::runtime_error("请先提交或回滚当前事务，再切换 Schema");
                if(options.schema.isEmpty()){active.db.reset();active.db=connectDatabase(profile);active.schema.clear();}
                else{try{applyQuerySchema(*active.db,profile,options.schema,cancel);active.schema=options.schema;}catch(...){active.db.reset();throw;}}
            }
            if(connected)connected();outcome.serverVersion=active.db->serverVersion();outcome.detectedVersion=detectVersion(profile.driver,outcome.serverVersion);active.db->setAutoCommit(options.autoCommit);
            auto publishCatalogChange=[&](bool rolledBack) {
                if(active.catalogChangesPending && active.db->transactionState()==TransactionState::Idle) {
                    outcome.catalogChanged|=!rolledBack;
                    active.catalogChangesPending=false;
                }
            };
            if(options.transaction!=TransactionAction::None) {
                if(options.transaction==TransactionAction::Commit && active.db->transactionState()==TransactionState::Failed)
                    throw std::runtime_error("事务已失败，请先回滚，不能提交");
                outcome.result=active.db->transaction(options.transaction,cancel);
                outcome.results<<outcome.result;
                if(outcome.result.error.isEmpty() && !outcome.result.cancelled)
                    publishCatalogChange(options.transaction==TransactionAction::Rollback);
            }
            else {
                qint64 remaining=limits.maxBytes;
                for(std::size_t i=0;i<statements.size();++i){outcome.failedStatement=int(i);auto span=statements[i];auto original=sql::statementSource(std::string_view(bytes.constData(),bytes.size()),span,dialect);auto text=QString::fromUtf8(original.data(),int(original.size()));outcome.statements<<text.left(4096);
                    if(cancel->load()){outcome.result.cancelled=true;break;}
                    if(outcome.results.size()>=256 || remaining<128){outcome.result.error="本批结果已达到 256 个结果 / 总缓存上限，剩余语句尚未执行，请分批运行";break;}
                    auto words=sql::lex(text.toStdString(),dialect).tokens;QString first;for(auto t:words)if(t.kind!=sql::TokenKind::Comment){auto utf=text.toUtf8();first=QString::fromUtf8(utf.mid(t.begin,t.end-t.begin)).toUpper();break;}
                    if((first=="COMMIT" || first=="END") && active.db->transactionState()==TransactionState::Failed)throw std::runtime_error("事务已失败，请执行 ROLLBACK");
                    const bool control=QStringList{"BEGIN","START","COMMIT","ROLLBACK","SAVEPOINT","RELEASE","END"}.contains(first);
                    if(!options.autoCommit && !control && active.db->transactionState()==TransactionState::Idle){auto began=active.db->transaction(TransactionAction::Begin,cancel);if(!began.error.isEmpty())throw std::runtime_error(began.error.toStdString());}
                    const bool catalogShape=QStringList{"CREATE","ALTER","DROP","RENAME","COMMENT"}.contains(first);
                    const bool structure=catalogShape || QStringList{"TRUNCATE","GRANT","REVOKE","SET","USE","DO","CALL","EXEC","EXECUTE","DECLARE","DISCARD"}.contains(first);
                    if(structure && !outcome.metadataChanged){outcome.metadataChanged=true;if(structureChanged)structureChanged();}
                    if(first=="SET" || first=="USE" || first=="DECLARE" || first=="DISCARD" || first=="CALL" || first=="EXEC" || first=="EXECUTE" || first=="DO")outcome.sessionContextChanged=true;
                    if(first=="SELECT")for(auto token:words)if(token.kind==sql::TokenKind::Word && text.toUtf8().mid(token.begin,token.end-token.begin).toUpper()=="SET_CONFIG")outcome.sessionContextChanged=true;
                    if(first=="CREATE")for(auto token:words)if(token.kind==sql::TokenKind::Word){auto raw=text.toUtf8().mid(token.begin,token.end-token.begin).toUpper();if(raw=="TEMP" || raw=="TEMPORARY")outcome.sessionContextChanged=true;}
                    QueryLimits current=limits;current.maxBytes=remaining;QElapsedTimer statementTimer;statementTimer.start();++outcome.attemptedStatements;auto result=span.compound?active.db->executeDefinition(text,current,cancel):active.db->execute(text,current,cancel);result.elapsedMs=statementTimer.elapsed();auto more=std::move(result.moreResults);result.moreResults.clear();outcome.results<<result;outcome.result=result;remaining-=result.cachedBytes;
                    if(result.error.isEmpty() && !result.cancelled) {
                        active.catalogChangesPending|=catalogShape;
                        publishCatalogChange(first=="ROLLBACK");
                    }
                    for(auto extra:more){if(outcome.results.size()>=256 || extra.cachedBytes>remaining){outcome.result.truncated=true;break;}remaining-=extra.cachedBytes;outcome.results<<std::move(extra);}
                    ++outcome.executedStatements;if(!result.error.isEmpty() || result.cancelled)break;outcome.failedStatement=-1;
                }
            }
            outcome.transactionState=active.db->transactionState();
            if(outcome.result.cancelled && profile.driver!=DatabaseKind::SQLite){sessions.erase(key);outcome.sessionClosed=true;outcome.transactionState=TransactionState::Idle;}
        }catch(const std::exception& e){outcome.result.error=QString::fromUtf8(e.what());auto it=sessions.find(key);if(it!=sessions.end() && !it->second.db){sessions.erase(it);}else if(it!=sessions.end() && it->second.db){outcome.transactionState=it->second.db->transactionState();if(!it->second.db->healthy()){sessions.erase(it);outcome.sessionClosed=true;outcome.transactionState=TransactionState::Unknown;}}}
        if(cancel->load()){outcome.result.cancelled=true;if(profile.driver!=DatabaseKind::SQLite && sessions.erase(key)){outcome.sessionClosed=true;outcome.transactionState=TransactionState::Idle;}}
        if(!outcome.result.error.isEmpty() && outcome.failedStatement>=0)outcome.result.error=QString("第 %1 条语句失败：\n").arg(outcome.failedStatement+1)+outcome.result.error;
        outcome.result.elapsedMs=timer.elapsed();return outcome;
    }
};
namespace {
QString queryKey(const QString& profile,const QString& database,const QString& session){return profile+QChar(0x1f)+database+QChar(0x1f)+session;}
}
struct QueryLane {
    QThread thread;
    QueryWorker* worker=new QueryWorker;
    Cancellation token;
    QString profileId,database,sessionId;
    int count=0;
    bool busy=false,closing=false;
    QueryLane(){worker->moveToThread(&thread);QObject::connect(&thread,&QThread::finished,worker,&QObject::deleteLater);thread.setObjectName("navidog-query-session");thread.start();}
    ~QueryLane(){if(token)token->store(true);thread.quit();thread.wait();}
};
bool QueryService::busy()const{if(busy_)return true;for(const auto& entry:queryLanes_)if(entry.second->busy || entry.second->closing)return true;return false;}
bool QueryService::runQuery(const ConnectionProfile& profile,const QString& sql,QueryLimits limits,QueryExecutionOptions options){
    if(operationBusy())return false;int active=0;for(const auto& entry:queryLanes_)if(entry.second->busy)++active;if(active>=4)return false;
    const auto key=queryKey(profile.id,profile.database,options.sessionId);auto found=queryLanes_.find(key);
    if(found==queryLanes_.end()){
        if(queryLanes_.size()>=16)return false;auto lane=std::make_unique<QueryLane>();lane->profileId=profile.id;lane->database=profile.database;lane->sessionId=options.sessionId;found=queryLanes_.emplace(key,std::move(lane)).first;
    }
    auto* lane=found->second.get();if(lane->busy || lane->closing)return false;lane->busy=true;lane->token=cancellation();auto token=lane->token;
    QMetaObject::invokeMethod(lane->worker,[this,lane,key,profile,sql,limits,options,token]{
        auto outcome=lane->worker->execute(profile,sql,limits,token,options,[this,lane,options]{const int count=lane->worker->sessions.size()+(options.explain?1:0);QMetaObject::invokeMethod(this,[this,lane,count]{lane->count=count;publishSessions();},Qt::QueuedConnection);},[this,profile]{QMetaObject::invokeMethod(this,[this,profile]{invalidateMetadata(profile.id,profile.database);},Qt::QueuedConnection);});const int count=lane->worker->sessions.size();
        QMetaObject::invokeMethod(this,[this,lane,key,count,outcome=std::move(outcome)]()mutable{
            lane->count=count;lane->busy=false;if(!count && !lane->closing)queryLanes_.erase(key);publishSessions();emit completed(std::move(outcome));
        },Qt::QueuedConnection);
    },Qt::QueuedConnection);return true;
}
void QueryService::closeQueryLanes(const QString& profileId,const QString* database,const QString* sessionId){
    for(auto& entry:queryLanes_){auto* lane=entry.second.get();if(lane->profileId!=profileId || (database && lane->database!=*database) || (sessionId && lane->sessionId!=*sessionId) || lane->closing)continue;
        lane->closing=true;if(lane->token)lane->token->store(true);const auto key=entry.first;
        QMetaObject::invokeMethod(lane->worker,[this,lane,key]{lane->worker->sessions.clear();QMetaObject::invokeMethod(this,[this,key]{queryLanes_.erase(key);publishSessions();},Qt::QueuedConnection);},Qt::QueuedConnection);
    }
}
void QueryService::cancelQuery(const QString& profileId,const QString& database,const QString& sessionId){auto it=queryLanes_.find(queryKey(profileId,database,sessionId));if(it!=queryLanes_.end() && it->second->token)it->second->token->store(true);}
void QueryService::publishSessions(){
    int count=metadataCount_;auto ids=metadataProfiles_;for(const auto& entry:queryLanes_)if(entry.second->count){count+=entry.second->count;if(!ids.contains(entry.second->profileId))ids<<entry.second->profileId;}
    sessionCount_=count;connectedProfiles_=ids;emit sessionCountChanged(count);emit connectionStatesChanged(ids);
}
QueryService::QueryService(QObject* parent) : QObject(parent), worker_(new QueryWorker), metadataWorker_(new QObject) {
    qRegisterMetaType<MetadataResult>();qRegisterMetaType<MetadataSnapshotPtr>();
    qRegisterMetaType<SourceDesignResult>();qRegisterMetaType<TableTaskResult>();qRegisterMetaType<NamespaceOptions>();qRegisterMetaType<NamespaceResult>();qRegisterMetaType<QueryOutcome>(); qRegisterMetaType<CatalogResult>(); qRegisterMetaType<TableDesignResult>();qRegisterMetaType<DataPageResult>();qRegisterMetaType<DataCommitResult>(); worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    thread_.setObjectName("navidog-catalog-worker"); thread_.start();
    metadataWorker_->moveToThread(&metadataThread_);
    connect(&metadataThread_, &QThread::finished, metadataWorker_, &QObject::deleteLater);
    metadataThread_.setObjectName("navidog-metadata-worker"); metadataThread_.start();
}
QueryService::~QueryService() { if(metadataCancellation_)metadataCancellation_->store(true);cancel();metadataThread_.quit();thread_.quit();metadataThread_.wait();thread_.wait();queryLanes_.clear(); }
bool QueryService::run(const ConnectionProfile& profile,const QString& sql,QueryLimits limits,QueryExecutionOptions options) {
    if(!options.sessionId.isEmpty())return runQuery(profile,sql,limits,options);
    if(busy())return false;
    busy_ = true; cancellation_ = cancellation(); auto token = cancellation_;
    QMetaObject::invokeMethod(worker_, [this, profile, sql, limits, token,options] {
        auto outcome = worker_->execute(profile,sql,limits,token,options,{},[this,profile]{QMetaObject::invokeMethod(this,[this,profile]{invalidateMetadata(profile.id,profile.database);},Qt::QueuedConnection);});
        const int count = worker_->sessions.size(); const auto ids=worker_->connectedProfiles();
        QMetaObject::invokeMethod(this, [this, count, ids, outcome = std::move(outcome)]() mutable {
            busy_ = false; updateSessions(count,ids); emit completed(std::move(outcome));
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
    return true;
}
bool QueryService::metadata(const ConnectionProfile& original,const QString& database,const QString& schema,QString requestId,bool refresh) {
    if(metadataPath_.isEmpty() || metadataLoads_>=8 || (refresh && (busy() || metadataRefreshing_)))return false;
    if(refresh){metadataRefreshing_=true;metadataCancellation_=cancellation();}
    auto token=refresh?metadataCancellation_:cancellation();auto profile=original;
    if(!database.isEmpty() && profile.driver!=DatabaseKind::SQLite && !usesSchemaAtConnection(profile.driver))profile.database=database;
    if(refresh){metadataProfileId_=profile.id;metadataDatabase_=profile.database;}
    if(refresh)invalidateMetadata(profile.id,profile.database);
    const auto epoch=metadataEpochs_.value(profile.id);const auto path=metadataPath_;++metadataLoads_;
    // Catalog navigation must never queue behind a slow automatic metadata scan.
    // Each refresh owns a short-lived driver connection on this dedicated thread.
    QMetaObject::invokeMethod(metadataWorker_,[this,profile,schema,requestId,refresh,token,epoch,path]{
        const OperationTimer timing(refresh ? TimedOperation::RefreshMetadata : TimedOperation::LoadMetadataCache);
        MetadataResult result;result.fromDisk=!refresh;
        try {MetadataStore store(path);if(refresh){if(token->load())throw std::runtime_error("metadata refresh cancelled");auto db=connectDatabase(profile);auto collected=collectMetadata(*db,profile,schema,token);if(token->load())throw std::runtime_error("metadata refresh cancelled");store.replace(collected);result.snapshot=std::make_shared<const MetadataSnapshot>(std::move(collected));}else result.snapshot=store.load(metadataKey(profile,profile.database,schema));}
        catch(const std::exception& e){result.error=QString::fromUtf8(e.what());}result.cancelled=token->load();if(result.cancelled){result.snapshot.reset();if(refresh)try{MetadataStore(path).invalidate(profile.id,profile.database);}catch(...){}}
        QMetaObject::invokeMethod(this,[this,profile,requestId,refresh,epoch,result]()mutable{
            --metadataLoads_;if(refresh){metadataRefreshing_=false;metadataProfileId_.clear();metadataDatabase_.clear();}
            if(metadataEpochs_.value(profile.id)!=epoch){result.snapshot.reset();result.error="元数据刷新期间已失效，等待后台重试";const auto path=metadataPath_;QMetaObject::invokeMethod(metadataWorker_,[path,profile]{try{MetadataStore(path).invalidate(profile.id);}catch(...){}},Qt::QueuedConnection);}
            emit metadataCompleted(requestId,result);
        },Qt::QueuedConnection);
    },Qt::QueuedConnection);return true;
}
void QueryService::invalidateMetadata(const QString& id,const QString& database) {
    ++metadataEpochs_[id];emit metadataInvalidated(id,database);if(metadataPath_.isEmpty())return;const auto path=metadataPath_;
    QMetaObject::invokeMethod(metadataWorker_,[path,id,database]{try{MetadataStore(path).invalidate(id,database);}catch(...){}},Qt::QueuedConnection);
}
void QueryService::removeMetadata(const QString& id) {
    invalidateMetadata(id);if(metadataPath_.isEmpty())return;const auto path=metadataPath_;
    QMetaObject::invokeMethod(metadataWorker_,[path,id]{try{MetadataStore(path).removeProfile(id);}catch(...){}},Qt::QueuedConnection);
}
bool QueryService::catalog(const ConnectionProfile& original, const CatalogNode& node, QString requestId,bool isolated) {
    if(busy_)return false;
    busy_ = true;catalogBusy_=true; cancellation_ = cancellation(); auto token = cancellation_;
    QMetaObject::invokeMethod(worker_, [this, original, node, requestId, token,isolated] {
        const OperationTimer timing(node.kind == "connection" ? TimedOperation::OpenConnection : TimedOperation::LoadCatalog);
        CatalogResult result; auto profile = original;
        if (!node.database.isEmpty() && profile.driver != navidog::DatabaseKind::SQLite && !usesSchemaAtConnection(profile.driver)) profile.database = node.database;
        try {
            if (token->load()) result.cancelled = true;
            else {
                auto fresh=isolated?connectDatabase(profile):std::unique_ptr<Database>{};
                auto& db=isolated?*fresh:*worker_->session(profile).db;
                result = loadCatalog(db, profile, node, token);
                result.serverVersion=db.serverVersion(); result.detectedVersion=detectVersion(profile.driver,result.serverVersion);
            }
        } catch (const std::exception& e) { result.error = QString::fromUtf8(e.what()); if(!isolated)worker_->sessions.erase(profile.id+QChar(0x1f)+profile.database); }
        if (token->load()) { result.cancelled = true; result.nodes.clear(); if(!isolated)worker_->sessions.erase(profile.id+QChar(0x1f)+profile.database); }
        const int count = worker_->sessions.size(); const auto ids=worker_->connectedProfiles();
        QMetaObject::invokeMethod(this, [this, requestId, count, ids, result=std::move(result)]() mutable {
            busy_=false;catalogBusy_=false; updateSessions(count,ids); emit catalogCompleted(requestId, std::move(result));
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
    return true;
}
void QueryService::disconnectProfile(const QString& id) {
    if(metadataRefreshing_ && metadataProfileId_==id && metadataCancellation_)metadataCancellation_->store(true);
    closeQueryLanes(id);
    QMetaObject::invokeMethod(worker_, [this,id] {
        worker_->disconnectProfile(id); const int count=worker_->sessions.size(); const auto ids=worker_->connectedProfiles();
        QMetaObject::invokeMethod(this,[this,count,ids] { updateSessions(count,ids); },Qt::QueuedConnection);
    },Qt::QueuedConnection);
}
bool QueryService::tableDefinition(const ConnectionProfile& p,const CatalogNode& n,QString id) {
    if(busy())return false;
    busy_=true;cancellation_=cancellation();auto token=cancellation_;
    QMetaObject::invokeMethod(worker_,[this,p,n,id,token]{
        const OperationTimer timing(TimedOperation::ReadTableDefinition);
        TableDesignResult result;
        try {auto db=connectDatabase(p);result.definition=readTableDefinition(*db,p,n,token);}
        catch(const std::exception& e){result.error=QString::fromUtf8(e.what());}
        result.cancelled=token->load();
        QMetaObject::invokeMethod(this,[this,id,result]{busy_=false;emit tableDesignCompleted(id,result);},Qt::QueuedConnection);
    },Qt::QueuedConnection);return true;
}
bool QueryService::saveTable(const ConnectionProfile& p,const TableDefinition& before,const TableDefinition& after,QString id) {
    if(busy())return false;invalidateMetadata(p.id,p.database);
    busy_=true;cancellation_=cancellation();auto token=cancellation_;
    QMetaObject::invokeMethod(worker_,[this,p,before,after,id,token]{
        const OperationTimer timing(TimedOperation::SaveTableDefinition);
        auto result=applyTableChanges(p,before,after,token);
        QMetaObject::invokeMethod(this,[this,id,result]{busy_=false;emit tableDesignCompleted(id,result);},Qt::QueuedConnection);
    },Qt::QueuedConnection);return true;
}
void QueryService::disconnectDatabase(const QString& id,const QString& database) {
    if(metadataRefreshing_ && metadataProfileId_==id && metadataDatabase_==database && metadataCancellation_)metadataCancellation_->store(true);
    closeQueryLanes(id,&database);
    QMetaObject::invokeMethod(worker_,[this,id,database]{
        const auto prefix=id+QChar(0x1f)+database;for(auto it=worker_->sessions.begin();it!=worker_->sessions.end();)if(it->first==prefix || it->first.startsWith(prefix+QChar(0x1f)))it=worker_->sessions.erase(it);else ++it; const int count=worker_->sessions.size(); const auto ids=worker_->connectedProfiles();
        QMetaObject::invokeMethod(this,[this,count,ids]{updateSessions(count,ids);},Qt::QueuedConnection);
    },Qt::QueuedConnection);
}
bool QueryService::dataPage(const ConnectionProfile& p,const DataPageRequest& request,QString id) {
    if(busy())return false;
    busy_=true;cancellation_=cancellation();auto token=cancellation_;
    QMetaObject::invokeMethod(worker_,[this,p,request,id,token]{
        const OperationTimer timing(TimedOperation::ReadTableData);
        auto result=readDataPage(p,request,token);QMetaObject::invokeMethod(this,[this,id,result]{busy_=false;emit dataPageCompleted(id,result);},Qt::QueuedConnection);},Qt::QueuedConnection);return true;
}
bool QueryService::commitData(const ConnectionProfile& p,const TableDefinition& schema,const QVector<DataRowChange>& changes,QString id) {
    if(busy())return false;
    busy_=true;cancellation_=cancellation();auto token=cancellation_;
    QMetaObject::invokeMethod(worker_,[this,p,schema,changes,id,token]{
        const OperationTimer timing(TimedOperation::SaveTableData);
        auto result=commitDataChanges(p,schema,changes,token);QMetaObject::invokeMethod(this,[this,id,result]{busy_=false;emit dataCommitCompleted(id,result);},Qt::QueuedConnection);},Qt::QueuedConnection);return true;
}
void QueryService::updateSessions(int count,const QStringList& ids) {
    metadataCount_=count;metadataProfiles_=ids;publishSessions();
}
void QueryService::disconnectSession(const QString& id,const QString& database,const QString& sessionId){
    if(!sessionId.isEmpty()){closeQueryLanes(id,&database,&sessionId);return;}
    QMetaObject::invokeMethod(worker_,[this,id,database,sessionId]{worker_->sessions.erase(id+QChar(0x1f)+database+(sessionId.isEmpty()?QString{}:QChar(0x1f)+sessionId));auto count=worker_->sessions.size();auto ids=worker_->connectedProfiles();QMetaObject::invokeMethod(this,[this,count,ids]{updateSessions(count,ids);},Qt::QueuedConnection);},Qt::QueuedConnection);
}
bool QueryService::sourceDefinition(const ConnectionProfile& p,const CatalogNode& node,QString id){
    if(busy())return false;busy_=true;cancellation_=cancellation();auto token=cancellation_;QMetaObject::invokeMethod(worker_,[this,p,node,id,token]{
        const OperationTimer timing(TimedOperation::ReadSourceDefinition);
        SourceDesignResult result;try{auto db=connectDatabase(p);result.definition=readSourceDefinition(*db,p,node,token);}catch(const std::exception& e){result.error=QString::fromUtf8(e.what());}result.cancelled=token->load();QMetaObject::invokeMethod(this,[this,id,result]{busy_=false;emit sourceDesignCompleted(id,result);},Qt::QueuedConnection);},Qt::QueuedConnection);return true;
}
bool QueryService::saveSource(const ConnectionProfile& p,const SourceDefinition& before,const CatalogNode& node,const QString& sql,bool remove,QString id){
    if(busy())return false;invalidateMetadata(p.id,p.database);busy_=true;cancellation_=cancellation();auto token=cancellation_;QMetaObject::invokeMethod(worker_,[this,p,before,node,sql,remove,id,token]{
        const OperationTimer timing(TimedOperation::SaveSourceDefinition);
        auto result=applySourceChanges(p,before,node,sql,remove,token);QMetaObject::invokeMethod(this,[this,id,result]{busy_=false;emit sourceDesignCompleted(id,result);},Qt::QueuedConnection);},Qt::QueuedConnection);return true;
}
bool QueryService::testConnection(const ConnectionProfile& profile,QString requestId) {
    if(busy())return false;
    busy_=true;cancellation_=cancellation();auto token=cancellation_;
    QMetaObject::invokeMethod(worker_,[this,profile,requestId,token]{
        QueryOutcome outcome;QElapsedTimer timer;timer.start();
        try {
            if(token->load())outcome.result.cancelled=true;
            else {
                auto db=connectDatabase(profile);
                outcome.serverVersion=db->serverVersion();outcome.detectedVersion=detectVersion(profile.driver,outcome.serverVersion);
                QueryLimits limits;limits.maxRows=1;limits.maxBytes=1024;
                outcome.result=db->execute((usesOracleSql(profile.driver) || profile.driver==DatabaseKind::Dameng) ? "SELECT 1 FROM dual" : profile.driver==DatabaseKind::Db2 ? "VALUES 1" : "SELECT 1",limits,token);
            }
        }catch(const SshHostKeyError& e){outcome.result.error=QString::fromUtf8(e.what());outcome.sshHostKey=e.publicKey;outcome.sshFingerprint=e.fingerprint;outcome.sshHostKeyMismatch=e.mismatch;}catch(const std::exception& e){outcome.result.error=QString::fromUtf8(e.what());}
        outcome.result.cancelled=outcome.result.cancelled || token->load();outcome.result.elapsedMs=timer.elapsed();
        recordOperationTiming(TimedOperation::TestConnection, outcome.result.elapsedMs);
        QMetaObject::invokeMethod(this,[this,requestId,outcome]{busy_=false;emit connectionTestCompleted(requestId,outcome);},Qt::QueuedConnection);
    },Qt::QueuedConnection);return true;
}
bool QueryService::tableTask(const ConnectionProfile& p,const TableTask& task,QString id) {
    if(busy())return false;if(tableActionWrites(task.action))invalidateMetadata(p.id,p.database);
    busy_=true;cancellation_=cancellation();auto token=cancellation_;
    QMetaObject::invokeMethod(worker_,[this,p,task,id,token]{
        auto result=runTableTask(p,task,token);
        QMetaObject::invokeMethod(this,[this,id,result]{busy_=false;emit tableTaskCompleted(id,result);},Qt::QueuedConnection);
    },Qt::QueuedConnection);return true;
}
bool QueryService::namespaceOptions(const ConnectionProfile& p,NamespaceKind kind,QString id) {
    if(busy())return false;
    busy_=true;cancellation_=cancellation();auto token=cancellation_;
    QMetaObject::invokeMethod(worker_,[this,p,kind,id,token]{
        auto result=loadNamespaceOptions(p,kind,token);
        QMetaObject::invokeMethod(this,[this,id,result]{busy_=false;emit namespaceOptionsCompleted(id,result);},Qt::QueuedConnection);
    },Qt::QueuedConnection);return true;
}
bool QueryService::createNamespace(const ConnectionProfile& p,const NamespaceDraft& draft,QString id) {
    if(busy())return false;invalidateMetadata(p.id,p.database);
    busy_=true;cancellation_=cancellation();auto token=cancellation_;
    QMetaObject::invokeMethod(worker_,[this,p,draft,id,token]{
        auto result=navidog::createNamespace(p,draft,token);
        QMetaObject::invokeMethod(this,[this,id,result]{busy_=false;emit namespaceCompleted(id,result);},Qt::QueuedConnection);
    },Qt::QueuedConnection);return true;
}
bool QueryService::dropNamespace(const ConnectionProfile& p,NamespaceKind kind,const QString& name,QString id) {
    if(busy())return false;
    invalidateMetadata(p.id,kind==NamespaceKind::Database?name:p.database);
    busy_=true;
    cancellation_=cancellation();
    auto token=cancellation_;
    QMetaObject::invokeMethod(worker_,[this,p,kind,name,id,token]{
        auto result=navidog::dropNamespace(p,kind,name,token);
        QMetaObject::invokeMethod(this,[this,id,result]{busy_=false;emit namespaceCompleted(id,result);},Qt::QueuedConnection);
    },Qt::QueuedConnection);
    return true;
}
void QueryService::cancel() {if(cancellation_)cancellation_->store(true);for(auto& entry:queryLanes_)if(entry.second->token)entry.second->token->store(true);}
}

void navidog::QueryService::cancelCatalog(){if(catalogBusy_ && cancellation_)cancellation_->store(true);}
