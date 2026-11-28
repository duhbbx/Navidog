#pragma once

#include "navidog/query_safety.hpp"
#include <QObject>
#include <QThreadPool>
#include <functional>

class QWidget;

class QuerySafetyController final : public QObject {
public:
    using Findings = QVector<navidog::QuerySafetyFinding>;
    using Delivery = std::function<void(Findings)>;

    explicit QuerySafetyController(QObject* parent);
    ~QuerySafetyController() override;

    void assess(const navidog::ConnectionProfile&, const QString& schema, const QString& sql,
                const QVector<navidog::BoundQueryStatement>&, const navidog::QuerySafetyPolicy&,
                bool measure, bool live, const navidog::Cancellation&, Delivery,
                navidog::ImpactQueryTrace = {});

    static bool confirm(QWidget* anchor, const navidog::ConnectionProfile&, const QString& schema,
                        const navidog::QuerySafetyPolicy&, const Findings&);

private:
    void measure(const navidog::ConnectionProfile&, const QString& schema,
                 const QVector<navidog::BoundQueryStatement>&, const navidog::QuerySafetyPolicy&,
                 bool live, Findings, const navidog::Cancellation&, Delivery, navidog::ImpactQueryTrace);
    QThreadPool analysis_, probes_;
    bool liveProbeActive_ = false;
};
