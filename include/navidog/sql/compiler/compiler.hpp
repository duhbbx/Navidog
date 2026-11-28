#pragma once

#include "types.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace navidog::sql::compiler {

enum class Dialect {
  MySQL,
  PostgreSQL,
  Oracle,
  SQLite,
  SQLServer,
  MariaDB,
  OceanBase,
  TiDB
};
constexpr bool isMySqlDialect(Dialect dialect) {
  return dialect == Dialect::MySQL || dialect == Dialect::MariaDB ||
         dialect == Dialect::OceanBase || dialect == Dialect::TiDB;
}
const char *dialectName(Dialect dialect);
std::optional<Dialect> parseDialect(std::string_view name);
// Source identifier folding; does not assume a database collation policy.
std::string canonicalIdentifier(Dialect, std::string value, bool quoted);
bool allowsDerivedColumnAliases(Dialect);

struct SourceRange {
  std::size_t begin = 0, end = 0;
};
struct SourcePosition {
  std::size_t line = 1, byteColumn = 1;
};
class SourceBuffer {
public:
  explicit SourceBuffer(std::string text, std::string name = "<input>");
  const std::string &text() const { return text_; }
  const std::string &name() const { return name_; }
  std::string_view slice(SourceRange range) const;
  SourcePosition position(std::size_t offset) const;

private:
  std::string text_, name_;
  std::vector<std::size_t> lines_;
};

// Stable diagnostic codes with half-open UTF-8 byte ranges. The editor adapter
// maps these to UTF-16 positions without coupling the compiler to Qt.
enum class DiagnosticCode {
  InvalidEncoding,
  LexicalError,
  ExpectedToken,
  UnsupportedSyntax,
  ResourceLimit,
  DuplicateAlias,
  UnknownTable,
  UnknownColumn,
  AmbiguousColumn,
  InvalidQueryShape,
  Stopped,
  UnknownRoutine,
  RoutineArgumentCount,
  CursorRuntime, PreparedRuntime
};
struct Diagnostic {
  DiagnosticCode code;
  SourceRange range;
  std::string message;
};
struct Limits {
  std::size_t sourceBytes = 4 * 1024 * 1024, tokens = 250000, nodes = 100000;
  std::size_t nesting = 128, statements = 200, diagnostics = 100;
  std::size_t queries = 1000, columnsPerQuery = 4096, inferredColumns = 32768;
  std::size_t groupingSets=256,groupingReferences=8192,types=8192;
};
struct LanguageOptions {
  Limits limits;
  bool mysqlAnsiQuotes = false;
  bool mysqlNoBackslashEscapes = false;
  bool mysqlPipesAsConcat = false;
  std::function<bool()> shouldStop;
  // Session values, when available. Missing facts must not be replaced by
  // server defaults, since either option can be overridden on a connection.
  std::optional<int> mysqlDivisionPrecisionIncrement;
  std::optional<bool> mysqlNoUnsignedSubtraction;
  bool calciteExtensions = false;
  bool describeSelectArity = false;
  // MySQL/MariaDB/TiDB native function-token lookup skips whitespace in this mode.
  bool mysqlIgnoreSpace = false;
};
enum class TokenKind {
  Word,
  Identifier,
  Number,
  String,
  Parameter,
  Symbol,
  Comment,
  End,
  Variable,
  Window,
  AggregateFilter // operands: [aggregate call, boolean predicate].
};
struct Token {
  TokenKind kind;
  SourceRange range;
  std::string value;
};

using IdentifierId = std::uint32_t;
using ExprId =
    std::uint32_t; // 0 is invalid; ASTContext owns all expression nodes.
