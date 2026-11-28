#pragma once
#include "navidog/sql/compiler/conversion.hpp"
namespace navidog::sql::compiler {
struct PgDiagnosticFieldInfo {
  PgDiagnosticField id;
  const char *spelling;
  bool current, stacked;
  bool available(PgDiagnosticArea area) const {
    return area == PgDiagnosticArea::Current ? current : stacked;
  }
};
inline constexpr PgDiagnosticFieldInfo pgDiagnosticFields[] = {
    {PgDiagnosticField::RowCount, "ROW_COUNT", true, false},
    {PgDiagnosticField::Context, "PG_CONTEXT", true, true},
    {PgDiagnosticField::ReturnedSqlstate, "RETURNED_SQLSTATE", false, true},
    {PgDiagnosticField::MessageText, "MESSAGE_TEXT", false, true},
    {PgDiagnosticField::ExceptionDetail, "PG_EXCEPTION_DETAIL", false, true},
    {PgDiagnosticField::ExceptionHint, "PG_EXCEPTION_HINT", false, true},
    {PgDiagnosticField::ExceptionContext, "PG_EXCEPTION_CONTEXT", false, true},
    {PgDiagnosticField::ColumnName, "COLUMN_NAME", false, true},
    {PgDiagnosticField::ConstraintName, "CONSTRAINT_NAME", false, true},
    {PgDiagnosticField::DatatypeName, "PG_DATATYPE_NAME", false, true},
    {PgDiagnosticField::TableName, "TABLE_NAME", false, true},
    {PgDiagnosticField::SchemaName, "SCHEMA_NAME", false, true},
};
enum class MySqlDiagnosticUse { SignalAndCondition, Condition, MariaDbCondition, Statement };
struct MySqlDiagnosticFieldInfo {
  MySqlDiagnosticField id;
  const char *spelling;
  MySqlDiagnosticUse use;
  constexpr MySqlDiagnosticFieldInfo(MySqlDiagnosticField id, const char *spelling,
      MySqlDiagnosticUse use = MySqlDiagnosticUse::SignalAndCondition)
      : id(id), spelling(spelling), use(use) {}
  bool readable(Dialect dialect,bool condition) const {
    if (!condition) return use==MySqlDiagnosticUse::Statement;
    switch (use) {
    case MySqlDiagnosticUse::Statement: return false;
    case MySqlDiagnosticUse::MariaDbCondition: return dialect==Dialect::MariaDB;
    case MySqlDiagnosticUse::Condition: return true;
    case MySqlDiagnosticUse::SignalAndCondition:
      return dialect!=Dialect::OceanBase || id==MySqlDiagnosticField::MessageText ||
                                             id==MySqlDiagnosticField::ErrorNumber;
    }
    return false;
  }
};
inline constexpr MySqlDiagnosticFieldInfo mysqlDiagnosticFields[] = {
    {MySqlDiagnosticField::ClassOrigin, "CLASS_ORIGIN"},
    {MySqlDiagnosticField::SubclassOrigin, "SUBCLASS_ORIGIN"},
    {MySqlDiagnosticField::MessageText, "MESSAGE_TEXT"},
    {MySqlDiagnosticField::ErrorNumber, "MYSQL_ERRNO"},
    {MySqlDiagnosticField::ConstraintCatalog, "CONSTRAINT_CATALOG"},
    {MySqlDiagnosticField::ConstraintSchema, "CONSTRAINT_SCHEMA"},
    {MySqlDiagnosticField::ConstraintName, "CONSTRAINT_NAME"},
    {MySqlDiagnosticField::CatalogName, "CATALOG_NAME"},
    {MySqlDiagnosticField::SchemaName, "SCHEMA_NAME"},
    {MySqlDiagnosticField::TableName, "TABLE_NAME"},
    {MySqlDiagnosticField::ColumnName, "COLUMN_NAME"},
    {MySqlDiagnosticField::CursorName, "CURSOR_NAME"},
    {MySqlDiagnosticField::ReturnedSqlstate,"RETURNED_SQLSTATE",MySqlDiagnosticUse::Condition},
    {MySqlDiagnosticField::RowNumber,"ROW_NUMBER",MySqlDiagnosticUse::MariaDbCondition},
    {MySqlDiagnosticField::Number,"NUMBER",MySqlDiagnosticUse::Statement},
    {MySqlDiagnosticField::RowCount,"ROW_COUNT",MySqlDiagnosticUse::Statement},
};
inline const MySqlDiagnosticFieldInfo *mysqlDiagnosticInfo(MySqlDiagnosticField id) {
  for (const auto &field:mysqlDiagnosticFields)
    if (field.id==id) return &field;
  return nullptr;
}
inline bool isSimpleMySqlUserVariable(const std::string &value) {
  return value.size()>1 && value.front()=='@' &&
         value.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789$_",1)==std::string::npos;
}
inline const std::string &routineSymbolLabel(const RoutineAst &ast, const ProceduralSymbol &symbol) {
  static const std::string noQualifier;
  if (isMySqlDialect(ast.dialect)) return noQualifier;
  const auto &scope = ast.scopes.at(symbol.scope);
  return symbol.kind == SymbolKind::ExceptionStatus ? ast.scopes.at(scope.parent).label : scope.label;
}
inline bool routineSymbolWritable(Dialect dialect, const ProceduralSymbol &symbol) {
  return symbol.kind != SymbolKind::ExceptionStatus &&
         !(dialect == Dialect::Oracle &&
           (symbol.kind == SymbolKind::LoopIndex ||
            (symbol.kind == SymbolKind::Parameter && symbol.mode == ParameterMode::In)));
}
inline bool routineSymbolVisible(const ProceduralSymbol &symbol, std::size_t position) {
  return symbol.kind != SymbolKind::Variable || symbol.origin.end <= position;
}
std::size_t statementEndIndex(Dialect, const std::vector<Token> &, std::size_t);
std::string normalizeConversionScript(Dialect, const std::string &,
                                      LanguageOptions,
                                      std::vector<Diagnostic> &,
                                      std::vector<std::size_t> *clientSeparators = nullptr);
RoutineCompilation compileRoutinePrefix(Dialect, std::string, LanguageOptions,
                                        const CatalogSnapshot *, NameResolutionContext = {});
struct RoutineCompletionSite {
  std::size_t offset = std::string::npos;
  bool statement = false;
  bool closesIf = false;
  bool cursorName = false;
  bool variableTarget = false;
  bool scalarVariable = false;
  bool labelTarget = false;
  bool preparedNameTarget = false;
  std::vector<std::string> keywords, conditionNames, preparedNames;
  std::vector<std::pair<std::string,bool>> labels; // Name and whether it names a loop.
};
RoutineCompilation parseRoutineForCompletion(Dialect, std::string, LanguageOptions,
                                             RoutineCompletionSite * = nullptr);
RoutineCompilation compileRoutineForCompletion(Dialect, std::string, LanguageOptions,
                                              const CatalogSnapshot *, NameResolutionContext,
                                              std::size_t cursor, RoutineCompletionSite * = nullptr);
bool lowerTarget(ModelProgram &, Dialect, const ConversionOptions &,
                 std::vector<Diagnostic> &);
bool lowerTypeTarget(BasicTypeSpec<std::vector<std::string>> &, Dialect,
                     Dialect, std::vector<Diagnostic> &);
std::optional<std::int64_t> exactSignedIntegerLiteral(const ModelStatement &,
                                                      ExprId, bool allowString = false);
GeneratedSql emitRoutine(const RoutineModel &, Dialect,
                         const ConversionOptions &);
} // namespace navidog::sql::compiler
