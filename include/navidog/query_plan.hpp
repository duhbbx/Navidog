#pragma once
#include "database.hpp"
namespace navidog {
// Estimated plan in an isolated session; never runs the submitted DML.
QueryResult readQueryPlan(const ConnectionProfile&,const QString&,QueryLimits,const Cancellation&,const std::function<void()>& connected={},const QString& schema={},PlanRequestTrace* trace=nullptr,const BoundQueryStatement* binding=nullptr);
}
