#include "navidog/sql/optimizer/materialized_view.hpp"
#include "navidog/sql/compiler/operator_catalog.hpp"
#include <algorithm>
#include <charconv>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace navidog::sql::optimizer {
namespace {
using namespace compiler;

struct Refusal {
  Diagnostic diagnostic;
};
[[noreturn]] void decline(SourceRange origin, const char *message,
                          DiagnosticCode code = DiagnosticCode::UnsupportedSyntax) {
  throw Refusal{{code, origin, message}};
}
void stop(const GenerationOptions &options) {
  if (options.shouldStop && options.shouldStop())
    decline({}, "materialized view matching stopped", DiagnosticCode::Stopped);
}
const ModelStatement &statement(const ModelProgram &program, QueryId id) {
  if (!id || id > program.queries.size() || program.queries.at(id - 1).id != id)
    decline({}, "invalid materialized view query reference", DiagnosticCode::InvalidQueryShape);
  return program.queries.at(id - 1);
}
const ModelExpression &expression(const ModelStatement &statement, ExprId id) {
  if (!id || id >= statement.expressions.size())
    decline(statement.origin, "invalid materialized view expression reference", DiagnosticCode::InvalidQueryShape);
  return statement.expressions.at(id);
}
using ColumnKey = std::pair<std::size_t, std::size_t>;
ColumnKey columnKey(const ColumnBinding &binding) { return {binding.table, binding.column}; }
using RelationOrder = std::vector<std::size_t>;
constexpr std::size_t maxStarRelations=5;
ColumnKey aligned(ColumnKey column,const RelationOrder &order) {
  if (!order.empty()) column.first=order.at(column.first);
  return column;
}
bool sameBaseIdentity(const TableDefinition &a, const TableDefinition &b) {
  return a.database == b.database && a.schema == b.schema && a.name == b.name;
}

void verifyBaseTables(const ModelStatement &q) {
  if (q.kind != StatementKind::Select || !q.catalogChecked || q.distinct || q.valuesQuery ||
      q.tableQuery != TableQuerySyntax::None || q.setOperation || !q.ctes.empty() || q.scope.parent ||
      q.selectInto || q.having || !q.groupingSets.empty() ||
      q.limit || q.offset || !q.orderBy.empty() || !q.namedWindows.empty() ||
      q.tables.empty() || q.tables.size()>maxStarRelations ||
      q.relations.size()!=2*q.tables.size()-1 || !q.from || q.from>q.relations.size() ||
      q.projections.empty())
    decline(q.origin, "materialized view matching requires a verified base projection or bounded join");
  for (const auto &table : q.tables) {
    if (table.query || table.pseudo || table.jsonTable || table.sampling || !table.columnAliases.empty() ||
        !table.definition || table.definition->kind != ObjectKind::Table ||
        !table.definition->complete || !table.definition->arityKnown ||
        table.definition->database.empty() || table.definition->schema.empty())
      decline(q.origin, "materialized view matching needs a verified base table identity");
  }
}
const ColumnBinding *directColumn(const ModelStatement &q, const ModelExpression &e) {
  if (e.kind != ExprKind::Name || e.localSymbol || e.outputColumn || !e.coalescedBindings.empty() ||
      !e.binding || e.binding->scope != q.id || e.binding->table >= q.tables.size())
    return nullptr;
  const auto &table = q.tables.at(e.binding->table);
  if (!table.definition || e.binding->column >= table.definition->columns.size()) return nullptr;
  return &*e.binding;
}
const ColumnDefinition &baseColumn(const ModelStatement &q, ColumnKey column) {
  return q.tables.at(column.first).definition->columns.at(column.second);
}
bool sameSqlType(SqlType a, SqlType b) {
  if (a.family == TypeFamily::Unknown || a.family == TypeFamily::Null ||
      !a.opaqueName.empty() || a.sqliteAffinity || a.arrayDimensions)
    return false;
  a.nullability = b.nullability = Nullability::Unknown;
  if (a.family == TypeFamily::Decimal && b.family == TypeFamily::Decimal &&
      !a.unsignedInteger.value_or(false) && !b.unsignedInteger.value_or(false))
    a.unsignedInteger = b.unsignedInteger = false;
  return a == b;
}
bool sameColumnType(const ColumnDefinition &a, const ColumnDefinition &b) {
  return a.sqlType && b.sqlType && sameSqlType(*a.sqlType, *b.sqlType);
}
bool exactInteger(const SqlType &type) {
  return type.family == TypeFamily::Integer && type.unsignedInteger == false &&
      (type.bits == 16 || type.bits == 32 || type.bits == 64) &&
      type.flavor.empty() && type.opaqueName.empty() && !type.arrayDimensions && !type.sqliteAffinity;
}
using JoinKey = std::pair<ColumnKey, ColumnKey>;
std::set<JoinKey> joinKeys(const ModelProgram &program, const ModelStatement &q,
                          const GenerationOptions &options,const RelationOrder &order={}) {
  std::set<JoinKey> keys;
  std::size_t visits = 0;
  std::set<RelationId> seen;
  const auto bit=[](std::size_t table) { return std::size_t{1}<<table; };
  const auto collect=[&](const auto &self,RelationId id)->std::size_t {
    stop(options);
    if (!id || id>q.relations.size() || !seen.insert(id).second)
      decline(q.origin,"invalid materialized view join tree",DiagnosticCode::InvalidQueryShape);
    const auto &relation=q.relations.at(id-1);
    if (relation.natural || relation.lateral || relation.implicitLateral || !relation.usingColumns.empty())
      decline(q.origin,"materialized view joins require independent inputs and an explicit ON predicate");
    if (!relation.left && !relation.right && relation.table<q.tables.size())
      return bit(relation.table);
    if (!relation.left || !relation.right || relation.join!=JoinKind::Inner || !relation.condition)
      decline(q.origin,"materialized view matching needs INNER JOIN inputs with an ON equality");
    const auto leftInput=self(self,relation.left),rightInput=self(self,relation.right);
    if (leftInput&rightInput)
      decline(q.origin,"materialized view join inputs overlap",DiagnosticCode::InvalidQueryShape);
    std::vector<ExprId> pending{relation.condition};
    while (!pending.empty()) {
      stop(options);
      const auto &e=expression(q,pending.back());
      pending.pop_back();
      if (++visits>127)
        decline(e.origin,"materialized view join exceeds the matching budget",DiagnosticCode::ResourceLimit);
      if (e.kind!=ExprKind::Binary || e.operands.size()!=2)
        decline(e.origin,"materialized view joins require conjunctive integer equality keys");
      if (e.value=="AND") {
        pending.insert(pending.end(),e.operands.begin(),e.operands.end());
        continue;
      }
      const auto &left=expression(q,e.operands[0]),&right=expression(q,e.operands[1]);
      const auto *a=directColumn(q,left),*b=directColumn(q,right);
      if (e.value!="=" || !a || !b ||
          !(((bit(a->table)&leftInput) && (bit(b->table)&rightInput)) ||
            ((bit(b->table)&leftInput) && (bit(a->table)&rightInput))) ||
          !exactInteger(program.types.get(left.sqlType)) ||
          !sameSqlType(program.types.get(left.sqlType),program.types.get(right.sqlType)))
        decline(e.origin,"materialized view joins require equal signed integer types across inputs");
      const auto first=aligned(columnKey(*a),order),second=aligned(columnKey(*b),order);
      keys.emplace(std::min(first,second),std::max(first,second));
    }
    return leftInput|rightInput;
  };
  if (collect(collect,q.from)!=(bit(q.tables.size())-1) || seen.size()!=q.relations.size())
    decline(q.origin,"materialized view join tree does not bind each base input",DiagnosticCode::InvalidQueryShape);
  return keys;
}
bool verifiedStar(const ModelStatement &q,const std::set<JoinKey> &keys,const RelationOrder &order={}) {
  if (q.tables.size()<3 || keys.size()<q.tables.size()-1) return false;
  for (const auto &table:q.tables)
    if (!table.definition->noDescendantsKnown || !table.definition->rowSecurityDisabledKnown)
      return false;
  const auto first=keys.begin();
  for (const auto center:{first->first.first,first->second.first}) {
    std::map<std::size_t,std::set<std::size_t>> dimensions;
    for (const auto &[a,b]:keys) {
      if (a.first==center) dimensions[b.first].insert(b.second);
      else if (b.first==center) dimensions[a.first].insert(a.second);
      else break;
    }
    const auto allEdgesMeetCenter=std::all_of(keys.begin(),keys.end(),[&](const auto &key) {
      return key.first.first==center || key.second.first==center;
    });
    if (!allEdgesMeetCenter || dimensions.size()+1!=q.tables.size()) continue;
    if (std::all_of(dimensions.begin(),dimensions.end(),[&](const auto &dim) {
          const auto table=aligned({dim.first,0},order).first;
          const auto &definition=*q.tables.at(table).definition;
          return definition.primaryKeyKnown && !definition.primaryKey.empty() &&
              std::all_of(definition.primaryKey.begin(),definition.primaryKey.end(),[&](const auto &name) {
                return std::any_of(dim.second.begin(),dim.second.end(),[&](auto column) {
                  return definition.columns.at(column).name==name;
                });
              });
        }))
      return true;
  }
  return false;
}
std::set<ColumnKey> groupColumns(const ModelProgram &program, const ModelStatement &q,
                                   const GenerationOptions &options,const RelationOrder &order={}) {
  std::set<ColumnKey> result;
  for (const auto group : q.groupBy) {
    stop(options);
    const auto &value = expression(q, group);
    const auto *binding = directColumn(q, value);
    if (!binding || !exactInteger(program.types.get(value.sqlType)))
      decline(value.origin, "materialized aggregation needs direct signed integer grouping columns");
    result.insert(aligned(columnKey(*binding),order));
  }
  return result;
}
enum class Aggregate { CountRows, CountColumn, Min, Max, SumBigInt };
using AggregateKey = std::pair<Aggregate, ColumnKey>;

std::optional<AggregateKey> aggregateKey(const ModelProgram &program, const ModelStatement &q,
                                         const ModelExpression &e) {
  if (e.kind != ExprKind::Call || e.distinct || e.window || e.operands.size() != 1 ||
      (e.routineResolved && (e.routineSchema != "pg_catalog" || e.routineKind != RoutineKind::Aggregate)))
    return {};
  const auto builtin = builtinOperator(Dialect::PostgreSQL, e.name, e.nameQuoted, e.operands.size());
  if (!builtin || builtin->kind != RoutineKind::Aggregate) return {};
  const auto &argument = expression(q, e.operands.front());
  const auto &result = program.types.get(e.sqlType);
  const auto *binding = directColumn(q, argument);
  if (builtin->name == "COUNT" && exactInteger(result) && result.bits == 64) {
    if (argument.kind == ExprKind::Star && argument.name.size() <= 1)
      return AggregateKey{Aggregate::CountRows, {0,0}};
    if (binding)
      return AggregateKey{Aggregate::CountColumn, columnKey(*binding)};
  }
  const auto &input = program.types.get(argument.sqlType);
  if (!binding || !exactInteger(input)) return {};
  if (builtin->name == "MIN" && sameSqlType(result, input))
    return AggregateKey{Aggregate::Min, columnKey(*binding)};
  if (builtin->name == "MAX" && sameSqlType(result, input))
    return AggregateKey{Aggregate::Max, columnKey(*binding)};
  if (builtin->name == "SUM" && input.bits == 64 && result.family == TypeFamily::Decimal &&
      result.opaqueName.empty() && !result.arrayDimensions && !result.precision && !result.scale)
    return AggregateKey{Aggregate::SumBigInt, columnKey(*binding)};
  return {};
}

ExprId appendExpression(ModelStatement &q, ModelExpression entry) {
  const auto id = static_cast<ExprId>(q.expressions.size());
  q.expressions.push_back(std::move(entry));
  return id;
}
ExprId rolledAggregate(ModelStatement &answer, const ModelExpression &original, Aggregate kind,
                       std::size_t slot, const TableDefinition &storage) {
  ModelExpression column;
  column.origin = original.origin;
  column.kind = ExprKind::Name;
  column.name = {storage.columns.at(slot).name};
  column.nameQuoted = {true};
  column.binding = ColumnBinding{0, slot, answer.id};
  const auto argument = appendExpression(answer, std::move(column));
  ModelExpression reduce;
  reduce.kind = ExprKind::Call;
  reduce.origin = original.origin;
  reduce.name = {"pg_catalog", kind == Aggregate::Min ? "min" : kind == Aggregate::Max ? "max" : "sum"};
  reduce.operands = {argument};
  const auto sum = appendExpression(answer, std::move(reduce));
  if (kind != Aggregate::CountRows && kind != Aggregate::CountColumn) return sum;

  // SUM(bigint) returns numeric. Cast the exact total to the original COUNT
  // type, with zero for a global aggregate with no surviving materialized row.
  ModelExpression zero;
  zero.origin = original.origin;
  zero.kind = ExprKind::Number;
  zero.value = "0";
  const auto zeroId = appendExpression(answer, std::move(zero));
  ModelExpression missing;
  missing.origin = original.origin;
  missing.kind = ExprKind::Call;
  missing.name = {"coalesce"};
  missing.operands = {sum, zeroId};
  const auto nonNull = appendExpression(answer, std::move(missing));
  auto bigint = std::make_shared<BasicTypeSpec<std::vector<std::string>>>();
  bigint->name = {"bigint"};
  ModelExpression count;
  count.kind = ExprKind::Cast;
  count.origin = original.origin;
  count.castType = std::move(bigint);
  count.operands = {nonNull};
  count.type = original.type;
  count.sqlType = original.sqlType;
  return appendExpression(answer, std::move(count));
}

// Predicate domains describe rows for which the predicate is TRUE. A NULL can
// survive IS NULL, but never an ordinary integer comparison.
struct Domain {
  std::int64_t min = std::numeric_limits<std::int32_t>::min();
  std::int64_t max = std::numeric_limits<std::int32_t>::max();
  bool null = true;
  bool empty() const { return !null && min > max; }
  bool subsetOf(const Domain &other) const {
    return (!null || other.null) && (min > max || (min >= other.min && max <= other.max));
  }
  void intersect(const Domain &other) {
    min = std::max(min, other.min);
    max = std::min(max, other.max);
    null &= other.null;
  }
};
struct Predicate {
  ExprId id;
  ColumnKey column;
  Domain domain;
};
struct Filter {
  std::vector<Predicate> predicates;
  std::map<ColumnKey, Domain> domains;
  Domain forColumn(ColumnKey column) const {
    const auto it = domains.find(column);
    return it == domains.end() ? Domain{} : it->second;
  }
};
std::optional<std::int64_t> literal(const ModelStatement &q, ExprId id) {
  const auto *number = &expression(q, id);
  bool negative = false;
  if (number->kind == ExprKind::Unary && number->operands.size() == 1 &&
      (number->value == "-" || number->value == "+")) {
    negative = number->value == "-";
    number = &expression(q, number->operands.front());
  }
  if (number->kind != ExprKind::Number || number->value.empty())
    return {};
  std::int32_t value = 0;
  const auto *end = number->value.data() + number->value.size();
  const auto parsed = std::from_chars(number->value.data(), end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != end || value < 0)
    return {};
  return negative ? -std::int64_t(value) : std::int64_t(value);
}
Filter filter(const ModelStatement &q,const GenerationOptions &options,const RelationOrder &order={}) {
  Filter result;
  if (!q.where) return result;
  std::vector<ExprId> work{q.where};
  std::size_t visits = 0;
  while (!work.empty()) {
    stop(options);
    const auto id = work.back();
    work.pop_back();
    const auto &e = expression(q, id);
    if (++visits > 127)
      decline(e.origin, "materialized view filter exceeds the matching budget", DiagnosticCode::ResourceLimit);
    if (e.kind == ExprKind::Binary && e.value == "AND" && e.operands.size() == 2) {
      work.push_back(e.operands[1]);
      work.push_back(e.operands[0]);
      continue;
    }
    Domain domain;
    const ColumnBinding *column = nullptr;
    if (e.kind == ExprKind::IsNull && e.operands.size() == 1 &&
        (e.value == "IS NULL" || e.value == "IS NOT NULL")) {
      column = directColumn(q, expression(q, e.operands.front()));
      if (e.value == "IS NULL") domain.max = domain.min - 1;
      else domain.null = false;
    } else if (e.kind == ExprKind::Binary && e.operands.size() == 2) {
      auto op = e.value;
      column = directColumn(q, expression(q, e.operands[0]));
      auto bound = literal(q, e.operands[1]);
      if (!column) {
        column = directColumn(q, expression(q, e.operands[1]));
        bound = literal(q, e.operands[0]);
        if (op == "<") op = ">";
        else if (op == ">") op = "<";
        else if (op == "<=") op = ">=";
        else if (op == ">=") op = "<=";
      }
      if (bound && column) {
        domain.null = false;
        if (op == "=") domain.min = domain.max = *bound;
        else if (op == "<") domain.max = *bound - 1;
        else if (op == "<=") domain.max = *bound;
        else if (op == ">") domain.min = *bound + 1;
        else if (op == ">=") domain.min = *bound;
        else column = nullptr;
      } else column = nullptr;
    }
    if (!column)
      decline(e.origin, "view and query filters require conjunctive integer bounds or NULL tests");
    const auto type = baseColumn(q, columnKey(*column)).sqlType;
    if (!type || type->family != TypeFamily::Integer || type->bits != 32 ||
        type->unsignedInteger != false || type->arrayDimensions || !type->opaqueName.empty())
      decline(e.origin, "materialized view bounds require a known PostgreSQL integer column");
    const auto key=aligned(columnKey(*column),order);
    result.domains[key].intersect(domain);
    result.predicates.push_back({id, key, domain});
  }
  return result;
}

// Import the checked filter in the query's relation order and type context.
ExprId importFilter(const ModelProgram &definition, const ModelStatement &view, ExprId id,
                    ModelProgram &base, ModelStatement &answer,
                    const GenerationOptions &options,const RelationOrder &order) {
  stop(options);
  auto predicate = expression(view, id);
  predicate.origin = answer.origin;
  predicate.sqlType = base.types.intern(definition.types.get(predicate.sqlType));
  if (const auto *column = directColumn(view, predicate)) {
    const auto key=aligned(columnKey(*column),order);
    const auto &table=answer.tables.at(key.first);
    predicate.name={table.alias.empty()?table.definition->name:table.alias,
                    table.definition->columns.at(key.second).name};
    predicate.nameQuoted={true,true};
    predicate.binding=ColumnBinding{key.first,key.second,answer.id};
  }
  for (auto &operand : predicate.operands)
    operand=importFilter(definition,view,operand,base,answer,options,order);
  const auto next = static_cast<ExprId>(answer.expressions.size());
  answer.expressions.push_back(std::move(predicate));
  return next;
}
GeneratedSql compensatingArm(const ModelProgram &query, const ModelProgram &definition,
                             const ModelStatement &view, const std::vector<ExprId> &extraGroups,
                             const GenerationOptions &options,const RelationOrder &order) {
  auto base = query;
  auto &answer = base.queries.front();
  for (const auto group : extraGroups) answer.projections.push_back({group, {}});
  for(auto &table:answer.tables)
    table.name={table.definition->schema,table.definition->name};
  ModelExpression remainder;
  remainder.kind = ExprKind::TruthTest;
  remainder.origin = answer.origin;
  remainder.value = "IS NOT TRUE";
  remainder.operands={importFilter(definition,view,view.where,base,answer,options,order)};
  remainder.type = ValueType::Boolean;
  remainder.sqlType = base.types.intern(categoryType(ValueType::Boolean, Nullability::NotNull));
  const auto remainderId = static_cast<ExprId>(answer.expressions.size());
  answer.expressions.push_back(std::move(remainder));
  if (answer.where) {
    ModelExpression conjunction;
    conjunction.kind = ExprKind::Binary;
    conjunction.origin = answer.origin;
    conjunction.value = "AND";
    conjunction.operands = {answer.where, remainderId};
    conjunction.type = ValueType::Boolean;
    conjunction.sqlType = answer.expressions.at(answer.where).sqlType;
    answer.where = static_cast<ExprId>(answer.expressions.size());
    answer.expressions.push_back(std::move(conjunction));
  } else {
    answer.where = remainderId;
  }
  return generateSql(base, Dialect::PostgreSQL, options);
}

GeneratedSql reducePartialGroups(const GeneratedSql &joined, const ModelProgram &query,
                                 const ModelStatement &requested, std::size_t width,
                                 const std::set<std::size_t> &groupSlots, const MemoryCatalog &catalog,
                                 const GenerationOptions &options) {
  // Give the intermediate columns unique names while retaining source output
  // names, including duplicate labels, in the final projection.
  std::string columns;
  for (std::size_t i = 0; i < width; ++i) {
    stop(options);
    if (i) columns += ", ";
    columns += "state_" + std::to_string(i);
  }
  const auto prefix = "SELECT " + columns + " FROM (";
  const auto suffix = ") AS navidog_partial_state (" + columns + ");";
  if (joined.sql.empty() || joined.sql.back() != ';')
    decline(requested.origin, "aggregate compensation requires a complete UNION ALL", DiagnosticCode::InvalidQueryShape);
  if (prefix.size() + suffix.size() > options.maxBytes ||
      joined.sql.size() - 1 > options.maxBytes - prefix.size() - suffix.size())
    decline(requested.origin, "aggregate compensation exceeds the SQL generation budget", DiagnosticCode::ResourceLimit);
  std::string wrapped(prefix);
  wrapped.append(joined.sql, 0, joined.sql.size() - 1);
  wrapped += suffix;
  LanguageOptions language;
  language.shouldStop = options.shouldStop;
  auto checked = compile(Dialect::PostgreSQL, std::move(wrapped), language, &catalog, requested.context);
  if (!checked->ok()) return {{}, std::move(checked->diagnostics)};

  auto &program = checked->model;
  auto &outer = program.queries.at(program.roots.front() - 1);
  if (outer.tables.size() != 1 || !outer.tables.front().definition ||
      outer.tables.front().definition->columns.size() != width || outer.projections.size() != width)
    decline(requested.origin, "aggregate compensation result has the wrong state width", DiagnosticCode::InvalidQueryShape);
  const auto &state = *outer.tables.front().definition;
  const auto inputs = std::move(outer.projections);
  outer.projections.clear();
  for (std::size_t i = 0; i < requested.projections.size(); ++i) {
    stop(options);
    const auto &output = requested.projections[i];
    const auto &value = expression(requested, output.expression);
    const auto &type = query.types.get(value.sqlType);
    const auto &actual = program.types.get(expression(outer, inputs[i].expression).sqlType);
    if (!sameSqlType(type, actual)) {
      throw Refusal{{DiagnosticCode::UnsupportedSyntax, value.origin,
          "aggregate compensation cannot prove intermediate column " + std::to_string(i + 1) +
          " (expected " + typeFamilyName(type.family) + ", inferred " + typeFamilyName(actual.family) + ")"}};
    }
    ExprId result;
    if (const auto key = aggregateKey(query, requested, value)) {
      ModelExpression typed;
      typed.origin = value.origin;
      typed.type = value.type;
      typed.sqlType = program.types.intern(type);
      result = rolledAggregate(outer, typed, key->first, i, state);
    } else {
      result = inputs[i].expression;
    }
    outer.projections.push_back({result, output.alias.empty() ? value.name.back() : output.alias});
  }
  for (const auto slot : groupSlots) outer.groupBy.push_back(inputs.at(slot).expression);
  return generateSql(program, Dialect::PostgreSQL, options);
}

struct MatchEvidence {
  MaterializationPattern pattern=MaterializationPattern::ProjectFilter;
  bool compensates=false;
};
std::optional<ModelProgram> expandFilteredScan(const ModelProgram &model,const GenerationOptions &options) {
  const auto &query=statement(model,model.roots.front());
  if (query.tables.size()!=1 || query.projections.size()!=1 || !query.where ||
      !query.groupBy.empty() || !query.tables.front().definition)
    return {};
  const auto &star=expression(query,query.projections.front().expression);
  if (star.kind!=ExprKind::Star || !star.name.empty()) return {};
  verifyBaseTables(query);
  const auto &columns=query.tables.front().definition->columns;
  if (columns.empty() || columns.size()>1024 || columns.size()+query.expressions.size()>4096)
    decline(query.origin,"materialized filtered scan exceeds the projected column budget",DiagnosticCode::ResourceLimit);
  auto expanded=model;
  auto &answer=expanded.queries.front();
  answer.projections.clear();
  for (std::size_t i=0;i<columns.size();++i) {
    stop(options);
    const auto &column=columns[i];
    if (!column.sqlType)
      decline(star.origin,"materialized filtered scan needs a declared type for every column");
    ModelExpression field;
    field.kind=ExprKind::Name;
    field.origin=star.origin;
    field.name={column.name};
    field.nameQuoted={true};
    field.binding=ColumnBinding{0,i,answer.id};
    field.type=column.type;
    field.sqlType=expanded.types.intern(*column.sqlType);
    answer.projections.push_back({static_cast<ExprId>(answer.expressions.size()),column.name});
    answer.expressions.push_back(std::move(field));
  }
  return expanded;
}
std::optional<ModelProgram> flattenProjectedStar(const ModelProgram &model,const GenerationOptions &options) {
  const auto &outer=statement(model,model.roots.front());
  if (model.size()!=2 || outer.kind!=StatementKind::Select || !outer.catalogChecked ||
      outer.tables.size()!=1 || !outer.tables.front().query || outer.relations.size()!=1 ||
      outer.from!=1 || outer.scope.parent || outer.selectInto || outer.distinct || outer.having ||
      outer.valuesQuery || outer.setOperation || !outer.ctes.empty() || !outer.groupingSets.empty() ||
      !outer.namedWindows.empty() || !outer.orderBy.empty() || outer.limit || outer.offset ||
      outer.projections.empty())
    return {};
  const auto &derived=outer.tables.front();
  const auto &relation=outer.relations.front();
  if (!derived.name.empty() || !derived.definition || derived.alias.empty() ||
      !derived.definition->complete || !derived.definition->arityKnown ||
      derived.cteMaterialization!=CteMaterialization::Default ||
      relation.left || relation.right || relation.table || relation.lateral || relation.implicitLateral)
    return {};
  const auto &inner=statement(model,derived.query);
  if (inner.id==outer.id || inner.tables.size()<3 || inner.where || !inner.groupBy.empty() ||
      inner.projections.size()!=derived.definition->columns.size())
    return {};
  verifyBaseTables(inner);
  if (outer.expressions.size()+inner.expressions.size()>4096)
    decline(outer.origin,"derived star projection exceeds the matching budget",DiagnosticCode::ResourceLimit);
  for (const auto &projected:inner.projections)
    if (!directColumn(inner,expression(inner,projected.expression))) return {};

  ModelProgram flattened;
  flattened.sourceDialect=model.sourceDialect;
  flattened.types=model.types;
  flattened.roots={1};
  flattened.queries={inner};
  auto &answer=flattened.queries.front();
  answer.id=1;
  answer.origin=outer.origin;
  answer.projections.clear();
  for (auto &item:answer.expressions)
    if (item.binding && item.binding->scope==inner.id) item.binding->scope=answer.id;
  std::vector<ExprId> imported(outer.expressions.size());
  const auto import=[&](const auto &self,ExprId id,std::size_t depth)->ExprId {
    stop(options);
    const auto &source=expression(outer,id);
    if (depth>=std::min<std::size_t>(128,options.maxNesting))
      decline(source.origin,"derived star expression exceeds the matching depth",DiagnosticCode::ResourceLimit);
    if (auto cached=imported[id]) return cached;
    if (const auto *field=directColumn(outer,source)) {
      const auto projected=inner.projections.at(field->column).expression;
      if (!sameSqlType(model.types.get(source.sqlType),model.types.get(expression(inner,projected).sqlType)))
        decline(source.origin,"derived star projection changes the underlying column type");
      return imported[id]=projected;
    }
    switch (source.kind) {
    case ExprKind::Call: case ExprKind::Number: case ExprKind::Unary:
    case ExprKind::Binary: case ExprKind::IsNull:
      break;
    case ExprKind::Star:
      if (source.name.empty()) break;
      [[fallthrough]];
    default:
      decline(source.origin,"derived star query requires direct fields and native aggregate expressions");
    }
    if (source.query || source.window || source.binding || source.outputColumn || source.localSymbol)
      decline(source.origin,"derived star query uses a dependent expression");
    auto copy=source;
    for (auto &operand:copy.operands) operand=self(self,operand,depth+1);
    const auto next=static_cast<ExprId>(answer.expressions.size());
    answer.expressions.push_back(std::move(copy));
    return imported[id]=next;
  };
  for (const auto &projected:outer.projections) {
    auto projection=projected;
    const auto &expressionBefore=expression(outer,projected.expression);
    if (projection.alias.empty() && !expressionBefore.name.empty())
      projection.alias=expressionBefore.name.back();
    projection.expression=import(import,projected.expression,0);
    answer.projections.push_back(std::move(projection));
  }
  for (auto group:outer.groupBy) answer.groupBy.push_back(import(import,group,0));
  answer.where=outer.where?import(import,outer.where,0):0;
  return flattened;
}
GeneratedSql match(const ModelProgram &input,const ModelProgram &definition,const TableDefinition &storage,
                   MaterializationSnapshot snapshot, GenerationOptions options,
                   MaterializationCoverage coverage,MatchEvidence *evidence=nullptr) {
  stop(options);
  if (snapshot != MaterializationSnapshot::Equivalent)
    decline({}, "materialized view snapshot and row visibility have not been verified");
  if (input.sourceDialect != Dialect::PostgreSQL || definition.sourceDialect != Dialect::PostgreSQL ||
      input.roots.size()!=1 || input.queries.empty() || input.queries.size()>2 || definition.roots.size()!=1 ||
      definition.queries.size() != 2)
    decline({}, "materialized view matching requires one PostgreSQL query and one view definition");
  const auto flattened=flattenProjectedStar(input,options);
  if (!flattened && input.size()!=1)
    decline({},"materialized view matching needs a direct query or a verified star projection");
  const auto expanded=flattened?std::nullopt:expandFilteredScan(input,options);
  const auto &query=flattened?*flattened:expanded?*expanded:input;
  const auto &requested = statement(query, query.roots.front());
  const auto &create = statement(definition, definition.roots.front());
  if (create.kind != StatementKind::Create || !create.ddl || !create.source ||
      create.ddl->object != ObjectKind::MaterializedView || create.ddl->withNoData || create.ddl->ifNotExists ||
      !storage.complete || !storage.arityKnown || storage.kind != ObjectKind::MaterializedView ||
      storage.name.empty() || storage.schema.empty() || storage.database.empty())
    decline(create.origin, "materialized view definition and verified storage identity do not match");
  const auto &name = create.ddl->name;
  if (name.empty() || name.size()>3 || name.back()!=storage.name ||
      (name.size()>1 ? name[name.size()-2] : create.context.schema)!=storage.schema ||
      (name.size()==3 ? name.front() : create.context.database)!=storage.database)
    decline(create.origin, "materialized view definition and verified storage identity do not match");
  const auto &view = statement(definition, create.source);
  verifyBaseTables(requested);
  verifyBaseTables(view);
  if (requested.tables.size() != view.tables.size())
    decline(requested.origin, "query and materialization need the same base relations");
  const auto sameBase = [&](std::size_t requestSlot, std::size_t viewSlot) {
    const auto &queryTable = requested.tables[requestSlot];
    const auto &viewTable = view.tables[viewSlot];
    const auto &queryBase = *queryTable.definition;
    const auto &viewBase = *viewTable.definition;
    return queryBase.database == storage.database && sameBaseIdentity(queryBase, viewBase) &&
        queryBase.columns.size() == viewBase.columns.size() && queryTable.inheritance == viewTable.inheritance;
  };
  const auto relations = requested.tables.size();
  RelationOrder toView(relations),toRequest(relations,relations);
  for (std::size_t requestSlot=0;requestSlot<relations;++requestSlot) {
    std::vector<std::size_t> matches;
    for (std::size_t viewSlot=0;viewSlot<relations;++viewSlot)
      if (sameBase(requestSlot,viewSlot)) matches.push_back(viewSlot);
    if (matches.size()>1 && relations<=2 && sameBase(requestSlot,requestSlot))
      matches={requestSlot};
    if (matches.size()!=1 || toRequest[matches.front()]!=relations)
      decline(requested.origin,"query and materialization need equivalent base relations and inheritance");
    toView[requestSlot]=matches.front();
    toRequest[matches.front()]=requestSlot;
  }
  const auto queryJoins = joinKeys(query, requested, options);
  if (queryJoins!=joinKeys(definition,view,options,toRequest))
    decline(requested.origin, "query and materialized view use different join keys");
  if (view.projections.size() != storage.columns.size() ||
      (!create.ddl->columns.empty() && create.ddl->columns.size() != storage.columns.size()) ||
      requested.projections.size() > 1024 || storage.columns.size() > 1024 ||
      requested.expressions.size() > 4096)
    decline(create.origin, "materialized view output shape is unverified or exceeds the matching budget");
  const auto viewGroups=groupColumns(definition,view,options,toRequest);
  const auto requestedGroups = groupColumns(query, requested, options);
  const bool rollup = !viewGroups.empty() ||
      std::any_of(view.projections.begin(),view.projections.end(),[&](const auto &projection){
        return aggregateKey(definition,view,expression(view,projection.expression)).has_value();
      });
  if (relations>2 && (!rollup || !verifiedStar(requested,queryJoins) || !verifiedStar(view,queryJoins,toView)))
    decline(requested.origin,"multi-table matching requires an aggregate tile on a verified fact and dimension star");
  if (!std::includes(viewGroups.begin(), viewGroups.end(), requestedGroups.begin(), requestedGroups.end()))
    decline(requested.origin, "materialized view does not group by every requested column");
  const auto comparableBaseColumn = [&](ColumnKey column) {
    const auto &base=baseColumn(view,aligned(column,toView));
    const auto &original = baseColumn(requested, column);
    return base.name == original.name && sameColumnType(base, original);
  };
  for (const auto &join : queryJoins)
    if (!comparableBaseColumn(join.first) || !comparableBaseColumn(join.second))
      decline(requested.origin, "materialized join keys do not match the verified base columns");
  for(const auto &group:viewGroups)
    if(!comparableBaseColumn(group))
      decline(requested.origin,"materialized grouping key does not match the verified base column");
  std::map<ColumnKey, std::size_t> slots;
  std::map<AggregateKey, std::size_t> aggregates;
  std::set<std::string> names;
  for (std::size_t i = 0; i < view.projections.size(); ++i) {
    stop(options);
    const auto &projection = view.projections[i];
    const auto &value = expression(view, projection.expression);
    const auto *binding = directColumn(view, value);
    auto aggregate = rollup ? aggregateKey(definition, view, value) : std::nullopt;
    if ((!binding && !aggregate) || value.name.empty())
      decline(value.origin, "materialized view needs direct columns or supported native aggregates");
    const auto &materialized = storage.columns[i];
    const auto &expectedName = create.ddl->columns.empty()
        ? projection.alias.empty() ? value.name.back() : projection.alias : create.ddl->columns[i];
    if (materialized.name != expectedName || !names.insert(materialized.name).second || !materialized.sqlType ||
        !sameSqlType(definition.types.get(value.sqlType), *materialized.sqlType))
      decline(value.origin, "materialized view column names or types do not match the verified projection");
    if (binding) {
      const auto key=aligned(columnKey(*binding),toRequest);
      if (!comparableBaseColumn(key) || (rollup && !viewGroups.count(key)))
        decline(value.origin, "materialized view output is not an equivalent base column");
      slots.emplace(key, i);
    } else {
      if(aggregate->first != Aggregate::CountRows) {
        aggregate->second=aligned(aggregate->second,toRequest);
        if(!comparableBaseColumn(aggregate->second))
          decline(value.origin, "materialized aggregate input does not match the query's base column");
      }
      aggregates.emplace(*aggregate, i);
    }
  }
  const auto queryFilter = filter(requested, options);
  const auto viewFilter=filter(view,options,toRequest);
  bool requiresCompensation = false;
  bool overlaps = true;
  for (const auto &[column, restriction] : viewFilter.domains) {
    if (!comparableBaseColumn(column))
      decline(requested.origin, "materialized view filter column does not match the base relation");
    auto common = queryFilter.forColumn(column);
    requiresCompensation |= !common.subsetOf(restriction);
    common.intersect(restriction);
    overlaps &= !common.empty();
  }
  if (requiresCompensation) {
    if (coverage != MaterializationCoverage::AllowPartial)
      decline(requested.origin, "query predicate does not imply the materialized view predicate");
    if (!overlaps || std::any_of(queryFilter.domains.begin(), queryFilter.domains.end(),
                               [](const auto &column) { return column.second.empty(); }))
      decline(requested.origin, "query and the materialized view have no proved overlapping domain");
  }

  auto rewritten = query;
  auto &answer = rewritten.queries.front();
  auto rebind = [&](ExprId id) {
    auto &e = answer.expressions.at(id);
    const auto *binding = directColumn(requested, requested.expressions.at(id));
    if (!binding)
      decline(e.origin, "materialized view rewrite needs direct projected columns");
    const auto position = slots.find(columnKey(*binding));
    if (position == slots.end())
      decline(e.origin, "materialized view does not store a required column");
    e.name = {storage.columns.at(position->second).name};
    e.nameQuoted = {true};
    e.binding = ColumnBinding{0, position->second, answer.id};
  };
  std::map<ColumnKey, std::size_t> projectedGroups;
  std::set<std::size_t> groupSlots;
  for (std::size_t i = 0; i < answer.projections.size(); ++i) {
    stop(options);
    auto &projection = answer.projections[i];
    const auto &source = expression(requested, projection.expression);
    if (const auto key = rollup ? aggregateKey(query, requested, source) : std::nullopt) {
      const auto column = aggregates.find(*key);
      if (column == aggregates.end() || !storage.columns.at(column->second).sqlType ||
          !sameSqlType(query.types.get(source.sqlType), *storage.columns.at(column->second).sqlType))
        decline(source.origin, "materialized view does not store the required aggregate state");
      projection.expression = rolledAggregate(answer, source, key->first, column->second, storage);
    } else {
      const auto *binding = directColumn(requested, source);
      if (!binding || (rollup && !requestedGroups.count(columnKey(*binding))))
        decline(source.origin, "materialized rewrite needs a direct column or supported aggregate");
      if (requiresCompensation && rollup) {
        projectedGroups.emplace(columnKey(*binding), i);
        groupSlots.insert(i);
      }
      rebind(projection.expression);
    }
    if (projection.alias.empty())
      projection.alias = source.name.back();
  }
  std::vector<ExprId> extraGroups;
  for (auto key : answer.groupBy) {
    if (requiresCompensation && rollup) {
      const auto &binding = *directColumn(requested, expression(requested, key));
      const auto [slot, missing] = projectedGroups.try_emplace(columnKey(binding), answer.projections.size());
      if (missing) {
        if (answer.projections.size() >= 1024)
          decline(requested.origin, "aggregate compensation exceeds the output column budget", DiagnosticCode::ResourceLimit);
        extraGroups.push_back(key);
        answer.projections.push_back({key, {}});
      }
      groupSlots.insert(slot->second);
    }
    rebind(key);
  }
  answer.where = 0;
  for (const auto &predicate : queryFilter.predicates) {
    stop(options);
    if (viewFilter.forColumn(predicate.column).subsetOf(predicate.domain)) continue;
    if (rollup && !viewGroups.count(predicate.column))
      decline(requested.origin, "a remaining filter would change rows within a materialized group");
    const auto &expression = requested.expressions.at(predicate.id);
    for (auto operand : expression.operands)
      if (directColumn(requested, requested.expressions.at(operand)))
        rebind(operand);
    if (!answer.where) answer.where = predicate.id;
    else {
      ModelExpression conjunction;
      conjunction.kind = ExprKind::Binary;
      conjunction.value = "AND";
      conjunction.operands = {answer.where, predicate.id};
      conjunction.type = ValueType::Boolean;
      conjunction.sqlType = requested.expressions.at(requested.where).sqlType;
      answer.where = static_cast<ExprId>(answer.expressions.size());
      answer.expressions.push_back(std::move(conjunction));
    }
  }
  ModelTable table;
  table.name = {storage.schema, storage.name};
  table.definition = std::make_shared<const TableDefinition>(storage);
  answer.tables = {std::move(table)};
  answer.scope.tables = {{storage.name, 0}};
  answer.relations = {ModelRelation{}};
  answer.from = 1;

  auto generated = generateSql(rewritten, Dialect::PostgreSQL, options);
  if (!generated.ok()) return generated;
  MemoryCatalog verified;
  verified.tables.push_back(storage);
  if (requiresCompensation) {
    const auto remainder=compensatingArm(query,definition,view,extraGroups,options,toRequest);
    if (!remainder.ok()) return remainder;
    constexpr std::string_view separator = " UNION ALL ";
    if (generated.sql.empty() || generated.sql.back() != ';')
      decline(requested.origin, "materialized view arm is not a complete SELECT", DiagnosticCode::InvalidQueryShape);
    generated.sql.pop_back();
    if (options.maxBytes - generated.sql.size() < separator.size() ||
        options.maxBytes - generated.sql.size() - separator.size() < remainder.sql.size())
      decline(requested.origin, "combined materialized view rewrite exceeds the SQL generation budget",
              DiagnosticCode::ResourceLimit);
    generated.sql.append(separator);
    generated.sql.append(remainder.sql);
    for(const auto &base:requested.tables) {
      if(std::none_of(verified.tables.begin(),verified.tables.end(),[&](const auto &found){
        return sameBaseIdentity(found,*base.definition);
      }))verified.tables.push_back(*base.definition);
    }
    if (rollup) {
      generated = reducePartialGroups(generated, query, requested, answer.projections.size(), groupSlots, verified, options);
      if (!generated.ok()) return generated;
    }
  }
  LanguageOptions language;
  language.shouldStop = options.shouldStop;
  auto checked = compile(Dialect::PostgreSQL, generated.sql, language, &verified, requested.context);
  if (!checked->ok()) return {{}, std::move(checked->diagnostics)};
  if (rollup || requiresCompensation) {
    const auto &output = statement(checked->model, checked->model.roots.front());
    if (output.projections.size() != requested.projections.size())
      decline(requested.origin, "materialized rewrite changed the output width", DiagnosticCode::InvalidQueryShape);
    for (std::size_t i = 0; i < output.projections.size(); ++i) {
      const auto &source = expression(requested, requested.projections[i].expression);
      const auto &actual = expression(output, output.projections[i].expression);
      if (!sameSqlType(query.types.get(source.sqlType), checked->model.types.get(actual.sqlType)))
        decline(source.origin, "materialized rewrite changed a result type");
    }
  }
  if (evidence) {
    evidence->compensates=requiresCompensation;
    evidence->pattern=expanded?MaterializationPattern::FilteredScan:
        flattened?MaterializationPattern::StarProjectAggregate:
        relations>2?MaterializationPattern::StarAggregate:
        rollup?MaterializationPattern::ProjectAggregate:
        relations==2?MaterializationPattern::ProjectJoin:MaterializationPattern::ProjectFilter;
  }
  return generated;
}
} // namespace

