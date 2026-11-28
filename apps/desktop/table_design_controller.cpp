#include "table_design_controller.hpp"
#include "table_pane.hpp"
#include "navidog/query_service.hpp"

#include <QMessageBox>
#include <QUuid>
#include <utility>

TableDesignController::TableDesignController(TablePane &pane, navidog::QueryService &service,
                                             ResolveProfile resolveProfile,
                                             HasDataDraft hasDataDraft)
    : QObject(&pane), pane_(pane), service_(service),
      resolveProfile_(std::move(resolveProfile)), hasDataDraft_(std::move(hasDataDraft)) {
    connect(&pane_, &TablePane::reloadRequested, this, &TableDesignController::reload);
    connect(&pane_, &TablePane::saveRequested, this, &TableDesignController::save);
    connect(&pane_, &TablePane::stopRequested, this, [this] {
        if (operation_ != Operation::Idle)
            service_.cancel();
    });
    connect(&service_, &navidog::QueryService::tableDesignCompleted,
            this, &TableDesignController::complete);
}

void TableDesignController::reload() {
    if (pane_.property("closingWorkspace").toBool() || operation_ != Operation::Idle)
        return;
    if (service_.busy()) {
        emit statusChanged("请等待当前操作完成后重新读取");
        return;
    }
    if (pane_.isLoaded() && pane_.dirty() &&
        QMessageBox::question(pane_.window(), "重新读取表结构",
                              "丢弃尚未保存的修改并重新读取？",
                              QMessageBox::Discard | QMessageBox::Cancel,
                              QMessageBox::Cancel) != QMessageBox::Discard)
        return;

    const auto profile = resolveProfile_();
    const auto id = QUuid::createUuid().toString(QUuid::Id128);
    if (!profile || !service_.tableDefinition(*profile, pane_.tableNode(), id))
        return;
    pane_.setConnectionProfile(*profile);
    start(Operation::Read, id, "正在读取表结构…");
}

void TableDesignController::save() {
    if (service_.busy() || operation_ != Operation::Idle || !pane_.canSave() ||
        pane_.property("closingWorkspace").toBool())
        return;
    pane_.commitEditor();
    if (hasDataDraft_(pane_.profileId, pane_.tableNode())) {
        QMessageBox::information(pane_.window(), "未提交的数据",
                                 "请先提交或放弃此表数据页中的修改，再保存表结构。");
        return;
    }
    const auto profile = resolveProfile_();
    const auto id = QUuid::createUuid().toString(QUuid::Id128);
    if (!profile || !service_.saveTable(*profile, pane_.originalDefinition(), pane_.definition(), id))
        return;
    pane_.setConnectionProfile(*profile);
    start(Operation::Save, id, "正在保存表结构…");
}

void TableDesignController::start(Operation operation, const QString &id, const QString &message) {
    operation_ = operation;
    requestId_ = id;
    pane_.setDesignInProgress(true, message);
    emit activityChanged();
    emit statusChanged(message);
}

void TableDesignController::complete(const QString &id, const navidog::TableDesignResult &result) {
    if (operation_ == Operation::Idle || requestId_ != id)
        return;
    const bool saved = operation_ == Operation::Save;
    operation_ = Operation::Idle;
    requestId_.clear();
    pane_.setDesignInProgress(false);
    emit activityChanged();
    const auto detail = pane_.applyDesignResult(result, saved);
    if (result.cancelled || !result.error.isEmpty()) {
        emit statusChanged(result.cancelled ? "表操作已停止" : "表操作失败");
        if (!result.cancelled)
            emit errorOccurred("表操作失败", detail);
    } else {
        emit statusChanged(detail);
        if (saved)
            emit structureSaved(pane_.profileId, pane_.tableNode());
    }
    emit operationFinished();
}
