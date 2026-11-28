#include "navidog/sql/compiler/script_context.hpp"
#include "navidog/sql/compiler/operator_catalog.hpp"
#include "parser.hpp"
#include <algorithm>
#include <set>
#include <unordered_map>

namespace navidog::sql::compiler {
std::vector<CatalogObjectDefinition>
MemoryCatalog::listObjects(const NameResolutionContext &ctx,CatalogObjectKind kind,std::size_t limit) const {
  std::vector<CatalogObjectDefinition> out;
  for(const auto &o:objects) {
    if(out.size()>=limit)break;
    if(o.kind==kind && (kind==CatalogObjectKind::Database || ctx.database.empty() || o.database==ctx.database))out.push_back(o);
  }
  return out;
}
std::vector<TableDefinition>
CatalogSnapshot::matchingRelations(const NameResolutionContext &context,std::size_t limit,
                                   const CatalogNameFilter &accept) const {
  auto out=listRelations(context,limit);
  if (accept)
    out.erase(std::remove_if(out.begin(),out.end(),[&](const auto &table) {
      return !accept(table.database,table.schema,table.name);
    }),out.end());
  return out;
}
std::vector<RoutineDefinition>
CatalogSnapshot::matchingRoutines(const NameResolutionContext &context,bool procedure,std::size_t limit,
                                  const CatalogNameFilter &accept) const {
  auto out=listRoutines(context,procedure,limit);
  if (accept)
    out.erase(std::remove_if(out.begin(),out.end(),[&](const auto &routine) {
      return !accept(routine.database,routine.schema,routine.name);
    }),out.end());
  return out;
}
std::vector<TableDefinition>
MemoryCatalog::listRelations(const NameResolutionContext &context,
                             std::size_t limit) const {
  return matchingRelations(context,limit,{});
}
std::vector<TableDefinition>
MemoryCatalog::matchingRelations(const NameResolutionContext &context,std::size_t limit,
                                 const CatalogNameFilter &accept) const {
  std::vector<TableDefinition> out;
  for (const auto &t : tables) {
    if (out.size() >= limit)
      break;
    if ((context.database.empty() || t.database == context.database) &&
        (!accept || accept(t.database,t.schema,t.name)))
      out.push_back(t);
  }
  return out;
}
std::vector<RoutineDefinition>
MemoryCatalog::listRoutines(const NameResolutionContext &context,
                            bool procedure, std::size_t limit) const {
  return matchingRoutines(context,procedure,limit,{});
}
std::vector<RoutineDefinition>
MemoryCatalog::matchingRoutines(const NameResolutionContext &context,bool procedure,std::size_t limit,
                                const CatalogNameFilter &accept) const {
  std::vector<RoutineDefinition> out;
  for (const auto &t : routines) {
    if (out.size() >= limit)
      break;
    if ((context.database.empty() || t.database == context.database) &&
        t.procedure == procedure && (!accept || accept(t.database,t.schema,t.name)))
      out.push_back(t);
  }
  return out;
}

std::optional<TableDefinition>
MemoryCatalog::findTable(const NameResolutionContext &ctx,
                         const std::vector<std::string> &name) const {
  if (name.empty() || name.size() > 3)
    return {};
  std::string database = ctx.database, schema = ctx.schema;
  if (name.size() == 3) {
    database = name[0];
    schema = name[1];
  } else if (name.size() == 2) {
    if (isMySqlDialect(ctx.dialect) || ctx.dialect == Dialect::SQLite)
      database = name[0];
    else
      schema = name[0];
  }
  for (const auto &t : tables)
    if (t.database == database && t.schema == schema && t.name == name.back())
      return t;
  return {};
}
std::vector<RoutineDefinition>
MemoryCatalog::findRoutines(const NameResolutionContext &ctx,
                            const std::vector<std::string> &name,
                            bool procedure) const {
  std::vector<RoutineDefinition> found;
  if (name.empty() || name.size() > 3)
    return found;
  const auto database = name.size() == 3 ? name[0] : ctx.database;
  const auto schema = name.size() > 1 ? name[name.size() - 2] : ctx.schema;
  for (const auto &routine : routines)
    if (routine.database == database && routine.schema == schema &&
        routine.name == name.back() && routine.procedure == procedure)
      found.push_back(routine);
  return found;
}
std::vector<std::size_t> relationLeaves(const ModelStatement &query, RelationId root,
                                       const std::function<void()> &checkCancellation) {
  std::vector<std::size_t> result;
  std::vector<RelationId> work{root};
  while (!work.empty()) {
    checkCancellation();
    const auto id = work.back();
    work.pop_back();
    const auto &relation = query.relations.at(id - 1);
    if (!relation.left) {
      result.push_back(relation.table);
    } else {
      work.push_back(relation.right);
      work.push_back(relation.left);
    }
  }
  return result;
}
namespace {
std::string identifier(const Compilation &c, const NamePart &name) {
  return canonicalIdentifier(c.dialect, c.ast.identifier(name.identifier),
                             name.quoted);
}
std::vector<std::string> qualified(const Compilation &c,
                                   const QualifiedName &name) {
  std::vector<std::string> result;
  for (const auto &p : name)
    result.push_back(identifier(c, p));
  return result;
}
ValueType literalType(const ModelExpression &e) {
  switch (e.kind) {
  case ExprKind::Null:
    return ValueType::Null;
  case ExprKind::MergeAction:
  case ExprKind::String:
    return ValueType::String;
  case ExprKind::Boolean:
  case ExprKind::Exists:
    return ValueType::Boolean;
  case ExprKind::Number:
    return e.value.find_first_of(".eE") == std::string::npos
               ? ValueType::Integer
               : ValueType::Decimal;
  default:
    return ValueType::Unknown;
  }
}
void inspectPreparedSource(const Compilation &owner, const ModelExpression &source, ModelPrepared &prepared) {
  if (source.kind != ExprKind::String)
    return;
  Compilation text(owner.dialect, source.value, owner.options, "<prepared statement>");
  lex(text);
  if (!text.diagnostics.empty())
    return;
  bool statement = false, terminated = false;
  std::size_t parameters = 0;
  bool preservesHandles = false;
  for (const auto &token : text.tokens) {
    if (token.kind == TokenKind::Comment || token.kind == TokenKind::End)
      continue;
    if (terminated)
      return; // Compound or multiple statements require full parsing.
    if (token.kind == TokenKind::Symbol && token.value == ";") {
      if (!statement) return;
      terminated = true;
      continue;
    }
    if (!statement && token.kind == TokenKind::Word) {
      const auto first = asciiUpper(token.value);
      preservesHandles = first == "SELECT" || first == "INSERT" || first == "UPDATE" ||
                         first == "DELETE" || first == "REPLACE";
    }
    statement = true;
    parameters += token.kind == TokenKind::Parameter && token.value == "?";
  }
  if (statement) {
    prepared.parameterCount = parameters;
    prepared.preservesHandles = preservesHandles;
  }
}
struct CteBinding {
  QueryId query = 0;
  std::shared_ptr<const TableDefinition> table;
  QueryId recursiveOwner = 0, recursiveMember = 0;
  CteMaterialization materialization = CteMaterialization::Default;
  bool hasRows = true;
};
using Ctes = std::unordered_map<std::string, CteBinding>;
struct ShapeColumn {
  std::string name;
  ValueType type;
  std::vector<ColumnBinding> bindings;
};
struct Shape {
  std::vector<ShapeColumn> columns;
  bool complete = true, arityKnown = true;
};
using Visible = std::vector<std::size_t>;
struct Frame {
  const ModelSelect *query;
  Visible visible;
  std::shared_ptr<const Shape> shape;
};
using Frames = std::vector<Frame>;

class Lowering {
public:
  Lowering(Compilation &c, const CatalogSnapshot *catalog,
           const NameResolutionContext &names)
      : c(c), catalog(catalog), names(names) {}
  QueryId run(const SelectStatement &s, Ctes ctes = {}, Frames outer = {},
              const CommonTableExpression *recursiveCte = nullptr) {
    checkStop(c);
    if (++loweringDepth > c.options.limits.nesting)
      limit("query lowering depth limit exceeded");
    struct LeaveDepth {
      std::size_t &depth;
      ~LeaveDepth() { --depth; }
    } leaveDepth{loweringDepth};
    if (c.model.queries.size() >= c.options.limits.queries)
      limit("query count limit exceeded");
    ModelSelect m;
    m.id = static_cast<QueryId>(c.model.queries.size() + 1);
    c.model.queries.emplace_back(); // Reserve ID before descending; never
                                    // retain arena references.
    m.kind = s.kind;
    m.fixedJoinOrder = s.fixedJoinOrder;
    if (s.transaction) {
      const auto &syntax = *s.transaction;
      auto &control = m.transaction.emplace();
      control.action = syntax.action;
      control.mode = syntax.mode;
      control.isolation = syntax.isolation;
      control.access = syntax.access;
      control.chain = syntax.chain;
      control.release = syntax.release;
      control.deferrable = syntax.deferrable;
      control.consistentSnapshot = syntax.consistentSnapshot;
      if (syntax.name) control.name = identifier(c, *syntax.name);
      m.transactionEffect = TransactionEffect::ExplicitControl;
    }
    m.role = s.role;
    if (m.role && m.role->scope == RoleScope::AccountDefault)
      m.transactionEffect = TransactionEffect::ImplicitCommit;
    m.recursiveWith = s.recursiveWith;
    m.tableQuery = s.tableQuery;
    m.valuesQuery = s.valuesQuery;
    m.sqlServerValueConstructor = s.sqlServerValueConstructor;
    m.routine = qualified(c, s.routine);
    m.sourceFile = c.source.name();
    m.origin = s.range;
    m.insertSource = s.insertSource;
    m.modifiers = s.modifiers;
    for (const auto &col : s.columns)
      m.columns.push_back(identifier(c, col));
    for (const auto &target : s.deleteTargets)
      m.deleteTargets.push_back(qualified(c, target));
    m.context = names;
    if (loweringDepth > 1)
      m.context.environment = SemanticEnvironment::Sql;
    m.distinct = s.distinct;
    m.catalogChecked = catalog && catalog->current();
    if (!outer.empty()) {
      m.scope.parent = outer.back().query->id;
      m.scope.parentTables = outer.back().visible;
    }
    struct RestoreNamespace {
      NameResolutionContext &names;
      std::optional<std::string> database,schema;
      ~RestoreNamespace() {
        if(database)names.database=std::move(*database);
        if(schema)names.schema=std::move(*schema);
      }
    } restoreNamespace{names, {}, {}};
    std::shared_ptr<ModelTrigger> trigger;
    std::string triggerSchema;
    if (s.trigger) {
      const auto &syntax = *s.trigger;
      trigger = std::make_shared<ModelTrigger>();
      trigger->name = qualified(c, syntax.name);
      trigger->target = qualified(c, syntax.target);
      trigger->targetOrigin = syntax.targetOrigin;
      trigger->event = syntax.event;
      trigger->timing = syntax.timing;
      trigger->drop = syntax.drop;
      trigger->temporary = syntax.temporary;
      trigger->ifGuard = syntax.ifGuard;
      trigger->explicitRow = syntax.explicitRow;
      trigger->compound = syntax.compound;
      trigger->order = syntax.order;
      trigger->definer = syntax.definer;
      if(syntax.relativeTrigger)trigger->relativeTrigger=identifier(c,*syntax.relativeTrigger);
      trigger->updateColumns = qualified(c, syntax.updateColumns);
      if (!syntax.drop) {
        std::optional<TableDefinition> table;
        if(c.dialect==Dialect::SQLite) {
          const bool explicitSchema = trigger->name.size() == 2;
          const auto schema = explicitSchema ? trigger->name[0] : std::string("main");
          bool temp = syntax.temporary || asciiUpper(schema) == "TEMP" ||
              (!explicitSchema && trigger->target.size() == 2 && asciiUpper(trigger->target[0]) == "TEMP");
          if (syntax.temporary && trigger->name.size() == 2 && asciiUpper(trigger->name[0]) != "TEMP")
            c.report(DiagnosticCode::InvalidQueryShape, s.range, "SQLite TEMP trigger must be stored in the temp schema");
          auto lookup = names;
          if (!temp && (!names.schema.empty() || asciiUpper(schema) != "MAIN")) lookup.schema = schema;
          if (catalog && !explicitSchema && trigger->target.size() == 1) {
            table = catalog->findTable(lookup, {"temp", trigger->target.front()});
            temp |= table.has_value();
          }
          if (!table && catalog) table = catalog->findTable(lookup, trigger->target);
          if (!table && catalog && catalog->tableLookupComplete(lookup, trigger->target))
            c.report(DiagnosticCode::UnknownTable, syntax.targetOrigin, "SQLite trigger target not found in the supplied catalog");
          if (table && ((table->kind == ObjectKind::View) != (syntax.timing == TriggerTiming::InsteadOf)))
            c.report(DiagnosticCode::InvalidQueryShape, syntax.targetOrigin, "SQLite views require INSTEAD OF triggers, and tables require BEFORE or AFTER");
          if (!temp) {
            if (trigger->target.size() == 2 && asciiUpper(schema) != asciiUpper(trigger->target[0]))
              c.report(DiagnosticCode::InvalidQueryShape, syntax.targetOrigin, "SQLite persistent trigger and its target must use the same schema");
            triggerSchema = schema;
            if (names.schema != lookup.schema) {
              restoreNamespace.schema = names.schema;
              m.context.schema = names.schema = lookup.schema;
            }
          }
        } else {
          const auto database=trigger->target.size()==2?trigger->target.front():
                              trigger->name.size()==2?trigger->name.front():names.database;
          auto lookup=names;
          lookup.database=database;
          lookup.schema=database;
          if(trigger->name.size()==2 && trigger->target.size()==2) {
            const auto equal=catalog?catalog->relationNameEquals(trigger->name[0],trigger->target[0]):
                                     trigger->name[0]==trigger->target[0];
            if(!equal)c.report(DiagnosticCode::InvalidQueryShape,syntax.targetOrigin,
                               "MySQL trigger and its subject table must belong to the same database");
          }
          if(catalog)table=catalog->findTable(lookup,trigger->target);
          if(!table && catalog && catalog->tableLookupComplete(lookup,trigger->target))
            c.report(DiagnosticCode::UnknownTable,syntax.targetOrigin,"MySQL trigger target not found in the supplied catalog");
          if(table && table->kind!=ObjectKind::Table)
            c.report(DiagnosticCode::InvalidQueryShape,syntax.targetOrigin,"MySQL triggers require a permanent base table");
          if(!database.empty() && names.database!=database) {
            restoreNamespace.database=names.database;
            m.context.database=names.database=database;
          }
          if(!database.empty() && names.schema!=database) {
            restoreNamespace.schema=names.schema;
            m.context.schema=names.schema=database;
          }
        }
        auto definition = table ? std::make_shared<const TableDefinition>(*table) : nullptr;
        const auto addRow = [&](const char *label) {
          ModelTable row;
          row.name = {label};
          row.alias = label;
          row.pseudo = true;
          row.definition = definition;
          if (definition) chargeColumns(definition->columns.size());
          m.scope.tables.push_back({label, m.tables.size()});
          m.tables.push_back(std::move(row));
        };
        if (syntax.event != TriggerEvent::Insert) addRow("old");
        if (syntax.event != TriggerEvent::Delete) addRow("new");
      }
      m.trigger = trigger;
      m.transactionEffect = c.dialect==Dialect::SQLite?TransactionEffect::TransactionalDdl:
                                                    TransactionEffect::ImplicitCommit;
    }
    std::unordered_map<ExprId, ExprId> ids{{0, 0}};
    std::vector<ExprId> ordered;
    auto work = expressionInputs(s);
    while (!work.empty()) {
      auto id = work.back();
      work.pop_back();
      if (!id || !ids.emplace(id, 0).second)
        continue;
      ordered.push_back(id);
      if ((ordered.size() & 255u) == 0)
        checkStop(c);
      for (auto child : c.ast.expression(id).operands)
        work.push_back(child);
    }
    std::sort(ordered.begin(), ordered.end());
    // Resolving a named window adds forward AST edges to its later definition.
    // Lower every operand first while preserving source-ID order where
    // possible.
    std::vector<ExprId> topological;
    std::unordered_map<ExprId, unsigned char> visited;
    for (auto root : ordered) {
      std::vector<std::pair<ExprId, bool>> stack{{root, false}};
      while (!stack.empty()) {
        auto [id, exit] = stack.back();
        stack.pop_back();
        if (exit) {
          visited[id] = 2;
          topological.push_back(id);
          continue;
        }
        if (visited[id] == 2)
          continue;
        if (visited[id] == 1) {
          c.report(DiagnosticCode::InvalidQueryShape,
                   c.ast.expression(id).range, "cyclic expression dependency");
          throw CompilationAborted{};
        }
        visited[id] = 1;
        stack.push_back({id, true});
        const auto &operands = c.ast.expression(id).operands;
        for (auto it = operands.rbegin(); it != operands.rend(); ++it)
          if (*it)
            stack.push_back({*it, false});
        if ((visited.size() & 255u) == 0)
          checkStop(c);
      }
    }
    ordered = std::move(topological);
    std::unordered_map<ExprId, QueryId> pendingQueries;
    m.expressions.emplace_back();
    for (auto id : ordered) {
      if ((id & 255u) == 0)
        checkStop(c);
      const auto &e = c.ast.expression(id);
      ModelExpression out;
      out.kind = e.kind;
      out.interval=e.interval;
      if (e.castType) out.castType = std::make_shared<const BasicTypeSpec<std::vector<std::string>>>(lowerTypeSpec(c,*e.castType));
      out.origin = e.range;
      out.value = e.value;
      out.name = qualified(c, e.name);
      for (const auto &part : e.name)
        out.nameQuoted.push_back(part.quoted);
      if (c.dialect == Dialect::SQLServer && e.kind == ExprKind::String) {
        const auto raw = c.source.slice(e.range);
        out.unicodeString =
            !raw.empty() && (raw.front() == 'N' || raw.front() == 'n');
      }
      out.distinct = e.distinct;
      out.window = e.window;
      out.windowSource = e.windowSource;
      out.implicitCaseElse=e.kind==ExprKind::Case && !e.explicitElse;
      out.withinGroup = e.withinGroup;
      out.jsonConstructor = e.jsonConstructor;
      for (auto operand : e.operands)
        out.operands.push_back(ids.at(operand));
      out.type = literalType(out);
      if (out.kind == ExprKind::Name || out.kind == ExprKind::Parameter) {
        auto spelling = out.kind == ExprKind::Parameter
                            ? std::vector<std::string>{out.value}
                            : out.name;
        auto canonical = [&](std::vector<std::string> n) {
          if (isMySqlDialect(c.dialect) ||
              c.dialect == Dialect::SQLServer)
            for (auto &p : n)
              for (auto &ch : p)
                if (ch >= 'A' && ch <= 'Z')
                  ch += 32;
          return n;
        };
        spelling = canonical(spelling);
        for (const auto &local : c.names.locals)
          if (canonical(local.name) == spelling) {
            out.localSymbol = local.symbol;
            out.type = local.type;
            break;
          }
      }
      if (e.kind == ExprKind::Case && !e.explicitElse) {
        ModelExpression null;
        null.kind = ExprKind::Null;
        null.type = ValueType::Null;
        null.origin = {e.range.end, e.range.end};
        out.operands.push_back(static_cast<ExprId>(m.expressions.size()));
        m.expressions.push_back(std::move(null));
      }
      ids[id] = static_cast<ExprId>(m.expressions.size());
      if (e.query)
        pendingQueries[ids[id]] = e.query;
      m.expressions.push_back(std::move(out));
    }
    if(s.package)lowerPackage(m,*s.package,ids);
    for (const auto &p : s.projections) {
      const auto expression=ids.at(p.expression);
      auto alias=p.alias?identifier(c,*p.alias):std::string{};
      if (!p.alias) {
        auto field=expression;
        while (m.expressions.at(field).kind==ExprKind::Subscript)
          field=m.expressions.at(field).operands.front();
        const auto &base=m.expressions.at(field);
        if (base.kind==ExprKind::ArrayConstructor) alias="array";
        else if (field!=expression && base.kind==ExprKind::Name && !base.name.empty()) alias=base.name.back();
        else if (field==expression && c.dialect==Dialect::PostgreSQL && base.kind==ExprKind::Call &&
                 !base.name.empty() && base.name.back()=="generate_series") alias=base.name.back();
      }
      m.projections.push_back({expression,alias,!p.alias && !alias.empty()});
    }
    std::unordered_map<std::string, bool> localCtes;
    if (s.recursiveWith)
      for (const auto &syntax : s.ctes)
        ctes[identifier(c, syntax.name)] = {};
    m.ctes.resize(s.ctes.size());
    for (const auto index : cteDependencyOrder(s)) {
      const auto &syntax = s.ctes[index];
      ModelSelect::CTE out;
      out.name = identifier(c, syntax.name);
      out.materialization = syntax.materialization;
      if (!localCtes.emplace(out.name, true).second)
        c.report(DiagnosticCode::DuplicateAlias, syntax.range,
                 "duplicate CTE name: " + out.name);
      const auto &body=c.ast.query(syntax.query);
      const bool writes=body.kind!=StatementKind::Select;
      if (writes && loweringDepth!=1)
        c.report(DiagnosticCode::InvalidQueryShape,syntax.range,
                 "a data-modifying CTE must belong to a top-level statement");
      out.query = run(body, ctes, outer,
                      s.recursiveWith ? &syntax : nullptr);
      const auto &lowered=c.model.queries[out.query-1];
      const bool returns=!writes || !lowered.returning.empty();
      auto definition = output(lowered);
      definition.name = out.name;
      if (!syntax.columns.empty()) {
        if (syntax.columns.size() > c.options.limits.columnsPerQuery)
          limit("CTE column count limit exceeded");
        chargeColumns(syntax.columns.size());
        if (definition.arityKnown &&
            (definition.columns.size() < syntax.columns.size() ||
             (c.dialect != Dialect::PostgreSQL && definition.columns.size() != syntax.columns.size())))
          c.report(DiagnosticCode::InvalidQueryShape, syntax.range,
                   "CTE column list does not match query output count");
        for (const auto &name : syntax.columns) out.columns.push_back(identifier(c, name));
        assignCteColumns(definition, out.columns, syntax.range);
      }
      applySqliteBooleanOutputNames(definition);
      out.definition = std::make_shared<const TableDefinition>(std::move(definition));
      ctes[out.name] = {out.query, out.definition, 0, 0, out.materialization, returns};
      m.ctes[index] = std::move(out);
    }
    if (s.setOperation) {
      auto set = *s.setOperation;
      set.left = run(c.ast.query(set.left), ctes, outer);
      auto definition = output(c.model.queries.at(set.left - 1));
      const bool recursiveCandidate = recursiveCte && s.ctes.empty() && set.kind == SetKind::Union;
      if (recursiveCandidate) {
        const auto name = identifier(c, recursiveCte->name);
        auto working = definition;
        working.name = name;
        if (!recursiveCte->columns.empty()) {
          if (recursiveCte->columns.size() > c.options.limits.columnsPerQuery)
            limit("recursive CTE column count limit exceeded");
          chargeColumns(recursiveCte->columns.size());
          std::vector<std::string> names;
          for (const auto &column : recursiveCte->columns)
            names.push_back(identifier(c, column));
          assignCteColumns(working, names, recursiveCte->range);
        }
        applySqliteBooleanOutputNames(working);
        const auto member = static_cast<QueryId>(c.model.queries.size() + 1);
        ctes[name] = {set.left, std::make_shared<const TableDefinition>(std::move(working)), m.id, member};
      }
      set.right = run(c.ast.query(set.right), ctes, outer);
      const auto &member = c.model.queries.at(set.right - 1);
      const auto right = output(member);
      if (recursiveCandidate && validateRecursiveMember(s, member, m.id)) {
        set.recursive = true;
        if (!definition.arityKnown || (!definition.complete && recursiveCte->columns.empty()))
          c.report(DiagnosticCode::UnsupportedSyntax, s.range,
                   "recursive CTE seed output needs a known shape and named columns");
      }
      m.setOperation = set;
      if (definition.arityKnown && right.arityKnown &&
          definition.columns.size() != right.columns.size())
        c.report(DiagnosticCode::InvalidQueryShape, set.origin,
                 "set operands must return the same number of columns");
      definition.arityKnown &= right.arityKnown;
      definition.complete &= definition.arityKnown;
      for (std::size_t i = 0;
           i < definition.columns.size() && i < right.columns.size(); ++i) {
        auto &a = definition.columns[i].type;
        const auto b = right.columns[i].type;
        if (set.recursive)
          continue; // Recursive columns use the seed descriptor; exact type checks run after binding.
        if (a == ValueType::Null)
          a = b;
        else if (b != ValueType::Null && a != b)
          a = (a == ValueType::Integer && b == ValueType::Decimal) ||
                      (a == ValueType::Decimal && b == ValueType::Integer)
                  ? ValueType::Decimal
                  : ValueType::Unknown;
      }
      ModelTable result;
      result.name = {"__set_result"};
      result.alias = "__set_result";
      result.query = set.left;
      result.pseudo = true;
      definition.name = "__set_result";
      result.definition = std::make_shared<const TableDefinition>(definition);
      m.tables.push_back(result);
      m.scope.tables.push_back({result.alias, 0});
      for (std::size_t i = 0; i < definition.columns.size(); ++i) {
        ModelExpression column;
        column.kind = ExprKind::Name;
        column.origin = s.range;
        column.name = {definition.columns[i].name};
        column.type = definition.columns[i].type;
        column.binding = ColumnBinding{0, i, m.id};
        auto id = ExprId(m.expressions.size());
        m.expressions.push_back(std::move(column));
        m.projections.push_back({id, definition.columns[i].name});
      }
      const auto shape = shapeForTables(m, {0});
      queryShapes[m.id] = shape;
      for (auto order : s.orderBy) {
        order.expression = ids.at(order.expression);
        m.orderBy.push_back(order);
        auto &expression = m.expressions.at(order.expression);
        if (expression.kind == ExprKind::Name && expression.name.size() != 1)
          c.report(DiagnosticCode::InvalidQueryShape, expression.origin,
                   "set ORDER BY cannot use an operand table qualifier");
        else if (expression.kind == ExprKind::Number) {
          bool valid = false;
          try {
            auto n = std::stoull(expression.value);
            valid = expression.value.find_first_not_of("0123456789") ==
                        std::string::npos &&
                    n > 0 &&
                    (!definition.arityKnown || n <= definition.columns.size());
            if (valid && definition.arityKnown)
              expression.outputColumn = std::size_t(n - 1);
          } catch (...) {
          }
          if (!valid)
            c.report(DiagnosticCode::InvalidQueryShape, expression.origin,
                     "set ORDER BY position is outside the output column list");
        } else if (expression.kind != ExprKind::Name &&
                   !(isMySqlDialect(c.dialect)))
          c.report(DiagnosticCode::UnsupportedSyntax, expression.origin,
                   "set ORDER BY in this baseline accepts output names or "
                   "column positions");
        else
          bind(m, order.expression, {0}, true, ctes, {}, pendingQueries, shape);
      }
      m.limit = ids.at(s.limit.count);
      m.offset = ids.at(s.limit.offset);
      m.limitSyntax = s.limit.syntax;
      m.limitWithTies = s.limit.withTies;
      bind(m, m.limit, {}, false, ctes, {}, pendingQueries,
           std::make_shared<const Shape>());
      bind(m, m.offset, {}, false, ctes, {}, pendingQueries,
           std::make_shared<const Shape>());
      const auto id = m.id;
      inferModelTypes(m, c.dialect);
      c.model.queries[id - 1] = std::move(m);
      return id;
    }
    auto tables = s.tables;
    std::size_t tableOffset = 0;
    if (s.prepared) {
      auto &prepared = m.prepared.emplace();
      prepared.action = s.prepared->action;
      prepared.name = identifier(c, s.prepared->name);
      prepared.source = ids.at(s.prepared->source);
      if (prepared.source)
        inspectPreparedSource(c, m.expressions.at(prepared.source), prepared);
      for (auto argument : s.prepared->arguments)
        prepared.arguments.push_back(ids.at(argument));
      if (prepared.action == PreparedAction::Execute)
        m.transactionEffect = TransactionEffect::Dynamic;
    }
    if (s.target) {
      const auto targetName = qualified(c, s.target->name);
      for (std::size_t i = 0; i < tables.size(); ++i) {
        const auto tableName = qualified(c, tables[i].name);
        const auto symbol =
            tables[i].alias
                ? identifier(c, *tables[i].alias)
                : (!tableName.empty() ? tableName.back() : std::string{});
        if ((s.targetInTables || (c.dialect == Dialect::SQLServer &&
                                  s.kind != StatementKind::Merge)) &&
            ((tableName == targetName && !(isMySqlDialect(c.dialect) &&
                 s.kind==StatementKind::Delete && tables[i].alias)) ||
             (targetName.size() == 1 && symbol == targetName.back()))) {
          m.targetTable = i;
          break;
        }
      }
      if (s.targetInTables && !m.targetTable)
        c.report(DiagnosticCode::UnknownTable, s.target->range,
                 "DML target is not present in FROM/JOIN");
      if (!m.targetTable) {
        tables.insert(tables.begin(), *s.target);
        m.targetTable = 0;
        tableOffset = 1;
      }
    }
    for (const auto &t : tables) {
      ModelTable out;
      out.name = qualified(c, t.name);
      if (!t.name.empty())
        out.sourceNameOrigin={t.name.front().range.begin,t.name.back().range.end};
      out.alias = t.alias ? identifier(c, *t.alias) : "";
      out.inheritance = t.inheritance;
      out.sampling = t.sampling;
      out.finalRead = t.finalRead;
      out.clickHouseSampling = t.clickHouseSampling;
      out.postgresFunction=t.postgresFunction;
      out.inferredFunctionAlias=t.inferredFunctionAlias;
      if(t.postgresFunction!=PostgresRowFunction::None)
        out.sourceFunctionColumnAliases=t.functionColumnAliases;
      if (t.ordinalityColumn)
        out.ordinalityColumn=identifier(c,*t.ordinalityColumn);
      const bool explicitLateral = t.lateralLeft && !t.jsonTable;
      out.join = explicitLateral && t.join == JoinKind::Inner ? JoinKind::Cross : t.join;
      out.condition = explicitLateral ? 0 : ids[t.condition];
      const auto cteIt =
          out.name.size() == 1
              ? std::find_if(ctes.begin(), ctes.end(),
                             [&](const auto &item) {
                               return relationEquals(item.first, out.name[0]);
                             })
              : ctes.end();
      if (t.jsonTable) {
        auto structure = std::make_shared<ModelJsonTable>();
        structure->document = ids.at(t.jsonTable->document);
        structure->path = t.jsonTable->path;
        if (t.lateralLeft)
          structure->documentTables = lateralInputs(s, t.lateralLeft, m.tables.size());
        TableDefinition output;
        output.name = out.alias;
        std::set<std::string> seen;
        structure->columns = jsonTableColumns(t.jsonTable->columns, output, seen, false);
        out.definition = std::make_shared<const TableDefinition>(std::move(output));
        out.jsonTable = std::move(structure);
        const auto &visible = out.jsonTable->documentTables;
        bind(m, out.jsonTable->document, visible, false, ctes, outer, pendingQueries, shapeForTables(m, visible));
      } else if (t.query) {
        auto frames = outer;
        if (t.lateralLeft) {
          auto visible = lateralInputs(s, t.lateralLeft, m.tables.size());
          auto shape = shapeForTables(m, visible);
          frames.push_back({&m, std::move(visible), std::move(shape)});
        }
        out.query = run(c.ast.query(t.query), ctes, std::move(frames));
        auto definition = output(c.model.queries[out.query - 1]);
        if (out.ordinalityColumn) {
          chargeColumns(1);
          SqlType type;
          type.family=TypeFamily::Integer;
          type.bits=64;
          type.unsignedInteger=false;
          type.nullability=Nullability::NotNull;
          definition.columns.push_back({*out.ordinalityColumn,ValueType::Integer,
                                        std::make_shared<const SqlType>(std::move(type)),true});
        }
        if (!t.columnAliases.empty()) {
          chargeColumns(t.columnAliases.size());
          for (const auto &name : t.columnAliases) out.columnAliases.push_back(identifier(c, name));
          applyDerivedColumnAliases(definition, out.columnAliases, t.range);
        }
        applySqliteBooleanOutputNames(definition);
        out.definition = std::make_shared<const TableDefinition>(std::move(definition));
      } else if (cteIt != ctes.end()) {
        const auto &cte = cteIt->second;
        out.query = cte.query;
        out.definition = cte.table;
        out.workingTableOf = cte.recursiveOwner;
        out.cteMaterialization = cte.materialization;
        if (!cte.query)
          c.report(DiagnosticCode::UnsupportedSyntax, t.range,
                   "CTE reference precedes its definition; recursive self reference requires a UNION step");
        else if (!cte.hasRows)
          c.report(DiagnosticCode::InvalidQueryShape,t.range,
                   "a data-modifying CTE needs RETURNING to be used as a row source");
        else if (cte.recursiveOwner && cte.recursiveMember != m.id)
          c.report(DiagnosticCode::UnsupportedSyntax, t.range,
                   "recursive working table must be a direct input of the recursive SELECT");
      } else if (catalog) {
        auto definition = catalog->findTable(names, out.name);
        if (definition) {
          chargeColumns(definition->columns.size());
          out.definition =
              std::make_shared<const TableDefinition>(std::move(*definition));
        }
        if (!out.definition && catalog->tableLookupComplete(names, out.name))
          c.report(DiagnosticCode::UnknownTable,
                   t.name.empty() ? t.range
                                  : SourceRange{t.name.front().range.begin,
                                                t.name.back().range.end},
                   "table not found in supplied catalog snapshot");
      }
      if (t.sampling && (out.query || out.jsonTable ||
          (out.definition && out.definition->kind!=ObjectKind::Table &&
           out.definition->kind!=ObjectKind::MaterializedView)))
        c.report(DiagnosticCode::InvalidQueryShape,t.range,
                 "TABLESAMPLE requires a table or materialized view");
      if (t.rowMatch) {
        if (m.kind!=StatementKind::Select || t.pivot || t.unpivot)
          c.report(DiagnosticCode::UnsupportedSyntax,t.rowMatch->origin,
                   "MATCH_RECOGNIZE requires an independent SELECT relation");
        applyRowMatch(*t.rowMatch,out);
      }
      if (t.pivot) {
        if (m.targetTable == m.tables.size())
          c.report(DiagnosticCode::InvalidQueryShape, t.pivot->origin, "a PIVOT result is not a writable target");
        applyPivot(*t.pivot, ids, m, out);
      }
      if (t.unpivot) {
        if (m.targetTable==m.tables.size())
          c.report(DiagnosticCode::InvalidQueryShape,t.unpivot->origin,"an UNPIVOT result is not a writable target");
        applyUnpivot(*t.unpivot,ids,m,out);
      }
      const auto symbol = out.alias.empty() && !out.name.empty() ? out.name.back() : out.alias;
      for (const auto &existing : m.scope.tables)
        if (!symbol.empty() && existing.name == symbol)
          c.report(DiagnosticCode::DuplicateAlias, t.range,
                   "duplicate table alias: " + symbol);
      m.scope.tables.push_back({symbol, m.tables.size()});
      m.tables.push_back(std::move(out));
    }
    for (auto argument : s.arguments)
      m.arguments.push_back(ids[argument]);
    for (const auto &window : s.namedWindows) {
      ModelSelect::NamedWindow out;
      out.name = identifier(c, window.name);
      out.specification = window.specification;
      out.source=window.source;
      out.origin = window.name.range;
      for (auto key : window.keys)
        out.keys.push_back(ids.at(key));
      m.namedWindows.push_back(std::move(out));
    }
    m.prewhere = ids[s.prewhere];
    m.qualify = ids[s.qualify];
    m.systemTime = ids[s.systemTime];
    m.groupByAll = s.groupByAll;
    if (s.limitBy) {
      m.limitBy = *s.limitBy;
      m.limitBy->count = ids.at(m.limitBy->count);
      m.limitBy->offset = ids.at(m.limitBy->offset);
      for (auto &key : m.limitBy->keys) key = ids.at(key);
    }
    m.where = ids[s.where];
    m.having = ids[s.having];
    m.limit = ids[s.limit.count];
    m.offset = ids[s.limit.offset];
    m.limitSyntax = s.limit.syntax;
    m.limitWithTies = s.limit.withTies;
    for (auto id : s.groupBy)
      m.groupBy.push_back(ids[id]);
    for(const auto &set:s.groupingSets) {
      std::vector<ExprId> mapped;for(auto id:set)mapped.push_back(ids.at(id));m.groupingSets.push_back(std::move(mapped));
    }
    for (auto o : s.orderBy) {
      o.expression = ids[o.expression];
      m.orderBy.push_back(std::move(o));
    }
    if (s.ddl) {
      m.ddl = lowerDdl(c, *s.ddl, ids);
      const auto &ddl = *m.ddl;
      if (ddl.partition) validatePartition(ddl, *s.ddl->partition);
      if (c.dialect == Dialect::TiDB || c.dialect == Dialect::OceanBase)
        validateDistribution(ddl, s.kind);
      if (c.dialect == Dialect::Oracle)
        m.transactionEffect = TransactionEffect::ImplicitCommit;
      else if (isMySqlDialect(c.dialect))
        m.transactionEffect =
            ddl.temporary && (s.kind == StatementKind::Create ||
                              s.kind == StatementKind::Drop)
                ? TransactionEffect::NonTransactionalWithoutCommit
                : TransactionEffect::ImplicitCommit;
      else if (ddl.object == ObjectKind::Database)
        m.transactionEffect = TransactionEffect::ForbiddenInTransaction;
      else
        m.transactionEffect = TransactionEffect::TransactionalDdl;
      if ((s.kind == StatementKind::Create && ddl.object == ObjectKind::Table &&
           (!ddl.definitions.empty() || !ddl.tableIndexes.empty())) ||
          s.kind == StatementKind::Alter || ddl.object == ObjectKind::Index) {
        ModelTable local;
        local.name =
            ddl.object == ObjectKind::Index ? ddl.indexedTable : ddl.name;
        if (s.kind == StatementKind::Create &&
            ddl.object == ObjectKind::Table) {
          TableDefinition def;
          def.name = local.name.back();
          for (const auto &col : ddl.definitions)
            def.columns.push_back(
                {col.name, col.type.arrayDimensions
                               ? ValueType::Unknown
                               : typeCategory(col.type.name, c.dialect)});
          chargeColumns(def.columns.size());
          local.definition =
              std::make_shared<const TableDefinition>(std::move(def));
          local.pseudo = true;
        } else if (catalog && !local.name.empty()) {
          auto definition = catalog->findTable(names, local.name);
          if (definition) {
            chargeColumns(definition->columns.size());
            local.definition =
                std::make_shared<const TableDefinition>(std::move(*definition));
          }
        }
        if (!local.name.empty()) {
          m.scope.tables.push_back({local.name.back(), m.tables.size()});
          m.tables.push_back(std::move(local));
        }
      }
    }
    if (s.target && m.targetTable)
      m.target = m.tables[*m.targetTable];
    for (const auto &r : s.relations) {
      ModelRelation out;
      out.left = r.left;
      out.right = r.right;
      out.table = r.table + tableOffset;
      const bool explicitLateral = r.lateral && !r.implicitLateral;
      out.join = explicitLateral && r.join == JoinKind::Inner ? JoinKind::Cross : r.join;
      out.condition = explicitLateral ? 0 : ids[r.condition];
      out.natural = r.natural;
      out.lateral = r.lateral;
      out.implicitLateral = r.implicitLateral;
      out.fixedOrder = r.fixedOrder;
      out.comma = r.comma;
      if(c.dialect==Dialect::PostgreSQL) out.sourceLateralKeyword=r.explicitLateral;
      if(c.dialect==Dialect::PostgreSQL) out.sourceJoinKind=r.join;
      for (const auto &col : r.usingColumns)
        out.usingColumns.push_back(identifier(c, col));
      m.relations.push_back(std::move(out));
    }
    m.from = s.from;
    m.sourceFrom = s.from;
    if (s.pragma) {
      const auto &in = *s.pragma;
      m.pragma = ModelPragma{qualified(c, in.name), in.style, ids.at(in.argument)};
    }
    if (s.copy) {
      const auto &syntax = *s.copy;
      auto copy = std::make_shared<ModelCopy>();
      static_cast<CopyOptions &>(*copy) = syntax;
      copy->columns = qualified(c, syntax.columns);
      copy->forceQuote = qualified(c, syntax.forceQuote);
      copy->forceNotNull = qualified(c, syntax.forceNotNull);
      copy->forceNull = qualified(c, syntax.forceNull);
      if (const auto error = copyOptionError(*copy)) c.report(DiagnosticCode::InvalidQueryShape, s.range, *error);
      const auto checkColumns = [&](const auto &cols, const auto &parts, bool force) {
        std::set<std::string> seen;
        for (std::size_t i = 0; i < cols.size(); ++i) {
          checkStop(c);
          if (!force && !seen.insert(cols[i]).second)
            c.report(DiagnosticCode::InvalidQueryShape, parts[i].range, "COPY column appears more than once");
          if (m.target && m.target->definition && m.target->definition->complete &&
              std::none_of(m.target->definition->columns.begin(),m.target->definition->columns.end(),
                           [&](const auto &column){return columnEquals(column.name,cols[i]);}))
            c.report(DiagnosticCode::UnknownColumn, parts[i].range, "COPY column not found in the supplied catalog");
          if (force && !copy->columns.empty() && std::find(copy->columns.begin(),copy->columns.end(),cols[i])==copy->columns.end())
            c.report(DiagnosticCode::InvalidQueryShape, parts[i].range, "COPY FORCE column is not in the transfer column list");
        }
      };
      checkColumns(copy->columns, syntax.columns, false);
      checkColumns(copy->forceQuote, syntax.forceQuote, true);
      checkColumns(copy->forceNotNull, syntax.forceNotNull, true);
      checkColumns(copy->forceNull, syntax.forceNull, true);
      if (s.where && !pendingQueries.empty())
        c.report(DiagnosticCode::InvalidQueryShape, m.expressions.at(ids.at(s.where)).origin,
                 "COPY WHERE does not accept a subquery");
      m.copy = std::move(copy);
    }
    if (s.vacuum) {
      using Kind = VacuumOptionKind;
      const auto &in = *s.vacuum;
      auto vacuum = std::make_shared<ModelVacuum>();
      vacuum->options = in.options;
      const auto last = [&](Kind kind) -> const VacuumOption * {
        const auto option = std::find_if(in.options.rbegin(), in.options.rend(),
                                         [&](const auto &value) { return value.kind == kind; });
        return option == in.options.rend() ? nullptr : &*option;
      };
      const auto enabled = [&](Kind kind, bool defaultValue = false) {
        const auto option = last(kind);
        if (!option) return defaultValue;
        const auto value = std::get_if<bool>(&option->argument);
        return !value || *value;
      };
      if (enabled(Kind::Full)) {
        if (const auto *parallel = last(Kind::Parallel); parallel && std::get<int>(parallel->argument) > 0)
          c.report(DiagnosticCode::InvalidQueryShape, parallel->origin, "VACUUM FULL cannot run in parallel");
        if (enabled(Kind::DisablePageSkipping))
          c.report(DiagnosticCode::InvalidQueryShape, last(Kind::DisablePageSkipping)->origin,
                   "VACUUM FULL cannot disable page skipping");
        if (!enabled(Kind::ProcessToast, true))
          c.report(DiagnosticCode::InvalidQueryShape, last(Kind::ProcessToast)->origin,
                   "VACUUM FULL requires PROCESS_TOAST");
      }
      const bool analyze = enabled(Kind::Analyze);
      for (const auto &target : in.targets) {
        checkStop(c);
        auto name = qualified(c, target.name);
        auto columns = qualified(c, target.columns);
        if (!columns.empty() && !analyze)
          c.report(DiagnosticCode::InvalidQueryShape, target.columns.front().range,
                   "VACUUM column lists require the ANALYZE option");
        if (catalog) {
          const auto table = catalog->findTable(names, name);
          if (!table && catalog->tableLookupComplete(names, name))
            c.report(DiagnosticCode::UnknownTable, {target.name.front().range.begin, target.name.back().range.end},
                     "VACUUM target not found in supplied catalog snapshot");
          if (table && table->complete && table->arityKnown) for (std::size_t i = 0; i < columns.size(); ++i) {
            if (std::none_of(table->columns.begin(), table->columns.end(), [&](const auto &column) {
                  return catalog->columnNameEquals(column.name, columns[i]);
                }))
              c.report(DiagnosticCode::UnknownColumn, target.columns[i].range,
                       "VACUUM column not found in supplied catalog snapshot");
          }
        }
        vacuum->targets.push_back({std::move(name), std::move(columns), target.origin});
      }
      m.vacuum = std::move(vacuum);
      m.transactionEffect = TransactionEffect::ForbiddenInTransaction;
    }
    if (s.inspection) {
      const auto &in = *s.inspection;
      ModelInspection inspection;
      inspection.kind = in.kind; inspection.object = in.object;
      inspection.scope = in.scope; inspection.full = in.full;
      inspection.extended = in.extended; inspection.ifNotExists = in.ifNotExists;
      inspection.target = qualified(c, in.target);
      if (in.database) inspection.database = identifier(c, *in.database);
      if (in.column) inspection.column = identifier(c, *in.column);
      inspection.pattern = ids.at(in.pattern);
      inspection.jobCount = in.jobCount;
      const bool relation = in.kind == InspectionKind::Columns || in.kind == InspectionKind::Describe ||
          in.kind == InspectionKind::Indexes || (in.kind == InspectionKind::CreateDefinition &&
          (in.object == InspectionObject::Table || in.object == InspectionObject::View));
      if (relation && catalog) {
        auto targetName = inspection.target;
        if (inspection.database && targetName.size() == 1)
          targetName.insert(targetName.begin(), *inspection.database);
        if (!catalog->findTable(names, targetName) && catalog->tableLookupComplete(names, targetName))
          c.report(DiagnosticCode::UnknownTable,
                   {in.target.front().range.begin, in.target.back().range.end},
                   "inspection target not found in supplied catalog snapshot");
      }
      auto definition = inspectionOutput(inspection, names);
      chargeColumns(definition.columns.size());
      ModelTable output;
      output.name = {definition.name}; output.pseudo = true;
      output.definition = std::make_shared<const TableDefinition>(std::move(definition));
      m.tables.push_back(std::move(output));
      m.scope.tables.push_back({"__inspection_output", 0});
      ModelRelation leaf; leaf.table = 0;
      m.relations.push_back(leaf); m.from = RelationId(m.relations.size());
      m.inspection = std::move(inspection);
    }

    if (s.target) {
      ModelTable reference;
      reference.name = qualified(c, s.target->name);
      reference.alias = s.target->alias ? identifier(c, *s.target->alias) : "";
      reference.inheritance = s.target->inheritance;
      m.targetReference = std::move(reference);
    }
    if (s.valuesQuery) {
      const auto width = s.rows.front().size();
      if (width > c.options.limits.columnsPerQuery)
        limit("VALUES output column limit exceeded");
      chargeColumns(width);
      TableDefinition definition;
      definition.name = "__values_result";
      for (std::size_t i = 0; i < width; ++i) {
        const auto name = c.dialect == Dialect::MySQL ? "column_" + std::to_string(i)
                                                       : "column" + std::to_string(i + 1);
        definition.columns.push_back({name, ValueType::Unknown});
        ModelExpression column;
        column.kind = ExprKind::Name;
        column.origin = s.range;
        column.name = {name};
        column.binding = ColumnBinding{0, i, m.id};
        m.projections.push_back({ExprId(m.expressions.size()), name});
        m.expressions.push_back(std::move(column));
      }
      ModelTable result;
      result.name = {definition.name};
      result.pseudo = true;
      result.definition = std::make_shared<const TableDefinition>(std::move(definition));
      m.tables.push_back(std::move(result));
      ModelRelation leaf;
      m.relations.push_back(leaf);
      m.from = RelationId(m.relations.size());
    }
    if (s.ddl && !m.tables.empty()) {
      ModelRelation leaf;
      leaf.table = 0;
      m.relations.push_back(leaf);
      m.from = RelationId(m.relations.size());
    }
    if (tableOffset) {
      ModelRelation leaf;
      leaf.table = 0;
      m.relations.push_back(leaf);
      const auto targetRoot = RelationId(m.relations.size());
      if (m.from) {
        ModelRelation cross;
        cross.left = targetRoot;
        cross.right = m.from;
        cross.join = JoinKind::Cross;
        m.relations.push_back(cross);
        m.from = RelationId(m.relations.size());
      } else
        m.from = targetRoot;
    }
    Visible visible;
    for (std::size_t i = 0; i < m.tables.size(); ++i)
      visible.push_back(i);
    auto shapes = relationShapes(m);
    auto shape = m.from ? shapes[m.from - 1] : std::make_shared<const Shape>();
    queryShapes[m.id] = shape;
    const auto visibleShape = shape;
    if (trigger && !trigger->drop) {
      trigger->when = ids.at(s.trigger->when);
      const auto firstQuery = c.model.queries.size();
      bind(m, trigger->when, visible, false, ctes, outer, pendingQueries, visibleShape);
      auto triggerScope = outer;
      triggerScope.push_back({&m, visible, visibleShape});
      for (auto step : s.trigger->body) trigger->body.push_back(run(c.ast.query(step), ctes, triggerScope));
      if (!triggerSchema.empty()) for (std::size_t i = firstQuery; i < c.model.queries.size(); ++i) {
        const auto &step = c.model.queries[i];
        for (const auto &table : step.tables)
          if (!table.query && !table.pseudo && table.name.size() == 2 &&
              asciiUpper(table.name[0]) != asciiUpper(triggerSchema))
            c.report(DiagnosticCode::InvalidQueryShape, step.origin, "SQLite persistent trigger body references another schema");
      }
    }
    auto lowerAssignments = [&](const std::vector<Assignment> &in,
                                std::vector<Assignment> &out) {
      for (const auto &a : in) {
        Assignment b;
        for (auto e : a.targets)
          b.targets.push_back(ids[e]);
        b.value = ids[a.value];
        out.push_back(std::move(b));
      }
    };
    lowerAssignments(s.assignments, m.assignments);
    lowerAssignments(s.conflict.assignments, m.conflict.assignments);
    m.mergeOn = ids.at(s.mergeOn);
    for (const auto &in : s.mergeClauses) {
      ModelMergeClause out;
      out.match = in.match;
      out.action = in.action;
      out.condition = ids.at(in.condition);
      out.deleteWhere = ids.at(in.deleteWhere);
      out.defaultValues = in.defaultValues;
      out.origin = in.origin;
      for (const auto &col : in.columns)
        out.columns.push_back(identifier(c, col));
      for (auto id : in.values)
        out.values.push_back(ids.at(id));
      lowerAssignments(in.assignments, out.assignments);
      m.mergeClauses.push_back(std::move(out));
    }
    for (const auto &row : s.rows) {
      std::vector<ExprId> values;
      for (auto e : row)
        values.push_back(ids[e]);
      m.rows.push_back(std::move(values));
    }
    for (const auto &p : s.returning)
      m.returning.push_back(
          {ids[p.expression], p.alias ? identifier(c, *p.alias) : ""});
    for (auto e : s.returningInto)
      m.returningInto.push_back(ids[e]);
    if (s.outputInto) {
      const auto &syntax=*s.outputInto;
      auto &sink=m.outputInto.emplace();
      sink.table=qualified(c,syntax.table);
      sink.tableVariable=syntax.tableVariable;
      sink.origin=syntax.origin;
      sink.tableOrigin=syntax.tableOrigin;
      for (const auto &col:syntax.columns) sink.columns.push_back(identifier(c,col));
      for (const auto &p:syntax.projections)
        sink.projections.push_back({ids.at(p.expression),p.alias?identifier(c,*p.alias):""});
      const auto table=catalog && !sink.table.empty()?catalog->findTable(names,sink.table):std::nullopt;
      const auto *tableDefinition=table?&*table:nullptr;
      if (!sink.table.empty()) {
        if (table && table->kind!=ObjectKind::Table)
          c.report(DiagnosticCode::InvalidQueryShape,sink.tableOrigin,"OUTPUT INTO requires a table");
        else if (!table && catalog && catalog->tableLookupComplete(names,sink.table))
          c.report(DiagnosticCode::UnknownTable,sink.tableOrigin,"OUTPUT INTO table not found");
      }
      validateTargetColumns(sink.columns,syntax.columns,tableDefinition,"OUTPUT INTO");
      if (!sink.columns.empty() && sink.columns.size()!=sink.projections.size() &&
          std::none_of(sink.projections.begin(),sink.projections.end(),[&](const auto &p) {
            return m.expressions.at(p.expression).kind==ExprKind::Star;
          }))
        c.report(DiagnosticCode::InvalidQueryShape,sink.origin,"OUTPUT projection count differs from the INTO columns");
    }
    m.conflict.action = s.conflict.action;
    m.replaceExisting = s.replaceExisting;
    m.conflict.targetWhere = ids[s.conflict.targetWhere];
    m.conflict.where = ids[s.conflict.where];
    if (s.conflict.constraint)
      m.conflict.constraint = identifier(c, *s.conflict.constraint);
    if (s.conflict.rowAlias) {
      m.conflict.rowAlias = identifier(c, *s.conflict.rowAlias);
      std::set<std::string> aliases;
      for (const auto &part : s.conflict.columnAliases) {
        auto name = identifier(c, part);
        if (!aliases.insert(asciiUpper(name)).second)
          c.report(DiagnosticCode::DuplicateAlias, part.range,
                   "duplicate inserted column alias: " + name);
        m.conflict.columnAliases.push_back(std::move(name));
      }
    }
    for (auto e : s.conflict.targets)
      m.conflict.targets.push_back(ids[e]);
    if (s.source)
      m.source = run(c.ast.query(s.source), ctes, outer);
    if (m.source && m.ddl && !m.ddl->tableIndexes.empty()) {
      auto &table=m.tables.front();
      auto combined=*table.definition;
      auto source=output(c.model.queries[m.source-1]);
      chargeColumns(source.columns.size());
      std::set<std::string> known;
      for (const auto &column:combined.columns) known.insert(asciiUpper(column.name));
      for (auto &column:source.columns)
        if (known.insert(asciiUpper(column.name)).second) combined.columns.push_back(std::move(column));
      combined.complete=source.complete;
      combined.arityKnown=source.arityKnown;
      table.definition=std::make_shared<const TableDefinition>(std::move(combined));
    }
    if (m.kind == StatementKind::Insert) {
      validateTargetColumns(m.columns,s.columns,m.target?m.target->definition.get():nullptr,"INSERT");
      for (const auto &row : m.rows) {
        const auto location=row.empty()?m.origin:SourceRange{
            m.expressions.at(row.front()).origin.begin,m.expressions.at(row.back()).origin.end};
        if (!m.rows.empty() && row.size()!=m.rows.front().size())
          c.report(DiagnosticCode::InvalidQueryShape,location,"VALUES row widths differ");
        if (!m.columns.empty() && row.size()!=m.columns.size())
          c.report(DiagnosticCode::InvalidQueryShape,location,"INSERT column/value counts differ");
      }
      if (m.source) {
        const auto &source = c.model.queries[m.source - 1];
        const auto sourceOutput = output(source);
        if (sourceOutput.arityKnown) {
          if (!m.columns.empty() && m.columns.size() != sourceOutput.columns.size())
            c.report(DiagnosticCode::InvalidQueryShape, source.origin,
                     "INSERT column/source query counts differ");
          else if (m.columns.empty())
            validateImplicitInsertWidth(m, sourceOutput.columns.size(), source.origin);
        }
      } else if (m.columns.empty() && !m.rows.empty()) {
        const auto &row = m.rows.front();
        if (!row.empty())
          validateImplicitInsertWidth(m, row.size(),
              {m.expressions[row.front()].origin.begin, m.expressions[row.back()].origin.end});
      }
    }
    for (const auto &p : m.projections)
      bind(m, p.expression, visible, false, ctes, outer, pendingQueries,
           visibleShape);
    for (const auto &window : m.namedWindows)
      for (auto key : window.keys)
        bind(m, key, visible, false, ctes, outer, pendingQueries, visibleShape);
    bind(m, m.prewhere, visible, false, ctes, outer, pendingQueries, visibleShape);
    bind(m, m.where, visible, false, ctes, outer, pendingQueries, visibleShape);
    bind(m, m.qualify, visible, true, ctes, outer, pendingQueries, visibleShape);
    bind(m, m.systemTime, {}, false, ctes, {}, pendingQueries, std::make_shared<const Shape>());
    if (m.limitBy) {
      bind(m, m.limitBy->count, {}, false, ctes, {}, pendingQueries, std::make_shared<const Shape>());
      bind(m, m.limitBy->offset, {}, false, ctes, {}, pendingQueries, std::make_shared<const Shape>());
      for (auto key : m.limitBy->keys)
        bind(m, key, visible, true, ctes, outer, pendingQueries, visibleShape);
    }
    const bool mysqlAliases = isMySqlDialect(c.dialect);
    const bool expressionAliases = mysqlAliases || c.dialect == Dialect::SQLite;
    const bool starOutput = std::any_of(
        m.projections.begin(), m.projections.end(), [&](const auto &p) {
          return m.expressions[p.expression].kind == ExprKind::Star;
        });
    auto ordinal = [&](ExprId id, bool positions, const char *clause) {
      auto &e = m.expressions[id];
      const bool integer =
          e.kind == ExprKind::Number && !e.value.empty() &&
          e.value.find_first_not_of("0123456789") == std::string::npos;
      if (c.dialect == Dialect::PostgreSQL) {
        if ((e.kind == ExprKind::Number && !integer) ||
            e.kind == ExprKind::String || e.kind == ExprKind::Boolean ||
            e.kind == ExprKind::Null) {
          c.report(DiagnosticCode::InvalidQueryShape, e.origin,
                   std::string("non-integer constant in ") + clause);
          return;
        }
        if (e.kind == ExprKind::Unary && e.value == "-" &&
            e.operands.size() == 1 &&
            m.expressions[e.operands[0]].kind == ExprKind::Number) {
          c.report(DiagnosticCode::InvalidQueryShape, e.origin,
                   std::string(clause) +
                       " position is outside the SELECT list");
          return;
        }
      }
      if (!positions || !integer)
        return;
      try {
        auto number = std::stoull(e.value);
        if (!number || (!starOutput && number > m.projections.size())) {
          c.report(DiagnosticCode::InvalidQueryShape, e.origin,
                   std::string(clause) +
                       " position is outside the SELECT list");
          return;
        }
        // '*' expands to an output shape that need not have one projection
        // node per column. Keep that case unresolved instead of guessing.
        if (!starOutput)
          e.outputColumn = std::size_t(number - 1);
      } catch (const std::out_of_range &) {
        c.report(DiagnosticCode::InvalidQueryShape, e.origin,
                 std::string(clause) + " position is outside the SELECT list");
      }
    };
    if (m.kind == StatementKind::Select) {
      for (auto id : m.groupBy)
        ordinal(id,
                mysqlAliases || c.dialect == Dialect::PostgreSQL ||
                    c.dialect == Dialect::SQLite,
                "GROUP BY");
      for (const auto &order : m.orderBy)
        ordinal(order.expression, true, "ORDER BY");
    }
    for (auto id : m.groupBy)
      bind(m, id, visible,
           expressionAliases || c.dialect == Dialect::PostgreSQL, ctes, outer,
           pendingQueries, visibleShape, 1, true, expressionAliases);
    bind(m, m.having, visible, expressionAliases, ctes, outer, pendingQueries,
         visibleShape, 1, true, true, mysqlAliases);
    for (const auto &o : m.orderBy)
      bind(m, o.expression, visible, true, ctes, outer, pendingQueries,
           visibleShape, 1, false, expressionAliases);
    for (std::size_t i = 0; i < m.relations.size(); ++i)
      if (m.relations[i].condition) {
        auto scope = relationLeaves(m, RelationId(i + 1), [&] { checkStop(c); });
        bind(m, m.relations[i].condition, scope, false, ctes, outer,
             pendingQueries, shapes[i]);
      }
    for (const auto &target : m.deleteTargets)
      if (std::none_of(m.tables.begin(), m.tables.end(), [&](const auto &t) {
            return (t.name == target && !(isMySqlDialect(c.dialect) && !t.alias.empty())) ||
                   (target.size() == 1 && !t.alias.empty() &&
                    t.alias == target.front());
          }))
        c.report(DiagnosticCode::UnknownTable, s.range,
                 "DELETE target is not present in FROM/JOIN");
    const Visible targetVisible =
        m.targetTable ? Visible{*m.targetTable} : Visible{};
    const auto targetShape = shapeForTables(m, targetVisible);
    auto bindAssignments =
        [&](const std::vector<Assignment> &assignments, const Visible &rhsScope,
            std::shared_ptr<const Shape> rhsShape, bool multi) {
          std::vector<std::pair<std::string,bool>> assignedColumns;
          const bool strictAssignments = c.dialect == Dialect::PostgreSQL ||
                                         c.dialect == Dialect::Oracle ||
                                         c.dialect == Dialect::SQLServer;
          for (const auto &a : assignments) {
            for (std::size_t i=0;i<a.targets.size();++i) {
              const auto e=a.targets[i];
              const auto &destination=m.expressions[e];
              const bool partial=destination.kind==ExprKind::Subscript;
              const auto root=partial?destination.operands.front():e;
              m.expressions[root].localSymbol = 0;
              bind(m, root, multi ? visible : targetVisible, false, ctes, outer,
                   pendingQueries, multi ? visibleShape : targetShape);
              if (partial) {
                for (auto part=std::next(destination.operands.begin());part!=destination.operands.end();++part)
                  bind(m,*part,rhsScope,false,ctes,outer,pendingQueries,rhsShape);
                const auto &assigned=m.expressions[a.value];
                const auto &value=assigned.kind==ExprKind::Row && assigned.operands.size()==a.targets.size()
                    ?m.expressions[assigned.operands[i]]:assigned;
                if (value.kind==ExprKind::Default)
                  c.report(DiagnosticCode::InvalidQueryShape,value.origin,
                           "DEFAULT is unavailable for a subscripted assignment");
              }
              const auto &target = m.expressions[root];
              if(m.kind==StatementKind::TriggerSet && !outer.empty() &&
                 outer.back().query->trigger && target.name.size()==2 &&
                 asciiUpper(target.name.front())=="NEW") {
                const auto &trigger=*outer.back().query->trigger;
                if(trigger.timing!=TriggerTiming::Before || trigger.event==TriggerEvent::Delete)
                  c.report(DiagnosticCode::InvalidQueryShape,target.origin,
                           "SET NEW is only valid in a BEFORE INSERT or BEFORE UPDATE trigger");
              }
              if (strictAssignments && !target.name.empty()) {
                if (std::any_of(assignedColumns.begin(), assignedColumns.end(),
                                [&](const auto &previous) {
                                  return (!partial || !previous.second) && columnEquals(previous.first,target.name.back());
                                }))
                  c.report(DiagnosticCode::InvalidQueryShape, target.origin,
                           "target column is assigned more than once in the "
                           "same SET clause");
                assignedColumns.emplace_back(target.name.back(),partial);
              }
            }
            bind(m, a.value, rhsScope, false, ctes, outer, pendingQueries,
                 rhsShape, a.targets.size());
          }
        };
    if (m.kind == StatementKind::Merge) {
      bind(m, m.mergeOn, visible, false, ctes, outer, pendingQueries,
           visibleShape);
      Visible sourceVisible;
      for (auto table : visible)
        if (!m.targetTable || table != *m.targetTable)
          sourceVisible.push_back(table);
      const auto sourceShape = shapeForTables(m, sourceVisible);
      for (std::size_t i = 0; i < m.mergeClauses.size(); ++i) {
        const auto &clause = m.mergeClauses[i];
        const auto &branchVisible =
            clause.match == MergeMatch::Matched            ? visible
            : clause.match == MergeMatch::NotMatchedTarget ? sourceVisible
                                                           : targetVisible;
        const auto branchShape =
            clause.match == MergeMatch::Matched            ? visibleShape
            : clause.match == MergeMatch::NotMatchedTarget ? sourceShape
                                                           : targetShape;
        bind(m, clause.condition, branchVisible, false, ctes, outer,
             pendingQueries, branchShape);
        bind(m, clause.deleteWhere, branchVisible, false, ctes, outer,
             pendingQueries, branchShape);
        bindAssignments(clause.assignments, branchVisible, branchShape, false);
        for (auto id : clause.values)
          bind(m, id, branchVisible, false, ctes, outer, pendingQueries,
               branchShape);
        validateTargetColumns(clause.columns,s.mergeClauses[i].columns,
                              m.target?m.target->definition.get():nullptr,"MERGE INSERT");
        if (clause.columns.empty())
          validateImplicitInsertWidth(m, clause.values.size(), clause.origin);
      }
      if (c.dialect == Dialect::Oracle && m.targetTable) {
        std::set<std::string> keys;
        std::vector<ExprId> pending{m.mergeOn};
        std::set<ExprId> seen;
        while (!pending.empty()) {
          const auto id = pending.back();
          pending.pop_back();
          if (!seen.insert(id).second)
            continue;
          const auto &e = m.expressions.at(id);
          if (e.kind == ExprKind::Name && !e.name.empty()) {
            bool target = e.binding && e.binding->scope == m.id &&
                          e.binding->table == *m.targetTable;
            if (!e.binding && e.name.size() == 2 && m.target)
              target =
                  e.name[0] == (m.target->alias.empty() ? m.target->name.back()
                                                        : m.target->alias);
            if (target)
              keys.insert(e.name.back());
          }
          pending.insert(pending.end(), e.operands.begin(), e.operands.end());
        }
        for (const auto &clause : m.mergeClauses)
          for (const auto &a : clause.assignments)
            for (auto id : a.targets) {
              const auto &e = m.expressions.at(id);
              if (!e.name.empty() && keys.count(e.name.back()))
                c.report(DiagnosticCode::InvalidQueryShape, e.origin,
                         "Oracle MERGE cannot update a target column "
                         "referenced by ON");
            }
      }
    }
    bindAssignments(
        m.assignments, m.kind == StatementKind::Insert ? Visible{} : visible,
        m.kind == StatementKind::Insert ? std::make_shared<const Shape>()
                                        : visibleShape,
        s.targetInTables && s.kind == StatementKind::Update);
    for (const auto &row : m.rows)
      for (auto e : row)
        bind(m, e, {}, false, ctes, outer, pendingQueries,
             std::make_shared<const Shape>());
    for (auto e : m.conflict.targets)
      bind(m, e, targetVisible, false, ctes, outer, pendingQueries,
           targetShape);
    bind(m, m.conflict.targetWhere, targetVisible, false, ctes, outer,
         pendingQueries, targetShape);
    Visible conflictVisible = targetVisible;
    auto conflictShape = targetShape;
    if (m.conflict.action == ConflictClause::Action::Update && m.target) {
      auto excluded = *m.target;
      excluded.name = {"excluded"};
      excluded.alias = "excluded";
      excluded.pseudo = true;
      conflictVisible.push_back(m.tables.size());
      m.scope.tables.push_back({"excluded", m.tables.size()});
      m.tables.push_back(std::move(excluded));
    }
    if (!m.conflict.rowAlias.empty() && m.target) {
      if (relationEquals(m.conflict.rowAlias, m.target->name.back()))
        c.report(DiagnosticCode::DuplicateAlias, s.conflict.rowAlias->range,
                 "inserted row alias conflicts with the target table");
      auto incoming = insertedRow(m);
      m.conflict.incomingTable = m.tables.size();
      conflictVisible.push_back(m.tables.size());
      m.scope.tables.push_back({incoming.alias, m.tables.size()});
      m.tables.push_back(std::move(incoming));
      conflictShape = shapeForTables(m, conflictVisible);
    }
    // MySQL exposes both sets of columns in the update values. Bare names
    // common to the target and the incoming row are ambiguous.
    bindAssignments(m.conflict.assignments, conflictVisible, conflictShape,
                    false);
    bind(m, m.conflict.where, conflictVisible, false, ctes, outer,
         pendingQueries, conflictShape);
    Visible returnVisible = visible;
    auto returnShape = visibleShape;
    const bool sqlServerOutput = c.dialect == Dialect::SQLServer && (!m.returning.empty() || m.outputInto);
    if (sqlServerOutput && m.target && m.targetTable) {
      // T-SQL OUTPUT reads the target through inserted/deleted. Other
      // source relations are accessible only by qualified column references.
      returnVisible.erase(std::remove(returnVisible.begin(),returnVisible.end(),*m.targetTable),returnVisible.end());
      returnShape=std::make_shared<const Shape>();
      for (const auto *label : {"inserted", "deleted"}) {
        if ((m.kind == StatementKind::Insert &&
             std::string(label) == "deleted") ||
            (m.kind == StatementKind::Delete &&
             std::string(label) == "inserted"))
          continue;
        auto pseudo = *m.target;
        pseudo.name = {label};
        pseudo.alias = label;
        pseudo.pseudo = true;
        returnVisible.push_back(m.tables.size());
        m.scope.tables.push_back({label, m.tables.size()});
        m.tables.push_back(std::move(pseudo));
      }
    }
    const auto bindOutput=[&](const ModelProjection &p) {
      const auto &expression=m.expressions.at(p.expression);
      if (sqlServerOutput && expression.kind==ExprKind::Star && expression.name.empty())
        c.report(DiagnosticCode::InvalidQueryShape,expression.origin,
                 "OUTPUT star requires a row or table qualifier");
      bind(m, p.expression, returnVisible, false, ctes, outer, pendingQueries,
           returnShape);
    };
    for (const auto &p : m.returning) bindOutput(p);
    if (m.outputInto)
      for (const auto &p:m.outputInto->projections) bindOutput(p);
    for (auto e : m.returningInto)
      bind(m, e, {}, false, ctes, outer, pendingQueries,
           std::make_shared<const Shape>());
    const Frames noPaginationOuter;
    const auto &paginationOuter=c.dialect==Dialect::SQLite?noPaginationOuter:outer;
    bind(m, m.limit, {}, false, ctes, paginationOuter, pendingQueries,
         std::make_shared<const Shape>());
    bind(m, m.offset, {}, false, ctes, paginationOuter, pendingQueries,
         std::make_shared<const Shape>());
    for (auto argument : m.arguments)
      bind(m, argument, {}, false, ctes, outer, pendingQueries,
           std::make_shared<const Shape>());
    if (!m.routine.empty())
      validateRoutine(m.routine, m.arguments.size(), true, s.range);
    if (m.ddl) {
      const auto &ddl = *m.ddl;
      const auto ddlShape=m.source && !ddl.tableIndexes.empty()?shapeForTables(m,visible):visibleShape;
      auto bindDdl = [&](ExprId e, bool local) {
        bind(m, e, local ? visible : Visible{}, false, ctes, outer,
             pendingQueries,
             local ? ddlShape : std::make_shared<const Shape>());
      };
      auto constraint = [&](const auto &k) { bindDdl(k.check, true); };
      auto column = [&](const auto &col) {
        bindDdl(col.defaultValue, false);
        bindDdl(col.generated, true);
        bindDdl(col.identityStart, false);
        bindDdl(col.identityIncrement, false);
        for (const auto &k : col.constraints)
          constraint(k);
      };
      for (const auto &col : ddl.definitions)
        column(col);
      for (const auto &k : ddl.constraints)
        constraint(k);
      for (const auto &action : ddl.actions) {
        bindDdl(action.value, action.kind == AlterKind::AlterType);
        if (action.column)
          column(*action.column);
        if (action.constraint)
          constraint(*action.constraint);
      }
      for (const auto &key : ddl.keys) {
        bindDdl(key.expression, true);
        bindDdl(key.prefixLength, false);
      }
      for (const auto &index:ddl.tableIndexes)
        for (const auto &key:index.keys) {
          bindDdl(key.expression,true);
          bindDdl(key.prefixLength,false);
        }
      bindDdl(ddl.predicate, true);
      bindDdl(ddl.commentValue, false);
      for (const auto &opt : ddl.sequenceOptions)
        bindDdl(opt.second, false);
      if (s.kind == StatementKind::Create && ddl.object == ObjectKind::Table &&
          !ddl.definitions.empty()) {
        std::unordered_map<std::string, bool> columnsSeen, constraintsSeen;
        std::size_t primaryKeys = 0;
        for (const auto &col : ddl.definitions)
          if (!columnsSeen.emplace(col.name, true).second)
            c.report(DiagnosticCode::DuplicateAlias, col.origin,
                     "duplicate declared column: " + col.name);
        auto validateConstraint = [&](const auto &key) {
          if (key.name && !constraintsSeen.emplace(*key.name, true).second)
            c.report(DiagnosticCode::DuplicateAlias, key.origin,
                     "duplicate constraint name");
          if (key.kind == ConstraintKind::PrimaryKey && ++primaryKeys > 1)
            c.report(DiagnosticCode::InvalidQueryShape, key.origin,
                     "multiple primary keys");
          for (const auto &column : key.columns)
            if (!columnsSeen.count(column))
              c.report(DiagnosticCode::UnknownColumn, key.origin,
                       "constraint column is not declared: " + column);
        };
        for (const auto &col : ddl.definitions)
          for (const auto &key : col.constraints)
            validateConstraint(key);
        for (const auto &key : ddl.constraints)
          validateConstraint(key);
      }
      if (m.source && !ddl.columns.empty()) {
        const auto definition = output(c.model.queries[m.source - 1]);
        if (definition.arityKnown &&
            definition.columns.size() != ddl.columns.size())
          c.report(DiagnosticCode::InvalidQueryShape, s.range,
                   "DDL column/source query counts differ");
      }
    }
    auto id = m.id;
    inferModelTypes(m, c.dialect);
    if (m.valuesQuery) finishValuesOutput(m);
    c.model.queries[id - 1] = std::move(m);
    return id;
  }

