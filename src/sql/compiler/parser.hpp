#pragma once
#include "navidog/sql/compiler/compiler.hpp"
#include <map>
#include <stdexcept>

namespace navidog::sql::compiler {
void lex(Compilation &compilation, bool opaqueComments = false,
         bool recoverSymbols = false);
std::optional<Diagnostic> validateVirtualTableArgument(std::string_view, std::function<bool()> shouldStop);
std::optional<std::string> copyOptionError(const ModelCopy &);
void collectDdlExpressions(const DdlSyntax &, std::vector<ExprId> &);
ModelDdl lowerDdl(const Compilation &, const DdlSyntax &,
                  const std::unordered_map<ExprId, ExprId> &);
void lowerAndBind(Compilation &, const CatalogSnapshot *,
                  const NameResolutionContext &);
std::vector<std::size_t> relationLeaves(const ModelStatement &, RelationId,
                                       const std::function<void()> &checkCancellation);
void validateModelSemantics(Compilation &);
void inferSqlTypes(Compilation &);
BasicTypeSpec<std::vector<std::string>> lowerTypeSpec(const Compilation &,const BasicTypeSpec<QualifiedName> &);
void validateGroupingSemantics(Compilation &);
void inferModelTypes(ModelSelect &,Dialect);
TableDefinition inspectionOutput(const ModelInspection &, const NameResolutionContext &);
struct ParseFailure {};
struct CompilationAborted {};
void checkStop(Compilation &);
class Parser {
public:
  explicit Parser(Compilation &context);
  virtual ~Parser() = default;
  void parse();
  const std::vector<Diagnostic> &analysisDiagnostics() const { return analysisDiagnostics_; }
  BasicTypeSpec<QualifiedName> parseTypeAt(std::size_t byteOffset);
  // Procedural dollar-quoted bodies supply a separately lexed stream whose
  // ranges still refer to this compilation source. Only used for type parsing.
  void setTypeTokenStream(const std::vector<Token> &);

protected:
  Compilation &context_;
  std::vector<Token> tokens_;
  std::size_t cursor_ = 0, depth_ = 0, syntaxNodes_ = 0;
  bool mergeOutput_ = false;
  std::vector<Diagnostic> analysisDiagnostics_;
  std::size_t projectionExpressionDepth_ = 0;
  const Token &current() const;
  const Token &previous() const;
  bool at(std::string_view spelling, std::size_t ahead = 0) const;
  bool eat(std::string_view spelling);
  Token take();
  void expect(std::string_view spelling);
  [[noreturn]] void fail(std::string message,
                         DiagnosticCode code = DiagnosticCode::ExpectedToken);
  virtual bool nameToken() const;
  virtual bool functionNameToken() const { return nameToken(); }
  virtual bool typeNameToken() const { return nameToken(); }
  virtual bool contextualIdentifier(std::string_view) const { return false; }
  virtual bool bareProjectionAliasToken() const { return nameToken(); }
  bool trailingProjectionAlias() const;
  NamePart namePart(bool qualified = false);
  QualifiedName qualifiedName(bool allowStar = false);
  ExprId node(Expr expression);
  void syntaxNode();
  void requireScalar(ExprId expression);
  void groupingClause(Statement &);
  std::size_t groupingSetCount_=0,groupingReferenceCount_=0;
  ExprId expression(int minimumPrecedence = 0, bool postgresqlRestricted = false);
  int mysqlExpressionLevel(ExprId) const;
  int postgresqlNonAssociativeLevel(ExprId) const;
  ExprId primary();
  ExprId sqlServerJsonConstructor(QualifiedName, std::size_t begin);
  ExprId sqlServerConversion(std::string function, std::size_t begin);
  ExprId patternEscapeOperand(int minimum);
  ExprId functionArgument(const QualifiedName &,std::size_t index);
  bool typedLiteralAhead() const;
  ExprId typedLiteral();
  bool intervalAhead() const;
  ExprId intervalExpression();
  ExprId extractExpression();
  void intervalTypeTail(BasicTypeSpec<QualifiedName> &);
  ExprId window(ExprId);
  struct PendingWindow {
    WindowSpec spec;
    std::vector<ExprId> keys;
    std::optional<NamePart> parent;
    std::optional<WindowSource> source = {};
  };
  struct WindowReference {
    ExprId expression;
    PendingWindow window;
    bool copy;
  };
  struct WindowScope {
    std::map<std::string, PendingWindow> definitions;
    std::vector<NamePart> names;
    std::vector<WindowReference> references;
  };
  std::vector<std::unique_ptr<WindowScope>> windowScopes_;
  std::size_t expandedWindowOperands_ = 0;
  PendingWindow windowBody();
  bool namedWindowsAllowed() const;
  std::string windowName(const NamePart &) const;
  bool deferWindowError(DiagnosticCode,SourceRange,std::string);
  PendingWindow inheritWindow(PendingWindow, const PendingWindow &);
  void windowClause(Statement &);
  void resolveWindows(Statement &);
  ExprId castType(ExprId base, std::size_t begin,
                  bool closeParenthesis = false);
  QueryId nestedQuery();
  void checkQuantifiedSubquery(QueryId);
  ExprId caseExpression(std::size_t begin);
  struct DmlGrammar {
    bool mysql = false, updateFrom = false, deleteUsing = false;
    bool conflict = false, returning = false, returningInto = false;
    bool output = false, defaultValues = true, multiValues = true;
    bool leadingWith = true, qualifiedAssignment = false;
    bool naturalUsing = true, defaultExpression = true;
  };
  virtual DmlGrammar dmlGrammar() const = 0;
  struct DdlGrammar {
    bool mysql = false, oracle = false, postgres = false, sqlserver = false,
         sqlite = false, tidb = false, oceanbase = false;
  };
  virtual DdlGrammar ddlGrammar() const = 0;
  virtual Statement inspectionStatement();
  virtual Statement pragmaStatement();
  virtual Statement vacuumStatement();
  virtual Statement copyStatement();
  virtual void createVirtualTable(DdlSyntax &);
  TriggerDefiner storedObjectDefiner();
  virtual Statement triggerStatement();
  virtual Statement packageStatement();
  BasicTypeSpec<QualifiedName> typeSpec(bool literalName=false);
  virtual void validateColumnType(const BasicTypeSpec<QualifiedName> &) {}
  Statement statement(bool nested = false);
  void ifNotExists(DdlSyntax &);
  std::uint64_t ddlUnsignedInteger();
  virtual ExprId postfix(ExprId base);
  virtual void selectPrefix(SelectStatement &statement);
  virtual void pagination(SelectStatement &statement);
  virtual bool tableAliasWithAS() const { return true; }
  virtual bool fullJoin() const { return true; }
  virtual bool nullOrdering() const { return false; }
  virtual bool requiresFrom() const { return false; }
  virtual std::pair<std::string, int> binaryOperator() const;
  virtual int precedence(std::string_view op) const;
  void limitOffset(SelectStatement &statement, bool comma, bool all);
  void offsetFetch(SelectStatement &statement, bool requireOrder);
  virtual ExprId limitValue();

private:
  Statement callStatement();
  bool transactionAhead() const;
  Statement transactionStatement();
  Statement preparedStatement();
  Statement roleStatement();
  TableSource jsonTableSource();
  std::vector<BasicJsonTableColumn<NamePart, QualifiedName>> jsonTableColumns(std::size_t &count);
  std::vector<CommonTableExpression> withClause(bool &recursive);
  ExprId arrayConstructor(bool named);
  QueryId nestedCteBody();
  Statement select(bool nested = false, bool embedded = false,
                   bool setOperand = false);
  bool queryAhead(std::size_t ahead = 0) const;
  bool derivedTableAhead();
  bool tableQueryAhead(std::size_t ahead = 0) const;
  bool valuesQueryAhead(std::size_t ahead = 0) const;
  void valueRows(Statement &);
  int setPrecedence() const;
  Statement setTail(Statement, int);
  Statement ddl();
  ObjectKind objectKind();
  BasicColumnSpec<NamePart, QualifiedName>
  columnSpec(bool allowMissingType = false);
  BasicConstraint<NamePart, QualifiedName>
  constraintSpec(std::optional<NamePart> column = {});
  void referenceTail(BasicConstraint<NamePart, QualifiedName> &);
  bool constraintStart() const;
  void columnList(std::vector<NamePart> &);
  BasicTableIndex<NamePart> tableIndex();
  void createDdl(Statement &, DdlSyntax &);
  void alterDdl(Statement &, DdlSyntax &);
  void dropDdl(DdlSyntax &);
  void truncateDdl(DdlSyntax &);
  void renameDdl(DdlSyntax &);
  void sequenceOptions(DdlSyntax &, bool alter = false);
  void mysqlPartition(DdlSyntax &);
  void tableOptions(DdlSyntax &);
  bool vendorTableOption(DdlSyntax &);
  bool distributionOption(DdlSyntax &, bool alter = false);
  void tableGroupSharding(DdlSyntax &);
  void ifExists(DdlSyntax &);
  Statement merge();
  Statement insert(bool replaceExisting = false);
  Statement update();
  Statement deleteStatement();
  void assignments(std::vector<Assignment> &out, bool qualified);
  ExprId assignmentValue();
  void returning(Statement &);
  void conflict(Statement &);
  void dmlTop(Statement &);
  void dmlTail(Statement &, bool allowLimit);
  void orderBy(Statement &);
  void projections(std::vector<Projection> &,bool allowAssignmentAliases=false);
  void fromClause(Statement &);
  RelationId joinedTable(Statement &);
  RelationId tableFactor(Statement &);
  RelationId relation(Statement &, Relation);
  QueryId storeQuery(Statement);
  void endStatement(bool nested = false);

  TableSource table(JoinKind kind, bool updateTarget=false, bool allowDeleteWildcard=false);
  bool pivotAhead() const;
  void pivotClause(TableSource &);
  bool unpivotAhead() const;
  void unpivotClause(TableSource &);
  void rowMatchClause(TableSource &);
  PostgresRowFunction postgresRowFunctionAhead() const;
  TableSource postgresFunctionSource(PostgresRowFunction);
  ClickHouseSampleRatio clickHouseSampleRatio();
  std::optional<NamePart> alias(bool table);
};
std::unique_ptr<Parser> createParser(Compilation &compilation);
bool mysqlLoadableFunctionAhead(const std::vector<Token> &,std::size_t at);
std::string asciiUpper(std::string_view value);
} // namespace navidog::sql::compiler