using QueryId = std::uint32_t; // 1-based arena index, 0 means no nested query.
struct NamePart {
  IdentifierId identifier = 0;
  bool quoted = false;
  SourceRange range;
};
using QualifiedName = std::vector<NamePart>;
enum class ExprKind {
  Name,
  Number,
  String,
  Null,
  Boolean,
  Parameter,
  Star,
  Unary,
  Binary,
  Call,
  Cast,
  Between,
  In,
  IsNull,
  Case,
  Subquery,
  Exists,
  Default,
  Row,
  Quantified,
  Variable,
  Window,
  AggregateFilter, // operands: [aggregate call, boolean predicate].
  TypedLiteral, // Structured source type plus decoded literal, no raw SQL.
  Interval,
  Extract,
  TruthTest,
  ArrayConstructor,
  Subscript,
  MergeAction,
  LikeEscape, // operands: [value, pattern, explicit escape expression].
  Convert // SQL Server CONVERT/TRY_CONVERT/TRY_CAST; value + optional style, castType owns the target.
};
// Subscript.value stores one kind per bracket pair. Operands start with the
// base expression, followed by the indices and written slice bounds in order.
enum class SubscriptKind : char { Index='i', Range='b', Lower='l', Upper='u', Full='s' };
// Window operands are [function, partition keys, order keys, frame offsets].
// Counts and bound kinds describe the owning operand vector in AST and IR.
enum class WindowBound {
  None,
  UnboundedPreceding,
  Preceding,
  CurrentRow,
  Following,
  UnboundedFollowing
};
enum class WindowExclusion { None, NoOthers, CurrentRow, Group, Ties };
const char *windowExclusionName(WindowExclusion);
struct WindowOrdering {
  bool descending = false;
  std::string nulls;
  bool explicitAscending = false;
};
struct WindowSpec {
  std::size_t partitions = 0;
  std::vector<WindowOrdering> ordering;
  std::string frameUnit;
  bool between = false;
  WindowBound start = WindowBound::None, end = WindowBound::None;
  WindowExclusion exclusion = WindowExclusion::None;
};
// Original PostgreSQL spelling, separate from the resolved semantic window.
// Local keys are a suffix of the resolved operands; no borrowed arena IDs.
struct WindowSource {
  std::string reference;
  bool bare = false;
  WindowSpec specification;
  std::size_t keys = 0;
};
struct NamedWindowDefinition {
  NamePart name;
  WindowSpec specification;
  std::vector<ExprId> keys;
  std::optional<WindowSource> source = {};
};
struct IntervalSpec {
  IntervalField leading=IntervalField::None,trailing=IntervalField::None;
  IntervalUnit mysqlUnit=IntervalUnit::None;
  std::optional<int> leadingPrecision,fractionalPrecision;
  bool prefixPrecision=false; // PostgreSQL INTERVAL(p) 'value'.
};
enum class JsonNullPolicy { Default, NullOnNull, AbsentOnNull };
struct JsonConstructorSpec {
  JsonNullPolicy nullPolicy=JsonNullPolicy::Default;
  bool returningJson=false; // SQL Server 2025 RETURNING JSON; otherwise nvarchar(max).
};
std::string validateIntervalSpec(const IntervalSpec &,Dialect);
bool validExtractField(std::string_view,Dialect);
const char *intervalFieldName(IntervalField);
const char *intervalUnitName(IntervalUnit);
template<class Qualified> struct BasicTypeSpec;
struct Expr {
  ExprKind kind = ExprKind::Null;
  SourceRange range;
  std::string
      value; // Original numeric spelling; decoded string; canonical operator.
  QualifiedName name;
  std::vector<ExprId> operands;
  QueryId query = 0;
  bool distinct = false, explicitElse = false;
  std::optional<WindowSpec> window = {};
  std::shared_ptr<const BasicTypeSpec<QualifiedName>> castType = {};
  std::shared_ptr<const IntervalSpec> interval = {};
  // Last operand is the per-row ordering argument for WITHIN GROUP.
  std::shared_ptr<const WindowOrdering> withinGroup = {};
  // JSON_OBJECT operands alternate key/value; JSON_ARRAY operands are elements.
  // Syntax policy is structured separately from the owning argument expressions.
  std::optional<JsonConstructorSpec> jsonConstructor = {};
  std::optional<WindowSource> windowSource = {};
};
struct Projection {
  ExprId expression = 0;
  std::optional<NamePart> alias;
};
enum class JoinKind { First, Cross, Inner, Left, Right, Full };
enum class SetKind { Union, Intersect, Except };
enum class TableQuerySyntax { None, Table, Only, IncludeDescendants };
using RelationId = std::uint32_t;
struct SetOperation {
  SetKind kind = SetKind::Union;
  QueryId left = 0, right = 0;
  bool all = false;
  SourceRange origin;
  bool recursive = false; // Bound seed/step union; syntax records WITH RECURSIVE separately.
};
enum class TableInheritance { Default, Only, IncludeDescendants };
template<class Name, class Qualified> struct BasicJsonTable;
using JsonTableSyntax = BasicJsonTable<NamePart, QualifiedName>;
using ModelJsonTable = BasicJsonTable<std::string, std::vector<std::string>>;
template<class Name> struct BasicPivotBucket {
  // SQL Server names output columns. Oracle and Calcite bind static values.
  std::optional<Name> name;
  std::vector<ExprId> values;
  SourceRange origin;
};
template<class Name, class QName> struct BasicPivot {
  std::string aggregate;
  QName argument, axes;
  std::optional<Name> aggregateAlias, relationAlias, inputAlias;
  std::vector<BasicPivotBucket<Name>> buckets;
  SourceRange origin, argumentOrigin, axisOrigin;
};
using PivotSyntax = BasicPivot<NamePart, QualifiedName>;
struct ModelPivot : BasicPivot<std::string, std::vector<std::string>> {
  std::optional<std::size_t> argumentOrdinal;
  std::vector<std::size_t> axisOrdinals, groupingOrdinals;
};
template<class Name> struct BasicUnpivot {
  struct Input {
    std::vector<Name> columns;
    std::vector<ExprId> labels;
    SourceRange origin;
  };
  std::vector<Name> measures, descriptors;
  std::optional<Name> inputAlias, relationAlias;
  std::vector<Input> inputs;
  std::optional<bool> includeNulls;
  SourceRange origin;
};
using UnpivotSyntax=BasicUnpivot<NamePart>;
struct ModelUnpivot : BasicUnpivot<std::string> {
  std::vector<std::size_t> retainedOrdinals;
  std::vector<std::vector<std::size_t>> inputOrdinals;
};
std::string unpivotDefaultLabel(const std::vector<std::string> &columns);
struct TableSampling {
  static constexpr std::uint32_t full=100000;
  std::uint32_t milliPercent=0;
  std::optional<std::int64_t> repeatable;
};
std::string tableSamplePercent(std::uint32_t milliPercent);
template<class Name>
struct BasicRowMatch {
  std::optional<Name> partition;
  Name order, measure, variable, testColumn;
  std::string comparison, nullOrdering;
  std::int64_t threshold=0;
  bool descending=false;
  SourceRange origin;
};
using RowMatchSyntax=BasicRowMatch<NamePart>;
using ModelRowMatch=BasicRowMatch<std::string>;
enum class PostgresRowFunction { None, Unnest, GenerateSeries };
struct TableSource {
  QualifiedName name;
  std::optional<NamePart> alias;
  JoinKind join = JoinKind::First;
  ExprId condition = 0;
  SourceRange range;
  QueryId query = 0;
  RelationId lateralLeft = 0;
  TableInheritance inheritance = TableInheritance::Default;
  std::optional<TableSampling> sampling = {};
  std::shared_ptr<const JsonTableSyntax> jsonTable;
  std::vector<NamePart> columnAliases = {};
  std::shared_ptr<const PivotSyntax> pivot;
  std::shared_ptr<const UnpivotSyntax> unpivot;
  std::optional<RowMatchSyntax> rowMatch = {};
  PostgresRowFunction postgresFunction=PostgresRowFunction::None;
  std::optional<NamePart> ordinalityColumn = {};
  bool inferredFunctionAlias = false;
  std::size_t functionColumnAliases = 0;
};
struct OrderItem {
  ExprId expression = 0;
  bool descending = false;
  std::string nulls;
  bool explicitAscending = false;
};
enum class LimitSyntax { None, LimitOffset, CommaLimit, Fetch, Top };
struct RowLimit {
  ExprId count = 0, offset = 0;
  LimitSyntax syntax = LimitSyntax::None;
  bool withTies = false;
};
enum class SelectIntoMode { FirstRowOrNull, ExactlyOne, SingleRowOrUnchanged };
struct SelectIntoAssignment {
  SourceRange origin;
  std::vector<std::uint32_t> targets;
  SelectIntoMode mode = SelectIntoMode::FirstRowOrNull;
};
enum class CteMaterialization { Default, Materialized, NotMaterialized };
struct CommonTableExpression {
  NamePart name;
  std::vector<NamePart> columns;
  QueryId query = 0;
  SourceRange range;
  CteMaterialization materialization = CteMaterialization::Default;
};
enum class StatementKind {
  Select,
  Insert,
  Update,
  Delete,
  Create,
  Alter,
  Drop,
  Truncate,
  Rename,
  Comment,
  Call,
  Merge,
  Inspect,
  Transaction,
  Prepared,
  Role,
  Pragma,
  Vacuum,
  Trigger,
  TriggerSet,
  Package,
  Copy
};
const char *statementKindName(StatementKind);
enum class TransactionAction { Begin, Commit, Rollback, Savepoint, RollbackTo, Release, NamedRollback };
enum class TransactionStartMode { Default, Deferred, Immediate, Exclusive };
enum class TransactionIsolation { Default, ReadUncommitted, ReadCommitted, RepeatableRead, Serializable };
enum class TransactionAccess { Default, ReadOnly, ReadWrite };
enum class TransactionEndOption { Default, Yes, No };
template<class Name> struct BasicTransaction {
  TransactionAction action = TransactionAction::Begin;
  TransactionStartMode mode = TransactionStartMode::Default;
  TransactionIsolation isolation = TransactionIsolation::Default;
  TransactionAccess access = TransactionAccess::Default;
  TransactionEndOption chain = TransactionEndOption::Default, release = TransactionEndOption::Default;
  std::optional<bool> deferrable;
  std::optional<Name> name;
  bool consistentSnapshot = false;
};
using TransactionSyntax = BasicTransaction<NamePart>;
using ModelTransaction = BasicTransaction<std::string>;
enum class PreparedAction { Prepare, Execute, Deallocate };
template<class Name> struct BasicPreparedStatement {
  PreparedAction action = PreparedAction::Prepare;
  Name name;
  ExprId source = 0;
  std::vector<ExprId> arguments;
};
using PreparedSyntax = BasicPreparedStatement<NamePart>;
struct ModelPrepared : BasicPreparedStatement<std::string> {
  std::optional<std::size_t> parameterCount;
  bool preservesHandles = false;
};
enum class RoleScope { Session, AccountDefault };
enum class RoleMode { Default, None, All, AllExcept, Named };
struct RoleAccount {
  std::string name;
  std::optional<std::string> host;
  SourceRange range;
};
struct RoleControl {
  RoleScope scope = RoleScope::Session;
  RoleMode mode = RoleMode::Named;
  std::vector<RoleAccount> roles, recipients;
};
// Administrative inspection is a distinct statement family, not a SELECT over
// a guessed information_schema view. Its output/privilege semantics are vendor-specific.
enum class InspectionKind { Databases, Tables, Columns, Indexes, TableStatus,
                            Variables, Status, ProcessList, CreateDefinition, Describe,
                            TableGroups, DdlStatus, DdlJobs };
