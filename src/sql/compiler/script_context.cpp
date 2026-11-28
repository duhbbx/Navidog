#include "navidog/sql/compiler/script_context.hpp"
#include <algorithm>
#include <iterator>
#include <map>
#include <tuple>

namespace navidog::sql::compiler {
namespace {
bool mysql(Dialect d) {
  return isMySqlDialect(d);
}
std::string fold(std::string s) {
  for (auto &c : s)
    if (c >= 'A' && c <= 'Z')
      c += 32;
  return s;
}
struct Key {
  std::string database, schema, name;
  bool operator<(const Key &b) const {
    return std::tie(database, schema, name) <
           std::tie(b.database, b.schema, b.name);
  }
};
enum class State { Present, Absent, Unknown };
struct DeclaredRelation {
  State state = State::Unknown;
  TableDefinition table;
};
} // namespace
struct ScriptContext::Impl {
  Dialect dialect;
  const CatalogSnapshot *base;
  NameResolutionContext names;
  bool inherited = true, insensitive = false;
  std::size_t bytes = 0;
  using Relations = std::map<Key, DeclaredRelation>;
  using ObjectKey = std::tuple<CatalogObjectKind, Key, std::string>;
  using Objects = std::map<ObjectKey, std::optional<CatalogObjectDefinition>>;
  template<class Map>
  using UndoLog = std::vector<std::pair<typename Map::key_type,
                                       std::optional<typename Map::mapped_type>>>;
  Relations relations;
  std::vector<RoutineDefinition> routines;
  Objects objects;
  UndoLog<Relations> relationUndo;
  UndoLog<Objects> objectUndo;
  struct Checkpoint {
    std::size_t relationEdits, objectEdits, routines, bytes;
  };
  struct Savepoint {
    std::string name;
    Checkpoint checkpoint;
  };
  std::optional<Checkpoint> transactionStart;
  std::vector<Savepoint> savepoints;
  bool startsWithSavepoint = false;
  struct PreparedHandle {
    bool allocated = true;
    std::optional<std::size_t> parameters;
    bool preservesHandles = false;
  };
  std::map<std::string, PreparedHandle> prepared;

