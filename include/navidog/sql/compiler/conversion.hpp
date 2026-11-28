#pragma once
#include "compiler.hpp"
#include <array>
#include <variant>

namespace navidog::sql::compiler {
using ProceduralScopeId = std::uint32_t;
using ProceduralSymbolId = std::uint32_t;
using ProceduralCursorId = std::uint32_t;
enum class SymbolKind { Parameter, Variable, Implicit, LoopIndex, ExceptionStatus };
enum class ProceduralKind {
  Block,
  Declare,
  Assign,
  If,
  While,
  Return,
  Sql,
  Null,
  SetNoCountOn,
  SetNoCountOff,
  Case,
  CaseWhen,
  CaseOperand,
  Repeat,
  Loop,
  Exit,
  Continue,
  NumericFor,
  RangeBound,
  ExceptionHandler,
  Rethrow,
  GetDiagnostics,
  TsqlTryCatch,
  TsqlThrow,
  MySqlCondition,
  MySqlHandler,
  MySqlSessionAssignment,
  CursorDeclare,
  CursorOpen,
  CursorFetch,
  CursorClose,
  MySqlSignal,
  MySqlResignal,
  MySqlSignalValue,
  MySqlGetDiagnostics
};
struct ProceduralScope {
  ProceduralScopeId id = 0, parent = 0;
  SourceRange origin;
  std::vector<ProceduralSymbolId> symbols;
  std::string label;
  std::vector<ProceduralCursorId> cursors;
};
struct ProceduralCursor {
  ProceduralCursorId id = 0;
  ProceduralScopeId scope = 0;
  std::string name;
  SourceRange origin;
};
struct CursorUse {
  ProceduralCursorId cursor = 0;
  SourceRange origin;
  std::vector<ProceduralSymbolId> targets;
};
struct ProceduralSymbol {
  ProceduralSymbolId id = 0;
  ProceduralScopeId scope = 0;
  SymbolKind kind = SymbolKind::Variable;
  ParameterMode mode = ParameterMode::In;
  std::string name;
  BasicTypeSpec<std::vector<std::string>> type;
  SourceRange origin, initializer;
};
enum class ExceptionConditionKind { Named, SqlState, Others, MySqlErrorCode, SqlWarning, NotFound, SqlException };
enum class MySqlHandlerMode { Continue, Exit };
enum class MySqlDiagnosticField {
  ClassOrigin, SubclassOrigin, MessageText, ErrorNumber, ConstraintCatalog,
  ConstraintSchema, ConstraintName, CatalogName, SchemaName, TableName, ColumnName, CursorName,
  ReturnedSqlstate, RowNumber, Number, RowCount
};
struct MySqlDiagnosticTarget {
  std::variant<ProceduralSymbolId,std::string> destination;
  MySqlDiagnosticField field;
  SourceRange origin;
};
struct MySqlDiagnosticsRead {
  bool stacked = false;
  std::vector<MySqlDiagnosticTarget> targets;
};
struct ExceptionCondition {
  ExceptionConditionKind kind = ExceptionConditionKind::Named;
  std::vector<std::string> name;
  std::string sqlState;
  SourceRange origin;
  std::uint32_t errorCode = 0;
};
enum class PgDiagnosticArea { Current, Stacked };
enum class PgDiagnosticField {
  RowCount, Context, ReturnedSqlstate, MessageText, ExceptionDetail, ExceptionHint,
  ExceptionContext, ColumnName, ConstraintName, DatatypeName, TableName, SchemaName
};
struct PgDiagnosticTarget {
  ProceduralSymbolId symbol = 0;
  PgDiagnosticField field = PgDiagnosticField::RowCount;
  SourceRange origin;
};
struct PgDiagnosticsRead {
  PgDiagnosticArea area = PgDiagnosticArea::Current;
  std::vector<PgDiagnosticTarget> targets;
};
struct TsqlThrowArgument {
  std::variant<ProceduralSymbolId, std::int64_t, std::string> value;
  bool unicode = false;
  SourceRange origin;
};
// Syntax nodes retain source spans; lowering creates owning expression/query
// IR.
struct ProceduralAstNode {
  ProceduralKind kind = ProceduralKind::Null;
  ProceduralScopeId scope = 0;
  ProceduralSymbolId target = 0;
  SourceRange origin, expression;
  std::vector<std::uint32_t> children, otherwise;
  // A CASE node owns ordered WHEN branches. A simple branch owns scalar
  // operands; a searched branch has one condition in expression.
  std::vector<std::uint32_t> caseOperands;
  bool hasElse = false;
  std::uint32_t loop = 0; // Loop identity or resolved EXIT target.
  std::string loopLabel;  // Source label when explicitly declared or referenced.
  ProceduralScopeId blockTarget = 0; // Target of a MySQL LEAVE from BEGIN ... END.
  std::optional<SelectIntoAssignment> selectInto;
  std::vector<std::uint32_t> rangeBounds;
  bool reverseFor = false;
  std::vector<std::uint32_t> handlers;
  std::vector<ExceptionCondition> conditions;
  std::optional<MySqlHandlerMode> mysqlHandlerMode;
  std::optional<MySqlDiagnosticField> mysqlSignalField;
  std::optional<MySqlDiagnosticsRead> mysqlDiagnostics;
  // Only names from the corresponding vendor grammar are stored here.
  std::string conditionName, sessionVariable;
  std::optional<PgDiagnosticsRead> pgDiagnostics;
  std::optional<std::array<TsqlThrowArgument,3>> throwArguments;
  std::optional<CursorUse> cursor;
  bool discardRows = false; // PL/pgSQL PERFORM executes a query without returning its rows.
};
enum class RoutineDefinitionAction { Create, CreateOrReplace, CreateOrAlter, Alter };
enum class RoutineDataAccess { Unspecified, ContainsSql, NoSql, ReadsSqlData, ModifiesSqlData };
struct RoutineAst {
  Dialect dialect = Dialect::PostgreSQL;
  bool function = false;
  RoutineDefinitionAction action = RoutineDefinitionAction::Create;
  RoutineDataAccess dataAccess = RoutineDataAccess::Unspecified;
  bool securityDefiner = false, deterministic = false, explicitLanguageSql = false;
  bool ifNotExists = false;
  std::optional<std::string> comment;
  std::optional<TriggerDefiner> definer;
  std::string volatility = "VOLATILE";
  std::vector<std::string> name;
  BasicTypeSpec<std::vector<std::string>> result;
  std::vector<ProceduralScope> scopes; // ID 0 reserved.
  std::vector<ProceduralSymbol> symbols;
  std::vector<ProceduralCursor> cursors; // ID 0 reserved, separate from scalar names.
  std::vector<ProceduralAstNode> nodes;
  std::vector<ProceduralSymbolId> parameters;
  std::uint32_t body = 0;
  SourceRange origin;
};
// The token stream contains significant tokens, in source order.
bool isRoutineStart(Dialect, const std::vector<Token> &, std::size_t index = 0);
struct ProceduralModelNode {
  ProceduralAstNode syntax;
  ModelProgram code; // Scalar projection or complete embedded SQL statement.
  std::optional<std::size_t> cursorArity; // Known output width on a cursor declaration.
};
struct RoutineModel {
  RoutineAst syntax;
  std::vector<ProceduralModelNode> nodes;
};
struct RoutineCompilation {
  RoutineAst ast;
  RoutineModel model;
  std::vector<Diagnostic> diagnostics;
  std::vector<Diagnostic> runtimeDiagnostics;
  bool ok() const { return diagnostics.empty() && ast.body != 0; }
};
RoutineCompilation compileRoutine(Dialect, std::string, LanguageOptions = {},
                                  const CatalogSnapshot * = nullptr, NameResolutionContext = {});
struct ScriptCheckResult {
  std::vector<Diagnostic> diagnostics;
  std::vector<Diagnostic> runtimeDiagnostics;
  std::size_t statements = 0, routines = 0;
  bool complete = true, metadataUsed = false;
  bool ok() const {return complete && diagnostics.empty();}
};
ScriptCheckResult checkSqlScript(Dialect,std::string,LanguageOptions = {},
                                const CatalogSnapshot * = nullptr,NameResolutionContext = {});
struct QualifierMapping {
  std::vector<std::string> source, target;
};
struct ConversionOptions {
  LanguageOptions language;
  GenerationOptions generation;
  std::vector<QualifierMapping> qualifiers;
  // Optional source shape/type enrichment; missing deployed objects do not
  // prevent conversion. Explicit script declarations remain semantically checked.
  const CatalogSnapshot *catalog = nullptr;
  NameResolutionContext names;
  bool allowDdlTransactionChange = false;
};
enum class ConversionChangeKind {
  Qualifier, Type, NullOrdering, Pagination, TableShorthand, DerivedValues,
  CteSyntax, WindowExclusion, SymmetricBounds, DateLiteral, OneRowSource,
  UpdateTarget, DdlCommit, RoutineBody, AggregateFilter
};
struct ConversionChange {
  ConversionChangeKind kind;
  std::size_t statement; // One-based script statement number.
  SourceRange origin;
  std::string before, after;
};
struct ConversionResult {
  Dialect source = Dialect::PostgreSQL, target = Dialect::MySQL;
  std::string sql, modelJson;
  std::vector<Diagnostic> diagnostics;
  std::vector<std::string> notes;
  std::vector<ConversionChange> changes;
  bool changesTruncated = false;
  bool targetValidated = false;
  bool ok() const { return targetValidated && diagnostics.empty(); }
};
// Atomic, offline conversion: any unsafe construct leaves sql empty. Original
// source and target dialect identities are retained in the result/report.
ConversionResult convertSql(Dialect source, Dialect target, std::string,
                            ConversionOptions = {});
std::string dumpRoutineModel(const RoutineCompilation &,std::size_t maxBytes=16*1024*1024,
                             std::function<bool()> shouldStop={});
} // namespace navidog::sql::compiler