enum class InspectionObject { Table, View, Database, Function, Procedure, Trigger, Event, TableGroup };
enum class InspectionScope { Default, Session, Global };
const char *inspectionKindName(InspectionKind);
const char *inspectionObjectName(InspectionObject);
template<class Name, class QName> struct BasicInspection {
  InspectionKind kind = InspectionKind::Tables;
  InspectionObject object = InspectionObject::Table;
  InspectionScope scope = InspectionScope::Default;
  QName target;
  std::optional<Name> database, column;
  ExprId pattern = 0;
  std::optional<std::uint64_t> jobCount;
  bool full = false, extended = false, ifNotExists = false;
};
using InspectionSyntax = BasicInspection<NamePart, QualifiedName>;
using ModelInspection = BasicInspection<std::string, std::vector<std::string>>;

// SQLite's optional PRAGMA argument is a signed number or a name/string.
// Argument spelling alone does not establish whether the command is read-only.
enum class PragmaArgumentStyle { None, Assignment, Parenthesized };
template<class QName> struct BasicPragma {
  QName name;
  PragmaArgumentStyle style = PragmaArgumentStyle::None;
  ExprId argument = 0;
};
using PragmaSyntax = BasicPragma<QualifiedName>;
using ModelPragma = BasicPragma<std::vector<std::string>>;

enum class VacuumOptionKind { Full, Freeze, Verbose, Analyze, DisablePageSkipping,
                              SkipLocked, IndexCleanup, ProcessToast, Truncate, Parallel };
enum class VacuumIndexCleanup { Auto, Enabled, Disabled };
const char *vacuumOptionName(VacuumOptionKind);
const char *vacuumIndexCleanupName(VacuumIndexCleanup);
struct VacuumOption {
  VacuumOptionKind kind;
  std::variant<std::monostate, bool, VacuumIndexCleanup, int> argument;
  SourceRange origin;
};
template<class Name, class QName> struct BasicVacuumTarget {
  QName name;
  std::vector<Name> columns;
  SourceRange origin;
};
template<class Name, class QName> struct BasicVacuum {
  std::vector<VacuumOption> options;
  std::vector<BasicVacuumTarget<Name, QName>> targets;
};
using VacuumSyntax = BasicVacuum<NamePart, QualifiedName>;
using ModelVacuum = BasicVacuum<std::string, std::vector<std::string>>;

enum class TriggerEvent { Insert, Update, Delete };
enum class TriggerTiming { Default, Before, After, InsteadOf };
enum class TriggerOrder { None, Follows, Precedes };
struct TriggerDefiner {
  bool currentUser=false;
  std::string user, host;
};
const char *triggerEventName(TriggerEvent);
const char *triggerTimingName(TriggerTiming);
template<class Name, class QName> struct BasicTrigger {
  QName name, target;
  SourceRange targetOrigin;
  TriggerEvent event = TriggerEvent::Insert;
  TriggerTiming timing = TriggerTiming::Default;
  bool drop = false, temporary = false, ifGuard = false, explicitRow = false;
  bool compound = false;
  std::optional<TriggerDefiner> definer;
  TriggerOrder order = TriggerOrder::None;
  std::optional<Name> relativeTrigger;
  std::vector<Name> updateColumns;
  ExprId when = 0;
  std::vector<QueryId> body;
};
using TriggerSyntax = BasicTrigger<NamePart, QualifiedName>;
using ModelTrigger = BasicTrigger<std::string, std::vector<std::string>>;

enum class CopyDirection { From, To };
enum class CopyFormat { Text, Csv, Binary };
enum class CopyHeader { Disabled, Enabled, Match };
const char *copyFormatName(CopyFormat);
const char *copyHeaderName(CopyHeader);
struct CopyOptions {
  CopyDirection direction = CopyDirection::From;
  std::optional<CopyFormat> format;
  std::optional<CopyHeader> header;
  std::optional<bool> freeze;
  std::optional<std::string> delimiter, nullMarker, quote, escape, encoding;
  bool forceQuoteAll = false;
};
template<class Name> struct BasicCopy : CopyOptions {
  std::vector<Name> columns, forceQuote, forceNotNull, forceNull;
};
using CopySyntax = BasicCopy<NamePart>;
using ModelCopy = BasicCopy<std::string>;

