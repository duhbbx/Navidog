#pragma once
#include "types.hpp"
#include "catalog.hpp"
#include "table_design.hpp"
#include "data_edit.hpp"
#include "namespace_design.hpp"
#include "table_actions.hpp"
#include "source_design.hpp"
#include "metadata_cache.hpp"
#include <QHash>
#include <QObject>
#include <QThread>
#include <map>

namespace navidog {
class QueryWorker;
struct QueryLane;
class QueryService final : public QObject {
    Q_OBJECT
public:
    explicit QueryService(QObject* parent = nullptr);
    ~QueryService() override;
    bool run(const ConnectionProfile&, const QString&, QueryLimits = {},QueryExecutionOptions = {});
    void setMetadataCachePath(QString path){metadataPath_=std::move(path);}
    bool metadata(const ConnectionProfile&,const QString& database,const QString& schema,QString requestId,bool refresh);
    void invalidateMetadata(const QString& profileId,const QString& database={});
    void removeMetadata(const QString& profileId);
    bool catalog(const ConnectionProfile&, const CatalogNode&, QString requestId,bool isolated=false);
    bool tableDefinition(const ConnectionProfile&,const CatalogNode&,QString requestId);
    bool saveTable(const ConnectionProfile&,const TableDefinition&,const TableDefinition&,QString requestId);
    bool dataPage(const ConnectionProfile&,const DataPageRequest&,QString requestId);
    bool commitData(const ConnectionProfile&,const TableDefinition&,const QVector<DataRowChange>&,QString requestId);
    bool sourceDefinition(const ConnectionProfile&,const CatalogNode&,QString requestId);
    bool saveSource(const ConnectionProfile&,const SourceDefinition&,const CatalogNode&,const QString&,bool remove,QString requestId);
    bool testConnection(const ConnectionProfile&, QString requestId);
    bool tableTask(const ConnectionProfile&,const TableTask&,QString requestId);
    bool namespaceOptions(const ConnectionProfile&,NamespaceKind,QString requestId);
    bool createNamespace(const ConnectionProfile&,const NamespaceDraft&,QString requestId);
    bool dropNamespace(const ConnectionProfile&,NamespaceKind,const QString& name,QString requestId);
    bool isConnected(const QString& id) const { return connectedProfiles_.contains(id); }
    void disconnectProfile(const QString& id);
    void disconnectDatabase(const QString& id,const QString& database);
    void disconnectSession(const QString& id,const QString& database,const QString& sessionId);
    void cancel();
    void cancelCatalog();
    void cancelQuery(const QString& profileId,const QString& database,const QString& sessionId);
    int sessionCount() const { return sessionCount_; }
    bool busy() const;
    bool operationBusy() const { return busy_ && !catalogBusy_; }
    bool catalogBusy() const { return catalogBusy_; }
signals:
    void completed(navidog::QueryOutcome outcome);
    void metadataCompleted(QString requestId,navidog::MetadataResult result);
    void metadataInvalidated(QString profileId,QString database);
    void catalogCompleted(QString requestId, navidog::CatalogResult result);
    void sessionCountChanged(int count);
    void connectionStatesChanged(QStringList profileIds);
    void connectionTestCompleted(QString requestId, navidog::QueryOutcome result);
    void namespaceOptionsCompleted(QString requestId,navidog::NamespaceOptions result);
    void tableTaskCompleted(QString requestId,navidog::TableTaskResult result);
    void namespaceCompleted(QString requestId,navidog::NamespaceResult result);
    void tableDesignCompleted(QString requestId,navidog::TableDesignResult result);
    void sourceDesignCompleted(QString requestId,navidog::SourceDesignResult result);
    void dataPageCompleted(QString requestId,navidog::DataPageResult result);
    void dataCommitCompleted(QString requestId,navidog::DataCommitResult result);
private:
    QString metadataPath_;
    QHash<QString,quint64> metadataEpochs_;
    int metadataLoads_=0;
    QThread thread_;
    QueryWorker* worker_;
    QThread metadataThread_;
    QObject* metadataWorker_;
    Cancellation cancellation_;
    bool busy_ = false;
    bool catalogBusy_ = false;
    bool metadataRefreshing_=false;
    Cancellation metadataCancellation_;
    QString metadataProfileId_,metadataDatabase_;
    int sessionCount_ = 0;
    QStringList connectedProfiles_;
    int metadataCount_=0;
    QStringList metadataProfiles_;
    std::map<QString,std::unique_ptr<QueryLane>> queryLanes_;
    bool runQuery(const ConnectionProfile&,const QString&,QueryLimits,QueryExecutionOptions);
    void closeQueryLanes(const QString& profileId,const QString* database=nullptr,const QString* sessionId=nullptr);
    void publishSessions();
    void updateSessions(int count, const QStringList& ids);
};
}
