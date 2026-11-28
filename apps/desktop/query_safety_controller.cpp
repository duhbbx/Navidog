#include "query_safety_controller.hpp"
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QRunnable>
#include <QTimer>
#include <algorithm>

namespace {
using namespace navidog;

void unavailable(QVector<QuerySafetyFinding>& findings, const QString& reason) {
    for (auto& item : findings)
        if (item.impact.status == ImpactStatus::Pending)
            item.impact = {ImpactStatus::Unavailable, 0, -1, false, reason};
}
}

QuerySafetyController::QuerySafetyController(QObject* parent) : QObject(parent) {
    analysis_.setMaxThreadCount(2);
    probes_.setMaxThreadCount(2);
}

QuerySafetyController::~QuerySafetyController() {
    analysis_.waitForDone();
    probes_.waitForDone();
}

void QuerySafetyController::assess(const ConnectionProfile& target, const QString& schema,
        const QString& sql, const QVector<BoundQueryStatement>& bindings, const QuerySafetyPolicy& policy,
        bool preview, bool live, const Cancellation& stop, Delivery done, ImpactQueryTrace trace) {
    if (!policy.enabled || !mightNeedQuerySafety(target.driver, sql)) {
        done({});
        return;
    }
    analysis_.start(QRunnable::create([this, target, schema, sql, bindings, policy, preview, live, stop,
                                      done = std::move(done), trace = std::move(trace)] {
        Findings findings;
        try {
            findings = inspectQuerySafety(target.driver, sql, policy, stop);
        } catch (const std::exception&) {
            if (!stop->load()) {
                QuerySafetyFinding unknown;
                unknown.operation = "SQL";
                unknown.risk = QueryRisk::Unverified;
                unknown.impact.detail = "本地写入风险分析未完成";
                findings << unknown;
            }
        }
        QMetaObject::invokeMethod(this, [this, target, schema, bindings, policy, preview, live, stop,
                                        findings = std::move(findings), done, trace] () mutable {
            if (stop->load()) return;
            if (preview) {
                measure(target, schema, bindings, policy, live, std::move(findings), stop, done, std::move(trace));
            } else {
                unavailable(findings, policy.previewBeforeRun ?
                    "当前事务或会话环境无法在独立连接中复现" : "执行前预查已关闭");
                done(std::move(findings));
            }
        }, Qt::QueuedConnection);
    }));
}

void QuerySafetyController::measure(const ConnectionProfile& target, const QString& schema,
        const QVector<BoundQueryStatement>& bindings, const QuerySafetyPolicy& policy,
        bool live, Findings findings, const Cancellation& stop, Delivery done, ImpactQueryTrace trace) {
    if (std::none_of(findings.begin(), findings.end(), [](const auto& item) {
        return item.impact.status == ImpactStatus::Pending;
    })) {
        done(std::move(findings));
        return;
    }
    struct Request { bool delivered = false; };
    const auto request = std::make_shared<Request>();
    auto *task = QRunnable::create([this, target, schema, bindings, policy, live, stop,
                                    request, findings, done, trace = std::move(trace)]() mutable {
        ImpactQueryTrace onDispatch;
        if (trace)
            onDispatch = [this, trace](int index, ImpactQueryKind kind, QString command) {
                QMetaObject::invokeMethod(this, [trace, index, kind, command = std::move(command)] {
                    trace(index, kind, command);
                }, Qt::QueuedConnection);
            };
        try {
            measureQueryImpact(target, schema, policy, bindings, findings, stop, onDispatch);
        } catch (const std::exception&) {
            unavailable(findings, "预查失败");
        }
        QMetaObject::invokeMethod(this, [this, live, stop, request, findings = std::move(findings), done]() mutable {
            if (live) liveProbeActive_ = false;
            if (request->delivered || stop->load()) return;
            request->delivered = true;
            done(std::move(findings));
        }, Qt::QueuedConnection);
    });
    if ((live && liveProbeActive_) || !probes_.tryStart(task)) {
        delete task;
        unavailable(findings, "预查连接繁忙");
        done(std::move(findings));
        return;
    }
    if (live) liveProbeActive_ = true;
    QTimer::singleShot(policy.timeoutMs, this, [request, stop, findings = std::move(findings), done]() mutable {
        if (request->delivered || stop->load()) return;
        request->delivered = true;
        stop->store(true);
        unavailable(findings, "预查超过等待时间");
        done(std::move(findings));
    });
}

bool QuerySafetyController::confirm(QWidget* anchor, const ConnectionProfile& target, const QString& schema,
                                    const QuerySafetyPolicy& policy, const Findings& findings) {
    QStringList risks, details, unmeasured;
    for (const auto& item : findings) {
        const auto heading = QString("第 %1 条（第 %2 行） · %3%4")
            .arg(item.index + 1).arg(item.line).arg(item.operation,
                item.target.isEmpty() ? QString{} : " " + item.target);
        const auto risk = queryRiskMessage(item.risk);
        const auto impact = queryImpactMessage(item.impact);
        if (requiresQueryConfirmation(item, policy)) {
            risks << heading;
            if (!risk.isEmpty()) risks << "  " + risk;
            if (policy.previewBeforeRun) risks << "  " + impact;
        } else if (policy.previewBeforeRun && item.impact.status == ImpactStatus::Unavailable) {
            unmeasured << heading + " · " + impact;
        }
    }
    if (risks.isEmpty()) return true;
    details << risks << unmeasured;
    QMessageBox dialog(QMessageBox::Warning, "确认执行危险语句",
        QString("将对 %1 执行可能大范围修改数据的语句。").arg(
            QStringList{target.name, target.database, schema}.filter(QRegularExpression(".+")).join(" / ")),
        QMessageBox::NoButton, anchor);
    dialog.setObjectName("confirmQuerySafety");
    dialog.setTextFormat(Qt::PlainText);
    const auto display = risks.mid(0, 10);
    auto summary = display.join('\n');
    if (display.size() < risks.size()) summary += "\n其余说明见详情。";
    if (policy.previewBeforeRun) summary += "\n预查反映检查时的可见数据，实际影响可能变化。";
    dialog.setInformativeText(summary);
    dialog.setDetailedText(details.join('\n'));
    auto* proceed = dialog.addButton("仍然执行", QMessageBox::AcceptRole);
    proceed->setObjectName("executeRiskySql");
    auto* cancel = dialog.addButton(QMessageBox::Cancel);
    dialog.setDefaultButton(cancel);
    dialog.setEscapeButton(cancel);
    dialog.exec();
    return dialog.clickedButton() == proceed;
}
