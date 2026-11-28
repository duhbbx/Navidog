#include "navidog/sql/compiler/compiler.hpp"
#include "navidog/sql/compiler/conversion.hpp"
#include "navidog/sql/compiler/formatter.hpp"
#include "navidog/sql/compiler/script_context.hpp"
#include "navidog/sql/optimizer/optimizer.hpp"
#include "navidog/sql/optimizer/interpreter.hpp"
#include "navidog/sql/optimizer/materialized_view.hpp"
#include "navidog/sql/optimizer/plan_sql.hpp"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <iostream>

using namespace navidog::sql::compiler;
namespace {
int rewriteUsingDeclarations(const std::string &path, std::string query,
                             const LanguageOptions &options, bool allowPartial) {
  using namespace navidog::sql::optimizer;
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    std::cerr << "Cannot open materialization declarations\n";
    return 2;
  }
  std::string bytes;
  char chunk[8192];
  while (input.read(chunk, sizeof(chunk)) || input.gcount()) {
    bytes.append(chunk, static_cast<std::size_t>(input.gcount()));
    if (bytes.size() > options.limits.sourceBytes) {
      std::cerr << "Materialization declarations exceed the source byte limit\n";
      return 1;
    }
  }
  if (input.bad()) {
    std::cerr << "Failed to read materialization declarations\n";
    return 2;
  }
  const NameResolutionContext names{"offline", "public", Dialect::PostgreSQL};
  auto syntax = compile(names.dialect, bytes, options, nullptr, names);
  const auto report = [](const auto &entries) {
    for (const auto &error : entries) std::cerr << error.message << '\n';
    return 1;
  };
  if (!syntax->syntaxValid)
    return report(syntax->diagnostics);

  ScriptContext catalog(names.dialect, nullptr, names);
  std::vector<std::unique_ptr<Compilation>> definitions;
  for (const auto &statement : syntax->statements) {
    auto unit = compile(names.dialect, std::string(syntax->source.slice(statement.range)), options, &catalog, names);
    if (!unit->ok())
      return report(unit->diagnostics);
    const auto &root = unit->model.front();
    if (root.kind != StatementKind::Create || !root.ddl ||
        (root.ddl->object != ObjectKind::Table && root.ddl->object != ObjectKind::MaterializedView) ||
        (root.ddl->object == ObjectKind::Table &&
         (root.source || root.ddl->definitions.empty() || root.ddl->temporary))) {
      std::cerr << "Declarations require explicit CREATE TABLE and CREATE MATERIALIZED VIEW definitions\n";
      return 1;
    }
    if (auto error = catalog.observe(unit->model, root.id))
      return report(std::vector<Diagnostic>{*error});
    if (root.ddl->object == ObjectKind::MaterializedView) {
      if (definitions.size()>=32) {
        std::cerr << "At most 32 materialized views can be registered per invocation\n";
        return 1;
      }
      definitions.push_back(std::move(unit));
    }
  }
  if (definitions.empty()) {
    std::cerr << "A materialized view definition is required\n";
    return 1;
  }
  std::vector<TableDefinition> storage;
  std::vector<RegisteredMaterialization> registered;
  storage.reserve(definitions.size());
  for (const auto &definition:definitions) {
    auto view=catalog.findTable(names,definition->model.front().ddl->name);
    if (!view) {
      std::cerr << "A declared materialized view output schema is unavailable\n";
      return 1;
    }
    if (std::any_of(storage.begin(),storage.end(),[&](const auto &existing) {
          return existing.database==view->database && existing.schema==view->schema && existing.name==view->name;
        })) {
      std::cerr << "Materialized view declarations must have distinct storage identities\n";
      return 1;
    }
    storage.push_back(std::move(*view));
    registered.push_back({&definition->model,&storage.back(),MaterializationSnapshot::Equivalent});
  }
  auto parsedQuery = compile(names.dialect, std::move(query), options, &catalog, names);
  if (!parsedQuery->ok()) return report(parsedQuery->diagnostics);
  GenerationOptions generation;
  generation.shouldStop=options.shouldStop;
  const auto rewrites=rewriteMaterializedViews(parsedQuery->model,registered,std::move(generation),
      allowPartial ? MaterializationCoverage::AllowPartial : MaterializationCoverage::FullOnly);
  if (rewrites.candidates.empty()) return report(rewrites.diagnostics);
  std::cout << rewrites.candidates.front().sql << '\n';
  return 0;
}
} // namespace

