#include "query_session_controller.hpp"
#include "connection_controller.hpp"
#include "query_pane.hpp"
#include "navidog/operation_timing.hpp"
#include "navidog/query_service.hpp"

#include <QMessageBox>
#include <QTabWidget>
#include <algorithm>
#include <utility>

QuerySessionController::QuerySessionController(
    navidog::QueryService &service, ConnectionController &connections,
    const QVector<navidog::ConnectionProfile> &profiles, QTabWidget &tabs,
    WorkspaceAccess workspace, QWidget *owner)
    : QObject(owner), service_(service), connections_(connections), profiles_(profiles),
      tabs_(tabs), workspace_(std::move(workspace)), owner_(owner) {
    connect(&service_, &navidog::QueryService::completed,
            this, &QuerySessionController::complete);
}

const navidog::ConnectionProfile *QuerySessionController::profileFor(const QString &id) const {
    const auto found = std::find_if(profiles_.cbegin(), profiles_.cend(),
        [&](const auto &profile) { return profile.id == id; });
    return found == profiles_.cend() ? nullptr : &*found;
}

void QuerySessionController::bind(QueryPane &pane) {
    connect(&pane, &QueryPane::runRequested, this, [this, &pane] { runEditor(pane); });
    connect(&pane, &QueryPane::analyzeRequested, this, [this, &pane] { analyzeEditor(pane); });
    connect(&pane, &QueryPane::runStatementRequested, this, [this, &pane] {
        if (!profileFor(pane.profileId))
            return;
        try {
            const auto statement = pane.statementAtCursor();
            if (statement.isEmpty())
                emit statusChanged("当前位置没有可运行的 SQL");
            else
                run(pane, statement);
        } catch (const std::exception &error) {
            emit errorOccurred("SQL 无法执行", QString::fromUtf8(error.what()));
        }
    });
    connect(&pane, &QueryPane::transactionRequested, this,
            [this, &pane](navidog::TransactionAction action) { submit(pane, {}, Mode::Run, action); });
    connect(&pane, &QueryPane::explainRequested, this,
            [this, &pane](const QString &sql) { submit(pane, sql, Mode::Explain); });
    connect(&pane, &QueryPane::stopRequested, this, [this, &pane] {
        cancel(pane);
        emit statusChanged("正在停止…");
    });
    connect(&pane, &QueryPane::autoCommitChanged, this, &QuerySessionController::activityChanged);
}

void QuerySessionController::run(QueryPane &pane, const QString &sql) {
    submit(pane, sql, Mode::Run);
}

void QuerySessionController::runEditor(QueryPane &pane) {
    if (pane.allowSqlAction())
        run(pane, pane.editor->selectedSqlOrAll());
}

void QuerySessionController::analyzeEditor(QueryPane &pane) {
    if (pane.kind == navidog::WorkspaceTabKind::Query && pane.allowSqlAction())
        submit(pane, pane.editor->selectedSqlOrAll(), Mode::Analyze);
}

void QuerySessionController::submit(QueryPane &pane, const QString &sql, Mode mode,
                                    navidog::TransactionAction action) {
    if (workspace_.closingTabs() || pane.property("closingWorkspace").toBool()) {
        emit statusChanged("正在处理标签关闭，请稍后执行");
        return;
    }
    if (pane.executing) {
        emit statusChanged("此标签正在执行，请等待完成或点击停止");
        return;
    }
    if (service_.operationBusy()) {
        emit statusChanged("正在读取或保存数据库对象，请等待完成后执行");
        return;
    }
    const auto id = pane.profileId;
    const auto *profile = profileFor(id);
    if (!profile) {
        emit statusChanged("请先选择或新建数据库连接");
        return;
    }
    const bool analyzeOnly = mode == Mode::Analyze;
    const QPointer<QueryPane> alive(&pane);
    auto target = *profile;
    if (!analyzeOnly) {
        const auto authenticated = connections_.authenticatedProfile(id);
        if (!authenticated || !alive || !workspace_.prepareContext(pane) ||
            !alive || pane.profileId != id || pane.property("closingWorkspace").toBool())
            return;
        target = *authenticated;
    }
    if (!pane.database.isEmpty() && target.driver != navidog::DatabaseKind::SQLite &&
        !navidog::usesOracleSql(target.driver))
        target.database = pane.database;
    navidog::QueryLimits limits;
    limits.maxRows = pane.limit->currentData().toInt();
    navidog::QueryExecutionOptions options;
    options.sessionId = pane.sessionId;
    options.autoCommit = pane.autoCommit->isChecked();
    options.transaction = action;
    options.explain = mode == Mode::Explain;
    options.analyzeOnly = analyzeOnly;
    options.schema = pane.schema;
    if (const auto redisKey = pane.property("redisPreviewKey"); redisKey.isValid())
        options.redisKey = QByteArray::fromBase64(redisKey.toString().toLatin1());
    if (!service_.run(target, sql, limits, options)) {
        emit statusChanged("最多同时执行 4 个查询、保留 16 个查询会话；请等待执行完成或关闭不用的查询会话后重试");
        return;
    }
    pending_.insert(pane.sessionId, {&pane, target.id, target.database});
    if (!analyzeOnly)
        pane.sessionDatabase = target.database;
    pane.startExecution(analyzeOnly, action);
    emit activityChanged();
    emit statusChanged(analyzeOnly ? "正在本地解析 · SQL 未执行" : "正在执行…");
}