struct Assignment {
  std::vector<ExprId> targets;
  ExprId value = 0;
};
enum class MergeMatch { Matched, NotMatchedTarget, NotMatchedSource };
enum class MergeAction { Update, Delete, Insert, Nothing };
template<class Name>struct BasicMergeClause {
  MergeMatch match=MergeMatch::Matched;
  MergeAction action=MergeAction::Nothing;
  ExprId condition=0,deleteWhere=0; // Oracle deleteWhere runs after UPDATE.
  std::vector<Assignment> assignments;
  std::vector<Name> columns;
  std::vector<ExprId> values;
  bool defaultValues=false;
  SourceRange origin;
};
using MergeClause=BasicMergeClause<NamePart>;
using ModelMergeClause=BasicMergeClause<std::string>;
enum class InsertSource { None, Values, Query, DefaultValues, Assignments };
struct ConflictClause {
  enum class Action { None, Nothing, Update, DuplicateKey };
  Action action = Action::None;
  std::vector<ExprId> targets;
  std::optional<NamePart> constraint;
  ExprId targetWhere = 0, where = 0;
  std::vector<Assignment> assignments;
  std::optional<NamePart> rowAlias; // MySQL VALUES / SET AS new.
  std::vector<NamePart> columnAliases;
};
// Explicit binary relation tree; table indices refer to the statement's leaf
// pool.
struct Relation {
  RelationId left = 0, right = 0;
  std::size_t table = 0;
  JoinKind join = JoinKind::First;
  ExprId condition = 0;
  bool natural = false;
  bool lateral = false;
  bool implicitLateral = false;
  bool fixedOrder = false;
  std::vector<NamePart> usingColumns;
  SourceRange range;
  bool comma = false;
  bool explicitLateral = false;
};
enum class ObjectKind { Table, View, Index, Schema, Database, Sequence, MaterializedView, TableGroup, LoadableFunction };
enum class TransactionEffect {
  PreservesTransaction,
  TransactionalDdl,
  ImplicitCommit,
  NonTransactionalWithoutCommit,
  ForbiddenInTransaction,
  ExplicitControl,
  Dynamic
};
const char *transactionEffectName(TransactionEffect);
const char *objectKindName(ObjectKind);
enum class ConstraintKind { PrimaryKey, Unique, ForeignKey, Check };
enum class ReferenceAction {
  None,
  NoAction,
  Restrict,
  Cascade,
  SetNull,
  SetDefault
};
enum class IdentityKind {
  None,
  AutoIncrement,
  Identity,
  GeneratedAlways,
  GeneratedByDefault
};
enum class AlterKind {
  AddColumn,
  DropColumn,
  RenameColumn,
  AlterType,
  SetDefault,
  DropDefault,
  SetNotNull,
  DropNotNull,
  AddConstraint,
  DropConstraint,
  RenameTable,
  ModifyColumn,
  ChangeColumn
};
enum class DropBehavior { Default, Cascade, Restrict, CascadeConstraints };
enum class SequenceOptionKind {
  Start,
  Increment,
  Minimum,
  Maximum,
  Cache,
  Cycle,
  NoMinimum,
  NoMaximum,
  NoCache,
  NoCycle
};
struct TypeArgument {
  TokenKind kind = TokenKind::Number;
  std::string value;
};
template <class Qualified> struct BasicTypeSpec {
  Qualified name;
  std::vector<bool> nameQuoted;
  std::vector<TypeArgument> arguments;
  std::vector<std::string> modifiers;
  std::optional<std::string> lengthSemantics;
  std::shared_ptr<const IntervalSpec> intervalType = {};
  int arrayDimensions = 0;
  SourceRange origin;
};
enum class ParameterMode { In, Out, InOut };
enum class PackageEdition { Default, Editionable, NonEditionable };
enum class PackageAuth { Default, Definer, CurrentUser };
enum class PackageDeclarationKind { Procedure, Function, Variable, Constant, Exception };
template <class Name, class Qualified> struct BasicPackageParameter {
  Name name;
  BasicTypeSpec<Qualified> type;
  ParameterMode mode=ParameterMode::In;
  bool noCopy=false;
  ExprId defaultValue=0;
  SourceRange origin;
};
template <class Name, class Qualified> struct BasicPackageDeclaration {
  PackageDeclarationKind kind=PackageDeclarationKind::Variable;
  Name name;
  BasicTypeSpec<Qualified> type; // Return type for a function.
  std::vector<BasicPackageParameter<Name, Qualified>> parameters;
  ExprId initializer=0;
  bool deterministic=false;
  SourceRange origin;
};
template <class Name, class Qualified> struct BasicPackageSpec {
  Qualified name;
  std::optional<Name> endName;
  std::vector<BasicPackageDeclaration<Name, Qualified>> declarations;
  PackageAuth auth=PackageAuth::Default;
  PackageEdition edition=PackageEdition::Default;
  bool drop=false, replace=false;
};
using PackageSyntax=BasicPackageSpec<NamePart, QualifiedName>;
using ModelPackage=BasicPackageSpec<std::string, std::vector<std::string>>;
// Conservative owning payload accounting for bounded consumers of ModelProgram.
std::size_t typeSyntaxBytes(const BasicTypeSpec<std::vector<std::string>> &);
enum class PrimaryKeyClustering { Unspecified, Clustered, Nonclustered };
enum class IndexScope { Unspecified, Local, Global };
enum class DistributionOptionKind { ShardRowIdBits, PreSplitRegions, AutoRandomBase,
                                    PlacementPolicy, TableGroup, Sharding };
