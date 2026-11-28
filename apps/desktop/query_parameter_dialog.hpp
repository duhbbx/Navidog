#pragma once

#include "navidog/query_parameters.hpp"

class QWidget;
namespace navidog::ui {
std::optional<QVector<QVariant>> promptQueryParameters(QWidget*, const QVector<QueryParameter>&);
}
