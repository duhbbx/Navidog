#include "query_session_controller.hpp"
#include "connection_controller.hpp"
#include "query_pane.hpp"
#include "query_parameter_dialog.hpp"
#include "navidog/operation_timing.hpp"
#include "navidog/query_service.hpp"

#include <QMessageBox>
#include <QRunnable>
#include <QScopeGuard>
#include <QTabWidget>
#include <algorithm>
#include <utility>

namespace {
navidog::ImpactQueryTrace previewMessages(QPointer<QueryPane> pane, bool live) {
    return [pane, live](int index, navidog::ImpactQueryKind kind, const QString& sql) {
        if (!pane) return;
        pane->messages->appendSql(QString("%1 · 第 %2 条 · %3")
            .arg(live ? "编辑时预查" : "执行前预查").arg(index + 1)
            .arg(kind == navidog::ImpactQueryKind::MatchingRows ? "匹配行数" : "目标总行数"), sql);
    };
}
}

QuerySessionController::QuerySessionController(
    navidog::QueryService &service, ConnectionController &connections,
    const QVector<navidog::ConnectionProfile> &profiles, QTabWidget &tabs,
    WorkspaceAccess workspace, const navidog::QuerySafetyPolicy& safetyPolicy, QWidget *owner)
    : QObject(owner), service_(service), connections_(connections), profiles_(profiles),
      tabs_(tabs), workspace_(std::move(workspace)), owner_(owner), safetyPolicy_(safetyPolicy), safety_(this) {
    parameterChecks_.setMaxThreadCount(2);
    liveSafetyTimer_.setSingleShot(true);
    connect(&liveSafetyTimer_, &QTimer::timeout, this, &QuerySessionController::checkLiveSafety);
    connect(&tabs_, &QTabWidget::currentChanged, this, &QuerySessionController::refreshLiveSafety);
    connect(&service_, &navidog::QueryService::completed,
            this, &QuerySessionController::complete);
}

QuerySessionController::~QuerySessionController() {
    if (liveSafetyCancel_) liveSafetyCancel_->store(true);
    for (const auto& stop : safetyReviews_) stop->store(true);
}

const navidog::ConnectionProfile *QuerySessionController::profileFor(const QString &id) const {
    const auto found = std::find_if(profiles_.cbegin(), profiles_.cend(),
        [&](const auto &profile) { return profile.id == id; });
    return found == profiles_.cend() ? nullptr : &*found;
}

void QuerySessionController::bind(QueryPane &pane) {
    connect(pane.editor, &SqlEditor::textChanged, this, [this, &pane] {
        if (tabs_.currentWidget() == &pane) refreshLiveSafety();
    });
    connect(&pane, &QueryPane::contextChangeRequested, this, &QuerySessionController::refreshLiveSafety);
    connect(&pane, &QObject::destroyed, this, [this, key = &pane] {
        if (auto stop = safetyReviews_.take(key)) stop->store(true);
    });
    connect(&pane, &QueryPane::runRequested, this, [this, &pane] { runEditor(pane); });
    connect(&pane, &QueryPane::analyzeRequested, this, [this, &pane] { analyzeEditor(pane); });
    connect(&pane, &QueryPane::runStatementRequested, this, [this, &pane] {
        if (!profileFor(pane.profileId))
            return;
        try {
            int begin = 0;
            const auto statement = pane.statementAtCursor(&begin);
            if (statement.isEmpty())
                emit statusChanged("当前位置没有可运行的 SQL");
            else
                run(pane, statement, begin);
        } catch (const std::exception &error) {
            emit errorOccurred("SQL 无法执行", QString::fromUtf8(error.what()));
        }
    });
    connect(&pane, &QueryPane::transactionRequested, this,
            [this, &pane](navidog::TransactionAction action) { submit(pane, {}, Mode::Run, -1, action); });
    connect(&pane, &QueryPane::explainRequested, this,
            [this, &pane](const QString &sql, int begin) { submit(pane, sql, Mode::Explain, begin); });
    connect(&pane, &QueryPane::stopRequested, this, [this, &pane] {
        cancel(pane);
        emit statusChanged("正在停止…");
    });
    connect(&pane, &QueryPane::autoCommitChanged, this, &QuerySessionController::activityChanged);
}

