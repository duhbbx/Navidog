#include "navidog/generation_configuration.hpp"
#include "navidog/local_store.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <stdexcept>

namespace navidog {
namespace {
constexpr auto documentKey = "data-generation-configurations";
constexpr auto format = "navidog-data-generation";
constexpr qint64 maxBytes = 1024 * 1024;

QJsonObject parseObject(const QByteArray& bytes) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        throw std::runtime_error("无法读取数据生成配置的 JSON");
    return document.object();
}

QJsonObject configurationJson(const GenerationConfiguration& configuration) {
    const auto& request = configuration.request;
    return {{"format", format}, {"version", 1},
            {"sizeMode", request.sizeMode == GenerationSizeMode::Capacity ? "capacity" : "rows"},
            {"size", request.size.trimmed()}, {"seed", QString::number(request.seed)},
            {"rules", configuration.rules}};
}

QByteArray encode(const GenerationConfiguration& configuration, QJsonDocument::JsonFormat style) {
    const auto checked = generationConfigurationFromJson(configurationJson(configuration));
    const auto bytes = QJsonDocument(configurationJson(checked)).toJson(style);
    if (bytes.size() > maxBytes) throw std::runtime_error("数据生成配置超过 1 MiB");
    return bytes;
}
}

GenerationConfiguration generationConfigurationFromJson(const QJsonObject& json) {
    if (json.value("format") != format || json.value("version") != 1)
        throw std::runtime_error("此文件不是受支持的 Navidog 数据生成配置");
    const auto mode = json.value("sizeMode").toString();
    const auto size = json.value("size").toString().trimmed();
    const auto seed = json.value("seed").toString();
    static const QRegularExpression digits("^[0-9]{1,20}$");
    bool seedValid = false;
    const auto seedValue = seed.toULongLong(&seedValid);
    const auto rulesValue = json.value("rules");
    if ((mode != "rows" && mode != "capacity") || size.isEmpty() || size.size() > 60 ||
        !seedValid || !digits.match(seed).hasMatch() || !rulesValue.isObject())
        throw std::runtime_error("数据生成配置的规模、随机种子或字段规则格式无效");
    GenerationConfiguration result;
    result.request = {mode == "capacity" ? GenerationSizeMode::Capacity : GenerationSizeMode::Rows,
                      size, seedValue, false};
    const auto keys = generationRuleKeys();
    const auto rules = rulesValue.toObject();
    for (auto it = rules.constBegin(); it != rules.constEnd(); ++it) {
        const auto rule = it.value().toObject();
        const auto kind = rule.value("kind");
        const auto value = rule.value("value");
        const auto distribution = rule.value("distribution");
        if (it.key().isEmpty() || !keys.contains(kind.toString()) ||
            !value.isString() || !distribution.isString())
            throw std::runtime_error(("数据生成配置包含无效的字段规则：" + it.key()).toStdString());
        result.rules.insert(it.key(), QJsonObject{{"kind", kind}, {"value", value},
                                                  {"distribution", distribution}});
    }
    return result;
}

QJsonObject generationConfigurations(const LocalStore& store) {
    const auto bytes = store.document(documentKey);
    return bytes ? parseObject(*bytes) : QJsonObject{};
}

bool saveGenerationConfiguration(LocalStore& store, const QString& name,
                                 const GenerationConfiguration& configuration, bool replace) {
    if (name.trimmed().isEmpty() || name.size() > 120)
        throw std::runtime_error("配置名称须为 1 至 120 个字符");
    const auto bytes = encode(configuration, QJsonDocument::Compact);
    bool saved = false;
    store.transaction([&] {
        auto entries = generationConfigurations(store);
        if (entries.contains(name) && !replace) return;
        entries.insert(name, QJsonDocument::fromJson(bytes).object());
        store.putDocument(documentKey, QJsonDocument(entries).toJson(QJsonDocument::Compact));
        saved = true;
    });
    return saved;
}

GenerationConfiguration importGenerationConfiguration(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error(file.errorString().toStdString());
    const auto bytes = file.read(maxBytes + 1);
    if (file.error() != QFileDevice::NoError)
        throw std::runtime_error(file.errorString().toStdString());
    if (bytes.size() > maxBytes) throw std::runtime_error("数据生成配置超过 1 MiB");
    return generationConfigurationFromJson(parseObject(bytes));
}

void exportGenerationConfiguration(const QString& path, const GenerationConfiguration& configuration) {
    const auto bytes = encode(configuration, QJsonDocument::Indented);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error(file.errorString().toStdString());
}
}