const char *distributionOptionName(DistributionOptionKind);
struct DistributionOption {
  DistributionOptionKind kind;
  std::variant<std::monostate, std::uint64_t, std::string> value;
  SourceRange origin;
};
struct AutoRandom {
  std::optional<std::uint64_t> shardBits, rangeBits;
  SourceRange origin;
};
template <class Name, class Qualified> struct BasicConstraint {
  ConstraintKind kind = ConstraintKind::Check;
  std::optional<Name> name;
  std::vector<Name> columns, referencedColumns;
  Qualified referencedTable;
  ExprId check = 0;
  ReferenceAction onDelete = ReferenceAction::None,
                  onUpdate = ReferenceAction::None;
  std::optional<bool> deferrable, initiallyDeferred;
  PrimaryKeyClustering clustering = PrimaryKeyClustering::Unspecified;
  SourceRange origin;
};
enum class JsonTableColumnKind { Path, Exists, Ordinality, Nested };
enum class JsonTableResponseKind { Unspecified, Null, Error, Default };
struct JsonTableResponse {
  JsonTableResponseKind kind = JsonTableResponseKind::Unspecified;
  std::string json;
};
template<class Name, class Qualified> struct BasicJsonTableColumn {
  JsonTableColumnKind kind = JsonTableColumnKind::Path;
  std::optional<Name> name;
  BasicTypeSpec<Qualified> type;
  std::string path;
  JsonTableResponse onEmpty, onError;
  std::vector<BasicJsonTableColumn> children;
  SourceRange origin;
};
template<class Name, class Qualified> struct BasicJsonTable {
  ExprId document = 0;
  std::string path;
  std::vector<std::size_t> documentTables;
  std::vector<BasicJsonTableColumn<Name,Qualified>> columns;
};
template <class Name, class Qualified> struct BasicColumnSpec {
  Name name;
  BasicTypeSpec<Qualified> type;
  std::optional<bool> nullable;
  ExprId defaultValue = 0, generated = 0, identityStart = 0,
         identityIncrement = 0;
  IdentityKind identity = IdentityKind::None;
  std::optional<AutoRandom> autoRandom;
  bool stored = false, defaultParenthesized = false;
  Qualified collation, characterSet;
  std::optional<std::string> comment;
  std::vector<BasicConstraint<Name, Qualified>> constraints;
  SourceRange origin;
  SourceRange nullabilityOrigin;
};
template <class Name, class Qualified> struct BasicAlterAction {
  AlterKind kind = AlterKind::AddColumn;
  std::optional<Name> name, newName;
  std::optional<BasicColumnSpec<Name, Qualified>> column;
  std::optional<BasicConstraint<Name, Qualified>> constraint;
  std::optional<BasicTypeSpec<Qualified>> type;
  ExprId value = 0;
  bool defaultParenthesized = false;
  std::optional<bool> nullable;
  bool ifExists = false, ifNotExists = false;
  DropBehavior dropBehavior = DropBehavior::Default;
  SourceRange origin;
};
struct IndexElement {
  ExprId expression = 0, prefixLength = 0;
  bool descending = false;
  std::string nulls;
};
enum class TableIndexMethod { Default, Btree, Hash };
template <class Name> struct BasicTableIndex {
  std::optional<Name> name;
  std::vector<IndexElement> keys;
  TableIndexMethod method = TableIndexMethod::Default;
  bool unique = false;
  SourceRange origin;
};
template <class Qualified> struct BasicTruncateTarget {
  Qualified name;
  bool only = false, descendants = false;
};
enum class PartitionMethod { Hash, Key };
template <class Name> struct BasicTablePartition {
  PartitionMethod method = PartitionMethod::Hash;
  bool linear = false;
  std::vector<Name> columns;
  std::optional<std::uint32_t> count, algorithm;
  SourceRange origin;
};
struct VirtualTableArgument {
  std::string payload;
  SourceRange origin;
};
template <class Name> struct BasicVirtualTable {
  Name module;
  bool argumentList = false;
  std::vector<VirtualTableArgument> arguments;
};
enum class MySqlViewAlgorithm { Unspecified, Undefined, Merge, TempTable };
enum class MySqlViewSecurity { Unspecified, Definer, Invoker };
struct MySqlViewOptions {
  MySqlViewAlgorithm algorithm = MySqlViewAlgorithm::Unspecified;
  MySqlViewSecurity security = MySqlViewSecurity::Unspecified;
  std::optional<TriggerDefiner> definer;
};
const char *mysqlViewAlgorithmName(MySqlViewAlgorithm);
const char *mysqlViewSecurityName(MySqlViewSecurity);
enum class MySqlLoadableResult { String, Integer, Real, Decimal };
struct MySqlLoadableFunction {
  MySqlLoadableResult result=MySqlLoadableResult::String;
  std::string library;
  bool aggregate=false;
};
const char *mysqlLoadableResultName(MySqlLoadableResult);
template <class Name, class Qualified> struct BasicDdl {
  ObjectKind object = ObjectKind::Table;
  Qualified name, indexedTable, likeTable, indexMethod;
  std::vector<Qualified> names;
  std::vector<std::pair<Qualified, Qualified>> renames;
  std::vector<Name> columns, includeColumns;
  std::vector<BasicColumnSpec<Name, Qualified>> definitions;
  std::vector<BasicConstraint<Name, Qualified>> constraints;
  std::vector<BasicAlterAction<Name, Qualified>> actions;
  std::vector<IndexElement> keys;
  std::vector<BasicTableIndex<Name>> tableIndexes;
  std::vector<BasicTruncateTarget<Qualified>> truncateTargets;
  ExprId predicate = 0, commentValue = 0;
  bool ifExists = false, ifNotExists = false, orReplace = false,
       temporary = false, globalTemporary = false, unlogged = false,
       unique = false, withNoData = false;
  bool purge = false, truncateOnly = false, truncateDescendants = false;
  std::optional<bool> restartIdentity, reuseStorage;
  DropBehavior dropBehavior = DropBehavior::Default;
  std::string engine, characterSet, collation, tablespace, onCommit, viewCheck;
  std::optional<MySqlViewOptions> mysqlViewOptions;
  std::optional<MySqlLoadableFunction> mysqlLoadableFunction;
  std::optional<std::string> tableComment;
  std::vector<DistributionOption> distribution;
  IndexScope indexScope = IndexScope::Unspecified;
  std::optional<BasicTablePartition<Name>> partition;
  std::shared_ptr<const BasicVirtualTable<Name>> virtualTable;
  std::vector<std::pair<SequenceOptionKind, ExprId>> sequenceOptions;
};
using DdlSyntax = BasicDdl<NamePart, QualifiedName>;
using ModelDdl = BasicDdl<std::string, std::vector<std::string>>;
enum class TypeUse { Declaration, Cast, Catalog, Signature, Literal };
const std::vector<std::string> &mysqlCastTypeNames();
struct TypeResolution { SqlType type; std::string error; };
TypeResolution resolveDeclaredType(const BasicTypeSpec<std::vector<std::string>> &, Dialect,
                                   TypeUse = TypeUse::Declaration);
std::optional<SqlType> catalogTypeFromText(Dialect,std::string);

template<class Qualified,class Column,class OutputProjection> struct BasicOutputInto {
  Qualified table;
  std::string tableVariable;
  std::vector<Column> columns;
  std::vector<OutputProjection> projections;
  SourceRange origin, tableOrigin;
};
struct Statement {
  StatementKind kind = StatementKind::Select;
  std::optional<DdlSyntax> ddl;
  std::optional<InspectionSyntax> inspection;
  std::optional<PragmaSyntax> pragma;
  std::shared_ptr<const VacuumSyntax> vacuum;
  std::shared_ptr<const TriggerSyntax> trigger;
  std::shared_ptr<const PackageSyntax> package;
  std::shared_ptr<const CopySyntax> copy;
  std::optional<TransactionSyntax> transaction;
  std::optional<PreparedSyntax> prepared;
  std::optional<RoleControl> role;
  QualifiedName routine;
  std::vector<ExprId> arguments;
  ExprId mergeOn=0;
  std::vector<MergeClause> mergeClauses;
  std::optional<TableSource> target;
  bool targetInTables = false;
  std::vector<NamePart> columns;
  InsertSource insertSource = InsertSource::None;
  bool replaceExisting = false; // MySQL REPLACE applies delete/insert semantics.
  std::vector<std::vector<ExprId>> rows;
  std::vector<Assignment> assignments;
  QueryId source = 0;
  std::vector<Projection> returning;
  std::vector<ExprId> returningInto;
  std::optional<BasicOutputInto<QualifiedName,NamePart,Projection>> outputInto;
  std::vector<QualifiedName> deleteTargets;
  std::vector<std::string> modifiers;
  ConflictClause conflict;
  std::vector<Relation> relations;
  RelationId from = 0;