void QuerySessionController::run(QueryPane &pane, const QString &sql, int documentStart) {
    submit(pane, sql, Mode::Run, documentStart);
}

void QuerySessionController::runEditor(QueryPane &pane) {
    if (pane.allowSqlAction()) {
        const auto cursor = pane.editor->textCursor();
        run(pane, pane.editor->selectedSqlOrAll(), cursor.hasSelection() ? cursor.selectionStart() : 0);
    }
}

void QuerySessionController::analyzeEditor(QueryPane &pane) {
    if (pane.kind == navidog::WorkspaceTabKind::Query && pane.allowSqlAction())
        submit(pane, pane.editor->selectedSqlOrAll(), Mode::Analyze);
}

void QuerySessionController::submit(QueryPane &pane, const QString &sql, Mode mode, int documentStart,
                                    navidog::TransactionAction action) {
    if (pane.executing || safetyReviews_.contains(&pane) || pane.property("bindingQueryParameters").toBool()) {
        emit statusChanged("此标签正在准备或执行查询");
        return;
    }
    QueryMessageSource source;
    if (documentStart >= 0) {
        auto* document = pane.editor->document();
        source = {document, document->revision(), documentStart, int(sql.size())};
    }
    const auto* profile = profileFor(pane.profileId);
    if (!profile || mode == Mode::Analyze || action != navidog::TransactionAction::None ||
        !navidog::mayHaveQueryParameters(profile->driver, sql) || workspace_.closingTabs() ||
        pane.property("closingWorkspace").toBool() || service_.operationBusy()) {
        dispatch(pane, sql, mode, action, source);
        return;
    }
    const auto id = pane.profileId;
    const auto session = pane.sessionId;
    const auto database = pane.database;
    const auto schema = pane.schema;
    const auto kind = profile->driver;
    const auto catalogDatabase = database.isEmpty() ? profile->database : database;
    auto metadata = workspace_.metadata ? workspace_.metadata(*profile, pane) : navidog::MetadataSnapshotPtr{};
    const QPointer<QueryPane> guarded(&pane);
    pane.setProperty("bindingQueryParameters", true);
    emit statusChanged("正在检查 SQL 参数…");
    parameterChecks_.start(QRunnable::create([this, guarded, sql, id, session, database, schema,
                                              kind, catalogDatabase, metadata, mode, action, source] {
        navidog::QueryParameterPlan plan;
        QString error;
        try {
            plan = navidog::inspectQueryParameters(kind, sql, catalogDatabase, schema, metadata);
        } catch (const std::exception& failure) {
            error = QString::fromUtf8(failure.what());
        }
        QMetaObject::invokeMethod(this, [this, guarded, sql, id, session, database, schema, kind, mode, action,
                                        plan = std::move(plan), error, source] {
            if (!guarded) return;
            const auto reset = qScopeGuard([guarded] {
                if (guarded) guarded->setProperty("bindingQueryParameters", false);
            });
            const auto valid = [&] {
                if (!guarded || workspace_.closingTabs() || guarded->property("closingWorkspace").toBool())
                    return false;
                const auto* activeProfile = profileFor(id);
                return activeProfile && activeProfile->driver == kind && guarded->profileId == id &&
                    guarded->sessionId == session && guarded->database == database && guarded->schema == schema;
            };
            if (!valid()) {
                emit statusChanged("查询上下文已变化，请重新运行");
                return;
            }
            if (!error.isEmpty()) {
                emit statusChanged("SQL 参数检查失败");
                emit errorOccurred("无法准备 SQL 参数", error);
                return;
            }
            QVector<navidog::BoundQueryStatement> bindings;
            if (!plan.parameters.isEmpty()) {
                if (mode == Mode::Explain && plan.statements.size() != 1) {
                    emit errorOccurred("无法查看执行计划", "请选择一条语句再查看执行计划");
                    return;
                }
                const auto values = navidog::ui::promptQueryParameters(owner_, plan.parameters);
                if (!values) {
                    emit statusChanged("已取消 SQL 执行");
                    return;
                }
                if (!valid()) {
                    emit statusChanged("查询上下文已变化，请重新运行");
                    return;
                }
                try {
                    bindings = navidog::bindQueryParameters(plan, *values);
                } catch (const std::exception& failure) {
                    emit errorOccurred("无法绑定 SQL 参数", QString::fromUtf8(failure.what()));
                    return;
                }
            }
            dispatch(*guarded, sql, mode, action, source, std::move(bindings));
        }, Qt::QueuedConnection);
    }));
}

