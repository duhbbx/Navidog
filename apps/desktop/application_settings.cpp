#include "application_settings.hpp"
#include "navidog/local_store.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace {
QByteArray read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65536)
        throw std::runtime_error("无法读取设置或设置超过 64 KiB");
    auto bytes = file.read(65537);
    if (bytes.size() > 65536 || file.error() != QFileDevice::NoError)
        throw std::runtime_error("无法完整读取设置");
    return bytes;
}
} // namespace

ApplicationSettings loadApplicationSettings(const QString &path) {
    ApplicationSettings s;
    const bool sqlite = path.endsWith(".sqlite");
    std::optional<QByteArray> stored;
    if (sqlite) {
        navidog::LocalStore db(path);
        stored = db.document("application-settings");
        if (!stored) {
            auto migrated = loadApplicationSettings(
                QFileInfo(path).dir().filePath("settings.json"));
            saveApplicationSettings(path, migrated);
            return migrated;
        }
    }
    if (!sqlite && !QFileInfo::exists(path)) {
        auto legacy = QFileInfo(path).dir().filePath("sql-format.yaml");
        if (QFileInfo::exists(legacy))
            s.formatting = navidog::sql::compiler::parseFormatStyle(
                read(legacy).toStdString());
        return s;
    }
    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(sqlite ? *stored : read(path), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        throw std::runtime_error("设置格式无效，未覆盖原文件");
    auto root = doc.object();
    if (root.value("version").toInt() != 1)
        throw std::runtime_error("不支持此设置版本，未覆盖原文件");
    auto flag = [&](const QString &key, bool fallback) {
        auto v = root.value(key);
        if (v.isUndefined())
            return fallback;
        if (!v.isBool())
            throw std::runtime_error("设置项必须是布尔值");
        return v.toBool();
    };
    auto number = [&](const QString &key, int fallback, int min, int max) {
        auto v = root.value(key);
        if (v.isUndefined())
            return fallback;
        if (!v.isDouble() || v.toDouble() != v.toInt() || v.toInt() < min ||
            v.toInt() > max)
            throw std::runtime_error("设置项数值超出范围");
        return v.toInt();
    };
    const auto exportDirectory = root.value("exportDirectory");
    if (!exportDirectory.isUndefined() && !exportDirectory.isString())
        throw std::runtime_error("默认保存目录设置无效");
    s.exportDirectory = exportDirectory.toString();
    s.restoreWorkspace = flag("restoreWorkspace", true);
    s.automaticUpdateCheck = flag("automaticUpdateCheck", true);
    s.editorWordWrap = flag("editorWordWrap", false);
    s.resultsBeside = flag("resultsBeside", false);
    s.autoMetadata = flag("autoMetadata", true);
    s.editorCompletion = flag("editorCompletion", true);
    s.completionMinPrefix = number("completionMinPrefix", 1, 1, 4);
    // Migrate the previous two-character default to the requested
    // first-character behavior, while retaining deliberate longer thresholds
    // and future edits.
    if (number("completionBehaviorVersion", 1, 1, 2) < 2 &&
        s.completionMinPrefix == 2)
        s.completionMinPrefix = 1;
    s.editorFontSize = number("editorFontSize", 12, 8, 24);
    s.editorIndentWidth = number("editorIndentWidth", 4, 1, 8);
    s.queryRowLimit = number("queryRowLimit", 1000, 1000, 10000);
    if (s.queryRowLimit != 1000 && s.queryRowLimit != 5000 &&
        s.queryRowLimit != 10000)
        throw std::runtime_error("查询行数设置无效");
    s.optimizerAssumedRows =
        number("optimizerAssumedRows", 10000, 0, 1000000000);
    s.optimizerMemoryMiB = number("optimizerMemoryMiB", 64, 1, 4096);
    s.optimizerTimeMs = number("optimizerTimeMs", 2000, 50, 10000);
    s.optimizerGroups = number("optimizerGroups", 2048, 32, 8192);
    s.optimizerMemoMiB = number("optimizerMemoMiB", 64, 1, 256);
    auto weight = [&](const QString &key, double fallback) {
        const auto v = root.value(key);
        if (v.isUndefined())
            return fallback;
        const auto n = v.toDouble(-1);
        if (!v.isDouble() || !std::isfinite(n) || n < 0 || n > 1e6)
            throw std::runtime_error("优化成本权重超出范围");
        return n;
    };
    s.optimizerCpuWeight = weight("optimizerCpuWeight", 1);
    s.optimizerIoWeight = weight("optimizerIoWeight", 100);
    s.optimizerMemoryWeight = weight("optimizerMemoryWeight", 0);
    if (s.optimizerCpuWeight + s.optimizerIoWeight + s.optimizerMemoryWeight <=
        0)
        throw std::runtime_error("成本权重不能全部为零");
    s.optimizerFilterPushdown = flag("optimizerFilterPushdown", true);
    s.optimizerJoinCommute = flag("optimizerJoinCommute", true);
    s.optimizerJoinAssociate = flag("optimizerJoinAssociate", true);
    s.optimizerHep = flag("optimizerHep", true);
    auto format = root.value("formatting");
    if (!format.isUndefined()) {
        if (!format.isString())
            throw std::runtime_error("格式化设置无效");
        s.formatting = navidog::sql::compiler::parseFormatStyle(
            format.toString().toStdString());
    }
    return s;
}
void saveApplicationSettings(const QString &path,
                             const ApplicationSettings &s) {
    auto format = navidog::sql::compiler::dumpFormatStyle(s.formatting);
    navidog::sql::compiler::parseFormatStyle(format);
    if (s.completionMinPrefix < 1 || s.completionMinPrefix > 4 ||
        s.editorFontSize < 8 || s.editorFontSize > 24 ||
        s.editorIndentWidth < 1 || s.editorIndentWidth > 8 ||
        (s.queryRowLimit != 1000 && s.queryRowLimit != 5000 &&
         s.queryRowLimit != 10000))
        throw std::runtime_error("设置项数值超出范围");
    if (s.optimizerAssumedRows < 0 || s.optimizerAssumedRows > 1000000000 ||
        s.optimizerMemoryMiB < 1 || s.optimizerMemoryMiB > 4096 ||
        s.optimizerTimeMs < 50 || s.optimizerTimeMs > 10000 ||
        s.optimizerGroups < 32 || s.optimizerGroups > 8192 ||
        s.optimizerMemoMiB < 1 || s.optimizerMemoMiB > 256)
        throw std::runtime_error("优化器预算超出范围");
    for (auto v :
         {s.optimizerCpuWeight, s.optimizerIoWeight, s.optimizerMemoryWeight})
        if (!std::isfinite(v) || v < 0 || v > 1e6)
            throw std::runtime_error("优化成本权重超出范围");
    if (s.optimizerCpuWeight + s.optimizerIoWeight + s.optimizerMemoryWeight <=
        0)
        throw std::runtime_error("成本权重不能全部为零");
    QJsonObject root{{"optimizerAssumedRows", s.optimizerAssumedRows},
                     {"optimizerMemoryMiB", s.optimizerMemoryMiB},
                     {"optimizerTimeMs", s.optimizerTimeMs},
                     {"optimizerGroups", s.optimizerGroups},
                     {"optimizerMemoMiB", s.optimizerMemoMiB},
                     {"optimizerCpuWeight", s.optimizerCpuWeight},
                     {"optimizerIoWeight", s.optimizerIoWeight},
                     {"optimizerMemoryWeight", s.optimizerMemoryWeight},
                     {"optimizerFilterPushdown", s.optimizerFilterPushdown},
                     {"optimizerJoinCommute", s.optimizerJoinCommute},
                     {"optimizerJoinAssociate", s.optimizerJoinAssociate},
                     {"optimizerHep", s.optimizerHep},
                     {"version", 1},
                     {"restoreWorkspace", s.restoreWorkspace},
                     {"exportDirectory", s.exportDirectory},
                     {"automaticUpdateCheck", s.automaticUpdateCheck},
                     {"editorWordWrap", s.editorWordWrap},
                     {"resultsBeside", s.resultsBeside},
                     {"autoMetadata", s.autoMetadata},
                     {"editorCompletion", s.editorCompletion},
                     {"completionMinPrefix", s.completionMinPrefix},
                     {"completionBehaviorVersion", 2},
                     {"editorFontSize", s.editorFontSize},
                     {"editorIndentWidth", s.editorIndentWidth},
                     {"queryRowLimit", s.queryRowLimit},
                     {"formatting", QString::fromStdString(format)}};
    auto bytes = QJsonDocument(root).toJson();
    if (path.endsWith(".sqlite")) {
        navidog::LocalStore db(path);
        db.putDocument("application-settings", bytes);
        return;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        throw std::runtime_error(file.errorString().toStdString());
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error(file.errorString().toStdString());
}

navidog::sql::optimizer::PlannerOptions
optimizationOptions(const ApplicationSettings &s) {
    navidog::sql::optimizer::PlannerOptions options;
    options.assumedRows = s.optimizerAssumedRows;
    options.memoryBytes = double(s.optimizerMemoryMiB) * 1048576;
    options.maxTimeMs = s.optimizerTimeMs;
    options.maxGroups = s.optimizerGroups;
    options.maxBytes = std::size_t(s.optimizerMemoMiB) * 1048576;
    options.maxExpressions =
        std::min<std::size_t>(32768, options.maxGroups * 4);
    options.maxRuleMatches =
        std::min<std::size_t>(65536, options.maxGroups * 8);
    options.weights = {s.optimizerCpuWeight, s.optimizerIoWeight,
                       s.optimizerMemoryWeight};
    options.filterPushdown = s.optimizerFilterPushdown;
    options.joinCommutation = s.optimizerJoinCommute;
    options.joinAssociation = s.optimizerJoinAssociate;
    options.heuristic.enabled = s.optimizerHep;
    return options;
}