  SourceRange range;
  bool distinct = false;
  bool fixedJoinOrder = false;
  std::vector<Projection> projections;
  std::vector<TableSource> tables;
  ExprId where = 0, having = 0;
  std::vector<ExprId> groupBy;
  // Empty vector: ordinary GROUP BY / no explicit grouping. A member empty
  // vector is the explicit grand-total grouping set; duplicates are retained.
  std::vector<std::vector<ExprId>> groupingSets;
  std::vector<OrderItem> orderBy;
  RowLimit limit;
  std::vector<CommonTableExpression> ctes;
  bool recursiveWith = false;
  TableQuerySyntax tableQuery = TableQuerySyntax::None;
  bool valuesQuery = false;
  bool sqlServerValueConstructor = false;
  std::vector<NamedWindowDefinition> namedWindows;
  std::optional<SetOperation> setOperation;
};

using SelectStatement =
    Statement; // Source compatibility with the first SELECT-only API.

class ASTContext {
public:
  ASTContext();
  IdentifierId intern(std::string value);
  const std::string &identifier(IdentifierId id) const;
  ExprId add(Expr node);
  const Expr &expression(ExprId id) const;
  QueryId addQuery(SelectStatement query);
  const SelectStatement &query(QueryId id) const;
  std::size_t queryCount() const { return queries_.size(); }
  std::size_t size() const { return expressions_.size() - 1; }

private:
  friend class Parser;
  Expr &mutableExpression(ExprId);
  std::vector<std::string> identifiers_;
  std::unordered_map<std::string, IdentifierId> identifierIds_;
  std::vector<Expr> expressions_;
  std::vector<SelectStatement> queries_;
};

enum class ValueType {
  Unknown,
  Null,
  Boolean,
  Integer,
  Decimal,
  String,
  Binary
};
struct ColumnDefinition {
  std::string name;
  ValueType type = ValueType::Unknown;
  std::shared_ptr<const SqlType> sqlType = {};
  // Explicit immediate schema guarantee, independent of soft inferred types.
  bool nonNullKnown=false;
};
ColumnDefinition declaredColumn(const BasicColumnSpec<std::string,std::vector<std::string>> &,Dialect);
struct TableDefinition {
  std::string database, schema, name;
  std::vector<ColumnDefinition> columns;
  bool complete = true; // false when a derived output includes unresolved '*' /
                        // unnamed expressions.
  bool arityKnown = true;
  ObjectKind kind = ObjectKind::Table;
  // Eligible non-nullable, non-deferrable primary-key knowledge for grouping dependencies.
  // Unknown differs from a known absence of an eligible key.
  bool primaryKeyKnown = false;
  std::vector<std::string> primaryKey;
  bool noDescendantsKnown = false; // PostgreSQL table has no inheritance children.
  bool rowSecurityDisabledKnown = false;
  std::vector<std::vector<std::string>> strictUniqueKeys;
  bool sessionTemporary=false;
};
ValueType typeCategory(const std::vector<std::string> &,Dialect);
struct ExternalSymbol {
  std::vector<std::string> name;
  std::uint32_t symbol = 0;
  ValueType type = ValueType::Unknown;
  std::shared_ptr<const SqlType> sqlType = {};
};
enum class RoutineKind { Unknown, Scalar, Aggregate, Window, Procedure, TableFunction };
enum class FunctionVolatility { Unknown, Immutable, Stable, Volatile };
struct RoutineDefinition {
  std::string database, schema, name;
  std::size_t minArguments = 0, maxArguments = 0;
  bool procedure = false, variadic = false;
  ValueType resultType = ValueType::Unknown;
  RoutineKind kind = RoutineKind::Unknown;
  FunctionVolatility volatility = FunctionVolatility::Unknown;
  std::shared_ptr<const SqlType> sqlType = {};
};
enum class LocalNamePolicy { PreferLocals, PreferColumns, ErrorOnConflict };
enum class SemanticEnvironment { Sql, ProceduralScalar, ProceduralCondition };
struct NameResolutionContext {
  std::string database, schema;
  Dialect dialect = Dialect::PostgreSQL;
  std::vector<ExternalSymbol> locals = {}; // Bound by the enclosing procedural scope.
  LocalNamePolicy localPolicy = LocalNamePolicy::PreferLocals;
  std::vector<RoutineDefinition> routines = {};
  SemanticEnvironment environment=SemanticEnvironment::Sql;
};
// Positive, bounded symbol enumeration; separate from DDL grammar kinds and
// negative existence proofs. Entries do not imply a live server connection.
enum class CatalogObjectKind { Database, Schema, Table, View, MaterializedView,
  Index, Sequence, Trigger, Function, Procedure, Type, Synonym, Package, Event };
struct CatalogObjectDefinition {
  CatalogObjectKind kind=CatalogObjectKind::Table;
  std::string database,schema,name,parent,detail;
};
// Providers receive dialect-normalized names. Database-specific catalogue
// matching (e.g. MySQL lower_case_table_names) belongs to the snapshot
// provider.

using CatalogNameFilter=std::function<bool(const std::string &database,const std::string &schema,
                                           const std::string &name)>;