GeneratedSql rewriteMaterializedView(const ModelProgram &query, const ModelProgram &definition,
                                     const TableDefinition &storage, MaterializationSnapshot snapshot,
                                     GenerationOptions options, MaterializationCoverage coverage) {
  try {
    return match(query, definition, storage, snapshot, std::move(options), coverage);
  } catch (const Refusal &failure) {
    return {{}, {failure.diagnostic}};
  } catch (const std::exception &) {
    return {{}, {{DiagnosticCode::InvalidQueryShape, {}, "invalid materialized view match input"}}};
  }
}
MaterializationSearch rewriteMaterializedViews(const ModelProgram &query,
    const std::vector<RegisteredMaterialization> &registered,GenerationOptions options,
    MaterializationCoverage coverage) {
  MaterializationSearch result;
  if (registered.empty() || registered.size()>32) {
    result.diagnostics.push_back({DiagnosticCode::ResourceLimit,{},"expected 1 to 32 registered materializations"});
    return result;
  }
  std::vector<Diagnostic> firstFailure;
  std::vector<bool> accepted(registered.size());
  const auto passes=coverage==MaterializationCoverage::AllowPartial?2:1;
  for (int pass=0;pass<passes;++pass) for (std::size_t i=0;i<registered.size();++i) {
    if (accepted[i]) continue;
    try {
      stop(options);
      const auto &view=registered[i];
      if (!view.definition || !view.storage)
        decline({},"materialization registration is incomplete",DiagnosticCode::InvalidQueryShape);
      MatchEvidence evidence;
      auto generated=match(query,*view.definition,*view.storage,view.snapshot,options,
                           pass?MaterializationCoverage::AllowPartial:MaterializationCoverage::FullOnly,&evidence);
      if (generated.ok()) {
        if (generated.sql.size()>options.maxBytes)
          decline({},"registered materializations exceed the SQL output budget",DiagnosticCode::ResourceLimit);
        options.maxBytes-=generated.sql.size();
        accepted[i]=true;
        result.candidates.push_back({i,evidence.pattern,evidence.compensates,std::move(generated.sql)});
      } else {
        if (!generated.diagnostics.empty() && generated.diagnostics.front().code==DiagnosticCode::Stopped)
          return {{},std::move(generated.diagnostics)};
        if (firstFailure.empty()) firstFailure=std::move(generated.diagnostics);
      }
    } catch (const Refusal &failure) {
      if (failure.diagnostic.code==DiagnosticCode::Stopped) return {{},{failure.diagnostic}};
      if (firstFailure.empty()) firstFailure.push_back(failure.diagnostic);
    } catch (const std::exception &) {
      if (firstFailure.empty())
        firstFailure.push_back({DiagnosticCode::InvalidQueryShape,{},"invalid registered materialization"});
    }
  }
  if (result.candidates.empty()) result.diagnostics=std::move(firstFailure);
  return result;
}
} // namespace navidog::sql::optimizer
