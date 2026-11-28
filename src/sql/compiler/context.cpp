#include "parser.hpp"
#include <algorithm>
#include <limits>

namespace navidog::sql::compiler {
bool allowsDerivedColumnAliases(Dialect dialect) {
  return dialect == Dialect::PostgreSQL || dialect == Dialect::MySQL || dialect == Dialect::SQLServer;
}
SourceBuffer::SourceBuffer(std::string text, std::string name)
    : text_(std::move(text)), name_(std::move(name)), lines_{0} {
  for (std::size_t i = 0; i < text_.size(); ++i) {
    if (text_[i] == '\r') {
      if (i + 1 < text_.size() && text_[i + 1] == '\n')
        ++i;
      lines_.push_back(i + 1);
    } else if (text_[i] == '\n')
      lines_.push_back(i + 1);
  }
}
std::string_view SourceBuffer::slice(SourceRange r) const {
  if (r.begin > r.end || r.end > text_.size())
    throw std::out_of_range("source range");
  return std::string_view(text_).substr(r.begin, r.end - r.begin);
}
SourcePosition SourceBuffer::position(std::size_t offset) const {
  offset = std::min(offset, text_.size());
  const auto line =
      std::upper_bound(lines_.begin(), lines_.end(), offset) - lines_.begin();
  return {static_cast<std::size_t>(line), offset - lines_[line - 1] + 1};
}
ASTContext::ASTContext() : identifiers_(1), expressions_(1) {}
IdentifierId ASTContext::intern(std::string value) {
  const auto found = identifierIds_.find(value);
  if (found != identifierIds_.end())
    return found->second;
  const auto id = static_cast<IdentifierId>(identifiers_.size());
  identifiers_.push_back(std::move(value));
  identifierIds_.emplace(identifiers_.back(), id);
  return id;
}
const std::string &ASTContext::identifier(IdentifierId id) const {
  return identifiers_.at(id);
}
ExprId ASTContext::add(Expr node) {
  expressions_.push_back(std::move(node));
  return static_cast<ExprId>(expressions_.size() - 1);
}
Expr &ASTContext::mutableExpression(ExprId id) { return expressions_.at(id); }
const Expr &ASTContext::expression(ExprId id) const {
  return expressions_.at(id);
}
QueryId ASTContext::addQuery(SelectStatement query) {
  queries_.push_back(std::move(query));
  return static_cast<QueryId>(queries_.size());
}
const SelectStatement &ASTContext::query(QueryId id) const {
  return queries_.at(id - 1);
}
Compilation::Compilation(Dialect d, std::string sql, LanguageOptions o,
                         std::string file)
    : dialect(d), options(o), source(std::move(sql), std::move(file)) {
  model.sourceDialect = d;
  model.mysqlNoBackslashEscapes = options.mysqlNoBackslashEscapes;
  model.mysqlPipesAsConcat = options.mysqlPipesAsConcat;
}
void Compilation::report(DiagnosticCode code, SourceRange range,
                         std::string message) {
  // Keep at least one error even if a caller sets diagnostics=0.
  if (diagnostics.size() < std::max<std::size_t>(1, options.limits.diagnostics))
    diagnostics.push_back({code, range, std::move(message)});
}
std::string asciiUpper(std::string_view s) {
  std::string result(s);
  for (char &c : result)
    if (c >= 'a' && c <= 'z')
      c -= 'a' - 'A';
  return result;
}
std::string canonicalIdentifier(Dialect d, std::string value, bool quoted) {
  if (quoted)
    return value;
  if (d == Dialect::Oracle)
    return asciiUpper(value);
  if (d == Dialect::PostgreSQL)
    for (char &c : value)
      if (c >= 'A' && c <= 'Z')
        c += 'a' - 'A';
  return value;
}
const char *transactionEffectName(TransactionEffect effect) {
  switch (effect) {
  case TransactionEffect::PreservesTransaction:
    return "preserves-transaction";
  case TransactionEffect::TransactionalDdl:
    return "transactional-ddl";
  case TransactionEffect::ImplicitCommit:
    return "implicit-commit";
  case TransactionEffect::NonTransactionalWithoutCommit:
    return "nontransactional-without-commit";
  case TransactionEffect::ForbiddenInTransaction:
    return "forbidden-in-transaction";
  case TransactionEffect::ExplicitControl:
    return "explicit-transaction-control";
  case TransactionEffect::Dynamic:
    return "depends-on-runtime-statement";
  }
  return "unknown";
}
const char *objectKindName(ObjectKind k) {
  switch (k) {
  case ObjectKind::Table:
    return "TABLE";
  case ObjectKind::View:
    return "VIEW";
  case ObjectKind::Index:
    return "INDEX";
  case ObjectKind::Schema:
    return "SCHEMA";
  case ObjectKind::Database:
    return "DATABASE";
  case ObjectKind::Sequence:
    return "SEQUENCE";
  case ObjectKind::TableGroup:
    return "TABLEGROUP";
  case ObjectKind::MaterializedView:
    return "MATERIALIZED VIEW";
  case ObjectKind::LoadableFunction:
    return "FUNCTION";
  }
  return "UNKNOWN";
}
const char *distributionOptionName(DistributionOptionKind kind) {
  switch (kind) {
  case DistributionOptionKind::ShardRowIdBits: return "SHARD_ROW_ID_BITS";
  case DistributionOptionKind::PreSplitRegions: return "PRE_SPLIT_REGIONS";
  case DistributionOptionKind::AutoRandomBase: return "AUTO_RANDOM_BASE";
  case DistributionOptionKind::PlacementPolicy: return "PLACEMENT POLICY";
  case DistributionOptionKind::TableGroup: return "TABLEGROUP";
  case DistributionOptionKind::Sharding: return "SHARDING";
  }
  return "";
}
const char *statementKindName(StatementKind k) {
  switch (k) {
  case StatementKind::Vacuum:
    return "VACUUM";
  case StatementKind::Trigger:
    return "TRIGGER";
  case StatementKind::TriggerSet:
    return "SET";
  case StatementKind::Package:
    return "PACKAGE";
  case StatementKind::Copy:
    return "COPY";
  case StatementKind::Pragma:
    return "PRAGMA";
  case StatementKind::Inspect:
    return "INSPECT";
  case StatementKind::Transaction:
    return "TRANSACTION";
  case StatementKind::Prepared:
    return "PREPARED";
  case StatementKind::Role:
    return "ROLE";
  case StatementKind::Call:
    return "CALL";
  case StatementKind::Merge:
    return "MERGE";
  case StatementKind::Select:
    return "SELECT";
  case StatementKind::Insert:
    return "INSERT";
  case StatementKind::Update:
    return "UPDATE";
  case StatementKind::Delete:
    return "DELETE";
  case StatementKind::Create:
    return "CREATE";
  case StatementKind::Alter:
    return "ALTER";
  case StatementKind::Drop:
    return "DROP";
  case StatementKind::Truncate:
    return "TRUNCATE";
  case StatementKind::Rename:
    return "RENAME";
  case StatementKind::Comment:
    return "COMMENT";
  }
  return "UNKNOWN";
}
const char *copyFormatName(CopyFormat format) {
  switch (format) {
  case CopyFormat::Text: return "text";
  case CopyFormat::Csv: return "csv";
  case CopyFormat::Binary: return "binary";
  }
  return "";
}
const char *copyHeaderName(CopyHeader header) {
  switch (header) {
  case CopyHeader::Disabled: return "FALSE";
  case CopyHeader::Enabled: return "TRUE";
  case CopyHeader::Match: return "MATCH";
  }
  return "";
}
const char *triggerEventName(TriggerEvent event) {
  switch (event) {
  case TriggerEvent::Insert: return "INSERT";
  case TriggerEvent::Update: return "UPDATE";
  case TriggerEvent::Delete: return "DELETE";
  }
  return "";
}
const char *triggerTimingName(TriggerTiming timing) {
  switch (timing) {
  case TriggerTiming::Default: return "";
  case TriggerTiming::Before: return "BEFORE";
  case TriggerTiming::After: return "AFTER";
  case TriggerTiming::InsteadOf: return "INSTEAD OF";
  }
  return "";
}
const char *vacuumOptionName(VacuumOptionKind option) {
  switch (option) {
  case VacuumOptionKind::Full: return "FULL";
  case VacuumOptionKind::Freeze: return "FREEZE";
  case VacuumOptionKind::Verbose: return "VERBOSE";
  case VacuumOptionKind::Analyze: return "ANALYZE";
  case VacuumOptionKind::DisablePageSkipping: return "DISABLE_PAGE_SKIPPING";
  case VacuumOptionKind::SkipLocked: return "SKIP_LOCKED";
  case VacuumOptionKind::IndexCleanup: return "INDEX_CLEANUP";
  case VacuumOptionKind::ProcessToast: return "PROCESS_TOAST";
  case VacuumOptionKind::Truncate: return "TRUNCATE";
  case VacuumOptionKind::Parallel: return "PARALLEL";
  }
  return "";
}
const char *vacuumIndexCleanupName(VacuumIndexCleanup value) {
  switch (value) {
  case VacuumIndexCleanup::Auto: return "AUTO";
  case VacuumIndexCleanup::Enabled: return "ON";
  case VacuumIndexCleanup::Disabled: return "OFF";
  }
  return "";
}
const char *dialectName(Dialect d) {
  switch (d) {
  case Dialect::MySQL:
    return "mysql";
  case Dialect::PostgreSQL:
    return "postgresql";
  case Dialect::Oracle:
    return "oracle";
  case Dialect::SQLite:
    return "sqlite";
  case Dialect::SQLServer:
    return "sqlserver";
  case Dialect::MariaDB:
    return "mariadb";
  case Dialect::OceanBase:
    return "oceanbase";
  case Dialect::TiDB:
    return "tidb";
  }
  return "unknown";
}
std::optional<Dialect> parseDialect(std::string_view name) {
  const auto key = asciiUpper(name);
  // Existing connection profiles use "postgres" as their persistent key.
  if (key == "POSTGRES")
    return Dialect::PostgreSQL;
  for (auto d :
       {Dialect::MySQL, Dialect::PostgreSQL, Dialect::Oracle, Dialect::SQLite,
        Dialect::SQLServer, Dialect::MariaDB, Dialect::OceanBase, Dialect::TiDB})
    if (key == asciiUpper(dialectName(d)))
      return d;
  return {};
}
std::optional<DialectProfile> parseDialectProfile(std::string_view name) {
  const auto key = asciiUpper(name);
  if (key == "DUCKDB") return DialectProfile{Dialect::PostgreSQL, SqlExtension::DuckDb};
  if (key == "CLICKHOUSE") return DialectProfile{Dialect::MySQL, SqlExtension::ClickHouse};
  if (key == "COCKROACHDB") return DialectProfile{Dialect::PostgreSQL, SqlExtension::CockroachDb};
  if (key == "DORIS") return DialectProfile{Dialect::MySQL, SqlExtension::Doris};
  if (key == "STARROCKS") return DialectProfile{Dialect::MySQL, SqlExtension::StarRocks};
  if (key == "YUGABYTEDB") return DialectProfile{Dialect::PostgreSQL, SqlExtension::YugabyteDb};
  if (key == "OPENGAUSS") return DialectProfile{Dialect::PostgreSQL};
  if (const auto dialect = parseDialect(name)) return DialectProfile{*dialect};
  return {};
}
std::unique_ptr<Compilation> compile(Dialect d, std::string sql,
                                     LanguageOptions options,
                                     const CatalogSnapshot *catalog,
                                     NameResolutionContext names,
                                     std::string file) {
  // SourceBuffer indexing must also be bounded before its allocation.
  if (sql.size() > options.limits.sourceBytes) {
    auto out = std::make_unique<Compilation>(d, "", options, std::move(file));
    out->report(DiagnosticCode::ResourceLimit, {},
                "source byte limit exceeded");
    return out;
  }
  auto out = std::make_unique<Compilation>(d, std::move(sql), options,
                                           std::move(file));
  names.dialect = d;
  out->names = names;
  try {
    checkStop(*out);
    lex(*out);
    std::unique_ptr<Parser> parser;
    if (out->diagnostics.empty()) {
      parser=createParser(*out);
      parser->parse();
    }
    out->syntaxValid = out->diagnostics.empty();
    if (out->syntaxValid && parser && !parser->analysisDiagnostics().empty()) {
      // A complete grammar can still have invalid named-window relationships.
      // Do not publish a canonical model built from a failed expansion.
      out->diagnostics=parser->analysisDiagnostics();
      return out;
    }
    if (out->syntaxValid) {
      lowerAndBind(*out, catalog, names);
      validateModelSemantics(*out);
      inferSqlTypes(*out);
      const auto exactSignedDomain=[](const SqlType &type,int dimensions) {
        return type.family==TypeFamily::Integer && type.unsignedInteger==false &&
            (type.bits==16 || type.bits==32 || type.bits==64) && type.arrayDimensions==dimensions &&
            type.opaqueName.empty() && type.flavor.empty();
      };
      if (out->diagnostics.empty() && d==Dialect::PostgreSQL)
        for (const auto &query:out->model) {
          checkStop(*out);
          for (const auto &table:query.tables) {
            if (table.postgresFunction!=PostgresRowFunction::Unnest) continue;
            const auto &source=out->model.at(table.query-1);
            const auto &call=source.expressions.at(source.projections.front().expression);
            const auto &argument=source.expressions.at(call.operands.front());
            const auto &type=out->model.types.get(argument.sqlType);
            if ((type.family!=TypeFamily::Unknown && !exactSignedDomain(type,1)) ||
                !type.opaqueName.empty() ||
                (call.routineResolved && (call.routineSchema!="pg_catalog" ||
                 call.routineKind!=RoutineKind::TableFunction)))
              out->report(DiagnosticCode::UnsupportedSyntax,argument.origin,
                          "native UNNEST relation supports one signed integer array");
          }
          if (!out->options.calciteExtensions) continue;
          for (const auto &call:query.expressions) {
            if (call.kind!=ExprKind::Call || call.value!="withinDistinct") continue;
            const auto &row=query.expressions.at(call.operands.front());
            for (const auto key:row.operands) {
              const auto &expression=query.expressions.at(key);
              const auto &type=out->model.types.get(expression.sqlType);
              if (!exactSignedDomain(type,0))
                out->report(DiagnosticCode::UnsupportedSyntax,expression.origin,
                            "WITHIN DISTINCT requires verified signed integer keys");
            }
          }
        }
    }
  } catch (const CompilationAborted &) {
    out->syntaxValid = false;
    out->model.queries.clear();
    out->model.roots.clear();
  }
  return out;
}
void checkStop(Compilation &c) {
  if (c.options.shouldStop && c.options.shouldStop()) {
    c.report(DiagnosticCode::Stopped, {}, "local analysis stopped");
    throw CompilationAborted{};
  }
}
} // namespace navidog::sql::compiler