  template<class Map>
  static void remember(Map &map, UndoLog<Map> &log, const typename Map::key_type &key) {
    const auto found = map.find(key);
    std::optional<typename Map::mapped_type> previous;
    if (found != map.end()) previous.emplace(std::move(found->second));
    log.emplace_back(key, std::move(previous));
  }
  template<class Map>
  static void rewind(Map &map, UndoLog<Map> &log, std::size_t position) {
    while (log.size() > position) {
      auto &entry = log.back();
      if (entry.second) map.insert_or_assign(std::move(entry.first), std::move(*entry.second));
      else map.erase(entry.first);
      log.pop_back();
    }
  }
  Checkpoint checkpoint() const {
    return {relationUndo.size(), objectUndo.size(), routines.size(), bytes};
  }
  void begin(bool fromSavepoint = false) {
    if (!transactionStart) {
      transactionStart = checkpoint();
      startsWithSavepoint = fromSavepoint;
    }
  }
  void rewind(Checkpoint state) {
    rewind(relations, relationUndo, state.relationEdits);
    rewind(objects, objectUndo, state.objectEdits);
    routines.resize(state.routines);
    bytes = state.bytes;
  }
  void finish() {
    transactionStart.reset();
    savepoints.clear();
    relationUndo.clear();
    objectUndo.clear();
    startsWithSavepoint = false;
  }
  void clearDeclarations() {
    finish();
    inherited = false;
    relations.clear();
    routines.clear();
    objects.clear();
    bytes = 0;
  }
  std::string canonical(std::string s) const {
    return insensitive ? fold(std::move(s)) : s;
  }
  Key key(const NameResolutionContext &ctx,
          const std::vector<std::string> &name) const {
    Key out{ctx.database, ctx.schema,
            name.empty() ? std::string{} : name.back()};
    if (name.size() == 3) {
      out.database = name[0];
      out.schema = name[1];
    } else if (name.size() == 2) {
      if (mysql(dialect))
        out.database = name[0];
      else
        out.schema = name[0];
    }
    if (mysql(dialect))
      out.schema = out.database;
    out.database = canonical(out.database);
    out.schema = canonical(out.schema);
    out.name = canonical(out.name);
    return out;
  }
  bool baseCurrent() const { return inherited && base && base->current(); }
  std::optional<Diagnostic> put(Key key, DeclaredRelation relation,
                                SourceRange origin) {
    std::size_t charge =
        sizeof(DeclaredRelation) + sizeof(Key) + key.database.size() +
        key.schema.size() + key.name.size() + relation.table.name.size() +
        relation.table.database.size() + relation.table.schema.size();
    for (const auto &col : relation.table.columns)
      charge += sizeof(col) + col.name.size();
    for (const auto &key : relation.table.primaryKey)
      charge += sizeof(key) + key.size();
    if (relations.size() >= 1024 || relation.table.columns.size() > 8192 ||
        charge > 8 * 1024 * 1024 - bytes)
      return Diagnostic{
          DiagnosticCode::ResourceLimit, origin,
          "script declaration context exceeds object/column/byte budget"};
    bytes += charge;
    if (transactionStart) remember(relations, relationUndo, key);
    relations[std::move(key)] = std::move(relation);
    return {};
  }
  void putObject(ObjectKey key, std::optional<CatalogObjectDefinition> object) {
    if (transactionStart) remember(objects, objectUndo, key);
    objects.insert_or_assign(std::move(key), std::move(object));
  }
  TableDefinition output(const ModelProgram &program, QueryId id) const {
    TableDefinition out;
    if (!id || id > program.queries.size()) {
      out.complete = out.arityKnown = false;
      return out;
    }
    const auto &q = program.queries[id - 1];
    if (q.projections.size() > 8192) {
      out.complete = out.arityKnown = false;
      return out;
    }
    for (const auto &p : q.projections) {
      if (out.columns.size() >= 8192) {
        out.complete = out.arityKnown = false;
        return out;
      }
      const auto &e = q.expressions.at(p.expression);
      if (e.kind == ExprKind::Star) {
        const ModelTable *table = nullptr;
        if (q.tables.size() == 1)
          table = &q.tables[0];
        else if (e.name.size() > 1)
          for (const auto &t : q.tables)
            if ((!t.alias.empty() && t.alias == e.name[e.name.size() - 2]) ||
                (t.alias.empty() && !t.name.empty() &&
                 t.name.back() == e.name[e.name.size() - 2])) {
              table = &t;
              break;
            }
        if (table && table->definition) {
          if (table->definition->columns.size() > 8192 - out.columns.size()) {
            out.complete = out.arityKnown = false;
            return out;
          }
          out.columns.insert(out.columns.end(),
                             table->definition->columns.begin(),
                             table->definition->columns.end());
          out.complete &= table->definition->complete;
          out.arityKnown &= table->definition->arityKnown;
        } else
          out.complete = out.arityKnown = false;
      } else {
        auto name = p.alias;
        if (name.empty() && e.kind == ExprKind::Name && !e.name.empty())
          name = e.name.back();
        if (name.empty())
          out.complete = false;
        out.columns.push_back({std::move(name), e.type,std::make_shared<const SqlType>(program.types.get(e.sqlType))});
      }
    }
    return out;
  }
};
ScriptContext::ScriptContext(Dialect dialect, const CatalogSnapshot *base,
                             NameResolutionContext names)
    : d(std::make_unique<Impl>()) {
  d->dialect = dialect;
  d->base = base;
  d->names = std::move(names);
  d->names.dialect = dialect;
  d->insensitive =
      base ? base->relationNameEquals("A", "a")
           : (dialect == Dialect::SQLServer || dialect == Dialect::SQLite);
}
ScriptContext::~ScriptContext() = default;
void ScriptContext::invalidate() {
  d->clearDeclarations();
  d->prepared.clear();
}
bool ScriptContext::current() const {
  return d->baseCurrent() || !d->relations.empty() || !d->routines.empty() || !d->objects.empty();
}
bool ScriptContext::relationNameEquals(std::string_view a,
                                       std::string_view b) const {
  return d->insensitive ? fold(std::string(a)) == fold(std::string(b)) : a == b;
}
bool ScriptContext::columnNameEquals(std::string_view a,
                                     std::string_view b) const {
  if (d->base)
    return d->base->columnNameEquals(a, b);
  return mysql(d->dialect) || d->dialect == Dialect::SQLServer ||
                 d->dialect == Dialect::SQLite
             ? fold(std::string(a)) == fold(std::string(b))
             : a == b;
}
std::optional<TableDefinition>
ScriptContext::findTable(const NameResolutionContext &ctx,
                         const std::vector<std::string> &name) const {
  if (name.empty() || name.size() > 3)
    return {};
  if (d->dialect == Dialect::Oracle &&
      name == std::vector<std::string>{"SYS", "DUAL"}) {
    TableDefinition table;
    table.database = ctx.database;
    table.schema = "SYS";
    table.name = "DUAL";
    table.columns = {{"DUMMY", ValueType::String}};
    return table;
  }
  if (auto it = d->relations.find(d->key(ctx, name)); it != d->relations.end())
    return it->second.state == State::Present
               ? std::optional<TableDefinition>{it->second.table}
               : std::optional<TableDefinition>{};
  return d->baseCurrent() ? d->base->findTable(ctx, name)
                          : std::optional<TableDefinition>{};
}
bool ScriptContext::tableLookupComplete(
    const NameResolutionContext &ctx,
    const std::vector<std::string> &name) const {
  if (name.empty() || name.size() > 3)
    return false;
  if (auto it = d->relations.find(d->key(ctx, name)); it != d->relations.end())
    return it->second.state != State::Unknown;
  return d->baseCurrent() && d->base->tableLookupComplete(ctx, name);
}
std::vector<CatalogObjectDefinition>
ScriptContext::listObjects(const NameResolutionContext &ctx,CatalogObjectKind kind,std::size_t limit) const {
  auto out=d->baseCurrent()?d->base->listObjects(ctx,kind,limit):std::vector<CatalogObjectDefinition>{};
  out.erase(std::remove_if(out.begin(),out.end(),[&](const auto &o){return d->objects.count({kind,d->key({o.database,o.schema,d->dialect},{o.name}),
    kind==CatalogObjectKind::Index && (mysql(d->dialect) || d->dialect==Dialect::SQLServer)?d->canonical(o.parent):std::string{}});}),out.end());
  for(const auto &entry:d->objects)
    if(out.size()<limit && std::get<0>(entry.first)==kind && entry.second && std::get<1>(entry.first).database==d->canonical(ctx.database))out.push_back(*entry.second);
  if(out.size()>limit)out.resize(limit);
  return out;
}
std::vector<TableDefinition>
ScriptContext::listRelations(const NameResolutionContext &ctx,
                             std::size_t limit) const {
  return matchingRelations(ctx,limit,{});
}
std::vector<TableDefinition>
ScriptContext::matchingRelations(const NameResolutionContext &ctx,std::size_t limit,
                                 const CatalogNameFilter &accept) const {
  const auto visible=[&](const std::string &database,const std::string &schema,const std::string &name) {
    return (!accept || accept(database,schema,name)) &&
        !d->relations.count(d->key({database,schema,d->dialect},{name}));
  };
  auto out = d->baseCurrent() ? d->base->matchingRelations(ctx,limit,visible)
                              : std::vector<TableDefinition>{};
  for (const auto &entry : d->relations)
    if (out.size() < limit && entry.second.state == State::Present &&
        entry.first.database == d->canonical(ctx.database) &&
        (!accept || accept(entry.second.table.database,entry.second.table.schema,entry.second.table.name)))
      out.push_back(entry.second.table);
  if (out.size() > limit)
    out.resize(limit);
  return out;
}
std::vector<RoutineDefinition>
ScriptContext::findRoutines(const NameResolutionContext &ctx,
                            const std::vector<std::string> &name,
                            bool procedure) const {
  auto out = d->baseCurrent() ? d->base->findRoutines(ctx, name, procedure)
                              : std::vector<RoutineDefinition>{};
  const auto key = d->key(ctx, name);
  for (const auto &routine : d->routines)
    if (routine.procedure == procedure &&
        d->key({routine.database, routine.schema, d->dialect}, {routine.name})
                .database == key.database &&
        d->key({routine.database, routine.schema, d->dialect}, {routine.name})
                .schema == key.schema &&
        (mysql(d->dialect) ? fold(routine.name) == fold(key.name)
                           : relationNameEquals(routine.name, key.name)))
      out.push_back(routine);
  return out;
}
bool ScriptContext::routineLookupComplete(const NameResolutionContext &ctx,
                                          const std::vector<std::string> &name,
                                          bool procedure) const {
  if (!findRoutines(ctx, name, procedure).empty())
    return true;
  return d->baseCurrent() &&
         d->base->routineLookupComplete(ctx, name, procedure);
}
std::vector<RoutineDefinition>
ScriptContext::listRoutines(const NameResolutionContext &ctx, bool procedure,
                            std::size_t limit) const {
  return matchingRoutines(ctx,procedure,limit,{});
}
std::vector<RoutineDefinition>
ScriptContext::matchingRoutines(const NameResolutionContext &ctx,bool procedure,std::size_t limit,
                                const CatalogNameFilter &accept) const {
  auto out = d->baseCurrent() ? d->base->matchingRoutines(ctx,procedure,limit,accept)
                              : std::vector<RoutineDefinition>{};
  for (const auto &routine : d->routines)
    if (out.size() < limit && routine.procedure == procedure &&
        relationNameEquals(routine.database, ctx.database) &&
        (!accept || accept(routine.database,routine.schema,routine.name)))
      out.push_back(routine);
  if (out.size() > limit)
    out.resize(limit);
  return out;
}
std::optional<Diagnostic>
ScriptContext::declareRoutine(const std::vector<std::string> &name,
                              RoutineDefinition definition,
                              SourceRange origin, bool ifNotExists) {
  if(definition.kind==RoutineKind::Unknown)
    definition.kind=definition.procedure?RoutineKind::Procedure:RoutineKind::Scalar;
  if (name.empty() || name.size() > 3) {
    invalidate();
    return {};
  }
  if (ifNotExists) {
    // A guarded declaration may leave an existing signature unchanged.
    if (!findRoutines(d->names, name, definition.procedure).empty() ||
        !routineLookupComplete(d->names, name, definition.procedure))
      return {};
  }
  std::size_t charge = sizeof(definition);
  for (const auto &part : name)
    charge += part.size();
  if (d->routines.size() >= 1024 || charge > 8 * 1024 * 1024 - d->bytes)
    return Diagnostic{DiagnosticCode::ResourceLimit, origin,
                      "script routine declaration budget exceeded"};
  auto key = d->key(d->names, name);
  definition.database = key.database;
  definition.schema = key.schema;
  definition.name = name.back();
  d->bytes += charge;
  d->routines.push_back(std::move(definition));
  return {};
}
std::optional<Diagnostic> ScriptContext::observe(const ModelProgram &program,
                                                 QueryId id) {
  if (!id || id > program.queries.size())
    return Diagnostic{DiagnosticCode::InvalidQueryShape,
                      {},
                      "invalid script statement reference"};
  const auto &q = program.queries[id - 1];
  if (q.role) {
    if (q.role->scope == RoleScope::Session)
      d->clearDeclarations();
    return {};
  }
  if (q.prepared) {
    const auto &command = *q.prepared;
    const auto name = fold(command.name);
    const auto known = d->prepared.find(name);
    if (name.empty() || name.size() > 4096 ||
        (known == d->prepared.end() && d->prepared.size() >= 1024))
      return Diagnostic{DiagnosticCode::ResourceLimit, q.origin, "prepared statement context limit exceeded"};
    if (command.action == PreparedAction::Prepare) {
      d->prepared[name] = {true, command.parameterCount, command.preservesHandles};
    } else {
      if (known != d->prepared.end() && !known->second.allocated)
        return Diagnostic{DiagnosticCode::InvalidQueryShape, q.origin, "prepared statement has been deallocated: " + command.name};
      if (command.action == PreparedAction::Deallocate) {
        d->prepared[name] = {false, {}};
      } else {
        if (known != d->prepared.end() && known->second.parameters &&
            *known->second.parameters != command.arguments.size())
          return Diagnostic{DiagnosticCode::InvalidQueryShape, q.origin,
                            "EXECUTE USING argument count does not match the prepared statement"};
        // A runtime statement may execute DDL or invoke a procedure that
        // alters the session's prepared statements.
        if (known != d->prepared.end() && known->second.preservesHandles)
          d->clearDeclarations();
        else
          invalidate();
      }
    }
    return {};
  }
  if (q.transaction) {
    const auto &control = *q.transaction;
    const bool tracked = d->dialect == Dialect::PostgreSQL || d->dialect == Dialect::SQLite;
    if (!tracked) {
      if (control.release == TransactionEndOption::Yes)
        invalidate();
      else if (control.action == TransactionAction::Rollback || control.action == TransactionAction::RollbackTo ||
          control.action == TransactionAction::NamedRollback)
        d->clearDeclarations();
      return {};
    }
    switch (control.action) {
    case TransactionAction::Begin:
      d->begin();
      break;
    case TransactionAction::Commit:
      d->finish();
      if (control.chain == TransactionEndOption::Yes) d->begin();
      break;
    case TransactionAction::Rollback:
      if (d->transactionStart) {
        d->rewind(*d->transactionStart);
        d->finish();
      } else {
        invalidate();
      }
      if (control.chain == TransactionEndOption::Yes) d->begin();
      break;
    case TransactionAction::Savepoint:
    case TransactionAction::RollbackTo:
    case TransactionAction::Release: {
      if (!control.name || control.name->size() > 4096)
        return Diagnostic{DiagnosticCode::ResourceLimit, q.origin, "invalid or over-budget savepoint name"};
      const auto name=d->dialect==Dialect::SQLite?fold(*control.name):*control.name;
      if (control.action == TransactionAction::Savepoint) {
        if (d->savepoints.size() >= 128)
          return Diagnostic{DiagnosticCode::ResourceLimit, q.origin, "script savepoint limit exceeded"};
        if (d->dialect == Dialect::SQLite) d->begin(true);
        if (d->transactionStart) d->savepoints.push_back({name, d->checkpoint()});
        break;
      }
      const auto found = std::find_if(d->savepoints.rbegin(), d->savepoints.rend(),
                                     [&](const auto &point) { return point.name == name; });
      if (found == d->savepoints.rend()) {
        invalidate(); // The transaction may have started outside this script.
        break;
      }
      if (control.action == TransactionAction::RollbackTo) {
        d->rewind(found->checkpoint);
        d->savepoints.erase(found.base(), d->savepoints.end());
      } else {
        d->savepoints.erase(std::prev(found.base()), d->savepoints.end());
        if (d->startsWithSavepoint && d->savepoints.empty()) d->finish();
      }
      break;
    }
    case TransactionAction::NamedRollback:
      invalidate();
      break;
    }
    return {};
  }
  if (q.kind == StatementKind::Call) {
    invalidate();
    return {};
  }
  if (!q.ddl)
    return {};
  const auto &ddl = *q.ddl;
  if (ddl.definitions.size() > 8192 || ddl.columns.size() > 8192 ||
      ddl.actions.size() > 8192)
    return Diagnostic{DiagnosticCode::ResourceLimit, q.origin,
                      "script declaration column/action budget exceeded"};
  if (ddl.object == ObjectKind::Database || ddl.object == ObjectKind::Schema) {
    invalidate();
    return {};
  }
  if(ddl.object==ObjectKind::Index || ddl.object==ObjectKind::Sequence) {
    const auto kind=ddl.object==ObjectKind::Index?CatalogObjectKind::Index:CatalogObjectKind::Sequence;
    auto put=[&](const std::vector<std::string> &name,bool present)->std::optional<Diagnostic>{
      auto objectContext=q.context;
      if(ddl.object==ObjectKind::Index && name.size()==1 && !ddl.indexedTable.empty()) {
        if(auto table=findTable(q.context,ddl.indexedTable)){objectContext.database=table->database;objectContext.schema=table->schema;}
        else if(ddl.indexedTable.size()>=2)objectContext.schema=ddl.indexedTable[ddl.indexedTable.size()-2];
      }
      auto key=d->key(objectContext,name);
      const auto parent=ddl.object==ObjectKind::Index && !ddl.indexedTable.empty()?ddl.indexedTable.back():std::string{};
      const auto owner=ddl.object==ObjectKind::Index && (mysql(d->dialect) || d->dialect==Dialect::SQLServer)?d->canonical(parent):std::string{};
      std::size_t charge=sizeof(CatalogObjectDefinition)+sizeof(Key)+key.database.size()*2+key.schema.size()*2+key.name.size()*2+parent.size()*2;
      if(d->objects.size()>=1024 || charge>8*1024*1024-d->bytes)
        return Diagnostic{DiagnosticCode::ResourceLimit,q.origin,"script object context budget exceeded"};
      d->bytes+=charge;
      if(present)d->putObject({kind,key,owner},CatalogObjectDefinition{kind,key.database,key.schema,name.empty()?std::string{}:name.back(),parent,"script declaration"});
      else d->putObject({kind,key,owner},std::nullopt);
      return {};
    };
    if(q.kind==StatementKind::Create)return put(ddl.name,true);
    if(q.kind==StatementKind::Drop) {
      if(ddl.names.empty())return put(ddl.name,false);
      for(const auto &name:ddl.names)if(auto error=put(name,false))return error;
    }
    return {};
  }
  if (ddl.object != ObjectKind::Table && ddl.object != ObjectKind::View &&
      ddl.object != ObjectKind::MaterializedView)
    return {};
  auto key = d->key(q.context, ddl.name);
  if (q.kind == StatementKind::Drop) {
    const auto names = ddl.names.empty()
                           ? std::vector<std::vector<std::string>>{ddl.name}
                           : ddl.names;
    for (const auto &name : names)
      if (auto error =
              d->put(d->key(q.context, name), {State::Absent, {}}, q.origin))
        return error;
    // CASCADE can affect dependent objects outside the declared table itself.
    if (ddl.dropBehavior == DropBehavior::Cascade ||
        ddl.dropBehavior == DropBehavior::CascadeConstraints)
      invalidate();
    return {};
  }
  if (q.kind == StatementKind::Create) {
    auto prior = d->relations.find(key);
    if (prior != d->relations.end() && prior->second.state == State::Present &&
        !ddl.ifNotExists && !ddl.orReplace && !ddl.temporary)
      return Diagnostic{DiagnosticCode::InvalidQueryShape, q.origin,
                        "relation is already declared in this script context"};
    if (ddl.ifNotExists) {
      if (findTable(q.context, ddl.name))
        return {};
      if (!tableLookupComplete(q.context, ddl.name))
        return d->put(std::move(key), {State::Unknown, {}}, q.origin);
    }
    TableDefinition table;
    if (ddl.virtualTable) {
      table.complete = table.arityKnown = false;
    } else if (!ddl.likeTable.empty()) {
      auto existing = findTable(q.context, ddl.likeTable);
      if (existing)
        table = *existing;
      else
        table.complete = table.arityKnown = false;
    } else if (q.source)
      table = d->output(program, q.source);
    if (!ddl.definitions.empty()) {
      table.columns.clear();
      table.complete = table.arityKnown = true;
      for (const auto &col : ddl.definitions)
        table.columns.push_back(declaredColumn(col,d->dialect));
    } else if (!ddl.columns.empty()) {
      std::vector<ColumnDefinition> columns;
      for (std::size_t i = 0; i < ddl.columns.size(); ++i)
        columns.push_back({ddl.columns[i], i < table.columns.size()?table.columns[i].type:ValueType::Unknown,
                           i<table.columns.size()?table.columns[i].sqlType:std::shared_ptr<const SqlType>{}});
      table.columns = std::move(columns);
      table.complete = table.arityKnown = true;
    }
    if(q.source && ddl.object==ObjectKind::Table && ddl.definitions.empty())for(auto &column:table.columns) {
      column.nonNullKnown=false;
      if(column.sqlType){auto type=*column.sqlType;type.nullability=Nullability::Unknown;column.sqlType=std::make_shared<const SqlType>(std::move(type));}
    }
    table.database = key.database;
    table.schema = key.schema;
    table.name = ddl.name.back();
    table.kind = ddl.object;
    table.sessionTemporary = ddl.temporary;
    table.noDescendantsKnown = d->dialect == Dialect::PostgreSQL && ddl.object == ObjectKind::Table;
    if (ddl.likeTable.empty() && !ddl.virtualTable) {
      table.primaryKeyKnown = true;
      table.primaryKey.clear();
      for (const auto &col : ddl.definitions)
        for (const auto &constraint : col.constraints)
          if (constraint.kind == ConstraintKind::PrimaryKey &&
              constraint.deferrable != true)
            table.primaryKey.push_back(col.name);
      for (const auto &constraint : ddl.constraints)
        if (constraint.kind == ConstraintKind::PrimaryKey &&
            constraint.deferrable != true)
          table.primaryKey = constraint.columns;
      if(d->dialect==Dialect::SQLite && !table.primaryKey.empty()) {
        bool nonNull=true;
        for(const auto &name:table.primaryKey) {
          auto column=std::find_if(table.columns.begin(),table.columns.end(),[&](const auto &c){return columnNameEquals(c.name,name);});
          nonNull &= column!=table.columns.end() && column->nonNullKnown;
        }
        bool rowidAlias=false;
        if(table.primaryKey.size()==1)for(const auto &column:ddl.definitions)
          if(columnNameEquals(column.name,table.primaryKey[0]) && column.type.name.size()==1 &&
             fold(column.type.name[0])=="integer" && column.type.arguments.empty() &&
             (column.type.nameQuoted.empty() || !column.type.nameQuoted[0]))rowidAlias=true;
        // Ordinary SQLite TEXT / INT / composite primary keys can contain NULL.
        // WITHOUT ROWID / STRICT syntax is not covered by this frontend yet.
        if(!nonNull && !rowidAlias)table.primaryKey.clear();
      }
    }
    return d->put(std::move(key), {State::Present, std::move(table)}, q.origin);
  }
  if (q.kind == StatementKind::Alter) {
    auto found = findTable(q.context, ddl.name);
    if (!found) {
      if (tableLookupComplete(q.context, ddl.name)) {
        if (ddl.ifExists)
          return {};
        return Diagnostic{DiagnosticCode::UnknownTable, q.origin,
                          "ALTER target does not exist in this script context"};
      }
      return d->put(std::move(key), {State::Unknown, {}}, q.origin);
    }
    auto table = *found;
    // Constraint changes/dependencies need a dedicated key model. Never
    // keep a primary-key exemption after an unmodeled ALTER.
    table.primaryKeyKnown = false;
    table.primaryKey.clear();
    for(auto &column:table.columns)if(!column.nonNullKnown && column.sqlType && column.sqlType->nullability==Nullability::NotNull) {
      auto type=*column.sqlType;type.nullability=Nullability::Unknown;column.sqlType=std::make_shared<const SqlType>(std::move(type));
    }
    auto newKey = key;
    for (const auto &action : ddl.actions) {
      auto column = std::find_if(
          table.columns.begin(), table.columns.end(), [&](const auto &col) {
            return action.name && columnNameEquals(col.name, *action.name);
          });
      if (action.kind == AlterKind::AddColumn && action.column) {
        const auto exists = std::any_of(
            table.columns.begin(), table.columns.end(), [&](const auto &col) {
              return columnNameEquals(col.name, action.column->name);
            });
        if (exists && !action.ifNotExists)
          return Diagnostic{
              DiagnosticCode::InvalidQueryShape, action.origin,
              "column is already declared in this script context"};
        if (!exists)
          table.columns.push_back(declaredColumn(*action.column,d->dialect));
      } else if (action.kind == AlterKind::DropColumn) {
        if (column == table.columns.end() && table.complete && !action.ifExists)
          return Diagnostic{DiagnosticCode::UnknownColumn, action.origin,
                            "ALTER column not found in this script context"};
        if (column != table.columns.end())
          table.columns.erase(column);
      } else if (action.kind == AlterKind::RenameColumn && action.newName) {
        if (column == table.columns.end() && table.complete)
          return Diagnostic{DiagnosticCode::UnknownColumn, action.origin,
                            "ALTER column not found in this script context"};
        if (std::any_of(table.columns.begin(), table.columns.end(),
                        [&](const auto &other) {
                          return (column == table.columns.end() ||
                                  &other != &*column) &&
                                 columnNameEquals(other.name, *action.newName);
                        }))
          return Diagnostic{
              DiagnosticCode::InvalidQueryShape, action.origin,
              "renamed column already exists in this script context"};
        if (column != table.columns.end())
          column->name = *action.newName;
      } else if (action.kind == AlterKind::AlterType && action.type) {
        if (column != table.columns.end()) {
          column->type = action.type->arrayDimensions ? ValueType::Unknown : typeCategory(action.type->name,d->dialect);
          auto type=resolveDeclaredType(*action.type,d->dialect).type;
          if(column->sqlType)type.nullability=column->sqlType->nullability;
          column->sqlType=std::make_shared<const SqlType>(std::move(type));
        }
      } else if ((action.kind == AlterKind::ModifyColumn ||
                  action.kind == AlterKind::ChangeColumn) &&
                 action.column) {
        if (column != table.columns.end())
          *column = declaredColumn(*action.column,d->dialect);
        else
          table.complete = false;
      } else if(action.kind==AlterKind::SetNotNull || action.kind==AlterKind::DropNotNull) {
        if(column!=table.columns.end()) {
          auto type=column->sqlType?*column->sqlType:categoryType(column->type);
          type.nullability=action.kind==AlterKind::SetNotNull?Nullability::NotNull:Nullability::Nullable;
          column->nonNullKnown=action.kind==AlterKind::SetNotNull;
          column->sqlType=std::make_shared<const SqlType>(std::move(type));
        }
      } else if (action.kind == AlterKind::RenameTable && action.newName) {
        table.name = *action.newName;
        newKey.name = d->canonical(table.name);
      }
    }
    if (newKey.name != key.name)
      if (auto error = d->put(key, {State::Absent, {}}, q.origin))
        return error;
    return d->put(std::move(newKey), {State::Present, std::move(table)},
                  q.origin);
  }
  if (q.kind == StatementKind::Rename) {
    invalidate();
    return {};
  }
  return {};
}
} // namespace navidog::sql::compiler
