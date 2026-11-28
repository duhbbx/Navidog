#pragma once

#include "navidog/types.hpp"
#include "navidog/metadata_cache.hpp"
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QThreadPool>
#include <functional>

class ConnectionController;
class QTabWidget;
class QueryPane;
class QWidget;
namespace navidog { class QueryService; }

class QuerySessionController final : public QObject {
    Q_OBJECT
public:
    struct WorkspaceAccess {
        std::function<bool()> closingTabs;
        std::function<bool(QueryPane &)> prepareContext;
        std::function<navidog::MetadataSnapshotPtr(const navidog::ConnectionProfile&, const QueryPane&)> metadata;
    };

    QuerySessionController(navidog::QueryService &, ConnectionController &,
                           const QVector<navidog::ConnectionProfile> &,
                           QTabWidget &, WorkspaceAccess, QWidget *owner);
    void bind(QueryPane &);
    void run(QueryPane &, const QString &sql);
    void runEditor(QueryPane &);
    void analyzeEditor(QueryPane &);
    void cancel(QueryPane &);
    bool release(QueryPane &, bool confirm = true);
    void disconnectProfile(const QString &id);
    void disconnectDatabase(const QString &id, const QString &database);
    bool hasActiveTransaction(const QString &id = {}, const QString *database = nullptr) const;

signals:
    void activityChanged();
    void operationFinished();
    void statusChanged(const QString &);
    void errorOccurred(const QString &title, const QString &detail);
    void serverInfoReceived(const QString &profileId, const QString &version,
                            navidog::DatabaseVersion);
    void catalogChanged(const QString &profileId);

private:
    enum class Mode { Run, Analyze, Explain };
    struct Request {
        QPointer<QueryPane> pane;
        QString profileId, database;
    };
    const navidog::ConnectionProfile *profileFor(const QString &id) const;
    void submit(QueryPane &, const QString &, Mode,
                navidog::TransactionAction = navidog::TransactionAction::None);
    void dispatch(QueryPane&, const QString&, Mode, navidog::TransactionAction,
                  QVector<navidog::BoundQueryStatement> = {});
    void complete(const navidog::QueryOutcome &);
    void resetScope(const QString &id, const QString *database);

    navidog::QueryService &service_;
    ConnectionController &connections_;
    const QVector<navidog::ConnectionProfile> &profiles_;
    QTabWidget &tabs_;
    WorkspaceAccess workspace_;
    QWidget *owner_;
    QHash<QString, Request> pending_;
    QThreadPool parameterChecks_;
};