class CatalogSnapshot {
public:
  // Completion enumeration is bounded and must never perform network I/O.
  virtual std::vector<TableDefinition> listRelations(const NameResolutionContext&,std::size_t) const {return {};}
  virtual std::vector<RoutineDefinition> listRoutines(const NameResolutionContext&,bool,std::size_t) const {return {};}
  // These overloads can push an in-memory completion predicate ahead of the result cap.
  virtual std::vector<TableDefinition> matchingRelations(const NameResolutionContext&,std::size_t,
                                                         const CatalogNameFilter &) const;
  virtual std::vector<RoutineDefinition> matchingRoutines(const NameResolutionContext&,bool,std::size_t,
                                                          const CatalogNameFilter &) const;
  virtual std::vector<CatalogObjectDefinition> listObjects(const NameResolutionContext&,CatalogObjectKind,std::size_t) const {return {};}
  virtual bool current() const {return true;}
  virtual ~CatalogSnapshot() = default;
  virtual std::optional<TableDefinition>
  findTable(const NameResolutionContext &,
            const std::vector<std::string> &name) const = 0;
  virtual bool tableLookupComplete(const NameResolutionContext &,
                                   const std::vector<std::string> &) const {
    return true;
  }
  virtual bool relationNameEquals(std::string_view a,
                                  std::string_view b) const {
    return a == b;
  }
  virtual bool columnNameEquals(std::string_view a, std::string_view b) const {
    return a == b;
  }
  virtual std::vector<RoutineDefinition>
  findRoutines(const NameResolutionContext &, const std::vector<std::string> &,
               bool) const {
    return {};
  }
  virtual bool routineLookupComplete(const NameResolutionContext &,
                                     const std::vector<std::string> &,
                                     bool) const {
    return false;
  }
};
class MemoryCatalog final : public CatalogSnapshot {
public:
  std::vector<CatalogObjectDefinition> objects;
  std::vector<CatalogObjectDefinition> listObjects(const NameResolutionContext&,CatalogObjectKind,std::size_t) const override;
  std::vector<TableDefinition> listRelations(const NameResolutionContext&,std::size_t) const override;
  std::vector<RoutineDefinition> listRoutines(const NameResolutionContext&,bool,std::size_t) const override;
  std::vector<TableDefinition> matchingRelations(const NameResolutionContext&,std::size_t,
                                                 const CatalogNameFilter &) const override;
  std::vector<RoutineDefinition> matchingRoutines(const NameResolutionContext&,bool,std::size_t,
                                                  const CatalogNameFilter &) const override;
  std::vector<TableDefinition> tables;
  std::vector<RoutineDefinition> routines;
  std::vector<RoutineDefinition> findRoutines(const NameResolutionContext &,
                                              const std::vector<std::string> &,
                                              bool) const override;
  std::optional<TableDefinition>
  findTable(const NameResolutionContext &,
            const std::vector<std::string> &name) const override;
};
struct ColumnBinding {
  std::size_t table = 0, column = 0;
  QueryId scope = 0;
};
// Owning, dialect-neutral model: no references into parser/AST/source storage.
struct ModelExpression {
  ExprKind kind = ExprKind::Null;
  SourceRange origin;
  std::string value;
  std::vector<std::string> name;
  std::vector<bool> nameQuoted;
  bool unicodeString = false;
  bool unresolvedBooleanName = false; // SQLite TRUE/FALSE may name a column.
  std::uint32_t localSymbol = 0;
  std::optional<std::size_t> outputColumn; // Resolved SELECT output alias / positional reference.
  std::vector<ExprId> operands;
  ValueType type = ValueType::Unknown;
  TypeId sqlType = 0;
  // Immutable source type for Cast and TypedLiteral. Shared by model copies.
  std::shared_ptr<const BasicTypeSpec<std::vector<std::string>>> castType = {};
  std::shared_ptr<const IntervalSpec> interval = {};
  std::optional<ColumnBinding> binding;
  std::vector<ColumnBinding> coalescedBindings;
  QueryId query = 0;
  bool distinct = false;
  std::optional<WindowSpec> window = {};
  // Resolved source routine facts. Unknown is not synonymous with scalar.
  bool routineResolved = false;
  std::shared_ptr<const SqlType> routineType = {};
  RoutineKind routineKind = RoutineKind::Unknown;
  FunctionVolatility routineVolatility = FunctionVolatility::Unknown;
  std::string routineSchema;
  std::shared_ptr<const WindowOrdering> withinGroup = {};
  std::optional<JsonConstructorSpec> jsonConstructor = {};
  std::optional<WindowSource> windowSource = {};
  bool implicitCaseElse = false;
};
struct ModelProjection {
  ExprId expression = 0;
  std::string alias;
  bool implicitAlias = false; // Binding/output name inferred without source AS.
};
struct ModelTable {
  std::vector<std::string> name;
  std::string alias;
  JoinKind join = JoinKind::First;
  ExprId condition = 0;
  std::shared_ptr<const TableDefinition> definition;
  QueryId query = 0;   // Derived query or CTE definition, if present.
  bool pseudo = false; // excluded / inserted / deleted, not a catalog table.
  QueryId workingTableOf = 0; // Owning recursive union; query points at the seed for typing.
  CteMaterialization cteMaterialization = CteMaterialization::Default;
  TableInheritance inheritance = TableInheritance::Default;
  std::optional<TableSampling> sampling = {};
  std::shared_ptr<const ModelJsonTable> jsonTable;
  std::vector<std::string> columnAliases = {};
  std::shared_ptr<const ModelPivot> pivot;
  std::shared_ptr<const ModelUnpivot> unpivot;
  std::shared_ptr<const TableDefinition> pivotInput, unpivotInput;
  std::optional<ModelRowMatch> rowMatch = {};
  std::shared_ptr<const TableDefinition> matchInput = {};
  PostgresRowFunction postgresFunction=PostgresRowFunction::None;
  std::optional<std::string> ordinalityColumn = {};
  SourceRange sourceNameOrigin;
  bool inferredFunctionAlias = false;
  std::optional<std::size_t> sourceFunctionColumnAliases = {};
};
struct ScopeSymbol {
  std::string name;
  std::size_t table = 0;
};
struct QueryScope {
  std::optional<std::size_t> parent;
  std::optional<std::vector<std::size_t>> parentTables;
  std::vector<ScopeSymbol> tables;
};
struct ModelRelation {
  RelationId left = 0, right = 0;
  std::size_t table = 0;
  JoinKind join = JoinKind::First;
  ExprId condition = 0;
  bool natural = false;
  bool lateral = false;
  bool implicitLateral = false;
  bool fixedOrder = false;
  std::vector<std::string> usingColumns;
  bool comma = false;
  std::optional<bool> sourceLateralKeyword = {};
  std::optional<JoinKind> sourceJoinKind = {};
};
struct ModelConflict {
  ConflictClause::Action action = ConflictClause::Action::None;
  std::vector<ExprId> targets;
  std::string constraint;
  ExprId targetWhere = 0, where = 0;
  std::vector<Assignment> assignments;
  std::string rowAlias;
  std::vector<std::string> columnAliases;
  std::optional<std::size_t> incomingTable; // Pseudo relation visible in UPSERT values.
};
struct ModelStatement {
  StatementKind kind = StatementKind::Select;
  std::optional<ModelDdl> ddl;
  std::optional<ModelInspection> inspection;
  std::optional<ModelPragma> pragma;
  std::shared_ptr<const ModelVacuum> vacuum;
  std::shared_ptr<const ModelTrigger> trigger;
  std::shared_ptr<const ModelPackage> package;
  std::shared_ptr<const ModelCopy> copy;
  std::optional<ModelTransaction> transaction;
  std::optional<ModelPrepared> prepared;
  std::optional<RoleControl> role;
  std::vector<std::string> routine;
  std::vector<ExprId> arguments;
  ExprId mergeOn=0;
  std::vector<ModelMergeClause> mergeClauses;
  std::string sourceFile;
  TransactionEffect transactionEffect = TransactionEffect::PreservesTransaction;
  SourceRange origin;
  std::optional<ModelTable> target, targetReference;
  std::optional<std::size_t> targetTable;
  std::vector<std::string> columns, modifiers;
  InsertSource insertSource = InsertSource::None;
  bool replaceExisting = false;
  std::vector<std::vector<ExprId>> rows;
  std::vector<Assignment> assignments;
  QueryId source = 0;
  std::vector<ModelProjection> returning;
  std::vector<ExprId> returningInto;
  std::optional<BasicOutputInto<std::vector<std::string>,std::string,ModelProjection>> outputInto;
  std::vector<std::vector<std::string>> deleteTargets;
  ModelConflict conflict;
  std::vector<ModelRelation> relations;
  RelationId from = 0, sourceFrom = 0;

