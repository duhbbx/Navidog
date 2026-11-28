#pragma once

#include "navidog/table_design.hpp"
#include <QObject>
#include <functional>
#include <optional>

class TablePane;
namespace navidog { class QueryService; }

class TableDesignController final : public QObject {
    Q_OBJECT
public:
    using ResolveProfile = std::function<std::optional<navidog::ConnectionProfile>()>;
    using HasDataDraft = std::function<bool(const QString &, const navidog::CatalogNode &)>;

    TableDesignController(TablePane &, navidog::QueryService &, ResolveProfile, HasDataDraft);

signals:
    void activityChanged();
    void operationFinished();
    void statusChanged(const QString &);
    void errorOccurred(const QString &title, const QString &detail);
    void structureSaved(const QString &profileId, const navidog::CatalogNode &);

private:
    enum class Operation { Idle, Read, Save };
    void reload();
    void save();
    void start(Operation, const QString &id, const QString &message);
    void complete(const QString &id, const navidog::TableDesignResult &);

    TablePane &pane_;
    navidog::QueryService &service_;
    ResolveProfile resolveProfile_;
    HasDataDraft hasDataDraft_;
    Operation operation_ = Operation::Idle;
    QString requestId_;
};