  std::optional<std::size_t> selectArity(QueryId id) {
    const auto &statement = c.model.queries.at(id - 1);
    if (statement.kind != StatementKind::Select)
      return {};
    if (statement.setOperation && !statement.tables.empty() &&
        statement.tables.front().definition && !statement.tables.front().definition->arityKnown)
      return {};
    const auto definition = output(statement);
    if (definition.arityKnown)
      return definition.columns.size();
    return {};
  }

private:
  void lowerPackage(ModelStatement &statement,const PackageSyntax &syntax,
                    const std::unordered_map<ExprId,ExprId> &ids) {
    using Kind=PackageDeclarationKind;
    auto package=std::make_shared<ModelPackage>();
    package->name=qualified(c,syntax.name);
    package->edition=syntax.edition;
    package->auth=syntax.auth;
    package->drop=syntax.drop;
    package->replace=syntax.replace;
    statement.transactionEffect=TransactionEffect::ImplicitCommit;
    if(syntax.endName) {
      package->endName=identifier(c,*syntax.endName);
      if(*package->endName!=package->name.back())
        c.report(DiagnosticCode::InvalidQueryShape,syntax.endName->range,
                 "package END name does not match its declaration");
    }
    statement.context.environment=SemanticEnvironment::ProceduralScalar;
    statement.context.locals.clear();
    std::map<std::string,ExternalSymbol> values;
    std::set<std::string> nonRoutines,routineNames;
    std::set<std::vector<std::string>> signatures;
    const auto bindInitializer=[&](ExprId root) {
      if(!root)return;
      std::vector<ExprId> work{root};
      std::set<ExprId> visited;
      while(!work.empty()) {
        const auto id=work.back();work.pop_back();
        if(!visited.insert(id).second)continue;
        checkStop(c);
        auto &expression=statement.expressions.at(id);
        if(expression.kind==ExprKind::Name) {
          const bool packageName=expression.name.size()==2 && expression.name.front()==package->name.back();
          if(expression.name.size()!=1 && !packageName) {
            c.report(DiagnosticCode::UnsupportedSyntax,expression.origin,
                     "package initializer external references require catalog-backed package binding");
            continue;
          }
          const auto found=values.find(expression.name.back());
          if(found==values.end()) {
            c.report(DiagnosticCode::UnknownColumn,expression.origin,
                     "package state is referenced before its declaration: "+expression.name.back());
            continue;
          }
          expression.localSymbol=found->second.symbol;
          expression.type=found->second.type;
        } else if(expression.kind!=ExprKind::Null && expression.kind!=ExprKind::Number &&
                  expression.kind!=ExprKind::String && expression.kind!=ExprKind::Boolean &&
                  expression.kind!=ExprKind::Unary && expression.kind!=ExprKind::Binary &&
                  expression.kind!=ExprKind::Case && expression.kind!=ExprKind::Cast) {
          c.report(DiagnosticCode::UnsupportedSyntax,expression.origin,
                   "package initializer needs a scalar literal or earlier package state");
          continue;
        }
        work.insert(work.end(),expression.operands.begin(),expression.operands.end());
      }
    };
    const auto lowerType=[&](const auto &input,TypeUse use) {
      auto type=lowerTypeSpec(c,input);
      auto resolved=resolveDeclaredType(type,c.dialect,use);
      if(!resolved.error.empty())
        c.report(DiagnosticCode::InvalidQueryShape,type.origin,resolved.error);
      if(use==TypeUse::Signature && (!type.arguments.empty() || type.lengthSemantics))
        c.report(DiagnosticCode::InvalidQueryShape,type.origin,
                 "Oracle formal parameter and result types cannot contain inline size constraints");
      return std::make_pair(std::move(type),std::move(resolved.type));
    };
    for(const auto &member:syntax.declarations) {
      checkStop(c);
      BasicPackageDeclaration<std::string,std::vector<std::string>> declaration;
      declaration.name=identifier(c,member.name);
      declaration.kind=member.kind;
      declaration.deterministic=member.deterministic;
      declaration.initializer=ids.at(member.initializer);
      declaration.origin=member.origin;
      const bool routine=member.kind==Kind::Procedure || member.kind==Kind::Function;
      if(routine) {
        if(nonRoutines.count(declaration.name))
          c.report(DiagnosticCode::InvalidQueryShape,member.name.range,"package name collides with a state declaration");
        routineNames.insert(declaration.name);
        std::set<std::string> parameters;
        std::vector<std::string> signature{declaration.name};
        for(const auto &parameter:member.parameters) {
          checkStop(c);
          BasicPackageParameter<std::string,std::vector<std::string>> bound;
          bound.name=identifier(c,parameter.name);
          bound.mode=parameter.mode;
          bound.noCopy=parameter.noCopy;
          bound.origin=parameter.origin;
          bound.defaultValue=ids.at(parameter.defaultValue);
          auto [type,resolved]=lowerType(parameter.type,TypeUse::Signature);
          bound.type=std::move(type);
          if(!parameters.insert(bound.name).second)
            c.report(DiagnosticCode::InvalidQueryShape,parameter.name.range,"duplicate package formal parameter");
          if((bound.noCopy && bound.mode==ParameterMode::In) ||
             (bound.defaultValue && bound.mode!=ParameterMode::In))
            c.report(DiagnosticCode::InvalidQueryShape,parameter.origin,
                     "NOCOPY requires OUT or IN OUT, and only IN parameters may have defaults");
          signature.push_back(bound.name);
          // Exact spellings, and the built-in NUMBER family, cannot overload on mode alone.
          std::string typeKey;
          if(resolved.family==TypeFamily::Decimal && resolved.opaqueName.empty())typeKey="NUMBER";
          else for(const auto &part:bound.type.name)typeKey+=std::to_string(part.size())+":"+part;
          signature.push_back(std::move(typeKey));
          bindInitializer(bound.defaultValue);
          declaration.parameters.push_back(std::move(bound));
        }
        if(!signatures.insert(std::move(signature)).second)
          c.report(DiagnosticCode::InvalidQueryShape,member.origin,"duplicate package routine signature");
        if(member.kind==Kind::Function)declaration.type=lowerType(member.type,TypeUse::Signature).first;
      } else {
        const bool inserted=nonRoutines.insert(declaration.name).second;
        if(!inserted || routineNames.count(declaration.name))
          c.report(DiagnosticCode::InvalidQueryShape,member.name.range,"duplicate package declaration");
        if(member.kind==Kind::Constant && !declaration.initializer)
          c.report(DiagnosticCode::InvalidQueryShape,member.origin,"package constant requires an initializer");
        bindInitializer(declaration.initializer);
        if(member.kind!=Kind::Exception) {
          auto [type,resolved]=lowerType(member.type,TypeUse::Declaration);
          declaration.type=std::move(type);
          ExternalSymbol symbol;
          symbol.name={declaration.name};
          symbol.symbol=static_cast<std::uint32_t>(package->declarations.size()+1);
          symbol.type=typeCategory(declaration.type.name,c.dialect);
          symbol.sqlType=std::make_shared<const SqlType>(std::move(resolved));
          values.emplace(declaration.name,symbol);
          statement.context.locals.push_back(std::move(symbol));
        }
      }
      package->declarations.push_back(std::move(declaration));
    }
    statement.package=std::move(package);
  }
  std::vector<std::size_t> cteDependencyOrder(const SelectStatement &owner) {
    std::vector<std::size_t> order;
    order.reserve(owner.ctes.size());
    if (!owner.recursiveWith || c.dialect != Dialect::PostgreSQL || owner.ctes.size() < 2) {
      for (std::size_t i = 0; i < owner.ctes.size(); ++i) order.push_back(i);
      return order;
    }
    if (owner.ctes.size() > c.options.limits.queries) limit("CTE dependency count exceeded");
    std::unordered_map<std::string, std::size_t> siblings;
    for (std::size_t i = 0; i < owner.ctes.size(); ++i) {
      const auto &cte = owner.ctes[i];
      const auto name = identifier(c, cte.name);
      if (!siblings.emplace(name, i).second) {
        c.report(DiagnosticCode::DuplicateAlias, cte.range, "duplicate CTE name: " + name);
        throw CompilationAborted{};
      }
    }
    std::vector<std::vector<std::size_t>> consumers(owner.ctes.size());
    std::vector<std::size_t> remaining(owner.ctes.size());
    const auto checkVisit = [&] {
      if (++cteDependencyVisits > c.options.limits.nodes * 4 + c.options.limits.queries)
        limit("CTE dependency traversal limit exceeded");
      if ((cteDependencyVisits & 255u) == 0) checkStop(c);
    };
    for (std::size_t index = 0; index < owner.ctes.size(); ++index) {
      std::set<std::size_t> dependencies;
      std::unordered_map<std::string, std::size_t> shadowed;
      const auto inspect = [&](const auto &self, const SelectStatement &query, std::size_t depth) -> void {
        checkVisit();
        if (depth >= c.options.limits.nesting) limit("CTE dependency nesting exceeded");
        if (query.recursiveWith)
          for (const auto &nested : query.ctes) ++shadowed[identifier(c, nested.name)];
        for (const auto &nested : query.ctes) {
          self(self, c.ast.query(nested.query), depth + 1);
          if (!query.recursiveWith) ++shadowed[identifier(c, nested.name)];
        }
        for (const auto &table : query.tables) {
          checkVisit();
          if (table.query) self(self, c.ast.query(table.query), depth + 1);
          else if (table.name.size() == 1) {
            const auto name = identifier(c, table.name.front());
            const auto candidate = siblings.find(name);
            if (candidate != siblings.end() && candidate->second != index && !shadowed.count(name))
              dependencies.insert(candidate->second);
          }
        }
        if (query.setOperation) {
          self(self, c.ast.query(query.setOperation->left), depth + 1);
          self(self, c.ast.query(query.setOperation->right), depth + 1);
        }
        if (query.source) self(self, c.ast.query(query.source), depth + 1);
        auto work = expressionInputs(query);
        std::set<ExprId> seen;
        while (!work.empty()) {
          const auto id = work.back();
          work.pop_back();
          if (!id || !seen.insert(id).second) continue;
          checkVisit();
          const auto &expression = c.ast.expression(id);
          if (expression.query) self(self, c.ast.query(expression.query), depth + 1);
          work.insert(work.end(), expression.operands.begin(), expression.operands.end());
        }
        for (const auto &nested : query.ctes) {
          const auto name = identifier(c, nested.name);
          if (--shadowed.at(name) == 0) shadowed.erase(name);
        }
      };
      inspect(inspect, c.ast.query(owner.ctes[index].query), 0);
      remaining[index] = dependencies.size();
      for (auto dependency : dependencies) consumers[dependency].push_back(index);
    }
    std::set<std::size_t> ready;
    for (std::size_t i = 0; i < remaining.size(); ++i)
      if (!remaining[i]) ready.insert(i);
    while (!ready.empty()) {
      checkStop(c);
      const auto next = *ready.begin();
      ready.erase(ready.begin());
      order.push_back(next);
      for (auto dependent : consumers[next])
        if (--remaining[dependent] == 0) ready.insert(dependent);
    }
    if (order.size() != owner.ctes.size()) {
      const auto first = std::find_if(remaining.begin(), remaining.end(), [](auto count) { return count != 0; });
      c.report(DiagnosticCode::UnsupportedSyntax, owner.ctes[first - remaining.begin()].range,
               "mutual recursion between PostgreSQL WITH items is not supported");
      throw CompilationAborted{};
    }
    return order;
  }
  std::vector<ExprId> expressionInputs(const SelectStatement &s) {
    std::vector<ExprId> work;
    for (const auto &window : s.namedWindows)
      work.insert(work.end(), window.keys.begin(), window.keys.end());
    for (auto argument : s.arguments)
      work.push_back(argument);
    if (s.prepared) {
      work.push_back(s.prepared->source);
      work.insert(work.end(), s.prepared->arguments.begin(), s.prepared->arguments.end());
    }
    for (const auto &p : s.projections)
      work.push_back(p.expression);
    for (const auto &t : s.tables) {
      if (!t.lateralLeft || t.jsonTable) work.push_back(t.condition);
      if (t.jsonTable) work.push_back(t.jsonTable->document);
      if (t.pivot)
        for (const auto &bucket:t.pivot->buckets)
          work.insert(work.end(),bucket.values.begin(),bucket.values.end());
      if (t.unpivot) for (const auto &input:t.unpivot->inputs)
        work.insert(work.end(),input.labels.begin(),input.labels.end());
    }
    if (s.inspection) work.push_back(s.inspection->pattern);
    if (s.pragma) work.push_back(s.pragma->argument);
    if (s.trigger) work.push_back(s.trigger->when);
    if(s.package)for(const auto &declaration:s.package->declarations) {
      work.push_back(declaration.initializer);
      for(const auto &parameter:declaration.parameters)work.push_back(parameter.defaultValue);
    }
    work.push_back(s.prewhere);
    work.push_back(s.qualify);
    work.push_back(s.systemTime);
    if (s.limitBy) {
      work.push_back(s.limitBy->count);
      work.push_back(s.limitBy->offset);
      work.insert(work.end(), s.limitBy->keys.begin(), s.limitBy->keys.end());
    }
    work.push_back(s.where);
    work.push_back(s.having);
    work.push_back(s.limit.count);
    work.push_back(s.limit.offset);
    for (auto id : s.groupBy)
      work.push_back(id);
    for (const auto &o : s.orderBy)
      work.push_back(o.expression);
    for (const auto &row : s.rows)
      for (auto e : row)
        work.push_back(e);
    auto assignmentWork = [&](const std::vector<Assignment> &assignments) {
      for (const auto &a : assignments) {
        work.push_back(a.value);
        for (auto t : a.targets)
          work.push_back(t);
      }
    };
    assignmentWork(s.assignments);
    assignmentWork(s.conflict.assignments);
    work.push_back(s.mergeOn);
    for (const auto &clause : s.mergeClauses) {
      work.push_back(clause.condition);
      work.push_back(clause.deleteWhere);
      work.insert(work.end(), clause.values.begin(), clause.values.end());
      assignmentWork(clause.assignments);
    }
    for (const auto &p : s.returning)
      work.push_back(p.expression);
    if (s.outputInto)
      for (const auto &p:s.outputInto->projections) work.push_back(p.expression);
    for (auto e : s.returningInto)
      work.push_back(e);
    for (auto e : s.conflict.targets)
      work.push_back(e);
    work.push_back(s.conflict.targetWhere);
    work.push_back(s.conflict.where);
    for (const auto &r : s.relations)
      if (!r.lateral || r.implicitLateral) work.push_back(r.condition);
    if (s.ddl)
      collectDdlExpressions(*s.ddl, work);
    return work;
  }
  Visible lateralInputs(const Statement &s, RelationId root, std::size_t built) {
    Visible inputs;
    std::vector<RelationId> work{root};
    while (!work.empty()) {
      checkStop(c);
      const auto id = work.back();
      work.pop_back();
      const auto &relation = s.relations.at(id - 1);
      if (relation.left) {
        work.push_back(relation.right);
        work.push_back(relation.left);
      } else {
        if (relation.table >= built) limit("lateral scope references an unbound input");
        inputs.push_back(relation.table);
      }
    }
    return inputs;
  }
  void validateDistribution(const ModelDdl &ddl, StatementKind statement) {
    using Option = DistributionOptionKind;
    using Clustering = PrimaryKeyClustering;
    const auto error = [&](SourceRange range, std::string message) {
      c.report(DiagnosticCode::InvalidQueryShape, range, std::move(message));
    };
    std::set<Option> seen;
    std::optional<std::uint64_t> shards;
    const DistributionOption *preSplit = nullptr;
    for (const auto &option : ddl.distribution) {
      if (!seen.insert(option.kind).second)
        error(option.origin, "duplicate " + std::string(distributionOptionName(option.kind)));
      if (option.kind == Option::ShardRowIdBits) {
        shards = std::get<std::uint64_t>(option.value);
        if (*shards > 15) error(option.origin, "SHARD_ROW_ID_BITS must be between 0 and 15");
      } else if (option.kind == Option::PreSplitRegions) {
        preSplit = &option;
        if (std::get<std::uint64_t>(option.value) > 15)
          error(option.origin, "PRE_SPLIT_REGIONS must be between 0 and 15");
      } else if (option.kind == Option::Sharding) {
        const auto &mode = std::get<std::string>(option.value);
        if (mode != "NONE" && mode != "PARTITION" && mode != "ADAPTIVE")
          error(option.origin, "SHARDING must be NONE, PARTITION, or ADAPTIVE");
      }
    }
    if (c.dialect != Dialect::TiDB || ddl.object != ObjectKind::Table) return;

    const BasicConstraint<std::string, std::vector<std::string>> *primary = nullptr;
    const auto findPrimary = [&](const auto &constraint) {
      if (constraint.kind == ConstraintKind::PrimaryKey && !primary) primary = &constraint;
    };
    for (const auto &constraint : ddl.constraints) findPrimary(constraint);
    for (const auto &column : ddl.definitions)
      for (const auto &constraint : column.constraints) findPrimary(constraint);

    const bool creating = statement == StatementKind::Create;
    std::optional<std::uint64_t> randomShards;
    const auto checkColumn = [&](const auto &column) {
      if (!column.autoRandom) return;
      const auto &random = *column.autoRandom;
      const auto shardBits = random.shardBits.value_or(5);
      const auto rangeBits = random.rangeBits.value_or(64);
      if (randomShards) error(random.origin, "only one column may use AUTO_RANDOM");
      randomShards = shardBits;
      if (shardBits < 1 || shardBits > 15)
        error(random.origin, "AUTO_RANDOM shard bits must be between 1 and 15");
      if (rangeBits < 32 || rangeBits > 64)
        error(random.origin, "AUTO_RANDOM range bits must be between 32 and 64");
      if (!column.type.name.empty() &&
          (column.type.name.size() != 1 ||
           (asciiUpper(column.type.name.front()) != "BIGINT" && asciiUpper(column.type.name.front()) != "INT8")))
        error(random.origin, "AUTO_RANDOM requires a BIGINT column");
      const auto unsignedType = std::any_of(column.type.modifiers.begin(), column.type.modifiers.end(),
          [](const auto &word) { return asciiUpper(word) == "UNSIGNED" || asciiUpper(word) == "ZEROFILL"; });
      if (rangeBits <= 64 && shardBits <= 15 && rangeBits < shardBits + (unsignedType ? 27 : 28))
        error(random.origin, "AUTO_RANDOM must leave at least 27 auto-increment bits");
      if (column.defaultValue || column.generated || column.identity != IdentityKind::None)
        error(random.origin, "AUTO_RANDOM conflicts with DEFAULT, GENERATED or AUTO_INCREMENT");
      if (creating && (!primary || primary->columns.empty() ||
          !columnEquals(primary->columns.front(), column.name) ||
          primary->clustering == Clustering::Nonclustered))
        error(random.origin, "AUTO_RANDOM requires the first column of a clustered PRIMARY KEY");
    };
    for (const auto &column : ddl.definitions) checkColumn(column);
    for (const auto &action : ddl.actions)
      if (action.column) checkColumn(*action.column);

    if (shards.value_or(0) &&
        (randomShards || (primary && primary->clustering == Clustering::Clustered))) {
      const auto option = std::find_if(ddl.distribution.begin(), ddl.distribution.end(),
          [](const auto &value) { return value.kind == Option::ShardRowIdBits; });
      error(option->origin, "SHARD_ROW_ID_BITS requires an implicit row ID");
    }
    const auto splitBits = randomShards ? randomShards : shards;
    if (preSplit && splitBits && std::get<std::uint64_t>(preSplit->value) > *splitBits)
      error(preSplit->origin, "PRE_SPLIT_REGIONS exceeds the configured shard bits");
  }
  void validatePartition(const ModelDdl &ddl, const BasicTablePartition<NamePart> &syntax) {
    const auto &partition = *ddl.partition;
    using Kind = ConstraintKind;
    const auto unique = [](Kind kind) { return kind == Kind::PrimaryKey || kind == Kind::Unique; };
    std::set<std::string> seen;
    for (std::size_t i = 0; i < partition.columns.size(); ++i) {
      checkStop(c);
      const auto &column = partition.columns[i];
      const auto range = syntax.columns[i].range;
      const auto usesColumn = [&](const auto &name) { return asciiUpper(name) == asciiUpper(column); };
      if (!seen.insert(asciiUpper(column)).second) {
        c.report(DiagnosticCode::DuplicateAlias, range, "partition column is repeated: " + column);
        continue;
      }
      const auto definition = std::find_if(ddl.definitions.begin(), ddl.definitions.end(), [&](const auto &part) {
        return usesColumn(part.name);
      });
      if (definition == ddl.definitions.end()) {
        c.report(DiagnosticCode::UnknownColumn, range, "partition column is not declared: " + column);
        continue;
      }
      if (partition.method == PartitionMethod::Hash) {
        const auto resolved = resolveDeclaredType(definition->type, c.dialect, TypeUse::Declaration);
        if (resolved.error.empty() && resolved.type.family != TypeFamily::Unknown &&
            resolved.type.family != TypeFamily::Integer && resolved.type.family != TypeFamily::Boolean)
          c.report(DiagnosticCode::InvalidQueryShape, range, "HASH partition column must be an integer");
      }
      for (const auto &constraint : ddl.constraints)
        if (unique(constraint.kind) && std::none_of(constraint.columns.begin(), constraint.columns.end(), usesColumn))
          c.report(DiagnosticCode::InvalidQueryShape, range, "every unique key must contain partition column: " + column);
      for (const auto &other : ddl.definitions)
        for (const auto &constraint : other.constraints)
          if (unique(constraint.kind) && !usesColumn(other.name))
            c.report(DiagnosticCode::InvalidQueryShape, range, "every unique key must contain partition column: " + column);
    }
  }
  void applyRowMatch(const RowMatchSyntax &syntax,ModelTable &table) {
    ModelRowMatch match;
    if (syntax.partition) match.partition=identifier(c,*syntax.partition);
    match.order=identifier(c,syntax.order);
    match.measure=identifier(c,syntax.measure);
    match.variable=identifier(c,syntax.variable);
    match.testColumn=identifier(c,syntax.testColumn);
    match.comparison=syntax.comparison;
    match.threshold=syntax.threshold;
    match.descending=syntax.descending;
    match.nullOrdering=syntax.nullOrdering;
    match.origin=syntax.origin;
    const auto input=table.definition;
    const auto field=[&](std::string_view name,SourceRange range)->const ColumnDefinition * {
      if (!input || !input->complete || !input->arityKnown) return nullptr;
      const auto found=std::find_if(input->columns.begin(),input->columns.end(),[&](const auto &value) {
        return columnEquals(value.name,name);
      });
      if (found!=input->columns.end()) return &*found;
      c.report(DiagnosticCode::UnknownColumn,range,"row pattern input column not found: "+std::string(name));
      return nullptr;
    };
    field(match.order,syntax.order.range);
    if (const auto *test=field(match.testColumn,syntax.testColumn.range);test && test->sqlType) {
      const auto family=test->sqlType->family;
      if (test->sqlType->arrayDimensions || (family!=TypeFamily::Integer &&
          family!=TypeFamily::Decimal && family!=TypeFamily::Numeric))
        c.report(DiagnosticCode::InvalidQueryShape,syntax.testColumn.range,
                 "row pattern comparison needs a numeric input column");
    }
    TableDefinition output;
    output.name=table.alias.empty()?table.name.back():table.alias;
    output.kind=ObjectKind::View;
    if (match.partition) {
      const auto *column=field(*match.partition,syntax.partition->range);
      output.columns.push_back(column?*column:ColumnDefinition{*match.partition});
      if (columnEquals(*match.partition,match.measure))
        c.report(DiagnosticCode::DuplicateAlias,syntax.measure.range,
                 "row pattern measure duplicates the partition column");
    }
    const auto type=std::make_shared<const SqlType>(resolveAggregateType(c.dialect,"COUNT",{}).type);
    output.columns.push_back({match.measure,valueCategory(*type),type,true});
    chargeColumns(output.columns.size());
    table.matchInput=input;
    table.definition=std::make_shared<const TableDefinition>(std::move(output));
    table.rowMatch=std::move(match);
  }
  void applyPivot(const PivotSyntax &syntax, const std::unordered_map<ExprId, ExprId> &ids,
                  const ModelSelect &query, ModelTable &table) {
    auto pivot = std::make_shared<ModelPivot>();
    pivot->aggregate = syntax.aggregate;
    pivot->argument = qualified(c, syntax.argument);
    pivot->axes = qualified(c, syntax.axes);
    pivot->origin = syntax.origin;
    pivot->argumentOrigin = syntax.argumentOrigin;
    pivot->axisOrigin = syntax.axisOrigin;
    if (syntax.aggregateAlias) pivot->aggregateAlias = identifier(c, *syntax.aggregateAlias);
    if (syntax.inputAlias) pivot->inputAlias = identifier(c, *syntax.inputAlias);
    if (syntax.relationAlias) pivot->relationAlias = identifier(c, *syntax.relationAlias);
    const auto input = table.definition;
    const bool known = input && input->complete && input->arityKnown;
    const auto sourceColumn = [&](const auto &name, SourceRange range) -> std::optional<std::size_t> {
      if (name.empty()) return {};
      if (name.size() > 1) {
        const std::vector<std::string> prefix(name.begin(), name.end() - 1);
        const bool matches = pivot->inputAlias
            ? prefix.size() == 1 && relationEquals(prefix.front(), *pivot->inputAlias)
            : (prefix.size() == 1 && !table.name.empty() && relationEquals(prefix.front(), table.name.back())) ||
              (prefix.size() == table.name.size() && std::equal(prefix.begin(), prefix.end(), table.name.begin(),
                   [&](const auto &a, const auto &b) { return relationEquals(a, b); }));
        if (!matches) {
          c.report(DiagnosticCode::UnknownColumn, range, "PIVOT column is outside the input relation");
          return {};
        }
      }
      if (!known) return {};
      std::optional<std::size_t> found;
      for (std::size_t i = 0; i < input->columns.size(); ++i) {
        if (!columnEquals(name.back(), input->columns[i].name)) continue;
        if (found) {
          c.report(DiagnosticCode::AmbiguousColumn, range, "ambiguous PIVOT input column: " + name.back());
          return {};
        }
        found = i;
      }
      if (!found) c.report(DiagnosticCode::UnknownColumn, range, "PIVOT input column not found: " + name.back());
      return found;
    };
    std::set<std::size_t> consumed;
    for (std::size_t i=0;i<pivot->axes.size();++i) {
      checkStop(c);
      const auto axis=sourceColumn(std::vector<std::string>{pivot->axes[i]},syntax.axes[i].range);
      if (!axis) continue;
      if (!consumed.insert(*axis).second)
        c.report(DiagnosticCode::DuplicateAlias,syntax.axes[i].range,"PIVOT axis column is repeated");
      pivot->axisOrdinals.push_back(*axis);
    }
    pivot->argumentOrdinal = sourceColumn(pivot->argument, pivot->argumentOrigin);
    if (pivot->argumentOrdinal) consumed.insert(*pivot->argumentOrdinal);
    TableDefinition output;
    output.name = pivot->relationAlias.value_or(table.name.empty() ? std::string{} : table.name.back());
    output.kind = ObjectKind::View;
    output.complete = !pivot->axes.empty() && pivot->axisOrdinals.size()==pivot->axes.size() &&
        (pivot->argument.empty() || pivot->argumentOrdinal);
    output.arityKnown = output.complete;
    if (output.complete) {
      for (std::size_t i = 0; i < input->columns.size(); ++i) {
        checkStop(c);
        if (consumed.count(i)) continue;
        pivot->groupingOrdinals.push_back(i);
        output.columns.push_back(input->columns[i]);
      }
    }
    SqlType measured;
    if (pivot->argumentOrdinal) {
      const auto &column = input->columns[*pivot->argumentOrdinal];
      measured = column.sqlType ? *column.sqlType : categoryType(column.type);
    }
    auto type = resolveAggregateType(c.dialect, pivot->aggregate, measured);
    if (!type.error.empty()) c.report(DiagnosticCode::InvalidQueryShape, pivot->argumentOrigin, type.error);
    auto result = std::make_shared<const SqlType>(std::move(type.type));
    const auto literalHeading=[&](const auto &constant)->std::string {
      if (constant.kind==ExprKind::String && !constant.unicodeString &&
          constant.value.find('\'')==std::string::npos)
        return "'"+constant.value+"'";
      if (c.dialect!=Dialect::PostgreSQL) return {};
      if (constant.kind==ExprKind::Number) return constant.value;
      if (constant.kind==ExprKind::Null) return "NULL";
      if (constant.kind==ExprKind::Unary && constant.operands.size()==1 &&
          (constant.value=="+" || constant.value=="-")) {
        const auto &number=query.expressions.at(constant.operands.front());
        if (number.kind==ExprKind::Number) return constant.value+number.value;
      }
      return {};
    };
    for (const auto &bucket : syntax.buckets) {
      checkStop(c);
      BasicPivotBucket<std::string> lowered;
      if (bucket.name) lowered.name = identifier(c, *bucket.name);
      for (auto id:bucket.values) lowered.values.push_back(ids.at(id));
      lowered.origin = bucket.origin;
      if (c.dialect!=Dialect::SQLServer && lowered.values.size()!=pivot->axes.size()) {
        c.report(DiagnosticCode::InvalidQueryShape,bucket.origin,"PIVOT bucket width must equal the axis width");
        output.complete=false;
      }
      std::string heading;
      if (lowered.name) heading = *lowered.name;
      else {
        for (auto id:lowered.values) {
          const auto value=literalHeading(query.expressions.at(id));
          if (value.empty()) { heading.clear(); break; }
          if (!heading.empty()) heading+='_';
          heading+=value;
        }
      }
      if (heading.empty()) output.complete = false;
      if (pivot->aggregateAlias && !heading.empty()) heading += "_" + *pivot->aggregateAlias;
      if (!heading.empty() && std::any_of(output.columns.begin(), output.columns.end(), [&](const auto &col) {
            return columnEquals(col.name, heading);
          })) c.report(DiagnosticCode::DuplicateAlias, bucket.origin, "PIVOT produces a duplicate output column: " + heading);
      output.columns.push_back({std::move(heading), valueCategory(*result), result,
          c.dialect==Dialect::PostgreSQL && pivot->aggregate=="COUNT"});
      pivot->buckets.push_back(std::move(lowered));
    }
    if (output.columns.size() > c.options.limits.columnsPerQuery)
      limit("PIVOT output column limit exceeded");
    chargeColumns(output.columns.size());
    table.pivotInput=input;
    table.definition = std::make_shared<const TableDefinition>(std::move(output));
    table.alias = pivot->relationAlias.value_or(std::string{});
    table.pivot = std::move(pivot);
  }
  void applyUnpivot(const UnpivotSyntax &syntax,const std::unordered_map<ExprId,ExprId> &ids,
                    const ModelSelect &query,ModelTable &table) {
    auto unpivot=std::make_shared<ModelUnpivot>();
    unpivot->origin=syntax.origin;
    const auto lowerNames=[&](const auto &names) {
      std::vector<std::string> result;
      for (const auto &name:names) result.push_back(identifier(c,name));
      return result;
    };
    unpivot->measures=lowerNames(syntax.measures);
    unpivot->descriptors=lowerNames(syntax.descriptors);
    unpivot->includeNulls=syntax.includeNulls;
    if (syntax.inputAlias) unpivot->inputAlias=identifier(c,*syntax.inputAlias);
    if (syntax.relationAlias) unpivot->relationAlias=identifier(c,*syntax.relationAlias);
    const auto input=table.definition;
    const bool known=input && input->complete && input->arityKnown;
    const bool oracle=c.dialect==Dialect::Oracle;
    const bool postgres=c.dialect==Dialect::PostgreSQL;
    std::set<std::size_t> consumed;
    std::vector<ValueType> categories(syntax.measures.size(),ValueType::Unknown);
    std::vector<bool> descriptorNotNull(syntax.descriptors.size(),true);
    std::map<std::string,std::pair<std::optional<std::size_t>,bool>> bindings;
    for (const auto &item:syntax.inputs) {
      checkStop(c);
      ModelUnpivot::Input lowered;
      lowered.origin=item.origin;
      lowered.columns=lowerNames(item.columns);
      for (auto label:item.labels) lowered.labels.push_back(ids.at(label));
      unpivot->inputs.push_back(std::move(lowered));
      if (item.columns.size()!=syntax.measures.size()) {
        c.report(DiagnosticCode::InvalidQueryShape,item.origin,"UNPIVOT input tuple width must match the measure list");
        continue;
      }
      if (!item.labels.empty() && item.labels.size()!=syntax.descriptors.size()) {
        c.report(DiagnosticCode::InvalidQueryShape,item.origin,"UNPIVOT label tuple width must match the descriptor list");
        continue;
      }
      for (std::size_t i=0;i<item.labels.size();++i) {
        const auto &label=query.expressions.at(ids.at(item.labels[i]));
        if (label.kind==ExprKind::Null || (oracle && label.kind==ExprKind::String && label.value.empty()))
          descriptorNotNull[i]=false;
      }
      if (!known) continue;
      std::vector<std::size_t> ordinals;
      for (std::size_t j=0;j<item.columns.size();++j) {
        const auto &name=unpivot->inputs.back().columns[j];
        auto [binding,first]=bindings.try_emplace(name);
        auto &[ordinal,ambiguous]=binding->second;
        if (first)
          for (std::size_t i=0;i<input->columns.size();++i) {
            if ((i&63u)==0) checkStop(c);
            if (!columnEquals(name,input->columns[i].name)) continue;
            if (ordinal) { ambiguous=true; break; }
            ordinal=i;
          }
        if (!ordinal || ambiguous) {
          c.report(ambiguous?DiagnosticCode::AmbiguousColumn:DiagnosticCode::UnknownColumn,item.columns[j].range,
                   ambiguous?"ambiguous UNPIVOT input column: "+name:"UNPIVOT input column not found: "+name);
          continue;
        }
        consumed.insert(*ordinal);
        ordinals.push_back(*ordinal);
        const auto type=input->columns[*ordinal].type;
        if (type==ValueType::Unknown || type==ValueType::Null) continue;
        auto &category=categories[j];
        if (category==ValueType::Unknown) category=type;
        else if (category!=type) {
          const auto numeric=[](ValueType kind) { return kind==ValueType::Integer || kind==ValueType::Decimal; };
          if ((oracle || postgres) && numeric(category) && numeric(type)) category=ValueType::Decimal;
          else c.report(DiagnosticCode::InvalidQueryShape,item.columns[j].range,
                        "UNPIVOT input columns for one measure have incompatible types");
        }
      }
      if (ordinals.size()==item.columns.size()) unpivot->inputOrdinals.push_back(std::move(ordinals));
    }
    TableDefinition output;
    output.name=unpivot->relationAlias.value_or(table.name.empty()?std::string{}:table.name.back());
    output.kind=ObjectKind::View;
    output.complete=known && unpivot->inputOrdinals.size()==syntax.inputs.size();
    output.arityKnown=output.complete;
    if (output.complete) {
      for (std::size_t i=0;i<input->columns.size();++i) {
        checkStop(c);
        if (consumed.count(i)) continue;
        unpivot->retainedOrdinals.push_back(i);
        output.columns.push_back(input->columns[i]);
      }
    }
    const auto add=[&](const std::string &name,SourceRange origin,SqlType type,bool nonNull) {
      if (std::any_of(output.columns.begin(),output.columns.end(),[&](const auto &field) {
            return columnEquals(field.name,name);
          }))
        c.report(DiagnosticCode::DuplicateAlias,origin,"UNPIVOT output column conflicts with another column: "+name);
      type.nullability=nonNull?Nullability::NotNull:Nullability::Nullable;
      const auto category=valueCategory(type);
      output.columns.push_back({name,category,std::make_shared<const SqlType>(std::move(type)),nonNull});
    };
    auto textLabel=catalogTypeFromText(c.dialect,postgres?"TEXT":oracle?"VARCHAR2(128)":"NVARCHAR(128)").value_or(SqlType{});
    if (oracle) textLabel.length.reset();
    for (std::size_t i=0;i<syntax.descriptors.size();++i)
      add(unpivot->descriptors[i],syntax.descriptors[i].range,oracle?SqlType{}:textLabel,descriptorNotNull[i]);
    const bool measureNotNull=syntax.measures.size()==1 && !unpivot->includeNulls.value_or(false);
    for (std::size_t i=0;i<syntax.measures.size();++i)
      add(unpivot->measures[i],syntax.measures[i].range,categoryType(categories[i]),measureNotNull);
    chargeColumns(output.columns.size());
    table.unpivotInput=input;
    table.definition=std::make_shared<const TableDefinition>(std::move(output));
    table.alias=unpivot->relationAlias.value_or(std::string{});
    table.unpivot=std::move(unpivot);
  }
  std::vector<BasicJsonTableColumn<std::string, std::vector<std::string>>>
  jsonTableColumns(const std::vector<BasicJsonTableColumn<NamePart, QualifiedName>> &syntax,
                   TableDefinition &output, std::set<std::string> &seen, bool complemented) {
    std::vector<BasicJsonTableColumn<std::string, std::vector<std::string>>> result;
    for (const auto &source : syntax) {
      checkStop(c);
      BasicJsonTableColumn<std::string, std::vector<std::string>> column;
      column.kind = source.kind;
      column.path = source.path;
      column.origin = source.origin;
      column.onEmpty = source.onEmpty;
      column.onError = source.onError;
      if (source.kind == JsonTableColumnKind::Nested) {
        column.children = jsonTableColumns(source.children, output, seen, true);
      } else {
        chargeColumns(1);
        column.name = identifier(c, *source.name);
        if (!seen.insert(asciiUpper(*column.name)).second)
          c.report(DiagnosticCode::DuplicateAlias, source.name->range,
                   "duplicate JSON_TABLE output column: " + *column.name);
        ColumnDefinition field;
        field.name = *column.name;
        if (source.kind == JsonTableColumnKind::Ordinality) {
          field.type = ValueType::Integer;
          SqlType type;
          type.family = TypeFamily::Integer;
          type.bits = 32;
          type.unsignedInteger = true;
          field.sqlType = std::make_shared<const SqlType>(std::move(type));
          field.nonNullKnown = !complemented;
        } else {
          column.type = lowerTypeSpec(c, source.type);
          const auto resolved = resolveDeclaredType(column.type, c.dialect, TypeUse::Declaration);
          if (!resolved.error.empty())
            c.report(DiagnosticCode::InvalidQueryShape, source.type.origin, resolved.error);
          field.type = typeCategory(column.type.name, c.dialect);
          field.sqlType = std::make_shared<const SqlType>(resolved.type);
        }
        output.columns.push_back(std::move(field));
      }
      result.push_back(std::move(column));
    }
    return result;
  }
  void finishValuesOutput(ModelSelect &q) {
    auto output = *q.tables.front().definition;
    auto &columns = output.columns;
    for (auto &column : columns) column.type = ValueType::Null;
    for (const auto &row : q.rows) {
      checkStop(c);
      if (row.size() != columns.size()) {
        c.report(DiagnosticCode::InvalidQueryShape, q.expressions[row.front()].origin,
                 "VALUES rows have different numbers of columns");
        continue;
      }
      for (std::size_t i = 0; i < row.size(); ++i) {
        const auto cell = q.expressions[row[i]].type;
        auto &column = columns[i].type;
        if (column == ValueType::Null) column = cell;
        else if (cell != ValueType::Null && column != cell) {
          const bool numbers = (column == ValueType::Integer && cell == ValueType::Decimal) ||
                               (column == ValueType::Decimal && cell == ValueType::Integer);
          column = numbers ? ValueType::Decimal : ValueType::Unknown;
        }
      }
    }
    for (std::size_t i = 0; i < columns.size(); ++i)
      q.expressions[q.projections[i].expression].type = columns[i].type;
    q.tables.front().definition = std::make_shared<const TableDefinition>(std::move(output));
  }
  void assignCteColumns(TableDefinition &definition, const std::vector<std::string> &names,
                        SourceRange origin) {
    if (c.dialect == Dialect::PostgreSQL) {
      if (!definition.arityKnown || names.size() <= definition.columns.size())
        applyDerivedColumnAliases(definition, names, origin);
      return;
    }
    definition.columns.resize(names.size());
    for (std::size_t i = 0; i < names.size(); ++i)
      definition.columns[i].name = names[i];
    definition.complete = true;
    definition.arityKnown = true;
  }
  void applyDerivedColumnAliases(TableDefinition &definition, const std::vector<std::string> &aliases,
                                 SourceRange origin) {
    const bool exact = c.dialect != Dialect::PostgreSQL;
    if (definition.arityKnown && (aliases.size() > definition.columns.size() ||
                                 (exact && aliases.size() != definition.columns.size()))) {
      c.report(DiagnosticCode::InvalidQueryShape, origin,
               "derived column alias count does not match the query output");
      return;
    }
    if (!definition.arityKnown) {
      // An unresolved wildcard can shift all later expressions. Do not
      // attach their inferred types to the declared names by position.
      definition.columns.assign(aliases.size(), ColumnDefinition{});
      definition.arityKnown = exact;
    }
    for (std::size_t i = 0; i < aliases.size(); ++i)
      definition.columns[i].name = aliases[i];
    definition.complete = definition.arityKnown &&
        std::all_of(definition.columns.begin(), definition.columns.end(),
                    [](const auto &column) { return !column.name.empty(); });
    if (exact) {
      std::set<std::string> names;
      for (const auto &alias : aliases)
        if (!names.insert(c.dialect == Dialect::MySQL ? asciiUpper(alias) : alias).second) {
          c.report(DiagnosticCode::DuplicateAlias, origin, "duplicate derived column alias: " + alias);
          break;
        }
    }
  }
  void applySqliteBooleanOutputNames(TableDefinition &definition) const {
    if (c.dialect != Dialect::SQLite) return;
    bool adjusted = false;
    for (std::size_t i = 0; i < definition.columns.size(); ++i) {
      auto &name = definition.columns[i].name;
      const auto word = asciiUpper(name);
      if (word != "TRUE" && word != "FALSE") continue;
      adjusted = true;
      if (definition.arityKnown) name = "column" + std::to_string(i + 1);
      else name.clear();
    }
    if (!adjusted) return;
    // The engine disambiguates duplicate output names with additional suffixes.
    // Preserve incomplete shape when such suffixes would need to be inferred.
    std::set<std::string> names;
    for (const auto &column : definition.columns)
      if (column.name.empty() || !names.insert(asciiUpper(column.name)).second) {
        definition.complete = false;
        break;
      }
  }
  bool validateRecursiveMember(const Statement &syntax, const ModelSelect &member,
                               QueryId owner) {
    const auto references = std::count_if(member.tables.begin(), member.tables.end(),
        [&](const auto &table) { return table.workingTableOf == owner; });
    if (!references) return false;
    const auto features = inspectQueryFeatures(member, c.dialect, c.options.limits.nodes,
                                               c.options.shouldStop);
    if (!features) limit("recursive member feature inspection limit exceeded");
    const bool outerJoin = std::any_of(member.relations.begin(), member.relations.end(),
        [](const auto &r) { return r.join == JoinKind::Left || r.join == JoinKind::Right ||
                                   r.join == JoinKind::Full; });
    if (references != 1 || member.distinct || member.having ||
        !member.groupBy.empty() || member.groupByAll || features->aggregate || features->window ||
        outerJoin || !member.orderBy.empty() || member.limit || member.offset ||
        !syntax.orderBy.empty() || syntax.limit.count || syntax.limit.offset)
      c.report(DiagnosticCode::UnsupportedSyntax, member.origin,
               "recursive member requires one direct work-table input and no aggregation, window, outer join or pagination");
    return true;
  }
  Compilation &c;
  std::size_t loweringDepth = 0, cteDependencyVisits = 0;
  const CatalogSnapshot *catalog;
  NameResolutionContext names;
  std::size_t inferredColumns = 0;
  std::unordered_map<QueryId, std::shared_ptr<const Shape>> queryShapes;
  [[noreturn]] void limit(const char *message) {
    c.report(DiagnosticCode::ResourceLimit, {}, message);
    throw CompilationAborted{};
  }
  void chargeColumns(std::size_t count) {
    checkStop(c);
    if (count > c.options.limits.columnsPerQuery ||
        count > c.options.limits.inferredColumns - inferredColumns)
      limit("inferred column budget exceeded");
    inferredColumns += count;
  }
  std::shared_ptr<const Shape> shapeForTables(const ModelSelect &m,
                                              const Visible &visible) {
    auto shape = std::make_shared<Shape>();
    for (auto i : visible) {
      const auto &t = m.tables.at(i);
      if (!t.definition) {
        shape->complete = false;
        shape->arityKnown = false;
        continue;
      }
      chargeColumns(t.definition->columns.size());
      shape->complete &= t.definition->complete;
      shape->arityKnown &= t.definition->arityKnown;
      for (std::size_t j = 0; j < t.definition->columns.size(); ++j) {
        const auto &col = t.definition->columns[j];
        shape->columns.push_back({col.name, col.type, {{i, j, m.id}}});
      }
    }
    return shape;
  }
  ModelTable insertedRow(const ModelStatement &m) {
    ModelTable incoming;
    incoming.name = {m.conflict.rowAlias};
    incoming.alias = m.conflict.rowAlias;
    incoming.pseudo = true;
    const auto &target = m.target->definition;
    auto written = m.columns;
    if (m.insertSource == InsertSource::Assignments) {
      for (const auto &assignment : m.assignments)
        written.push_back(m.expressions.at(assignment.targets.front()).name.back());
    } else if (written.empty() && target && target->complete && !m.rows.empty() &&
               m.rows.front().size() == target->columns.size()) {
      for (const auto &column : target->columns)
        written.push_back(column.name);
    }
    const auto &names = m.conflict.columnAliases.empty() ? written : m.conflict.columnAliases;
    TableDefinition shape;
    shape.name = m.conflict.rowAlias;
    shape.complete = shape.arityKnown = !names.empty();
    chargeColumns(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
      checkStop(c);
      ColumnDefinition field;
      if (target && i < written.size()) {
        const auto column = std::find_if(target->columns.begin(), target->columns.end(),
            [&](const auto &candidate) { return columnEquals(candidate.name, written[i]); });
        if (column != target->columns.end()) field = *column;
      }
      field.name = names[i];
      shape.columns.push_back(std::move(field));
    }
    incoming.definition = std::make_shared<const TableDefinition>(std::move(shape));
    return incoming;
  }
  std::vector<std::shared_ptr<const Shape>>
  relationShapes(const ModelSelect &m) {
    std::vector<std::shared_ptr<const Shape>> shapes;
    for (const auto &r : m.relations) {
      checkStop(c);
      if (!r.left) {
        shapes.push_back(shapeForTables(m, {r.table}));
        continue;
      }
      const auto &a = *shapes.at(r.left - 1);
      const auto &b = *shapes.at(r.right - 1);
      auto out = std::make_shared<Shape>();
      out->complete = a.complete && b.complete;
      out->arityKnown = a.arityKnown && b.arityKnown;
      auto usingNames = r.usingColumns;
      if (r.natural)
        for (const auto &col : a.columns)
          if (std::any_of(b.columns.begin(), b.columns.end(),
                          [&](const auto &other) {
                            return columnEquals(other.name, col.name);
                          }))
            usingNames.push_back(col.name);
      std::vector<std::string> merged;
      for (const auto &name : usingNames) {
        if (std::any_of(merged.begin(), merged.end(), [&](const auto &item) {
              return columnEquals(item, name);
            })) {
          c.report(DiagnosticCode::DuplicateAlias, m.origin,
                   "duplicate USING column: " + name);
          continue;
        }
        merged.push_back(name);
        std::vector<ShapeColumn> left, right;
        for (const auto &col : a.columns)
          if (columnEquals(col.name, name))
            left.push_back(col);
        for (const auto &col : b.columns)
          if (columnEquals(col.name, name))
            right.push_back(col);
        if ((a.complete && left.size() != 1) ||
            (b.complete && right.size() != 1)) {
          c.report(DiagnosticCode::UnknownColumn, m.origin,
                   "USING column must occur once on each side: " + name);
          continue;
        }
        if (left.size() == 1 && right.size() == 1) {
          auto col = left.front();
          col.bindings.insert(col.bindings.end(),
                              right.front().bindings.begin(),
                              right.front().bindings.end());
          if (col.type != right.front().type)
            col.type = ValueType::Unknown;
          out->columns.push_back(std::move(col));
        }
      }
      for (const auto *side : {&a, &b})
        for (const auto &col : side->columns)
          if (!std::any_of(merged.begin(), merged.end(), [&](const auto &item) {
                return columnEquals(item, col.name);
              }))
            out->columns.push_back(col);
      chargeColumns(out->columns.size());
      shapes.push_back(std::move(out));
    }
    return shapes;
  }
  bool mysqlColumnNames() const {
    return isMySqlDialect(c.dialect);
  }
  bool columnEquals(std::string_view a, std::string_view b) const {
    return a == b || (catalog && catalog->columnNameEquals(a, b)) ||
           (mysqlColumnNames() && asciiUpper(a) == asciiUpper(b));
  }
  void validateTargetColumns(const std::vector<std::string> &columns,
                             const std::vector<NamePart> &written,
                             const TableDefinition *def,
                             std::string_view operation) {
    if (columns.size() > c.options.limits.columnsPerQuery) {
      c.report(DiagnosticCode::ResourceLimit,
               written[c.options.limits.columnsPerQuery].range,
               std::string(operation)+" target column limit exceeded");
      return;
    }
    std::set<std::string> seen;
    std::set<std::size_t> matched;
    for (std::size_t i = 0; i < columns.size(); ++i) {
      checkStop(c);
      const auto &col = columns[i];
      bool duplicate = !seen.insert(mysqlColumnNames() ? asciiUpper(col) : col).second;
      bool missing = false;
      if (def && !duplicate) {
        const auto found = std::find_if(def->columns.begin(), def->columns.end(),
            [&](const auto &declared) { return columnEquals(declared.name, col); });
        if (found != def->columns.end())
          duplicate = !matched.insert(found - def->columns.begin()).second;
        else
          missing = def->complete;
      }
      if (duplicate) {
        c.report(DiagnosticCode::InvalidQueryShape, written[i].range,
                 "duplicate " + std::string(operation) + " column: " + col);
      } else if (missing) {
        c.report(DiagnosticCode::UnknownColumn, written[i].range,
                 "unknown " + std::string(operation) + " column: " + col);
      }
    }
  }
  void validateImplicitInsertWidth(const ModelSelect &m, std::size_t supplied,
                                   SourceRange origin) {
    if (m.target && m.target->definition && m.target->definition->complete &&
        m.target->definition->arityKnown &&
        supplied > m.target->definition->columns.size())
      c.report(DiagnosticCode::InvalidQueryShape, origin,
               "INSERT supplies more values than the target has columns");
  }
  bool relationEquals(std::string_view a, std::string_view b) const {
    return catalog ? catalog->relationNameEquals(a, b) : a == b;
  }
  bool matchesTable(const ModelSelect &m, std::size_t i,
                    const std::vector<std::string> &prefix) const {
    const auto &t = m.tables[i];
    return (t.pseudo && (c.dialect == Dialect::SQLServer || m.trigger) && prefix.size() == 1 &&
            asciiUpper(prefix[0]) == asciiUpper(t.alias)) ||
           (prefix.size() == 1 &&
            relationEquals(prefix[0], m.scope.tables[i].name)) ||
           (t.alias.empty() && prefix.size() == t.name.size() &&
            std::equal(prefix.begin(), prefix.end(), t.name.begin(),
                       [&](const auto &a, const auto &b) {
                         return relationEquals(a, b);
                       }));
  }
  TableDefinition output(const ModelSelect &m) {
    TableDefinition result;
    const bool returning=m.kind!=StatementKind::Select;
    const auto &fields=returning?m.returning:m.projections;
    for (const auto &p : fields) {
      const auto &e = m.expressions[p.expression];
      if (e.kind == ExprKind::Star) {
        std::vector<std::string> prefix = e.name;
        if (!prefix.empty())
          prefix.pop_back();
        const auto joined = queryShapes.at(m.id);
        if (prefix.empty()) {
          chargeColumns(joined->columns.size());
          for (const auto &col : joined->columns) {
            result.columns.push_back({col.name,col.type});
            if (returning && col.bindings.size()==1) {
              const auto &binding=col.bindings.front();
              const auto &definition=m.tables.at(binding.table).definition;
              if (definition && binding.column<definition->columns.size())
                result.columns.back().sqlType=definition->columns[binding.column].sqlType;
            }
          }
          result.complete &= joined->complete;
          result.arityKnown &= joined->arityKnown;
        } else
          for (std::size_t i = 0; i < m.tables.size(); ++i) {
            if (!matchesTable(m, i, prefix))
              continue;
            const auto &t = m.tables[i];
            if (!t.definition) {
              result.complete = false;
              result.arityKnown = false;
              continue;
            }
            chargeColumns(t.definition->columns.size());
            result.columns.insert(result.columns.end(),
                                  t.definition->columns.begin(),
                                  t.definition->columns.end());
            result.complete &= t.definition->complete;
            result.arityKnown &= t.definition->arityKnown;
          }
        if (result.columns.size() > c.options.limits.columnsPerQuery)
          limit("derived output column limit exceeded");
      } else {
        if (result.columns.size() >= c.options.limits.columnsPerQuery)
          limit("derived output column count limit exceeded");
        chargeColumns(1);
        std::string name = p.alias;
        if (name.empty() && e.kind == ExprKind::Name)
          name = e.name.back();
        if (name.empty())
          result.complete = false;
        result.columns.push_back({std::move(name), e.type});
      }
    }
    return result;
  }
  struct Lookup {
    std::vector<ColumnBinding> bindings;
    bool qualifier = false, complete = true;
  };
  Lookup lookup(const Frame &frame, const ModelExpression &e) const {
    Lookup result;
    const auto &m = *frame.query;
    const bool qualified = e.name.size() > 1;
    const std::vector<std::string> prefix(e.name.begin(), e.name.end() - 1);
    if (!qualified && frame.shape) {
      result.complete = frame.shape->complete;
      for (const auto &col : frame.shape->columns)
        if (m.inspection ? asciiUpper(col.name) == asciiUpper(e.name.back())
                         : columnEquals(col.name, e.name.back())) {
          if (!col.bindings.empty())
            result.bindings.push_back(col.bindings.front());
        }
      return result;
    }
    for (auto i : frame.visible) {
      const auto &t = m.tables[i];
      if (qualified && !matchesTable(m, i, prefix))
        continue;
      result.qualifier |= qualified;
      if (!t.definition || !t.definition->complete) {
        result.complete = false;
        continue;
      }
      for (std::size_t j = 0; j < t.definition->columns.size(); ++j)
        if (columnEquals(t.definition->columns[j].name, e.name.back()))
          result.bindings.push_back({i, j, m.id});
      if (qualified && c.dialect==Dialect::SQLite && m.trigger && t.pseudo && result.bindings.empty()) {
        const auto name = asciiUpper(e.name.back());
        if (name == "ROWID" || name == "_ROWID_" || name == "OID")
          result.complete = false; // SQLite implicit row identity is not published by all catalogs.
      }
    }
    return result;
  }
  void bindName(ModelSelect &m, ModelExpression &e, const Visible &visible,
                const Frames &outer, std::shared_ptr<const Shape> shape) {
    if (e.value=="MYSQL_ROUTINE_INTEGER") {
      // stable_integer's ident denotes a stored-program local, even when a
      // query column has the same spelling. It is not a general column expr.
      if (!e.localSymbol)
        c.report(DiagnosticCode::UnknownColumn,e.origin,
                 "this MySQL window argument requires a declared routine integer variable");
      return;
    }
    if (e.name.empty() || (e.localSymbol && m.context.localPolicy ==
                                                LocalNamePolicy::PreferLocals))
      return;
    const auto local = e.localSymbol;
    Frames frames = outer;
    frames.push_back({&m, visible, shape});
    const bool qualified = e.name.size() > 1, star = e.kind == ExprKind::Star;
    for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
      auto found = lookup(*it, e);
      if (qualified && !found.qualifier)
        continue;
      if (star)
        return;
      if (!found.complete) {
        if (local)
          c.report(DiagnosticCode::UnsupportedSyntax, e.origin,
                   "cannot resolve column/local-variable precedence without "
                   "complete table metadata");
        return; // Unresolved local tables may shadow outer columns.
      }
      if (local && !found.bindings.empty()) {
        if (m.context.localPolicy == LocalNamePolicy::ErrorOnConflict) {
          c.report(DiagnosticCode::AmbiguousColumn, e.origin,
                   "procedural variable conflicts with column: " +
                       e.name.back());
          return;
        }
        e.localSymbol = 0;
      }
      if (found.bindings.size() > 1) {
        c.report(DiagnosticCode::AmbiguousColumn, e.origin,
                 "ambiguous column: " + e.name.back());
        return;
      }
      if (found.bindings.size() == 1) {
        if (!qualified && it->shape)
          for (const auto &col : it->shape->columns)
            if (columnEquals(col.name, e.name.back()) &&
                col.bindings.size() > 1) {
              e.coalescedBindings = col.bindings;
              e.type = col.type;
              return;
            }
        e.binding = found.bindings[0];
        e.type = it->query->tables[e.binding->table]
                     .definition->columns[e.binding->column]
                     .type;
        return;
      }
      if (qualified) {
        c.report(DiagnosticCode::UnknownColumn, e.origin,
                 "column not found in the nearest matching table scope");
        return;
      }
    }
    if (local)
      return;
    c.report(DiagnosticCode::UnknownColumn, e.origin,
             qualified ? "unknown or out-of-scope table qualifier"
                       : "column not found: " + e.name.back());
  }
  ValueType validateRoutine(const std::vector<std::string> &name,
                            std::size_t arguments, bool procedure,
                            SourceRange origin,ModelExpression *call=nullptr) {
    bool localName = false, matched = false;
    ValueType result = ValueType::Unknown;
    auto merge = [&](const RoutineDefinition &routine) {
      const auto type=routine.resultType;
      if(call) {
        const auto kind=routine.procedure?RoutineKind::Procedure:routine.kind;
        if(!matched) {
          call->routineKind=kind;call->routineVolatility=routine.volatility;
          call->routineSchema=routine.schema;call->routineType=routine.sqlType;
        } else {
          if(!call->routineType || !routine.sqlType || *call->routineType!=*routine.sqlType)call->routineType.reset();
          if(call->routineKind!=kind)call->routineKind=RoutineKind::Unknown;
          if(call->routineVolatility==FunctionVolatility::Unknown || routine.volatility==FunctionVolatility::Unknown)
            call->routineVolatility=FunctionVolatility::Unknown;
          else if(static_cast<int>(routine.volatility)>static_cast<int>(call->routineVolatility))call->routineVolatility=routine.volatility;
          if(call->routineSchema!=routine.schema)call->routineSchema.clear();
        }
        call->routineResolved=true;
      }
      if (!matched) {
        matched = true;
        result = type;
      } else if (result != type)
        result = ValueType::Unknown;
    };
    for (const auto &routine : names.routines) {
      if (name.empty() || name.size() > 3 || routine.procedure != procedure)
        continue;
      const auto database = name.size() == 3 ? name[0] : names.database;
      const auto schema =
          name.size() > 1 ? name[name.size() - 2] : names.schema;
      if (routine.database != database || routine.schema != schema ||
          !relationEquals(routine.name, name.back()))
        continue;
      localName = true;
      if (arguments >= routine.minArguments &&
          (routine.variadic || arguments <= routine.maxArguments))
        merge(routine);
    }
    if (matched)
      return result;
    if (localName) {
      c.report(DiagnosticCode::RoutineArgumentCount, origin,
               "no declared routine overload accepts this argument count");
      return ValueType::Unknown;
    }
    if (!catalog)
      return ValueType::Unknown;
    const auto candidates = catalog->findRoutines(names, name, procedure);
    if (candidates.empty()) {
      if (catalog->routineLookupComplete(names, name, procedure))
        c.report(DiagnosticCode::UnknownRoutine, origin,
                 "routine not found in current metadata snapshot");
      return ValueType::Unknown;
    }
    // SQLite resolves a matching fixed-arity overload before a variadic one
    // (e.g. one-argument aggregate max versus multi-argument scalar max).
    // Other dialects also need argument type resolution; do not guess from
    // counts alone there and hide an ambiguous aggregate/scalar category.
    const bool fixedSqlite=names.dialect==Dialect::SQLite &&
      std::any_of(candidates.begin(),candidates.end(),[&](const auto &candidate){
        return !candidate.variadic && arguments>=candidate.minArguments && arguments<=candidate.maxArguments;
      });
    // A fixed zero-argument pg_catalog window function wins over a
    // variadic signature from that same schema (e.g. RANK's hypothetical
    // aggregate overload). There are no argument types to disambiguate.
    const bool pgFixedWindow=names.dialect==Dialect::PostgreSQL && !arguments &&
      std::any_of(candidates.begin(),candidates.end(),[](const auto &candidate) {
        return candidate.schema=="pg_catalog" && !candidate.variadic &&
               !candidate.maxArguments && candidate.kind==RoutineKind::Window;
      });
    for (const auto &candidate : candidates)
      if ((!fixedSqlite || !candidate.variadic) && arguments >= candidate.minArguments &&
          (candidate.variadic || arguments <= candidate.maxArguments) &&
          !(pgFixedWindow && candidate.schema=="pg_catalog" && candidate.variadic))
        merge(candidate);
    if (matched)
      return result;
    c.report(DiagnosticCode::RoutineArgumentCount, origin,
             "no cached routine overload accepts this argument count");
    return ValueType::Unknown;
  }
  bool bindAlias(ModelSelect &m, ModelExpression &expression,
                 const Visible &visible, std::shared_ptr<const Shape> shape,
                 bool inputFirst, bool groupInputOnly = false) {
    if (expression.kind != ExprKind::Name || expression.name.size() != 1 ||
        (expression.localSymbol &&
         m.context.localPolicy == LocalNamePolicy::PreferLocals))
      return false;
    if (inputFirst) {
      const auto input = lookup(Frame{&m, visible, shape}, expression);
      if (!input.complete)
        return false;
      if (!input.bindings.empty()) {
        if (!groupInputOnly)
          return false;
        for (auto id : m.groupBy) {
          const auto &group = m.expressions.at(id);
          if (group.binding &&
              std::any_of(input.bindings.begin(), input.bindings.end(),
                          [&](const auto &candidate) {
                            return candidate.table == group.binding->table &&
                                   candidate.column == group.binding->column;
                          }))
            return false;
        }
      }
    }
    std::size_t matches = 0;
    for (std::size_t index = 0; index < m.projections.size(); ++index) {
      const auto &p = m.projections[index];
      if (!p.alias.empty() && columnEquals(p.alias, expression.name[0])) {
        ++matches;
        expression.outputColumn = index;
        expression.type = m.expressions[p.expression].type;
      }
    }
    if (matches > 1)
      c.report(DiagnosticCode::AmbiguousColumn, expression.origin,
               "ambiguous output alias");
    return matches > 0;
  }
  bool bindSqliteBooleanName(ModelSelect &m, ModelExpression &e, const Visible &visible,
                             const Frames &outer, const std::shared_ptr<const Shape> &shape) {
    if (c.dialect != Dialect::SQLite || e.kind != ExprKind::Boolean)
      return false;
    e.unresolvedBooleanName = false;
    e.type = ValueType::Boolean;
    auto candidate = e;
    candidate.kind = ExprKind::Name;
    candidate.name = {e.value};
    candidate.value.clear();
    candidate.type = ValueType::Unknown;
    const auto inspect = [&](const Frame &frame) {
      const auto found = lookup(frame, candidate);
      if (!found.complete) {
        e.unresolvedBooleanName = true;
        e.type = ValueType::Unknown;
        return true;
      }
      if (found.bindings.empty()) return false;
      e = std::move(candidate);
      bindName(m, e, visible, outer, shape);
      return true;
    };
    if (inspect(Frame{&m, visible, shape})) return true;
    for (auto it = outer.rbegin(); it != outer.rend(); ++it)
      if (inspect(*it)) return true;
    return false;
  }
  void bind(ModelSelect &m, ExprId root, const Visible &visible,
            bool outputAlias, const Ctes &ctes, const Frames &outer,
            std::unordered_map<ExprId, QueryId> &pending,
            std::shared_ptr<const Shape> shape, std::size_t expectedColumns = 1,
            bool inputFirst = false, bool nestedAliases = false,
            bool groupInputOnly = false) {
    if (!root)
      return;
    checkStop(c);
    if (outputAlias && bindAlias(m, m.expressions[root], visible, shape,
                                 inputFirst, groupInputOnly))
      return;
    std::vector<ExprId> work{root};
    std::size_t visits = 0;
    while (!work.empty()) {
      if ((visits++ & 255u) == 0)
        checkStop(c);
      auto id = work.back();
      work.pop_back();
      auto &e = m.expressions[id];
      if (outputAlias && nestedAliases &&
          bindAlias(m, e, visible, shape, inputFirst, groupInputOnly))
        continue;
      for (auto child : e.operands)
        work.push_back(child);
      const auto nested = pending.find(id);
      if (nested != pending.end()) {
        const auto astId = nested->second;
        pending.erase(nested);
        auto frames = outer;
        frames.push_back({&m, visible, shape});
        e.query = run(c.ast.query(astId), ctes, std::move(frames));
        const auto result = output(c.model.queries[e.query - 1]);
        std::size_t arity=id==root?expectedColumns:1;
        if (e.kind==ExprKind::ArrayConstructor) arity=1;
        else if ((e.kind==ExprKind::In || e.kind==ExprKind::Quantified) && !e.operands.empty() &&
                 m.expressions[e.operands.front()].kind==ExprKind::Row)
          arity=m.expressions[e.operands.front()].operands.size();
        if (e.kind!=ExprKind::Exists && result.arityKnown && result.columns.size()!=arity)
          c.report(DiagnosticCode::InvalidQueryShape,e.origin,e.kind==ExprKind::ArrayConstructor?
                   "ARRAY subquery must return one column":"scalar/IN subquery must return one column");
        if (e.kind == ExprKind::Subquery && result.columns.size() == 1)
          e.type = result.columns[0].type;
      }
      if (e.kind == ExprKind::Call && e.value != "KEYWORD")
        e.type = validateRoutine(e.name, e.operands.size(), false, e.origin,&e);
      if (!bindSqliteBooleanName(m, e, visible, outer, shape) &&
          (e.kind == ExprKind::Name || e.kind == ExprKind::Star))
        bindName(m, e, visible, outer, shape);
    }
  }
};
} // namespace
void lowerAndBind(Compilation &c, const CatalogSnapshot *catalog,
                  const NameResolutionContext &names) {
  ScriptContext script(c.dialect, catalog, names);
  Lowering lower(c, &script, names);
  for (const auto &statement : c.statements) {
    const auto errors = c.diagnostics.size();
    const auto root = lower.run(statement);
    c.model.roots.push_back(root);
    if (c.options.describeSelectArity && c.statements.size() == 1 &&
        c.diagnostics.size() == errors)
      c.selectArity = lower.selectArity(root);
    if (c.diagnostics.size() == errors)
      if (auto error = script.observe(c.model, root)) {
        c.report(error->code, error->range, error->message);
        if (error->code == DiagnosticCode::ResourceLimit)
          throw CompilationAborted{};
      }
  }
}
} // namespace navidog::sql::compiler