void QuerySessionController::dispatch(QueryPane &pane, const QString &sql, Mode mode,
                                      navidog::TransactionAction action,
                                      const QueryMessageSource& source,
                                      QVector<navidog::BoundQueryStatement> bindings) {
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
    const auto originalDriver = target.driver;
    const auto originalDatabase = pane.database;
    const auto originalSchema = pane.schema;
    if (!analyzeOnly) {
        const auto authenticated = connections_.authenticatedProfile(id);
        if (!authenticated || !alive || !workspace_.prepareContext(pane) ||
            !alive || pane.profileId != id || pane.property("closingWorkspace").toBool())
            return;
        target = *authenticated;
        if (!bindings.isEmpty() && (target.driver != originalDriver ||
            pane.database != originalDatabase || pane.schema != originalSchema)) {
            emit statusChanged("查询上下文已变化，请重新确认参数");
            return;
        }
    }
    if (!pane.database.isEmpty() && !navidog::isFileDatabase(target.driver) &&
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
    options.bindings = std::move(bindings);
    options.traceStatements = mode == Mode::Run;
    if (const auto redisKey = pane.property("redisPreviewKey"); redisKey.isValid())
        options.redisKey = QByteArray::fromBase64(redisKey.toString().toLatin1());
    if (mode == Mode::Run && action == navidog::TransactionAction::None && safetyPolicy_.enabled &&
        navidog::mightNeedQuerySafety(target.driver, sql)) {
        if (liveSafetyCancel_) liveSafetyCancel_->store(true);
        const auto stop = navidog::cancellation();
        const auto policy = safetyPolicy_;
        const auto revision = pane.editor->document()->revision();
        const auto scopeDatabase = pane.database;
        const bool measure = policy.previewBeforeRun && !pane.hasActiveTransaction() &&
            !pane.sessionContextChanged && pane.transactionState != navidog::TransactionState::Unknown;
        safetyReviews_.insert(&pane, stop);
        pane.setSafetyReviewPending(true);
        emit statusChanged("正在检查写入风险…");
        safety_.assess(target, options.schema, sql, options.bindings, policy, measure, false, stop,
            [this, alive, target, sql, limits, options, stop, policy, revision, scopeDatabase, source]
            (QuerySafetyController::Findings findings) {
                const auto valid = [&] {
                    return alive && safetyReviews_.value(alive.data()) == stop &&
                        !workspace_.closingTabs() && !alive->property("closingWorkspace").toBool() &&
                        alive->profileId == target.id && profileFor(target.id) &&
                        alive->sessionId == options.sessionId && alive->database == scopeDatabase &&
                        alive->schema == options.schema && alive->editor->document()->revision() == revision;
                };
                const auto finish = [&] {
                    if (alive && safetyReviews_.value(alive.data()) == stop) {
                        safetyReviews_.remove(alive.data());
                        alive->setSafetyReviewPending(false);
                    }
                };
                if (!valid()) {
                    finish();
                    emit statusChanged("SQL 或连接上下文已改变，请重新运行");
                    return;
                }
                if (!alive->editor->largeDocument() && !alive->editor->textCursor().hasSelection() &&
                    sql == alive->editor->toPlainText())
                    alive->editor->setLiveSafetyMarks(querySafetyMarks(findings, policy, true));
                const auto confirmed = QuerySafetyController::confirm(owner_, target, options.schema, policy, findings);
                const bool current = valid();
                finish();
                if (!confirmed || !current) {
                    if (!confirmed && alive) {
                        alive->messages->appendPlainText("已取消执行，原 SQL 未发送到驱动。");
                        alive->resultTabs->setCurrentWidget(alive->messages);
                    }
                    emit statusChanged(confirmed ? "SQL 或连接上下文已改变，请重新运行" : "已取消危险语句执行");
                    return;
                }
                alive->editor->setLiveSafetyMarks({});
                execute(*alive, target, sql, limits, options, source);
            }, previewMessages(alive, false));
        return;
    }
    execute(pane, target, sql, limits, options, source);
}

void QuerySessionController::execute(QueryPane &pane, const navidog::ConnectionProfile& target,
        const QString& sql, navidog::QueryLimits limits, const navidog::QueryExecutionOptions& options,
        const QueryMessageSource& source) {
    if (!service_.run(target, sql, limits, options)) {
        emit statusChanged("最多同时执行 4 个查询、保留 16 个查询会话；请等待执行完成或关闭不用的查询会话后重试");
        return;
    }
    pending_.insert(pane.sessionId, {&pane, target.id, target.database, source});
    if (!options.analyzeOnly)
        pane.sessionDatabase = target.database;
    pane.startExecution(options.analyzeOnly, options.transaction);
    emit activityChanged();
    emit statusChanged(options.analyzeOnly ? "正在本地解析 · SQL 未执行" : "正在执行…");
}

void QuerySessionController::refreshLiveSafety() {
    if (liveSafetyCancel_) liveSafetyCancel_->store(true);
    if (liveSafetyPane_) liveSafetyPane_->editor->setLiveSafetyMarks({});
    liveSafetyPane_.clear();
    liveSafetyTimer_.stop();
    if (safetyPolicy_.enabled && safetyPolicy_.previewWhileEditing && safetyPolicy_.warnings)
        liveSafetyTimer_.start(1100);
}

void QuerySessionController::checkLiveSafety() {
    auto *pane = qobject_cast<QueryPane*>(tabs_.currentWidget());
    if (!pane || pane->kind != navidog::WorkspaceTabKind::Query ||
        pane->executing || safetyReviews_.contains(pane) || !pane->scriptReady() ||
        pane->hasActiveTransaction() || pane->sessionContextChanged ||
        pane->transactionState == navidog::TransactionState::Unknown || pane->editor->largeDocument() ||
        pane->editor->document()->characterCount() > 64 * 1024 || !service_.isConnected(pane->profileId)) return;
    const auto *profile = profileFor(pane->profileId);
    if (!profile) return;
    auto target = *profile; // Only reuse available credentials; never prompt during typing.
    if (target.driver != navidog::DatabaseKind::SQLite && target.password.isEmpty() &&
        target.credentialSource != navidog::CredentialSource::Passwordless &&
        !target.oracle.externalAuthentication) return;
    const auto database = pane->database;
    const auto schema = pane->schema;
    if (!database.isEmpty() && !navidog::isFileDatabase(target.driver) && !navidog::usesOracleSql(target.driver))
        target.database = database;
    int begin = 0;
    QString sql;
    try { sql = pane->statementAtCursor(&begin); }
    catch (const std::exception&) { return; }
    if (!navidog::mightNeedQuerySafety(target.driver, sql)) return;
    const auto stop = liveSafetyCancel_ = navidog::cancellation();
    const auto session = pane->sessionId;
    const auto revision = pane->editor->document()->revision();
    const auto policy = safetyPolicy_;
    const QPointer<QueryPane> guarded(pane);
    liveSafetyPane_ = pane;
    safety_.assess(target, schema, sql, {}, policy, true, true, stop,
        [this, guarded, session, database, schema, revision, stop, policy, begin](QuerySafetyController::Findings findings) {
            if (!guarded || tabs_.currentWidget() != guarded || liveSafetyCancel_ != stop ||
                guarded->sessionId != session || guarded->database != database || guarded->schema != schema ||
                guarded->editor->document()->revision() != revision || guarded->hasActiveTransaction() ||
                guarded->sessionContextChanged || guarded->executing) return;
            for (auto& item : findings) {
                item.begin += begin;
                item.end += begin;
            }
            guarded->editor->setLiveSafetyMarks(querySafetyMarks(findings, policy, true));
        }, previewMessages(guarded, true));
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
        request.pane->applyOutcome(outcome, message, profile, request.source);
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
    if (auto stop = safetyReviews_.take(&pane)) {
        stop->store(true);
        pane.setSafetyReviewPending(false);
        return;
    }
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
    if (auto stop = safetyReviews_.take(&pane)) {
        stop->store(true);
        pane.setSafetyReviewPending(false);
    }
    if (liveSafetyPane_ == &pane) refreshLiveSafety();
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
            pane && pane->profileId == id && (!database || pane->sessionDatabase == *database)) {
            if (auto stop = safetyReviews_.take(pane)) {
                stop->store(true);
                pane->setSafetyReviewPending(false);
            }
            pane->resetSession();
        }
    refreshLiveSafety();
}
