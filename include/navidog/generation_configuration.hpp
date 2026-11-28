#pragma once

#include "data_generation.hpp"

namespace navidog {

class LocalStore;

struct GenerationConfiguration {
    GenerationRequest request;
    QJsonObject rules;
};

QJsonObject generationConfigurations(const LocalStore&);
bool saveGenerationConfiguration(LocalStore&, const QString& name,
                                 const GenerationConfiguration&, bool replace = false);
GenerationConfiguration generationConfigurationFromJson(const QJsonObject&);
GenerationConfiguration importGenerationConfiguration(const QString& path);
void exportGenerationConfiguration(const QString& path, const GenerationConfiguration&);

}