int main(int argc, char **argv) {
  std::optional<Dialect> dialect;
  SqlExtension extension = SqlExtension::None;
  bool ast = false, format = false, dumpStyle = false, optimize = false, evaluate = false;
  std::string stylePath, preset = "Readable";
  std::optional<Dialect> emitDialect, convertDialect;
  std::string materializationPath;
  bool verifiedMaterialization = false, allowPartialMaterialization = false;
  bool ddlReview = false, emitPlan = false;
  bool calciteExtensions = false;
  bool mysqlIgnoreSpace = false;
  navidog::sql::optimizer::LocalRow bindings;
  navidog::sql::optimizer::LocalExchangeSpec exchange;
  bool exchangeCountSpecified=false;
  std::string path;
  auto usage = [] {
    std::cerr << "Usage: navidog-sql --dialect "
                 "mysql|postgresql|oracle|sqlite|sqlserver|mariadb|oceanbase|tidb|"
                 "duckdb|clickhouse|cockroachdb|doris|starrocks|yugabytedb|opengauss "
                 "[--ast|--model|--emit source-dialect|--convert "
                 "target-dialect|--optimize|--emit-plan|--evaluate [--bind number|true|false|null]...|--format] [--style file] "
                 "[--preset Readable|Compact|Expanded] [--dump-format-config] [--calcite-extensions] [--mysql-ignore-space] "
                 "[file|-]\n"
                 "       navidog-sql --dialect postgresql --evaluate --exchange-key column "
                 "[--exchange-key column]... [--exchange-partitions 4] [file|-]\n"
                 "       navidog-sql --dialect postgresql --rewrite-materialized-view declarations.sql "
                 "--verified-materialized-snapshot [--allow-partial-materialization] [query.sql|-]\n";
  };
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help") {
      usage();
      return 0;
    }
    if (arg == "--dialect" && i + 1 < argc) {
      const auto profile = parseDialectProfile(argv[++i]);
      if (!profile) {
        usage();
        return 2;
      }
      dialect = profile->dialect;
      extension = profile->extension;
    } else if (arg == "--emit" && i + 1 < argc) {
      emitDialect = parseDialect(argv[++i]);
      if (!emitDialect) {
        usage();
        return 2;
      }
    } else if (arg == "--convert" && i + 1 < argc) {
      convertDialect = parseDialect(argv[++i]);
      if (!convertDialect) {
        usage();
        return 2;
      }
    } else if (arg == "--allow-ddl-transaction-change")
      ddlReview = true;
    else if (arg == "--calcite-extensions")
      calciteExtensions = true;
    else if (arg == "--mysql-ignore-space")
      mysqlIgnoreSpace = true;
    else if (arg == "--optimize")
      optimize = true;
    else if (arg == "--evaluate")
      evaluate = true;
    else if (arg == "--emit-plan")
      emitPlan = true;
    else if ((arg=="--exchange-key" || arg=="--exchange-partitions") && i+1<argc) {
      const std::string_view input=argv[++i];
      std::size_t number=0;
      const auto parsed=std::from_chars(input.data(),input.data()+input.size(),number);
      if (parsed.ec!=std::errc{} || parsed.ptr!=input.data()+input.size() || !number) {
        std::cerr << "Exchange keys and partition count need positive integers\n";
        return 2;
      }
      if (arg=="--exchange-key")
        exchange.keys.push_back(number-1);
      else {
        exchange.partitions=number;
        exchangeCountSpecified=true;
      }
    }
    else if (arg == "--bind" && i + 1 < argc) {
      auto value=navidog::sql::optimizer::parseLocalBinding(argv[++i]);
      if (!value || bindings.size()>=1024) {
        std::cerr << "--bind needs an integer, exact decimal, true, false or null with at most 1024 arguments\n";
        return 2;
      }
      bindings.push_back(std::move(*value));
    } else if (arg == "--rewrite-materialized-view" && i + 1 < argc)
      materializationPath = argv[++i];
    else if (arg == "--verified-materialized-snapshot")
      verifiedMaterialization = true;
    else if (arg == "--allow-partial-materialization")
      allowPartialMaterialization = true;
    else if (arg == "--format")
      format = true;
    else if (arg == "--dump-format-config")
      dumpStyle = true;
    else if (arg == "--style" && i + 1 < argc)
      stylePath = argv[++i];
    else if (arg == "--preset" && i + 1 < argc)
      preset = argv[++i];
    else if (arg == "--ast")
      ast = true;
    else if (arg == "--model")
      ast = false;
    else if (path.empty() && (arg == "-" || arg.empty() || arg[0] != '-'))
      path = arg;
    else {
      usage();
      return 2;
    }
  }
  if (!dialect) {
    usage();
    return 2;
  }
  if ((!materializationPath.empty() && (!verifiedMaterialization || *dialect != Dialect::PostgreSQL ||
       ast || format || dumpStyle || optimize || evaluate || emitPlan || emitDialect || convertDialect)) ||
      (materializationPath.empty() && (verifiedMaterialization || allowPartialMaterialization))) {
    std::cerr << "Materialized view rewrite needs an independent PostgreSQL invocation and verified snapshot\n";
    return 2;
  }
  if (evaluate && (ast || format || emitDialect || convertDialect || optimize || dumpStyle)) {
    std::cerr << "--evaluate requires a separate invocation\n";
    return 2;
  }
  if (emitPlan && (*dialect!=Dialect::PostgreSQL || ast || format || dumpStyle ||
                   optimize || evaluate || emitDialect || convertDialect)) {
    std::cerr << "--emit-plan requires a separate PostgreSQL invocation\n";
    return 2;
  }
  if (!bindings.empty() && (!evaluate || *dialect!=Dialect::PostgreSQL)) {
    std::cerr << "--bind requires --evaluate with the PostgreSQL dialect\n";
    return 2;
  }
  if ((!exchange.keys.empty() || exchangeCountSpecified) &&
      (!evaluate || exchange.keys.empty() || !bindings.empty() || *dialect!=Dialect::PostgreSQL)) {
    std::cerr << "Hash exchange needs one or more output positions and PostgreSQL --evaluate without --bind\n";
    return 2;
  }
  FormatStyle style;
  try {
    style = formatPreset(preset);
    if (!stylePath.empty()) {
      std::ifstream config(stylePath, std::ios::binary);
      if (!config)
        throw std::runtime_error("Cannot open format style");
      std::string bytes;
      char buffer[4096];
      while (config.read(buffer, sizeof(buffer)) || config.gcount()) {
        bytes.append(buffer, static_cast<std::size_t>(config.gcount()));
        if (bytes.size() > 65536)
          throw std::runtime_error("Format style exceeds 64 KiB");
      }
      style = parseFormatStyle(bytes, style);
    }
    if (dumpStyle) {
      std::cout << dumpFormatStyle(style);
      return 0;
    }
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 2;
  }
  std::ifstream file;
  std::istream *input = &std::cin;
  if (!path.empty() && path != "-") {
    file.open(path, std::ios::binary);
    if (!file) {
      std::cerr << "Cannot open input file\n";
      return 2;
    }
    input = &file;
  }
  LanguageOptions options;
  options.extension = extension;
  options.calciteExtensions=calciteExtensions;
  options.mysqlIgnoreSpace=mysqlIgnoreSpace;
  std::string source;
  char chunk[8192];
  while (input->read(chunk, sizeof(chunk)) || input->gcount()) {
    source.append(chunk, static_cast<std::size_t>(input->gcount()));
    if (source.size() > options.limits.sourceBytes) {
      std::cerr << "Source byte limit exceeded\n";
      return 1;
    }
  }
  if (input->bad()) {
    std::cerr << "Failed to read input\n";
    return 2;
  }
  if (!materializationPath.empty())
    return rewriteUsingDeclarations(materializationPath, std::move(source), options, allowPartialMaterialization);
  if (convertDialect) {
    if (format || emitDialect || optimize) {
      std::cerr << "conversion cannot be combined with formatting/emission\n";
      return 2;
    }
    ConversionOptions conversion;
    conversion.language = options;
    conversion.allowDdlTransactionChange = ddlReview;
    auto result = convertSql(*dialect, *convertDialect, source, conversion);
    if (result.ok())
      std::cout << (ast ? result.modelJson : result.sql) << '\n';
    SourceBuffer original(source, path.empty() ? "<stdin>" : path);
    for (const auto &d : result.diagnostics) {
      auto pos = original.position(d.range.begin);
      std::cerr << pos.line << ':' << pos.byteColumn << ": " << d.message
                << '\n';
    }
    return result.ok() ? 0 : 1;
  }
  if (format) {
    if (emitDialect || ast || optimize) {
      std::cerr << "--format cannot be combined with --ast or --emit\n";
      return 2;
    }
    FormatOptions formatting;
    formatting.language = options;
    auto formatted = formatSql(*dialect, source, style, formatting);
    if (!formatted.ok()) {
      for (const auto &error : formatted.diagnostics)
        std::cerr << error.message << '\n';
      return 1;
    }
    std::cout << formatted.sql;
    return 0;
  }
  auto result = compile(*dialect, std::move(source), options, nullptr, {},
                        path.empty() ? "<stdin>" : path);
  const auto reportDiagnostics = [&](const auto &diagnostics) {
    for (const auto &diagnostic : diagnostics) {
      auto pos = result->source.position(diagnostic.range.begin);
      std::cerr << result->source.name() << ':' << pos.line << ':'
                << pos.byteColumn << ": " << diagnostic.message << '\n';
    }
  };
  if (evaluate) {
    if (!result->ok()) {
      reportDiagnostics(result->diagnostics);
      return 1;
    }
    using namespace navidog::sql::optimizer;
    Interpretation rows;
    if (!exchange.keys.empty()) rows=interpretPartitioned(result->model,exchange);
    else if (bindings.empty()) rows=interpret(result->model);
    else {
      const auto prepared=prepareLocalQuery(result->model);
      if (!prepared.ok()) {
        reportDiagnostics(prepared.diagnostics);
        return 1;
      }
      rows=executeLocalQuery(*prepared.query,bindings);
    }
    if (rows.ok()) std::cout << navidog::sql::optimizer::dumpInterpretation(rows) << '\n';
    else reportDiagnostics(rows.diagnostics);
    return rows.ok() ? 0 : 1;
  }
  if (emitPlan) {
    if (!result->ok()) {
      reportDiagnostics(result->diagnostics);
      return 1;
    }
    using namespace navidog::sql::optimizer;
    const auto plan=navidog::sql::optimizer::optimize(result->model);
    if (!plan.ok()) {
      reportDiagnostics(plan.diagnostics);
      return 1;
    }
    const auto root=plan.heuristicRoot ? plan.heuristicRoot : plan.logical.root;
    const auto generated=generatePlanSql(plan.logical,root);
    if (!generated.ok()) {
      reportDiagnostics(generated.diagnostics);
      return 1;
    }
    std::cout << generated.sql << '\n';
    return 0;
  }
  if (optimize && result->ok()) {
    if (emitDialect || ast) {
      std::cerr << "--optimize cannot be combined with --ast/--emit\n";
      return 2;
    }
    auto plan = navidog::sql::optimizer::optimize(result->model);
    std::cout << navidog::sql::optimizer::dumpOptimization(plan) << '\n';
    return plan.ok() ? 0 : 1;
  }
  if (emitDialect && result->ok()) {
    auto generated = generateSql(result->model, *emitDialect);
    if (generated.ok())
      std::cout << generated.sql << '\n';
    else {
      for (const auto &d : generated.diagnostics)
        std::cerr << d.message << '\n';
      return 1;
    }
  } else if (!emitDialect)
    std::cout << (ast ? dumpAST(*result) : dumpModel(*result)) << '\n';
  reportDiagnostics(result->diagnostics);
  return result->ok() ? 0 : 1;
}
