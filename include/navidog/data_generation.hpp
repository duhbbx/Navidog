#pragma once

#include "table_design.hpp"
#include <QJsonObject>
#include <functional>

namespace navidog {

enum class GenerationSizeMode { Rows, Capacity };

struct GenerationRequest {
    GenerationSizeMode sizeMode = GenerationSizeMode::Rows;
    QString size;
    quint64 seed = 1;
    bool preview = false;
};

inline constexpr qint64 maximumGenerationRows = 1000000;
using GenerationProgress = std::function<void(qint64 done, qint64 total)>;

QJsonObject suggestedGenerationRules(const TableDefinition&, DatabaseKind);
QStringList generationRuleLabels();
QStringList generationRuleKeys();
QString generationRuleExample(const QString& kind, const TableColumn& column);

}
