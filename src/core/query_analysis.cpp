#include "navidog/database_sql.hpp"
#include "navidog/query_analysis.hpp"
#include "navidog/sql/compiler/compiler.hpp"
#include "navidog/sql/compiler/conversion.hpp"
#include "navidog/sql/compiler/script_context.hpp"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <algorithm>
#include <chrono>

namespace navidog {
namespace {
sql::compiler::Dialect dialect(DatabaseKind kind) {
  if (auto selected = compilerDialectFor(kind))
    return *selected;
  throw std::invalid_argument("unknown database dialect");
}
QString label(SqlAnalysisState state) {
  switch (state) {
  case SqlAnalysisState::Parsed:
    return "结构已识别";
  case SqlAnalysisState::NotCovered:
    return "本地未能解析";
  case SqlAnalysisState::LimitReached:
    return "超过本地分析上限";
  case SqlAnalysisState::Stopped:
    return "本地分析已停止";
  }
  return {};
}
} // namespace
SqlExecutionAnalysis
analyzeQueryScript(const ConnectionProfile &profile, const QString &schema,
                   std::string_view source,
                   const std::vector<sql::StatementSpan> &statements,
                   const Cancellation &cancel) {
  SqlExecutionAnalysis report;
  report.totalStatements = static_cast<int>(statements.size());
  QElapsedTimer timer;
  timer.start();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  constexpr std::size_t totalBytes = 2 * 1024 * 1024,
                        statementBytes = 1024 * 1024;
  if (source.size() > totalBytes) {
    report.truncated = true;
    return report;
  }
  report.sourceHash =
      QCryptographicHash::hash(QByteArray(source.data(), int(source.size())),
                               QCryptographicHash::Sha256);
  std::size_t astBytes = 0, generatedBytes = 0;
  std::unique_ptr<sql::compiler::ScriptContext> declarations;
  try {
    declarations = std::make_unique<sql::compiler::ScriptContext>(
        dialect(profile.driver), nullptr,
        sql::compiler::NameResolutionContext{profile.database.toStdString(),
                                             schema.toStdString(),
                                             dialect(profile.driver),
                                             {}});
  } catch (const std::exception &) {
    report.truncated = true;
    return report;
  }
  // One source index for global byte locations; no repeated prefix scans.
  sql::compiler::SourceBuffer positions{std::string(source)};
  for (std::size_t i = 0; i < statements.size(); ++i) {
    if (i >= 64 || timer.elapsed() >= 100) {
      report.truncated = true;
      break;
    }
    SqlStatementAnalysis item;
    item.index = static_cast<int>(i);
    const auto span = statements[i];
    if (span.begin > span.end || span.end > source.size()) {
      report.truncated = true;
      break;
    }
    item.sourceBegin = int(span.begin);
    auto pos = positions.position(span.begin);
    item.line = int(pos.line);
    item.byteColumn = int(pos.byteColumn);
    if (cancel && cancel->load()) {
      item.state = SqlAnalysisState::Stopped;
      report.statements << item;
      report.truncated = true;
      break;
    }
    if (span.end - span.begin > statementBytes) {
      item.state = SqlAnalysisState::LimitReached;
      report.statements << item;
      continue;
    }
    try {
      sql::compiler::LanguageOptions options;
      options.extension = compilerExtensionFor(profile.driver);
      options.limits.sourceBytes = statementBytes;
      options.limits.tokens = 40000;
      options.limits.nodes = 20000;
      options.limits.nesting = 64;
      options.limits.diagnostics = 1;
      options.shouldStop = [cancel, deadline] {
        return (cancel && cancel->load()) ||
               std::chrono::steady_clock::now() >= deadline;
      };
      const auto original = sql::statementSource(
          source, span,
          scriptDialect(profile.driver));
      const bool mysqlTrigger=span.compound && isMySqlFamily(profile.driver) &&
          sql::isCreateTrigger(original,sql::Dialect::MySQL);
      const bool oraclePackage=span.compound && usesOracleSql(profile.driver) &&
          sql::isPackageSpecification(original,sql::Dialect::Oracle);
      if (span.compound && profile.driver != DatabaseKind::SQLite && !mysqlTrigger && !oraclePackage) {
        auto routine = sql::compiler::compileRoutine(
            dialect(profile.driver), std::string(original), options,
            declarations.get(),
            {profile.database.toStdString(),
             schema.toStdString(),
             dialect(profile.driver),
             {}});
        item.kind = routine.ast.function ? "FUNCTION" : "PROCEDURE";
        if (routine.ok()) {
          sql::compiler::RoutineDefinition definition;
          definition.procedure = !routine.ast.function;
          definition.minArguments = definition.maxArguments =
              routine.ast.parameters.size();
          definition.resultType =
              routine.ast.result.arrayDimensions
                  ? sql::compiler::ValueType::Unknown
                  : sql::compiler::typeCategory(routine.ast.result.name,
                                                dialect(profile.driver));
          for (auto id : routine.ast.parameters)
            if (routine.ast.symbols[id].initializer.end >
                routine.ast.symbols[id].initializer.begin)
              --definition.minArguments;
          if (auto error = declarations->declareRoutine(
                  routine.ast.name, std::move(definition), routine.ast.origin))
            routine.diagnostics.push_back(*error);
        }
        if (routine.ok()) {
          item.state = SqlAnalysisState::Parsed;
          try {
            const auto json = sql::compiler::dumpRoutineModel(
                routine, 2 * 1024 * 1024 - astBytes, options.shouldStop);
            item.unifiedAst = QJsonDocument::fromJson(
                                  QByteArray(json.data(), int(json.size())))
                                  .object();
            astBytes += json.size();
          } catch (const std::exception &) {
            item.astTruncated = true;
          }
          QStringList routineName;
          for (const auto &part : routine.ast.name)
            routineName << QString::fromStdString(part);
          item.objects << routineName.join('.').left(256);
          for (const auto &node : routine.model.nodes)
            for (const auto &query : node.code.queries) {
              ++item.queries;
              item.expressions += int(query.expressions.size()) - 1;
              for (const auto &table : query.tables)
                if (!table.query && !table.pseudo && !table.name.empty() &&
                    item.objects.size() < 32) {
                  QStringList parts;
                  for (const auto &part : table.name)
                    parts << QString::fromStdString(part);
                  auto name = parts.join('.').left(256);
                  if (!item.objects.contains(name))
                    item.objects << name;
                }
            }
          sql::compiler::ConversionOptions generation;
          generation.language = options;
          generation.generation.maxBytes = 2 * 1024 * 1024 - generatedBytes;
          generation.generation.shouldStop = options.shouldStop;
          generation.names = {profile.database.toStdString(),
                              schema.toStdString(),
                              dialect(profile.driver),
                              {}};
          auto generated = sql::compiler::convertSql(
              dialect(profile.driver), dialect(profile.driver),
              std::string(original), generation);
          if (generated.ok()) {
            item.generatedSql = QString::fromStdString(generated.sql);
            generatedBytes += generated.sql.size();
          } else if (!generated.diagnostics.empty())
            item.generationDetail =
                QString::fromStdString(generated.diagnostics.front().message)
                    .left(512);
        } else {
          item.state = SqlAnalysisState::NotCovered;
          if (!routine.diagnostics.empty()) {
            const auto &diagnostic = routine.diagnostics.front();
            item.detail = QString::fromStdString(diagnostic.message).left(512);
            if (diagnostic.code == sql::compiler::DiagnosticCode::ResourceLimit)
              item.state = SqlAnalysisState::LimitReached;
            if (diagnostic.code == sql::compiler::DiagnosticCode::Stopped) {
              item.state = cancel && cancel->load()
                               ? SqlAnalysisState::Stopped
                               : SqlAnalysisState::LimitReached;
              report.truncated = true;
            }
            const auto position =
                positions.position(span.begin + diagnostic.range.begin);
            item.line = int(position.line);
            item.byteColumn = int(position.byteColumn);
          }
        }
        report.statements << std::move(item);
        continue;
      }
      auto result =
          sql::compiler::compile(dialect(profile.driver), std::string(original),
                                 options, declarations.get(),
                                 {profile.database.toStdString(),
                                  schema.toStdString(),
                                  dialect(profile.driver),
                                  {}});
      if (result->ok())
        for (auto root : result->model.roots)
          if (auto error = declarations->observe(result->model, root))
            result->diagnostics.push_back(*error);
      if (!result->ok())
        for (const auto &token : result->tokens)
          if (token.kind == sql::compiler::TokenKind::Word) {
            const auto word = QString::fromStdString(token.value).toUpper();
            if (QStringList{"SET", "USE", "BEGIN", "START", "COMMIT",
                            "ROLLBACK", "DISCARD", "DO"}
                    .contains(word))
              declarations->invalidate();
            break;
          }
      for (const auto &token : result->tokens)
        if (token.kind == sql::compiler::TokenKind::Word) {
          item.kind = QString::fromStdString(token.value).toUpper();
          break;
        }
      if (result->ok()) {
        item.state = SqlAnalysisState::Parsed;
        item.kind = QString::fromLatin1(sql::compiler::statementKindName(
            result->model[result->model.roots.front() - 1].kind));
        try {
          const auto json =
              sql::compiler::dumpModel(*result, 2 * 1024 * 1024 - astBytes);
          item.unifiedAst =
              QJsonDocument::fromJson(QByteArray(json.data(), int(json.size())))
                  .object();
          astBytes += json.size();
        } catch (const std::exception &) {
          item.astTruncated = true;
        }
        sql::compiler::GenerationOptions generationOptions;
        generationOptions.maxBytes = 2 * 1024 * 1024 - generatedBytes;
        generationOptions.shouldStop = options.shouldStop;
        auto generated = sql::compiler::generateSql(
            result->model, result->dialect, generationOptions);
        if (generated.ok()) {
          generatedBytes += generated.sql.size();
          item.generatedSql = QString::fromUtf8(generated.sql.data(),
                                                int(generated.sql.size()));
        } else if (!generated.diagnostics.empty())
          item.generationDetail =
              QString::fromStdString(generated.diagnostics.front().message)
                  .left(512);
        item.queries = int(result->model.size());
        for (const auto &query : result->model) {
          if (query.ddl && item.objects.size() < 32) {
            QStringList parts;
            for (const auto &name : query.ddl->name)
              parts << QString::fromStdString(name);
            if (!parts.isEmpty() && !item.objects.contains(parts.join('.')))
              item.objects << parts.join('.').left(256);
          }
          item.expressions += int(query.expressions.size()) - 1;
          for (const auto &table : query.tables)
            if (!table.query && !table.pseudo && !table.name.empty()) {
              if (item.objects.size() >= 32)
                break;
              QStringList parts;
              for (const auto &name : table.name)
                parts << QString::fromStdString(name);
              const auto name = parts.join('.');
              if (!item.objects.contains(name))
                item.objects << name.left(256);
              if (item.objects.size() >= 32)
                break;
            }
        }
      } else {
        item.state = SqlAnalysisState::NotCovered;
        if (!result->diagnostics.empty()) {
          const auto &diagnostic = result->diagnostics.front();
          item.detail = QString::fromStdString(diagnostic.message).left(512);
          if (diagnostic.code == sql::compiler::DiagnosticCode::ResourceLimit)
            item.state = SqlAnalysisState::LimitReached;
          if (diagnostic.code == sql::compiler::DiagnosticCode::Stopped) {
            item.state = cancel && cancel->load()
                             ? SqlAnalysisState::Stopped
                             : SqlAnalysisState::LimitReached;
            report.truncated = true;
          }
          pos = positions.position(span.begin + diagnostic.range.begin);
          item.line = int(pos.line);
          item.byteColumn = int(pos.byteColumn);
        }
      }
    } catch (const std::exception &) {
      item.state = SqlAnalysisState::NotCovered;
      item.detail = "本地分析未完成，请以数据库结果为准";
    }
    report.statements << std::move(item);
  }
  report.elapsedMs = timer.elapsed();
  return report;
}
QString queryAnalysisText(const QueryOutcome &outcome) {
  const auto &report = outcome.analysis;
  QString text = QString("本地解析 · %1 / %2 条 · %3 "
                         "ms\n本地解析仅辅助检查，数据库负责最终校验。\n")
                     .arg(report.statements.size())
                     .arg(report.totalStatements)
                     .arg(report.elapsedMs);
  for (const auto &item : report.statements) {
    QString execution;
    if (outcome.analyzedOnly)
      execution = "仅解析 · 未执行";
    else if (outcome.explained)
      execution = !outcome.result.error.isEmpty() || outcome.result.cancelled
                      ? "执行计划未完成"
                      : "执行计划已返回";
    else if (item.index == outcome.failedStatement &&
             item.index < outcome.attemptedStatements)
      execution = outcome.result.cancelled ? "执行已停止" : "执行失败";
    else if (item.index < outcome.attemptedStatements)
      execution = "执行成功";
    else
      execution = "未执行";
    text += QString("\n第 %1 条 · %2 · %3 · %4\n")
                .arg(item.index + 1)
                .arg(item.kind.isEmpty() ? "SQL" : item.kind, label(item.state),
                     execution);
    if (item.state == SqlAnalysisState::Parsed)
      text += QString("%1 个语句 / 子查询节点 · %2 "
                      "个表达式；未进行数据库元数据与完整类型检查。\n")
                  .arg(item.queries)
                  .arg(item.expressions);
    if (!item.objects.isEmpty())
      text += "对象（最多 32 项）：" + item.objects.join("、") + "\n";
    if (!item.detail.isEmpty())
      text += QString("第 %1 行，字节列 %2：%3\n")
                  .arg(item.line)
                  .arg(item.byteColumn)
                  .arg(item.detail);
  }
  if (report.truncated)
    text += "\n部分 SQL "
            "超出本地分析数量、大小或时间上限；数据库执行结果仍以数据和消息页为"
            "准。\n";
  return text;
}
} // namespace navidog