void QuerySessionController::complete(const navidog::QueryOutcome &outcome) {
    const navidog::OperationTimer timing(navidog::TimedOperation::ApplyQueryResults);
    const auto request = pending_.take(outcome.sessionId);
    const auto &result = outcome.result;
    const auto message = outcome.analyzedOnly ? QString{} : QueryPane::resultSummary(result);
    if (!outcome.analyzedOnly)
        emit serverInfoReceived(outcome.profileId, outcome.serverVersion, outcome.detectedVersion);
    if (request.pane && request.pane->sessionId == outcome.sessionId) {
        const auto *profile = !outcome.analyzedOnly &&
                              outcome.transactionAction == navidog::TransactionAction::None
                                  ? profileFor(outcome.profileId) : nullptr;
        request.pane->applyOutcome(outcome, message, profile);
    }
    if (outcome.analyzedOnly) {
        emit statusChanged(result.cancelled ? "本地解析已停止"
                           : result.error.isEmpty() ? "本地 AST 解析完成 · SQL 未执行"
                                                    : "本地解析失败");
    } else {
        if (outcome.catalogChanged)
            emit catalogChanged(outcome.profileId);
        emit statusChanged(!result.error.isEmpty() ? QString("SQL 执行失败")
                           : outcome.explained && !result.cancelled ? "执行计划 · " + message
                                                                   : message);
    }
    emit activityChanged();
    if (!outcome.analyzedOnly && !result.error.isEmpty() && !result.cancelled)
        emit errorOccurred("SQL 执行失败", result.error);
    emit operationFinished();
}

void QuerySessionController::cancel(QueryPane &pane) {
    const auto request = pending_.constFind(pane.sessionId);
    if (request != pending_.cend())
        service_.cancelQuery(request->profileId, request->database, pane.sessionId);
}

bool QuerySessionController::release(QueryPane &pane, bool confirm) {
    if (confirm && (pane.hasActiveTransaction() ||
                    pane.transactionState == navidog::TransactionState::Unknown) &&
        QMessageBox::question(owner_, "关闭查询会话",
                              "此标签的未提交事务将回滚。继续关闭会话？",
                              QMessageBox::Yes | QMessageBox::Cancel,
                              QMessageBox::Cancel) != QMessageBox::Yes)
        return false;
    service_.disconnectSession(pane.profileId, pane.sessionDatabase, pane.sessionId);
    pane.resetSession();
    return true;
}

bool QuerySessionController::hasActiveTransaction(const QString &id, const QString *database) const {
    for (int i = 0; i < tabs_.count(); ++i)
        if (auto *pane = qobject_cast<QueryPane *>(tabs_.widget(i));
            pane && (id.isEmpty() || pane->profileId == id) &&
            (!database || pane->sessionDatabase == *database) && pane->hasActiveTransaction())
            return true;
    return false;
}

void QuerySessionController::disconnectProfile(const QString &id) {
    service_.disconnectProfile(id);
    resetScope(id, nullptr);
}

void QuerySessionController::disconnectDatabase(const QString &id, const QString &database) {
    service_.disconnectDatabase(id, database);
    resetScope(id, &database);
}

void QuerySessionController::resetScope(const QString &id, const QString *database) {
    for (int i = 0; i < tabs_.count(); ++i)
        if (auto *pane = qobject_cast<QueryPane *>(tabs_.widget(i));
            pane && pane->profileId == id && (!database || pane->sessionDatabase == *database))
            pane->resetSession();
}
