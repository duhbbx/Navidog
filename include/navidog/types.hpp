#pragma once
#include "estimated_plan.hpp"
#include "database_types.hpp"
#include "connection_options.hpp"
#include "sql_analysis.hpp"
#include "query_trace.hpp"
#include <QMetaType>
#include <QStringList>
#include <QVariant>
#include <QVector>
#include <atomic>
#include <memory>
#include <optional>
#include <tuple>

namespace navidog {
struct ConnectionProfile {
    QString groupId,notes;
    AdvancedConnectionOptions advanced;
    SslConnectionOptions ssl;
    SshConnectionOptions ssh;
    HttpConnectionOptions http;
    OracleConnectionOptions oracle;
    QString jdbcDriverJar;
    QString mongoAuthDatabase;
    bool h2File = false;
    QString credentialStorePath; // Runtime workspace binding; never serialized.
    bool secretsLoaded=false;
    bool savePassword=false;
    // Runtime transport endpoint. Never serialized or entered in the settings form.
    QString transportHost;
    int transportPort=0;
    QString id, name, host = "localhost", database, user, password;
    DatabaseKind driver = DatabaseKind::SQLite;
    DatabaseVersion version = DatabaseVersion::Auto, detectedVersion = DatabaseVersion::Unknown;
    QString serverVersion;
    CredentialSource credentialSource = CredentialSource::Prompt;
    QString credentialContainer, credentialVariable;
    int port = 5432;
    bool readOnly = false;
    bool tls = true;
    bool operator==(const ConnectionProfile& p)const {
        return std::tie(advanced,ssl,ssh,http,oracle,jdbcDriverJar,mongoAuthDatabase,h2File,savePassword,notes,groupId,id,name,host,database,user,password,driver,version,detectedVersion,serverVersion,credentialSource,credentialContainer,credentialVariable,port,readOnly,tls)==std::tie(p.advanced,p.ssl,p.ssh,p.http,p.oracle,p.jdbcDriverJar,p.mongoAuthDatabase,p.h2File,p.savePassword,p.notes,p.groupId,p.id,p.name,p.host,p.database,p.user,p.password,p.driver,p.version,p.detectedVersion,p.serverVersion,p.credentialSource,p.credentialContainer,p.credentialVariable,p.port,p.readOnly,p.tls);
    }
    bool operator!=(const ConnectionProfile& p)const{return !(*this==p);}
};
struct ConnectionGroup { QString id, name; };
struct WorkspaceProfiles { QVector<ConnectionProfile> connections; QVector<ConnectionGroup> groups; };
enum class TransactionState { Idle, Active, Failed, Unknown, Unavailable };
enum class TransactionAction { None, Begin, Commit, Rollback };
struct BoundQueryMarker {
    int begin, end, valueIndex; // Relative UTF-8 offsets in original SQL.
};
struct BoundQueryStatement {
    int index = 0;
    QString original, sql;
    QVector<QVariant> values;
    QVector<BoundQueryMarker> markers;
};
struct QueryExecutionOptions {
    QString sessionId;
    bool autoCommit=true,explain=false,analyzeOnly=false;
    TransactionAction transaction=TransactionAction::None;
    QString schema;
    std::optional<QByteArray> redisKey;
    QVector<BoundQueryStatement> bindings;
    bool traceStatements = false;
};
struct QueryLimits {
    int maxRows = 1000;
    qint64 maxBytes = 16 * 1024 * 1024;
    int maxCellBytes = 64 * 1024;
    bool rejectInvalidTextEncoding = false;
};
struct QueryResult {
    QVector<QueryResult> moreResults;
    QStringList columns;
    // Aligned with columns; empty entries mean the driver has no declared type.
    QStringList columnTypes;
    QVector<QVector<QVariant>> rows;
    QString error;
    qint64 affectedRows = 0, elapsedMs = 0, cachedBytes = 0;
    bool truncated = false, cancelled = false, cellTruncated = false;
};
struct QueryOutcome {
    QString profileId,database,sessionId;
    bool explained=false,analyzedOnly=false,metadataChanged=false,sessionContextChanged=false;
    bool catalogChanged=false; // Successful DDL visible to a separate session.
    TransactionAction transactionAction=TransactionAction::None;
    QVector<QueryResult> results;
    QStringList statements;
    QVector<QueryStatementTrace> statementTrace;
    TransactionState transactionState=TransactionState::Idle;
    int executedStatements=0,failedStatement=-1;
    bool sessionClosed=false;
    QString sshHostKey,sshFingerprint;
    bool sshHostKeyMismatch=false;
    QString serverVersion;
    DatabaseVersion detectedVersion = DatabaseVersion::Unknown;
    QueryResult result;
    QStringList tables;
    SqlExecutionAnalysis analysis;
    EstimatedPlan estimatedPlan;
    int attemptedStatements=0; // Original statements actually handed to a driver.
};
using Cancellation = std::shared_ptr<std::atomic_bool>;
inline Cancellation cancellation() { return std::make_shared<std::atomic_bool>(false); }
}
Q_DECLARE_METATYPE(navidog::QueryResult)
Q_DECLARE_METATYPE(navidog::QueryOutcome)