  QueryId id = 0;
  NameResolutionContext context;
  bool distinct = false;
  bool fixedJoinOrder = false;
  std::vector<ModelExpression> expressions; // ID 0 reserved.
  std::vector<ModelProjection> projections;
  std::vector<ModelTable> tables;
  ExprId where = 0, having = 0;
  std::vector<ExprId> groupBy;
  // Empty vector: ordinary GROUP BY / no explicit grouping. A member empty
  // vector is the explicit grand-total grouping set; duplicates are retained.
  std::vector<std::vector<ExprId>> groupingSets;
  std::vector<OrderItem> orderBy;
  ExprId limit = 0, offset = 0;
  LimitSyntax limitSyntax = LimitSyntax::None;
  bool limitWithTies = false;
  std::optional<SelectIntoAssignment> selectInto; // Bound routine symbols, never table creation.
  QueryScope scope;
  bool catalogChecked = false;
  struct CTE {
    std::string name;
    std::vector<std::string> columns;
    QueryId query = 0;
    std::shared_ptr<const TableDefinition> definition;
    CteMaterialization materialization = CteMaterialization::Default;
  };
  std::vector<CTE> ctes;
  bool recursiveWith = false;
  TableQuerySyntax tableQuery = TableQuerySyntax::None;
  bool valuesQuery = false;
  bool sqlServerValueConstructor = false;
  struct NamedWindow { std::string name; WindowSpec specification; std::vector<ExprId> keys; SourceRange origin; std::optional<WindowSource> source = {}; };
  std::vector<NamedWindow> namedWindows;
  std::optional<SetOperation> setOperation;
};
using ModelSelect = ModelStatement;
struct QueryFeatures {
  bool aggregate=false,window=false;
  bool uncertainRoutine=false,tableFunction=false;
};
// Query-block properties; never descend into a subquery's independent block.
// Nullopt means cancellation, invalid references or the explicit visit cap.
std::optional<QueryFeatures> inspectQueryFeatures(const ModelStatement &,Dialect,
    std::size_t maxVisits=250000,std::function<bool()> shouldStop={});
// A flat owning arena: nested queries/parent scopes refer to 1-based IDs. Copy
// this entire program when retaining nested/correlated queries beyond
// Compilation.
struct ModelProgram {
  static constexpr std::uint32_t formatVersion = 1;
  Dialect sourceDialect = Dialect::PostgreSQL;
  bool mysqlNoBackslashEscapes = false, mysqlPipesAsConcat = false;
  TypeContext types;
  std::vector<ModelSelect> queries;
  std::vector<QueryId> roots;
  bool empty() const { return queries.empty(); }
  std::size_t size() const { return queries.size(); }
  const ModelSelect &at(std::size_t i) const { return queries.at(i); }
  const ModelSelect &operator[](std::size_t i) const { return queries[i]; }
  const ModelSelect &front() const { return queries.front(); }
  auto begin() const { return queries.begin(); }
  auto end() const { return queries.end(); }
};
struct Compilation {
  Compilation(Dialect dialect, std::string sql, LanguageOptions options,
              std::string file);
  Dialect dialect;
  LanguageOptions options;
  SourceBuffer source;
  NameResolutionContext names;
  std::vector<Token>
      tokens; // Includes comments for future source-preserving tooling.
  ASTContext ast;
  std::vector<SourceRange>
      identifierRanges; // Source roles for non-destructive formatting.
  std::vector<SelectStatement> statements;
  ModelProgram model;
  std::vector<Diagnostic> diagnostics;
  std::optional<std::size_t> selectArity;
  bool syntaxValid = false;
  bool ok() const { return syntaxValid && diagnostics.empty(); }
  void report(DiagnosticCode code, SourceRange range, std::string message);
};
std::unique_ptr<Compilation> compile(Dialect dialect, std::string sql,
                                     LanguageOptions options = {},
                                     const CatalogSnapshot *catalog = nullptr,
                                     NameResolutionContext names = {},
                                     std::string file = "<input>");
struct GenerationOptions {
  std::size_t maxBytes = 2 * 1024 * 1024, maxNesting = 128, maxVisits = 250000;
  std::function<bool()> shouldStop;
  struct LocalName { std::string scope, name; };
  std::vector<LocalName> localNames;
  bool compactExpressions = false;
};
struct GeneratedSql {
  std::string sql;
  std::vector<Diagnostic> diagnostics;
  bool ok() const { return diagnostics.empty(); }
};
// Generates canonical SQL solely from the owning IR. Unsupported constructs or
// unsafe cross-dialect semantics fail atomically with no partial SQL output.
GeneratedSql generateSql(const ModelProgram &, Dialect target,
                         GenerationOptions options = {});
// The resolver gets every column name or positional output reference in
// this expression. Returned components are always quoted as identifiers.
using ExpressionColumnResolver = std::function<std::optional<std::vector<std::string>>(QueryId, ExprId)>;
// An optional backend can supply an already rendered nested SELECT, without a semicolon.
using ExpressionQueryResolver = std::function<std::optional<std::string>(QueryId)>;
GeneratedSql generateExpression(const ModelProgram &, QueryId, ExprId, Dialect,
                                GenerationOptions options = {},
                                ExpressionColumnResolver columns = {},
                                ExpressionQueryResolver subqueries = {});
GeneratedSql generateTypeSql(const BasicTypeSpec<std::vector<std::string>>&,Dialect);
std::string dumpAST(const Compilation &compilation);
std::string dumpModel(const Compilation &compilation,
                      std::size_t maxBytes = 16 * 1024 * 1024);

} // namespace navidog::sql::compiler
