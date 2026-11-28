#include "conversion_internal.hpp"
#include "navidog/sql/compiler/script_context.hpp"
#include "parser.hpp"
#include "sqlserver_syntax.hpp"
#include "navidog/sql/compiler/operator_catalog.hpp"
#include <algorithm>
#include <set>
#include <sstream>

namespace navidog::sql::compiler {
namespace {
// Conversion can describe objects which have not been deployed. A supplied
// source catalog enriches known shapes/types, but absence is not an error.
// ScriptContext still checks declarations made explicitly in this script.
class ConversionCatalog final : public CatalogSnapshot {
  const CatalogSnapshot *base;
public:
  explicit ConversionCatalog(const CatalogSnapshot *snapshot) : base(snapshot) {}
  bool current() const override { return base && base->current(); }
  bool relationNameEquals(std::string_view a, std::string_view b) const override {
    return base ? base->relationNameEquals(a,b) : a==b;
  }
  bool columnNameEquals(std::string_view a, std::string_view b) const override {
    return base ? base->columnNameEquals(a,b) : a==b;
  }
  std::optional<TableDefinition> findTable(const NameResolutionContext &names,
      const std::vector<std::string> &name) const override {
    return current() ? base->findTable(names,name) : std::optional<TableDefinition>{};
  }
  bool tableLookupComplete(const NameResolutionContext &,
      const std::vector<std::string> &) const override { return false; }
  std::vector<RoutineDefinition> findRoutines(const NameResolutionContext &names,
      const std::vector<std::string> &name, bool procedure) const override {
    return current() ? base->findRoutines(names,name,procedure) : std::vector<RoutineDefinition>{};
  }
  bool routineLookupComplete(const NameResolutionContext &,
      const std::vector<std::string> &,bool) const override { return false; }
};
bool mysqlFamily(Dialect d) {
  return isMySqlDialect(d);
}
bool sameFamily(Dialect a, Dialect b) {
  return a == b || (mysqlFamily(a) && mysqlFamily(b));
}
void issue(std::vector<Diagnostic> &out, SourceRange where,
           const std::string &message) {
  out.push_back({DiagnosticCode::UnsupportedSyntax, where, message});
}
std::string joinNames(const std::vector<std::string> &parts, std::string_view separator = ".") {
  std::string text;
  for (const auto &part : parts) {
    if (!text.empty()) text.append(separator);
    text += part;
  }
  return text;
}
std::string typeSpelling(const BasicTypeSpec<std::vector<std::string>> &type) {
  auto spelling=joinNames(type.name);
  if (!type.arguments.empty()) {
    spelling += '(';
    for (std::size_t i=0;i<type.arguments.size();++i) {
      if (i) spelling += ", ";
      spelling += type.arguments[i].value;
    }
    spelling += ')';
  }
  return spelling;
}
struct ConversionChanges {
  ConversionResult *result=nullptr;
  std::size_t statement=0, start=0, end=0;
  void operator()(ConversionChangeKind kind, SourceRange origin,
                  std::string before, std::string after) const {
    if (!result || before == after ||
        (kind == ConversionChangeKind::Type && asciiUpper(before) == asciiUpper(after))) return;
    if (result->changes.size() >= 2048) {
      result->changesTruncated = true;
      return;
    }
    origin.begin = std::min(start+origin.begin, end);
    origin.end = std::min(start+origin.end, end);
    result->changes.push_back({kind, statement, origin, std::move(before), std::move(after)});
  }
};
void mapName(std::vector<std::string> &name, const ConversionOptions &options) {
  if (name.size() < 2)
    return;
  for (const auto &mapping : options.qualifiers)
    if (name.size() > mapping.source.size() &&
        std::equal(mapping.source.begin(), mapping.source.end(),
                   name.begin())) {
      std::vector<std::string> mapped = mapping.target;
      mapped.insert(mapped.end(), name.begin() + mapping.source.size(),
                    name.end());
      name = std::move(mapped);
      return;
    }
}
void mapType(BasicTypeSpec<std::vector<std::string>> &type, Dialect source,
             Dialect target, std::vector<Diagnostic> &errors) {
  if (sameFamily(source, target))
    return;
  if(source==Dialect::PostgreSQL && type.name.size()==2) {
    const auto known=resolveDeclaredType(type,source).type;
    if(known.family==TypeFamily::Integer && known.bits && known.opaqueName.empty()) {
      type.name={*known.bits==16?"SMALLINT":*known.bits==32?"INTEGER":"BIGINT"};type.nameQuoted={false};
    }
  }
  if (type.name.size() != 1 ||
      (!type.nameQuoted.empty() && type.nameQuoted[0])) {
    issue(errors, type.origin,
          "user-defined/qualified types require an explicit type mapping");
    return;
  }
  auto n = asciiUpper(type.name[0]);
  if (!type.modifiers.empty() || type.arrayDimensions || type.lengthSemantics) {
    issue(errors, type.origin,
          "type modifiers/arrays/length semantics need an explicit semantic "
          "mapping");
    return;
  }
  if(source==Dialect::Oracle && (n=="INT" || n=="INTEGER" || n=="SMALLINT")) {
    issue(errors,type.origin,"Oracle integer synonyms have NUMBER(38,0) domain; mapping them to a native narrow integer requires explicit range constraints");return;
  }
  if (n == "INT" || n == "INTEGER") {
    type.name = {target == Dialect::Oracle ? "NUMBER" : "INTEGER"};
    if (target == Dialect::Oracle) {
      issue(errors, type.origin,
            "Oracle NUMBER has no exact built-in 32-bit INTEGER domain; add "
            "explicit range constraints");
      return;
    }
    type.arguments.clear();
  } else if (n == "SMALLINT" || n == "BIGINT") {
    if (target == Dialect::Oracle)
      issue(errors, type.origin,
            "integer range enforcement requires explicit Oracle constraints");
    type.name = {n};
  } else if (n == "DECIMAL" || n == "NUMERIC" || n == "NUMBER") {
    if (type.arguments.size() != 2) {
      issue(errors, type.origin,
            "decimal precision and scale must both be explicit for exact "
            "conversion");
      return;
    }
    if (target == Dialect::SQLite) {
      issue(errors, type.origin,
            "SQLite numeric affinity cannot preserve fixed decimal precision");
      return;
    }
    type.name = {target == Dialect::Oracle ? "NUMBER" : "DECIMAL"};
  } else if (n == "VARCHAR" || n == "VARCHAR2") {
    if (source == Dialect::Oracle || target == Dialect::Oracle) {
      issue(errors, type.origin,
            "Oracle empty strings/length semantics differ; an explicit string "
            "policy is required");
      return;
    }
    type.name = {"VARCHAR"};
  } else if (n == "TEXT" && source == Dialect::PostgreSQL &&
             target == Dialect::SQLite)
    type.name = {"TEXT"};
  else if (n == "BOOLEAN" &&
           (source == Dialect::PostgreSQL || source == Dialect::SQLite) &&
           (target == Dialect::PostgreSQL || target == Dialect::SQLite))
    type.name = {"BOOLEAN"};
  else if(n=="DATE")
    issue(errors,type.origin,"native DATE domains / SQL modes differ, and SQLite DATE is numeric affinity; only proved portable date constants have an automatic mapping");
  else
    issue(errors, type.origin,
          "no proven equivalent mapping for type " + n +
              "; precision/time-zone/collation/domain semantics may differ");
}
} // namespace
std::optional<std::int64_t> exactSignedIntegerLiteral(const ModelStatement &q,
                                                      ExprId id, bool allowString) {
  if(!id || id>=q.expressions.size())return {};
  const auto *e=&q.expressions[id];std::string sign;
  if(e->kind==ExprKind::Unary && (e->value=="-" || e->value=="+") && e->operands.size()==1) {
    sign=e->value;e=&q.expressions.at(e->operands[0]);if(e->kind!=ExprKind::Number)return {};
  }
  if (e->kind != ExprKind::Number && !(allowString && e->kind == ExprKind::String)) return {};
  const auto text=sign+e->value;
  const auto start=!text.empty() && (text[0]=='-' || text[0]=='+')?1u:0u;
  if(text.size()<=start || text.size()>20 || text.find_first_not_of("0123456789",start)!=std::string::npos)return {};
  try{return std::stoll(text);}catch(const std::exception &){return {};}
}
namespace {
bool exactIntegerCast(const ModelProgram &program,const ModelStatement &q,const ModelExpression &e,
                      const SqlType &from,const SqlType &to) {
  if(from.family!=TypeFamily::Integer || to.family!=TypeFamily::Integer || !from.bits || from.bits!=to.bits ||
     !from.unsignedInteger || from.unsignedInteger!=to.unsignedInteger || from.arrayDimensions || to.arrayDimensions || from.sqliteAffinity || to.sqliteAffinity || e.operands.size()!=1)return false;
  const auto &input=q.expressions.at(e.operands[0]);if(input.kind==ExprKind::Null)return true;
  const auto bits=*to.bits;if(bits<1 || bits>64)return false;
  if(auto value=exactSignedIntegerLiteral(q,e.operands[0],true)) {
    if(*to.unsignedInteger)return *value>=0 && (bits>=63 || std::uint64_t(*value)<(std::uint64_t{1}<<bits));
    if(bits==64)return true;
    const auto range=std::int64_t{1}<<(bits-1);return *value>=-range && *value<range;
  }
  const auto &type=program.types.get(input.sqlType);
  if(type.family!=TypeFamily::Integer || !type.bits || !type.unsignedInteger || type.arrayDimensions || type.sqliteAffinity)return false;
  if(*to.unsignedInteger)return *type.unsignedInteger && *type.bits<=bits;
  return *type.unsignedInteger?*type.bits<bits:*type.bits<=bits;
}
void convertCast(ModelProgram &program,ModelStatement &q,ModelExpression &e,Dialect source,Dialect target,
                 std::vector<Diagnostic> &errors) {
  auto syntax=e.castType?*e.castType:BasicTypeSpec<std::vector<std::string>>{};
  if(!e.castType){syntax.name=e.name;syntax.nameQuoted=e.nameQuoted;syntax.origin=e.origin;}
  if(!e.value.empty() || !syntax.arguments.empty()) {issue(errors,e.origin,"parameterized casts require typed precision and rounding mapping");return;}
  if(source==Dialect::SQLite || target==Dialect::SQLite || source==Dialect::MariaDB || target==Dialect::MariaDB || source==Dialect::OceanBase || target==Dialect::OceanBase || source==Dialect::TiDB || target==Dialect::TiDB) {
    issue(errors,e.origin,"this engine's CAST affinity / result domain has no proved cross-product mapping");return;
  }
  const auto original=resolveDeclaredType(syntax,source,TypeUse::Cast);
  if(source==Dialect::PostgreSQL && syntax.name.size()==2 && original.type.family==TypeFamily::Integer &&
     original.type.bits && original.type.opaqueName.empty()) {
    syntax.name={*original.type.bits==16?"SMALLINT":*original.type.bits==32?"INTEGER":"BIGINT"};syntax.nameQuoted={false};
  }
  const auto name=syntax.name.empty()?std::string{}:asciiUpper(syntax.name[0]);
  if(source==Dialect::MySQL && name=="SIGNED" && (target==Dialect::PostgreSQL || target==Dialect::SQLServer)) {
    syntax.name={"BIGINT"};syntax.nameQuoted={false};
  } else if(target==Dialect::MySQL && original.type.family==TypeFamily::Integer && original.type.bits==64 && original.type.unsignedInteger==false) {
    syntax.name={"SIGNED"};syntax.nameQuoted={false};
  } else {
    if(source==Dialect::PostgreSQL && name=="INT2")syntax.name={"SMALLINT"};
    if(source==Dialect::PostgreSQL && name=="INT4")syntax.name={"INTEGER"};
    if(source==Dialect::PostgreSQL && name=="INT8")syntax.name={"BIGINT"};
    const auto before=errors.size();mapType(syntax,source,target,errors);if(errors.size()!=before)return;
  }
  const auto lowered=resolveDeclaredType(syntax,target,TypeUse::Cast);
  if(!original.error.empty() || !lowered.error.empty()) {issue(errors,e.origin,"target CAST form is unavailable or changes the native result domain: "+lowered.error);return;}
  if(!exactIntegerCast(program,q,e,original.type,lowered.type)) {
    issue(errors,e.origin,"CAST input/result types do not prove identical rounding, overflow and input-conversion behavior; provide exact types or an explicit mapping policy");return;
  }
  e.name=syntax.name;e.nameQuoted=syntax.nameQuoted;e.castType=std::make_shared<const BasicTypeSpec<std::vector<std::string>>>(std::move(syntax));
}
bool statementSeparator(const Token &token) {
  return token.kind == TokenKind::Symbol && token.value == ";";
}
} // namespace
std::size_t statementEndIndex(Dialect dialect, const std::vector<Token> &tokens, std::size_t start) {
  const auto word = [&](std::size_t index, std::string_view value) {
    return index < tokens.size() && tokens[index].kind == TokenKind::Word &&
           asciiUpper(tokens[index].value) == value;
  };
  bool trigger = dialect == Dialect::SQLite && word(start, "CREATE") &&
      (word(start+1, "TRIGGER") || ((word(start+1, "TEMP") || word(start+1, "TEMPORARY")) && word(start+2, "TRIGGER")));
  bool package=false;
  if(dialect==Dialect::Oracle && word(start,"CREATE")) {
    auto next=start+1;
    if(word(next,"OR") && word(next+1,"REPLACE"))next+=2;
    if(word(next,"EDITIONABLE") || word(next,"NONEDITIONABLE"))++next;
    package=word(next,"PACKAGE") && !word(next+1,"BODY");
  }
  if(mysqlFamily(dialect) && word(start,"CREATE")) {
    auto header=start+1;
    if(word(header,"DEFINER")) {
      while(header<start+10 && header<tokens.size() && !word(header,"TRIGGER") &&
            !statementSeparator(tokens[header]))++header;
    }
    if(word(header,"TRIGGER")) {
      for(auto i=header+1;i<tokens.size() && tokens[i].kind!=TokenKind::End &&
          !statementSeparator(tokens[i]);++i) {
        if(!word(i,"FOR") || !word(i+1,"EACH") || !word(i+2,"ROW"))continue;
        auto step=i+3;
        if(word(step,"FOLLOWS") || word(step,"PRECEDES"))step+=2;
        trigger=word(step,"BEGIN");
        break;
      }
    }
  }
  for (auto i = start; i < tokens.size(); ++i) {
    if (tokens[i].kind == TokenKind::End) return i;
    if(!statementSeparator(tokens[i]))continue;
    if(package) {
      auto end=i-1;
      if(!word(end,"END") && end>start &&
         (tokens[end].kind==TokenKind::Identifier || tokens[end].kind==TokenKind::Word))--end;
      if(end>start && word(end,"END") &&
         (statementSeparator(tokens[end-1]) || word(end-1,"AS") || word(end-1,"IS")))return i;
      continue;
    }
    // A supported trigger body ends with a step's semicolon followed by END.
    // END in a CASE expression is not preceded by a statement separator.
    if (!trigger || (i >= start+2 && word(i-1,"END") &&
        statementSeparator(tokens[i-2]))) return i;
  }
  return tokens.size();
}
namespace {
void shift(std::vector<Diagnostic> &diagnostics, std::size_t offset) {
  for (auto &d : diagnostics) {
    d.range.begin += offset;
    d.range.end += offset;
  }
}
std::string mysqlClientDefinition(std::string sql) {
  while(!sql.empty() && (sql.back()=='\n' || sql.back()==' '))sql.pop_back();
  if(!sql.empty() && sql.back()==';')sql.pop_back();
  std::string delimiter="$$";
  while(sql.find(delimiter)!=std::string::npos)delimiter+='$';
  return "DELIMITER "+delimiter+"\n"+sql+delimiter+"\nDELIMITER ;";
}
} // namespace
std::string normalizeConversionScript(Dialect dialect,
                                      const std::string &source,
                                      LanguageOptions options,
                                      std::vector<Diagnostic> &errors,
                                      std::vector<std::size_t> *clientSeparators) {
  if (source.size() > options.limits.sourceBytes) {
    issue(errors, {}, "source byte limit exceeded");
    return {};
  }
  Compilation c(dialect, source, options, "<script>");
  try {
    lex(c, true, true);
  } catch (const CompilationAborted &) {
    errors = c.diagnostics;
    return {};
  }
  if (!c.diagnostics.empty()) {
    errors = c.diagnostics;
    return {};
  }
  std::vector<SourceRange> opaque;
  for (const auto &t : c.tokens)
    if (t.kind == TokenKind::String || t.kind == TokenKind::Identifier ||
        t.kind == TokenKind::Comment)
      opaque.push_back(t.range);
  auto protectedAt = [&](std::size_t i) {
    auto it = std::upper_bound(
        opaque.begin(), opaque.end(), i,
        [](auto at, const auto &range) { return at < range.begin; });
    return it != opaque.begin() && i < (--it)->end;
  };
  std::string result = source, delimiter = ";";
  for (std::size_t line = 0; line < source.size();) {
    auto end = source.find('\n', line);
    if (end == std::string::npos)
      end = source.size();
    auto begin = line;
    while (begin < end && (source[begin] == ' ' || source[begin] == '\t' ||
                           source[begin] == '\r'))
      ++begin;
    auto last = end;
    while (last > begin &&
           (source[last - 1] == ' ' || source[last - 1] == '\t' ||
            source[last - 1] == '\r'))
      --last;
    const auto text = source.substr(begin, last - begin), up = asciiUpper(text);
    bool directive = false;
    if (!protectedAt(begin) && mysqlFamily(dialect) && up.size() > 9 &&
        up.compare(0, 9, "DELIMITER") == 0 && (up[9] == ' ' || up[9] == '\t')) {
      delimiter = text.substr(10);
      auto start = delimiter.find_first_not_of(" \t");
      delimiter = start == std::string::npos ? "" : delimiter.substr(start);
      if (delimiter.empty() || delimiter.size() > 16 ||
          delimiter.find_first_of(" \t'\"`\\") != std::string::npos ||
          delimiter == "/*" || delimiter == "--") {
        issue(errors, {begin, last}, "unsupported client delimiter");
        return {};
      }
      directive = true;
    }
    if (!protectedAt(begin) && dialect == Dialect::SQLServer && up == "GO")
      directive = true;
    if (!protectedAt(begin) && dialect == Dialect::SQLServer &&
        up.rfind("GO ", 0) == 0) {
      issue(errors, {begin, last},
            "GO repetition counts/comments are not supported; split the batch "
            "explicitly");
      return {};
    }
    if (!protectedAt(begin) && dialect == Dialect::Oracle && text == "/")
      directive = true;
    if (directive) {
      for (auto i = begin; i < end; ++i)
        result[i] = ' ';
      if ((dialect == Dialect::SQLServer && up == "GO") ||
          (dialect == Dialect::Oracle && text == "/"))
        result[begin] = ';';
    } else if (mysqlFamily(dialect) && delimiter != ";") {
      for (auto i = begin; i < end;) {
        if (!protectedAt(i) &&
            source.compare(i, delimiter.size(), delimiter) == 0) {
          if (clientSeparators)
            clientSeparators->push_back(i);
          result[i] = ';';
          for (std::size_t j = 1; j < delimiter.size(); ++j)
            result[i + j] = ' ';
          i += delimiter.size();
        } else
          ++i;
      }
    }
    line = end < source.size() ? end + 1 : end;
  }
  return result;
}
static std::optional<Diagnostic> rememberRoutine(ScriptContext &context,
                                                 const RoutineAst &ast) {
  RoutineDefinition def;
  def.procedure = !ast.function;
  def.minArguments = def.maxArguments = ast.parameters.size();
  if(ast.function)def.sqlType=std::make_shared<const SqlType>(resolveDeclaredType(ast.result,ast.dialect,TypeUse::Signature).type);
  def.resultType = ast.result.arrayDimensions
                       ? ValueType::Unknown
                       : typeCategory(ast.result.name, ast.dialect);
  for (auto id : ast.parameters)
    if (id < ast.symbols.size() &&
        ast.symbols[id].initializer.end > ast.symbols[id].initializer.begin)
      --def.minArguments;
  return context.declareRoutine(ast.name, std::move(def), ast.origin, ast.ifNotExists);
}
ScriptCheckResult checkSqlScript(Dialect dialect, std::string source,
                                 LanguageOptions options,
                                 const CatalogSnapshot *catalog,
                                 NameResolutionContext names) {
  ScriptCheckResult result;
  auto normalized =
      normalizeConversionScript(dialect, source, options, result.diagnostics);
  if (!result.diagnostics.empty()) {
    result.complete = false;
    return result;
  }
  Compilation lexed(dialect, normalized, options, "<script>");
  try {
    lex(lexed, true);
  } catch (const CompilationAborted &) {
    result.diagnostics = lexed.diagnostics;
    result.complete = false;
    return result;
  }
  if (!lexed.diagnostics.empty()) {
    result.diagnostics = lexed.diagnostics;
    result.complete = false;
    return result;
  }
  std::vector<Token> tokens;
  for (const auto &t : lexed.tokens)
    if (t.kind != TokenKind::Comment)
      tokens.push_back(t);
  ScriptContext context(dialect, catalog, names);
  const auto *activeCatalog = &context;
  for (std::size_t i = 0;
       i < tokens.size() && tokens[i].kind != TokenKind::End;) {
    if (statementSeparator(tokens[i])) {
      ++i;
      continue;
    }
    if (options.shouldStop && options.shouldStop()) {
      result.diagnostics.push_back(
          {DiagnosticCode::Stopped, tokens[i].range, "script check stopped"});
      result.complete = false;
      break;
    }
    if (++result.statements > options.limits.statements) {
      result.diagnostics.push_back({DiagnosticCode::ResourceLimit,
                                    tokens[i].range,
                                    "script statement limit exceeded"});
      result.complete = false;
      break;
    }
    const auto begin = tokens[i].range.begin;
    std::size_t end = begin;
    std::vector<Diagnostic> errors;
    if (isRoutineStart(dialect, tokens, i)) {
      ++result.routines;
      auto routine = compileRoutinePrefix(dialect, normalized.substr(begin),
                                          options, activeCatalog, names);
      errors = routine.diagnostics;
      shift(routine.runtimeDiagnostics, begin);
      for (auto &warning : routine.runtimeDiagnostics)
        if (result.runtimeDiagnostics.size() < std::max<std::size_t>(1, options.limits.diagnostics))
          result.runtimeDiagnostics.push_back(std::move(warning));
      if (routine.ok())
        if (auto error = rememberRoutine(context, routine.ast))
          errors.push_back(*error);
      end = begin + routine.ast.origin.end;
      if (!routine.ok() && end <= begin) {
        shift(errors, begin);
        result.diagnostics.insert(result.diagnostics.end(), errors.begin(),
                                  errors.end());
        result.complete = false;
        break;
      }
    } else {
      const auto next = statementEndIndex(dialect, tokens, i);
      end = next < tokens.size() ? tokens[next].range.end : normalized.size();
      auto c = compile(dialect, normalized.substr(begin, end - begin), options,
                       activeCatalog, names);
      errors = c->diagnostics;
      if (c->ok()) {
        for (auto root : c->model.roots)
          if (auto error = context.observe(c->model, root))
            errors.push_back(*error);
      } else {
        const auto word = asciiUpper(tokens[i].value);
        if (word == "SET" || word == "USE" || word == "DO" || word == "BEGIN" ||
            word == "START" || word == "COMMIT" || word == "ROLLBACK" ||
            word == "SAVEPOINT" || word == "RELEASE" || word == "DISCARD")
          context.invalidate();
      }
    }
    shift(errors, begin);
    result.diagnostics.insert(result.diagnostics.end(), errors.begin(),
                              errors.end());
    result.metadataUsed |= catalog && catalog->current();
    while (i < tokens.size() && tokens[i].range.begin < end)
      ++i;
    if (end <= begin) {
      result.complete = false;
      break;
    }
    if (result.diagnostics.size() >= options.limits.diagnostics) {
      result.diagnostics.resize(
          std::max<std::size_t>(1, options.limits.diagnostics));
      result.complete = false;
      break;
    }
  }
  return result;
}

static bool portableIntegerValues(const ModelStatement &query) {
  if (query.rows.empty() || query.rows.front().empty() || query.limit || query.offset ||
      !query.orderBy.empty() || !query.ctes.empty()) return false;
  std::vector<bool> typed(query.rows.front().size());
  for (const auto &row : query.rows) {
    if (row.size() != typed.size()) return false;
    for (std::size_t i = 0; i < row.size(); ++i) {
      const auto *cell = &query.expressions.at(row[i]);
      if (cell->kind == ExprKind::Null) continue;
      if (cell->kind == ExprKind::Unary && cell->operands.size() == 1 &&
          (cell->value == "+" || cell->value == "-"))
        cell = &query.expressions.at(cell->operands.front());
      if (cell->kind != ExprKind::Number || cell->value.empty() || cell->value.size() > 9 ||
          cell->value.find_first_not_of("0123456789") != std::string::npos) return false;
      typed[i] = true;
    }
  }
  return std::all_of(typed.begin(), typed.end(), [](bool known) { return known; });
}
static bool portableCountOperand(const ModelStatement &query, const SqlType &type,
                                 const ModelExpression &value) {
  if (value.kind==ExprKind::Null) return true;
  if (value.kind!=ExprKind::Name || value.name.empty() || value.localSymbol ||
      value.outputColumn || !value.coalescedBindings.empty() ||
      type.arrayDimensions || !type.opaqueName.empty())return false;
  // A bare range variable can denote the whole composite row in PostgreSQL.
  if(!value.binding && value.name.size()==1)
    for(const auto& table:query.tables)
      if(table.alias==value.name.front() || (table.alias.empty() && !table.name.empty() &&
         table.name.back()==value.name.front()))return false;
  return type.family==TypeFamily::Unknown || type.family==TypeFamily::Integer ||
         type.family==TypeFamily::String || type.family==TypeFamily::Decimal;
}
static bool portableCountPredicate(const ModelProgram &program,const ModelStatement &query,ExprId root) {
  std::vector<ExprId> pending{root};
  for (std::size_t count=0;!pending.empty();++count) {
    if(count>=32 || pending.size()>32)return false;
    const auto &e=query.expressions.at(pending.back());
    pending.pop_back();
    if(e.type!=ValueType::Boolean)return false;
    if(e.kind==ExprKind::IsNull && e.operands.size()==1) {
      const auto &value=query.expressions.at(e.operands.front());
      if(!portableCountOperand(query,program.types.get(value.sqlType),value) &&
         !exactSignedIntegerLiteral(query,e.operands.front(),false))return false;
      continue;
    }
    if(e.kind==ExprKind::Unary && e.value=="NOT" && e.operands.size()==1) {
      pending.push_back(e.operands.front());
      continue;
    }
    if(e.kind!=ExprKind::Binary || e.operands.size()!=2)return false;
    if(e.value=="AND" || e.value=="OR") {
      pending.insert(pending.end(), e.operands.begin(),e.operands.end());
      continue;
    }
    if(e.value!="=" && e.value!="<>" && e.value!=">" && e.value!="<" &&
       e.value!=">=" && e.value!="<=")return false;
    for(const auto id:e.operands) {
      if(exactSignedIntegerLiteral(query,id,false))continue;
      const auto &value=query.expressions.at(id);
      const auto &type=program.types.get(value.sqlType);
      if(value.kind!=ExprKind::Name || !value.binding || !type.bits ||
         type.family!=TypeFamily::Integer || type.unsignedInteger!=false ||
         !portableCountOperand(query,type,value))return false;
    }
  }
  return true;
}
bool lowerTarget(ModelProgram &model, Dialect target,
                 const ConversionOptions &options,
                 std::vector<Diagnostic> &errors, const ConversionChanges &change) {
  using Change = ConversionChangeKind;
  const auto source = model.sourceDialect;
  const bool family = sameFamily(source, target);
  const auto mappedName=[&](auto &name,SourceRange origin) {
    if(options.qualifiers.empty() || name.size()<2)return;
    const auto before=joinNames(name);
    mapName(name,options);
    change(Change::Qualifier,origin,before,joinNames(name));
  };
  const auto mappedType=[&](auto &type) {
    const auto before=typeSpelling(type);
    mapType(type,source,target,errors);
    change(Change::Type,type.origin,before,typeSpelling(type));
  };
  std::set<QueryId> portableValueTables;
  if ((source == Dialect::PostgreSQL && target == Dialect::SQLServer) ||
      (source == Dialect::SQLServer && target == Dialect::PostgreSQL)) {
    for (const auto &query : model.queries)
      for (const auto &table : query.tables) {
        if (!table.name.empty() || table.alias.empty() || table.columnAliases.empty() ||
            !table.query || table.query > model.queries.size()) continue;
        const auto &values = model.queries[table.query - 1];
        if (values.valuesQuery && !values.rows.empty() &&
            table.columnAliases.size() == values.rows.front().size() && portableIntegerValues(values))
          portableValueTables.insert(table.query);
      }
  }
  const std::set<std::string> portableCalls = {
      "COUNT",      "SUM",         "MIN",         "MAX",
      "AVG",        "ABS",         "COALESCE",    "NULLIF",
      "LOWER",      "UPPER",       "CHAR_LENGTH", "CHARACTER_LENGTH",
      "ROW_NUMBER", "RANK",        "DENSE_RANK",  "LAG",
      "LEAD",       "FIRST_VALUE", "LAST_VALUE", "GROUPING"};
  const auto nullFirst = [](Dialect dialect) {
    return mysqlFamily(dialect) || dialect == Dialect::SQLite ||
           dialect == Dialect::SQLServer;
  };
  auto preserveNullOrdering = [&](auto &ordering, SourceRange origin) {
    if (family)
      return;
    for (auto &order : ordering) {
      const auto explicitBefore = !order.nulls.empty();
      const auto desired =
          order.nulls.empty()
              ? ((nullFirst(source) != order.descending) ? "FIRST" : "LAST")
              : order.nulls;
      const bool explicitNulls = target == Dialect::PostgreSQL ||
                                 target == Dialect::Oracle ||
                                 target == Dialect::SQLite;
      if (explicitNulls) {
        if (!order.nulls.empty() || nullFirst(source) != nullFirst(target))
          order.nulls = desired;
      } else if (desired ==
                 ((nullFirst(target) != order.descending) ? "FIRST" : "LAST"))
        order.nulls.clear();
      else
        issue(errors, origin,
              "target cannot preserve NULL ordering without "
              "duplicating/retyping ordering expressions");
      const auto explicitAfter = !order.nulls.empty();
      if (explicitBefore != explicitAfter)
        change(Change::NullOrdering,origin,
               desired+(explicitBefore?" (explicit)":" (default)"),
               desired+(explicitAfter?" (explicit)":" (default)"));
    }
  };
  auto preserveExclusion=[&](WindowSpec &spec,SourceRange origin) {
    if(spec.exclusion==WindowExclusion::None || target==Dialect::PostgreSQL || target==Dialect::SQLite)return;
    if(spec.exclusion==WindowExclusion::NoOthers) {
      spec.exclusion=WindowExclusion::None;
      change(Change::WindowExclusion,origin,"EXCLUDE NO OTHERS","default frame exclusion");
    }
    else issue(errors,origin,"target has no supported equivalent for window EXCLUDE; frame subtraction is not an exact NULL / peer / aggregate rewrite");
  };
  for (auto &q : model.queries) {
    if (options.language.shouldStop && options.language.shouldStop()) {
      errors.push_back(
          {DiagnosticCode::Stopped, q.origin, "conversion cancelled"});
      return false;
    }
    if (!family && q.having && q.groupBy.empty() && q.groupingSets.empty() &&
        mysqlFamily(source) != mysqlFamily(target)) {
      const auto features =
          inspectQueryFeatures(q, source, 250000, options.language.shouldStop);
      if (!features) {
        errors.push_back({DiagnosticCode::ResourceLimit, q.origin,
                          "query feature analysis stopped or exceeded limits"});
        return false;
      }
      if (!features->aggregate)
        issue(errors, q.origin,
              "HAVING without grouping/aggregates is a row filter in MySQL but "
              "an implicit group in this target/source; an explicit query "
              "rewrite is required");
    }
    if (!family &&
        (q.kind == StatementKind::Update || q.kind == StatementKind::Delete)) {
      if (!q.modifiers.empty() || q.limit || !q.orderBy.empty() ||
          !q.deleteTargets.empty())
        issue(errors, q.origin,
              "DML modifiers/multi-target/ordering/row limits need a "
              "deterministic target rewrite");
      if (q.kind == StatementKind::Update) {
        if (mysqlFamily(source) != mysqlFamily(target) &&
            q.assignments.size() > 1) {
          // MySQL single-table assignments read earlier updated values; the
          // other baselines evaluate RHS expressions against the old row.
          // Detect dependencies instead of silently reordering or inventing
          // temporary values with different trigger/snapshot behavior.
          struct Earlier {
            std::string name;
            std::optional<ColumnBinding> binding;
          };
          std::vector<Earlier> earlier;
          auto key = [&](std::string name) {
            return mysqlFamily(source) || mysqlFamily(target) ? asciiUpper(name)
                                                              : name;
          };
          std::size_t inspected = 0;
          for (const auto &assignment : q.assignments) {
            std::vector<std::pair<QueryId, ExprId>> work{
                {q.id, assignment.value}};
            std::set<std::pair<QueryId, ExprId>> seen;
            bool dependent = false;
            while (!work.empty() && !earlier.empty()) {
              auto current = work.back();
              work.pop_back();
              if (!seen.insert(current).second)
                continue;
              if (++inspected > options.language.limits.nodes * 8) {
                errors.push_back(
                    {DiagnosticCode::ResourceLimit, q.origin,
                     "assignment dependency analysis limit exceeded"});
                return false;
              }
              if ((inspected & 127u) == 0 && options.language.shouldStop &&
                  options.language.shouldStop()) {
                errors.push_back({DiagnosticCode::Stopped, q.origin,
                                  "assignment dependency analysis stopped"});
                return false;
              }
              const auto &owner = model.queries.at(current.first - 1);
              const auto &e = owner.expressions.at(current.second);
              if (e.kind == ExprKind::Name && !e.localSymbol && !e.name.empty())
                for (const auto &old : earlier)
                  if (key(old.name) == key(e.name.back())) {
                    if (e.binding && old.binding) {
                      dependent = e.binding->scope == old.binding->scope &&
                                  e.binding->table == old.binding->table &&
                                  e.binding->column == old.binding->column;
                    } else {
                      dependent = true;
                      if (owner.id == q.id && e.name.size() > 1 && q.target) {
                        const auto targetName = q.target->alias.empty()
                                                    ? q.target->name.back()
                                                    : q.target->alias;
                        dependent =
                            key(e.name[e.name.size() - 2]) == key(targetName);
                      }
                    }
                    if (dependent)
                      break;
                  }
              if (dependent)
                break;
              for (auto operand : e.operands)
                work.push_back({owner.id, operand});
              if (e.query) {
                const auto &nested = model.queries.at(e.query - 1);
                for (ExprId id = 1; id < nested.expressions.size(); ++id)
                  work.push_back({nested.id, id});
                for (const auto &cte : nested.ctes)
                  if (cte.query) {
                    const auto &body = model.queries.at(cte.query - 1);
                    for (ExprId id = 1; id < body.expressions.size(); ++id)
                      work.push_back({body.id, id});
                  }
              }
            }
            if (dependent)
              issue(errors, q.expressions.at(assignment.value).origin,
                    "UPDATE assignment evaluation order differs: this RHS "
                    "reads a previously assigned target column; an explicit "
                    "old/new-row rewrite is required");
            for (auto id : assignment.targets) {
              const auto &lhs = q.expressions.at(id);
              if (!lhs.name.empty())
                earlier.push_back({lhs.name.back(), lhs.binding});
            }
          }
        }
        if (mysqlFamily(source) && q.sourceFrom) {
          const auto &root = q.relations.at(q.sourceFrom - 1);
          if (!root.left && !root.right && q.targetTable &&
              root.table == *q.targetTable) {
            q.sourceFrom = 0;
            change(Change::UpdateTarget,q.origin,"UPDATE table reference","UPDATE target");
          } else
            issue(errors, q.origin,
                  "joined UPDATE needs cardinality/key-based target rewriting");
        } else if (q.sourceFrom)
          issue(
              errors, q.origin,
              "UPDATE FROM joins need cardinality/key-based target rewriting");
        if (mysqlFamily(target) && !q.sourceFrom && q.targetTable) {
          ModelRelation leaf;
          leaf.table = *q.targetTable;
          q.relations.push_back(leaf);
          q.sourceFrom = RelationId(q.relations.size());
          change(Change::UpdateTarget,q.origin,"UPDATE target","UPDATE table reference");
        }
        if (target == Dialect::SQLServer && q.targetReference &&
            !q.targetReference->alias.empty())
          issue(errors, q.origin,
                "UPDATE aliases require a SQL Server FROM rewrite");
      } else if (q.sourceFrom)
        issue(errors, q.origin,
              "joined DELETE needs key/cardinality-based target rewriting");
    }
    if (!family && q.kind == StatementKind::Merge)
      issue(errors, q.origin,
            "MERGE requires explicit uniqueness/cardinality, trigger and "
            "concurrency policies; dialect syntax alone does not establish an "
            "equivalent upsert");
    if (source != target && std::any_of(q.tables.begin(), q.tables.end(), [](const auto &table) {
          return table.pivot != nullptr;
        }))
      issue(errors, q.origin, "PIVOT needs a target mapping for implicit groups, bucket values and aggregate result types");
    if (source!=target && std::any_of(q.tables.begin(),q.tables.end(),[](const auto &table) { return bool(table.unpivot); }))
      issue(errors,q.origin,"UNPIVOT requires explicit target NULL, label and output type semantics");
    if (q.pragma && source != target)
      issue(errors, q.origin, "SQLite PRAGMA settings and side effects require explicit target mapping");
    if (q.vacuum && source != target)
      issue(errors, q.origin, "PostgreSQL VACUUM changes storage and transaction behavior and requires an explicit target policy");
    if (q.copy && source != target)
      issue(errors, q.origin, "PostgreSQL COPY requires a matching client transfer protocol and data format");
    if (q.trigger && source != target)
      issue(errors, q.origin, "triggers need an explicit mapping for trigger rows, event timing, security, and transactional behavior");
    if(q.package && source!=target)
      issue(errors,q.origin,"Oracle packages require explicit mappings for their state, signatures, and authorization");
    if (q.ddl && !q.ddl->tableIndexes.empty() && source!=target)
      issue(errors,q.origin,"inline MySQL indexes need an explicit target deployment mapping");
    if (q.ddl && q.ddl->mysqlViewOptions && source != target)
      issue(errors, q.origin, "view algorithm, definer and security require an explicit target mapping");
    if (q.ddl && q.ddl->mysqlLoadableFunction && source != target)
      issue(errors,q.origin,"loadable functions require a target library and deployment mapping");
    if (q.ddl && q.ddl->virtualTable && source != target)
      issue(errors, q.origin, "SQLite virtual tables require matching target modules and argument semantics");
    if (q.inspection && source != target)
      issue(errors, q.origin, "administrative inspection output, privileges and version semantics require explicit target mapping");
    if (source != target && q.setOperation && q.setOperation->recursive)
      issue(errors, q.origin, "recursive CTE conversion requires matching seed type, iteration and termination semantics");
    if (source != target && std::any_of(q.ctes.begin(), q.ctes.end(), [](const auto &cte) {
          return cte.materialization != CteMaterialization::Default;
        }))
      issue(errors, q.origin, "CTE materialization and evaluation sharing require matching target semantics");
    if (source != target) {
      if (q.limit || q.offset) {
        const auto simple=[&](ExprId id) {
          if (!id) return true;
          const auto &bound=q.expressions.at(id);
          return bound.kind==ExprKind::Number && !bound.value.empty() &&
              bound.value.find_first_not_of("0123456789")==std::string::npos &&
              bool(exactSignedIntegerLiteral(q,id,false));
        };
        if (!simple(q.limit) || !simple(q.offset))
          issue(errors,q.origin,"non-literal, NULL or dynamic pagination requires an explicit numeric, null and target grammar mapping");
      }
      if (q.limit || q.offset) {
        const auto pagination=[&](Dialect dialect)->std::string {
          if(dialect==Dialect::SQLServer && !q.offset)return q.limitWithTies?"TOP WITH TIES":"TOP";
          if(q.limitWithTies)return "FETCH FIRST … WITH TIES";
          if(dialect==Dialect::Oracle || dialect==Dialect::SQLServer)
            return q.limit ? (q.offset ? "OFFSET … ROWS FETCH NEXT" : "FETCH NEXT") : "OFFSET … ROWS";
          return q.offset ? (q.limit?"LIMIT … OFFSET":"OFFSET") : "LIMIT";
        };
        auto written=pagination(source);
        if(q.limitSyntax==LimitSyntax::CommaLimit)written="LIMIT offset, count";
        else if(q.limitSyntax==LimitSyntax::Fetch) {
          if(q.limitWithTies)written="FETCH FIRST … WITH TIES";
          else if(q.offset && q.limit)written="OFFSET … ROWS FETCH NEXT";
          else written=q.limit?"FETCH NEXT":"OFFSET … ROWS";
        }
        change(Change::Pagination,q.origin,std::move(written),pagination(target));
      }
      if (q.limitWithTies) {
        const auto literal = [&](ExprId id) {
          const auto value = exactSignedIntegerLiteral(q, id, false);
          return value && *value >= 0;
        };
        const bool compatibleKeys = std::all_of(q.orderBy.begin(), q.orderBy.end(), [&](const auto &order) {
          const auto &key = q.expressions.at(order.expression);
          const auto id = key.outputColumn ? q.projections.at(*key.outputColumn).expression : order.expression;
          if (exactSignedIntegerLiteral(q, id, false)) return true;
          const auto &type = model.types.get(q.expressions.at(id).sqlType);
          return type.family == TypeFamily::Integer && type.bits && !type.arrayDimensions &&
                 type.unsignedInteger == false && type.opaqueName.empty();
        });
        if ((target != Dialect::PostgreSQL && target != Dialect::Oracle && target != Dialect::SQLServer) ||
            q.orderBy.empty() || !compatibleKeys || !literal(q.limit) || (q.offset && !literal(q.offset)) ||
            (target == Dialect::SQLServer && (q.offset || q.setOperation)))
          issue(errors, q.origin, "WITH TIES requires matching ordered integer keys, literal bounds and target pagination");
      }
      if (std::any_of(q.tables.begin(), q.tables.end(), [](const auto &table) { return table.jsonTable != nullptr; }))
        issue(errors, q.origin, "JSON_TABLE path and conversion behavior require a native target implementation");
      if (source == Dialect::PostgreSQL && q.recursiveWith) {
        // Lowering assigns query IDs to WITH definitions in dependency order.
        // The target receives that order when dropping the source RECURSIVE syntax.
        std::stable_sort(q.ctes.begin(), q.ctes.end(),
                         [](const auto &a, const auto &b) { return a.query < b.query; });
      }
      if(q.recursiveWith)
        change(Change::CteSyntax,q.origin,"WITH RECURSIVE","WITH");
      q.recursiveWith = false;
      if (target != Dialect::PostgreSQL && target != Dialect::SQLServer && target != Dialect::Oracle &&
          std::any_of(q.relations.begin(), q.relations.end(), [](const auto &r) { return r.lateral; }))
        issue(errors, q.origin, "lateral scope and null extension are not mapped for this target dialect");
      if (q.valuesQuery) {
        if (portableValueTables.count(q.id)) {
          const auto before = q.sqlServerValueConstructor?"T-SQL table value constructor":"VALUES query";
          q.sqlServerValueConstructor = target == Dialect::SQLServer;
          change(Change::DerivedValues,q.origin,before,
                 q.sqlServerValueConstructor?"T-SQL table value constructor":"VALUES query");
        } else
          issue(errors, q.origin, "VALUES query conversion requires compatible column names and coercion");
      }
      if (source == Dialect::PostgreSQL) for (auto &cte : q.ctes) {
        if (cte.columns.empty()) continue;
        if (!cte.definition || !cte.definition->arityKnown || !cte.definition->complete) {
          issue(errors, q.origin, "target requires one alias per CTE column; all source output names must be known");
          continue;
        }
        if (cte.columns.size() == cte.definition->columns.size()) continue;
        std::set<std::string> names;
        std::vector<std::string> expanded;
        for (const auto &column : cte.definition->columns) {
          if (!names.insert(asciiUpper(column.name)).second) {
            issue(errors, q.origin, "cannot expand a partial CTE column list with potentially ambiguous names");
            break;
          }
          expanded.push_back(column.name);
        }
        if (expanded.size() == cte.definition->columns.size()) {
          change(Change::CteSyntax,q.origin,joinNames(cte.columns,", "),joinNames(expanded,", "));
          cte.columns = std::move(expanded);
        }
      }
      for (const auto &table : q.tables) {
        if (table.columnAliases.empty()) continue;
        if (!allowsDerivedColumnAliases(target))
          issue(errors, q.origin, "target does not support the derived column alias list");
        else if (target != Dialect::PostgreSQL &&
                 (!table.definition || !table.definition->arityKnown ||
                  table.definition->columns.size() != table.columnAliases.size()))
          issue(errors, q.origin, "target requires one derived alias per column; source has a partial alias list");
      }
      if (q.tableQuery == TableQuerySyntax::Only || q.tableQuery == TableQuerySyntax::IncludeDescendants)
        issue(errors, q.origin, "TABLE inheritance modifiers require an equivalent target inheritance model");
      if ((q.targetReference && q.targetReference->inheritance != TableInheritance::Default) ||
          std::any_of(q.tables.begin(), q.tables.end(),
                      [](const auto &table) { return table.inheritance != TableInheritance::Default; }))
        issue(errors, q.origin, "explicit relation inheritance requires matching parent/child scan semantics");
      if (std::any_of(q.tables.begin(),q.tables.end(),[](const auto &table) { return table.sampling.has_value(); }))
        issue(errors,q.origin,"TABLESAMPLE requires equivalent native sampling and seed semantics");
      if (std::any_of(q.tables.begin(),q.tables.end(),[](const auto &table) { return table.rowMatch.has_value(); }))
        issue(errors,q.origin,"MATCH_RECOGNIZE requires equivalent target row pattern semantics");
      if (std::any_of(q.tables.begin(),q.tables.end(),[](const auto &table) {
            return table.postgresFunction!=PostgresRowFunction::None;
          }))
        issue(errors,q.origin,"PostgreSQL set-returning function requires equivalent target row-source semantics");
      if(q.tableQuery==TableQuerySyntax::Table)
        change(Change::TableShorthand,q.origin,"TABLE relation","SELECT * FROM relation");
      q.tableQuery = TableQuerySyntax::None;
    }
    if (!family && q.kind == StatementKind::Call)
      issue(errors, q.origin,
            "CALL/EXEC procedure calling conventions need signature/context "
            "mapping");
    if (!family && q.conflict.action != ConflictClause::Action::None)
      issue(errors, q.origin,
            "upsert conflict targets and unique-key behavior have no automatic "
            "equivalent");
    if (source != target && q.replaceExisting)
      issue(errors, q.origin, "REPLACE requires explicit mapping of unique keys, delete triggers and insert defaults");
    if (source != target && q.transaction)
      issue(errors, q.origin, "transaction controls require explicit session, DDL and savepoint policy for the target");
    if (source != target && q.prepared)
      issue(errors, q.origin, "prepared statements require a target session and parameter-binding policy");
    if (source != target && q.role)
      issue(errors, q.origin, "MySQL role controls require a target account and privilege mapping");
    if (!mysqlFamily(target) &&
        (q.fixedJoinOrder || std::any_of(q.relations.begin(), q.relations.end(),
                                        [](const auto &relation) { return relation.fixedOrder; })))
      issue(errors, q.origin, "fixed join order has no equivalent directive in the target dialect");
    if (source != target && !q.conflict.rowAlias.empty())
      issue(errors, q.origin, "MySQL incoming UPSERT row aliases require target conflict and assignment mapping");
    if (!family && (!q.returning.empty() || !q.returningInto.empty()))
      issue(errors, q.origin,
            "RETURNING/OUTPUT result and trigger semantics need explicit "
            "mapping");
    if (source!=target && q.outputInto)
      issue(errors,q.outputInto->origin,"OUTPUT INTO requires explicit mapping of its additional write target");
    if (!family && q.kind == StatementKind::Select && !q.setOperation) {
      if (target == Dialect::Oracle && !q.from && q.tables.empty()) {
        ModelTable dual;
        dual.name = {"SYS", "DUAL"};
        q.tables.push_back(dual);
        q.scope.tables.push_back({"DUAL", 0});
        ModelRelation leaf;
        leaf.table = 0;
        q.relations.push_back(leaf);
        q.from = RelationId(q.relations.size());
        change(Change::OneRowSource,q.origin,"SELECT","SELECT … FROM SYS.DUAL");
      } else if (source == Dialect::Oracle && target != Dialect::Oracle &&
                 q.tables.size() == 1 &&
                 q.tables[0].name == std::vector<std::string>{"SYS", "DUAL"}) {
        const bool references =
            std::any_of(q.expressions.begin(), q.expressions.end(),
                        [](const auto &e) {
                          return (e.kind == ExprKind::Name && !e.localSymbol) ||
                                 (e.kind == ExprKind::Star && !e.name.empty());
                        }) ||
            std::any_of(
                q.projections.begin(), q.projections.end(), [&](const auto &p) {
                  return q.expressions.at(p.expression).kind == ExprKind::Star;
                });
        if (!references) {
          q.tables.clear();
          q.relations.clear();
          q.from = 0;
          q.scope.tables.clear();
          change(Change::OneRowSource,q.origin,"FROM SYS.DUAL","SELECT without FROM");
        }
      }
    }
    if (!family && (mysqlFamily(source) || source == Dialect::SQLite) &&
        (target == Dialect::PostgreSQL || target == Dialect::Oracle ||
         target == Dialect::SQLServer)) {
      auto condition = [&](ExprId id) {
        if (!id)
          return;
        const auto &e = q.expressions.at(id);
        if (e.type != ValueType::Boolean && e.type != ValueType::Null)
          issue(errors, e.origin,
                "truth-value coercion requires an explicit predicate rewrite "
                "for the target dialect");
      };
      condition(q.where);
      condition(q.having);
      condition(q.conflict.where);
      condition(q.conflict.targetWhere);
      for (const auto &r : q.relations)
        condition(r.condition);
      if (q.ddl) {
        const auto &d = *q.ddl;
        condition(d.predicate);
        auto constraint = [&](const auto &k) { condition(k.check); };
        auto column = [&](const auto &col) {
          for (const auto &k : col.constraints)
            constraint(k);
        };
        for (const auto &col : d.definitions)
          column(col);
        for (const auto &k : d.constraints)
          constraint(k);
        for (const auto &a : d.actions) {
          if (a.column)
            column(*a.column);
          if (a.constraint)
            constraint(*a.constraint);
        }
      }
      for (const auto &e : q.expressions) {
        if ((e.kind == ExprKind::Binary &&
             (e.value == "AND" || e.value == "OR")) ||
            (e.kind == ExprKind::Unary && e.value == "NOT"))
          for (auto id : e.operands)
            condition(id);
        if (e.kind == ExprKind::Case && e.value == "searched")
          for (std::size_t i = 0; i + 1 < e.operands.size(); i += 2)
            condition(e.operands[i]);
        if (e.kind == ExprKind::AggregateFilter && e.operands.size() == 2)
          condition(e.operands[1]);
      }
    }
    if (q.target)
      mappedName(q.target->name, q.origin);
    if (q.targetReference)
      mappedName(q.targetReference->name, q.origin);
    for (auto &t : q.tables)
      mappedName(t.name, q.origin);
    const bool countFilterTarget=source==Dialect::PostgreSQL &&
        (target==Dialect::MySQL || target==Dialect::SQLServer);
    std::set<ExprId> windowBases;
    if(countFilterTarget)for(const auto& value:q.expressions)
      if(value.kind==ExprKind::Window && !value.operands.empty())
        windowBases.insert(value.operands.front());
    std::vector<ModelExpression> appended;
    for (auto &e : q.expressions) {
      if (source!=target && e.kind==ExprKind::LikeEscape) {
        issue(errors,e.origin,"LIKE/ILIKE ESCAPE requires an explicit target escape and collation policy");
        continue;
      }
      if (source!=target && e.kind==ExprKind::Binary &&
          (e.value=="GLOB" || e.value=="NOT GLOB")) {
        issue(errors,e.origin,"SQLite GLOB wildcard and case-sensitive matching requires an explicit target mapping");
        continue;
      }
      if (source!=target && e.kind==ExprKind::Binary &&
          (e.value=="REGEXP" || e.value=="NOT REGEXP")) {
        issue(errors,e.origin,"regular-expression engines and collations require an explicit target mapping");
        continue;
      }
      if (source != target && e.kind == ExprKind::TruthTest) {
        const auto type = q.expressions.at(e.operands.front()).type;
        if ((target != Dialect::PostgreSQL && !mysqlFamily(target)) ||
            (!family && type != ValueType::Boolean && type != ValueType::Null))
          issue(errors, e.origin, "IS TRUE/FALSE/UNKNOWN requires a compatible Boolean operand and target truth semantics");
      }
      if (source != target && e.kind == ExprKind::Between &&
          (e.value == "BETWEEN SYMMETRIC" || e.value == "NOT BETWEEN SYMMETRIC")) {
        const auto &value = q.expressions.at(e.operands[0]);
        const auto low = exactSignedIntegerLiteral(q, e.operands[1], false);
        const auto high = exactSignedIntegerLiteral(q, e.operands[2], false);
        const bool simple = value.kind == ExprKind::Null ||
                            (value.kind == ExprKind::Name && value.binding && value.type == ValueType::Integer) ||
                            exactSignedIntegerLiteral(q, e.operands[0], false).has_value();
        if (low && high && simple) {
          const auto before=e.value+" "+std::to_string(*low)+" AND "+std::to_string(*high);
          if (*low > *high) std::swap(e.operands[1], e.operands[2]);
          e.value.resize(e.value.size() - std::string_view(" SYMMETRIC").size());
          change(Change::SymmetricBounds,e.origin,before,e.value+" "+
                 std::to_string(std::min(*low,*high))+" AND "+std::to_string(std::max(*low,*high)));
        } else {
          issue(errors, e.origin, "BETWEEN SYMMETRIC needs exact integer bounds to preserve comparison and evaluation");
        }
      }
      if(source!=target && (e.kind==ExprKind::Interval || e.kind==ExprKind::Extract)) {
        issue(errors,e.origin,"interval / EXTRACT conversion requires an explicit calendar, field, precision and result-type mapping");continue;
      }
      if(e.kind==ExprKind::TypedLiteral && source!=target) {
        const auto &t=e.castType;
        // Only an explicit Gregorian ISO DATE is portable without session
        // DateStyle/NLS/time-zone/precision or custom input-function policy.
        auto portableDate=[](const std::string &s) {
          if(s.size()!=10 || s[4]!='-' || s[7]!='-')return false;
          for(std::size_t i=0;i<s.size();++i)if(i!=4 && i!=7 && (s[i]<'0' || s[i]>'9'))return false;
          const int y=std::stoi(s.substr(0,4)),m=std::stoi(s.substr(5,2)),d=std::stoi(s.substr(8,2));
          if(y<1000 || m<1 || m>12 || d<1)return false;
          const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};
          return d<=days[m-1]+(m==2 && y%4==0 && (y%100!=0 || y%400==0));
        };
        const bool date=t && t->name.size()==1 && asciiUpper(t->name[0])=="DATE" &&
          (t->nameQuoted.empty() || !t->nameQuoted[0]) && t->arguments.empty() &&
          t->modifiers.empty() && !t->arrayDimensions && portableDate(e.value);
        if(!date || target==Dialect::SQLite) {
          issue(errors,e.origin,"typed literal requires an explicit target input/type/time-zone policy; only Gregorian DATE literals (1000..9999) have a proven mapping");
        } else if(target==Dialect::SQLServer) {
          if(q.expressions.size()+appended.size()>=options.language.limits.nodes) {
            issue(errors,e.origin,"target expression node limit exceeded");continue;
          }
          ModelExpression string;string.kind=ExprKind::String;string.type=ValueType::String;
          string.origin=e.origin;string.value=e.value.substr(0,4)+e.value.substr(5,2)+e.value.substr(8,2);
          change(Change::DateLiteral,e.origin,"DATE '"+e.value+"'","CAST('"+string.value+"' AS DATE)");
          const auto id=static_cast<ExprId>(q.expressions.size()+appended.size());
          appended.push_back(std::move(string));
          e.kind=ExprKind::Cast;e.value.clear();e.name={"DATE"};e.nameQuoted={false};e.operands={id};
        }
        continue;
      }
      if (e.kind == ExprKind::Name && !e.localSymbol) {
        if (e.name.size() > 2)
          mappedName(e.name, e.origin);
      }
      if (e.window)
        preserveNullOrdering(e.window->ordering, e.origin);
      if (source!=target && e.withinGroup) {
        issue(errors,e.origin,"ordered-set aggregate requires an explicit target mapping");
        continue;
      }
      if (source!=target && e.jsonConstructor) {
        issue(errors,e.origin,"JSON constructor requires an explicit target null policy, key syntax and result-type mapping");
        continue;
      }
      if(e.window)preserveExclusion(*e.window,e.origin);
      if (source!=target && e.kind==ExprKind::Convert) {
        issue(errors,e.origin,"SQL Server CONVERT/TRY conversion requires an explicit style, failure/null and target-type mapping");
        continue;
      }
      if(e.kind==ExprKind::Cast && source!=target) {
        const auto before=e.castType?typeSpelling(*e.castType):joinNames(e.name);
        convertCast(model,q,e,source,target,errors);
        change(Change::Type,e.origin,before,e.castType?typeSpelling(*e.castType):joinNames(e.name));
        continue;
      }
      if (family) {
        if (source != target && e.kind == ExprKind::Call && !e.routineResolved) {
          const auto builtin = builtinOperator(source, e.name, e.nameQuoted, e.operands.size());
          if (builtin && !builtinOperator(target, e.name, e.nameQuoted, e.operands.size()))
            issue(errors, e.origin, builtin->name + " has no target builtin in this dialect");
        }
        continue;
      }
      auto numeric = [](ValueType t) {
        return t == ValueType::Integer || t == ValueType::Decimal;
      };
      auto temporal=[&](ExprId id) {
        const auto family=model.types.get(q.expressions.at(id).sqlType).family;
        return family==TypeFamily::Date || family==TypeFamily::Time || family==TypeFamily::Timestamp || family==TypeFamily::Interval;
      };
      if(e.kind==ExprKind::Binary && (e.value=="+" || e.value=="-" || e.value=="*" || e.value=="/" || e.value=="%"))
        for(auto id:e.operands)if(temporal(id)) {
          issue(errors,e.origin,"temporal arithmetic requires an explicit interval/calendar and result-type mapping");break;
        }
      auto mixed = [&](const std::vector<ExprId> &ids) {
        ValueType previous = ValueType::Unknown;
        const bool anyTemporal=std::any_of(ids.begin(),ids.end(),temporal);
        if(anyTemporal)for(auto id:ids) {
          const auto &expression=q.expressions.at(id);
          if(expression.kind!=ExprKind::Null && !temporal(id))return true;
        }
        for (auto id : ids) {
          const auto type = q.expressions.at(id).type;
          if (type == ValueType::Unknown || type == ValueType::Null)
            continue;
          if (previous != ValueType::Unknown && type != previous &&
              !(numeric(type) && numeric(previous)))
            return true;
          previous = type;
        }
        return false;
      };
      if (e.kind == ExprKind::Binary && e.operands.size() == 2) {
        if (e.value == "+" || e.value == "-" || e.value == "*")
          for (auto id : e.operands) {
            const auto type = q.expressions.at(id).type;
            if (type != ValueType::Unknown && type != ValueType::Null &&
                !numeric(type)) {
              issue(errors, e.origin,
                    "arithmetic/string/boolean implicit conversions differ "
                    "between dialects; use an explicit typed rewrite");
              break;
            }
          }
        else if ((e.value == "=" || e.value == "<>" || e.value == "<" ||
                  e.value == ">" || e.value == "<=" || e.value == ">=") &&
                 mixed(e.operands))
          issue(errors, e.origin,
                "mixed-type comparison requires an explicit coercion/collation "
                "policy");
      }
      if (e.kind == ExprKind::Call && e.name.size() == 1 &&
          (asciiUpper(e.name[0]) == "COALESCE" ||
           asciiUpper(e.name[0]) == "NULLIF") &&
          mixed(e.operands))
        issue(errors, e.origin,
              "mixed-type function arguments require an explicit "
              "result/coercion policy");
      if (e.kind == ExprKind::Case) {
        std::vector<ExprId> values;
        for (std::size_t i = e.value == "simple" ? 2 : 1; i < e.operands.size();
             i += 2)
          values.push_back(e.operands[i]);
        if (!e.operands.empty())
          values.push_back(e.operands.back());
        if (mixed(values))
          issue(errors, e.origin,
                "mixed-type CASE results require an explicit result/coercion "
                "policy");
      }
      if (e.kind == ExprKind::Variable ||
          (e.kind == ExprKind::Parameter && !e.localSymbol))
        issue(errors, e.origin,
              "session variables/bind parameters require an explicit target "
              "binding policy");
      if (e.unresolvedBooleanName)
        issue(errors, e.origin, "SQLite TRUE/FALSE needs column metadata to distinguish a name from a literal");
      if (e.kind == ExprKind::Binary &&
          (e.value == "/" || e.value == "%" || e.value == "DIV" || e.value == "XOR" || e.value == "ILIKE" ||
           e.value == "NULL_SAFE_EQUAL" || e.value == "NOT ILIKE" ||
           e.value == "IS DISTINCT FROM" || e.value == "IS NOT DISTINCT FROM" ||
           e.value == "CONCAT"))
        issue(errors, e.origin,
              "operator " + e.value +
                  " has precision/null/collation semantics requiring an "
                  "explicit rewrite");
      if (e.kind == ExprKind::String && e.value.empty() &&
          (source == Dialect::Oracle || target == Dialect::Oracle))
        issue(errors, e.origin,
              "Oracle empty string is NULL; an explicit string/null policy is "
              "required");
      if (e.kind == ExprKind::Call) {
        auto n = e.name.empty() ? "" : asciiUpper(e.name.back());
        if((n=="SUM" || n=="AVG") && !e.routineResolved && e.operands.size()==1) {
          const auto &input=model.types.get(q.expressions.at(e.operands[0]).sqlType);
          const auto from=resolveAggregateType(source,n,input),to=resolveAggregateType(target,n,input);
          const auto known=[](const SqlType &type){return (type.family==TypeFamily::Integer && type.bits) || (type.family==TypeFamily::Floating && type.bits) || type.family==TypeFamily::Decimal;};
          if(!to.error.empty() || (known(from.type) && known(to.type) &&
             (from.type.family!=to.type.family || from.type.bits!=to.type.bits || from.type.unsignedInteger!=to.type.unsignedInteger ||
              from.type.precision!=to.type.precision || from.type.scale!=to.type.scale ||
              (from.type.family==TypeFamily::Decimal && (!from.type.precision || !from.type.scale || !to.type.precision || !to.type.scale)))))
            issue(errors,e.origin,"aggregate result domains / overflow or fractional behavior differ in this target; an explicit typed aggregate mapping is required");
        }
        if ((n == "CHAR_LENGTH" || n == "CHARACTER_LENGTH") &&
            (target == Dialect::Oracle || target == Dialect::SQLServer))
          issue(errors, e.origin,
                "character length functions differ for empty strings/trailing "
                "spaces in this target");
        if(e.routineResolved && portableCalls.count(n) && e.routineSchema!="pg_catalog")
          issue(errors,e.origin,"resolved source routine cannot be assumed equivalent to a target builtin with the same name");
        if (e.name.size() != 1 || !portableCalls.count(n))
          issue(errors, e.origin,
                "function " + n +
                    " has no proven cross-dialect mapping (evaluation, types "
                    "or volatility may differ)");
        else {
          e.name = {n};
          e.nameQuoted = {false};
        }
      }
      if (e.kind==ExprKind::AggregateFilter &&
          target!=Dialect::PostgreSQL && target!=Dialect::SQLite) {
        const auto id=static_cast<ExprId>(&e-q.expressions.data());
        if(!countFilterTarget || e.operands.size()!=2 ||
           !portableCountPredicate(model,q,e.operands[1]) ||
           windowBases.count(id)) {
          issue(errors,e.origin,"aggregate FILTER needs a verified target rewrite for this expression and dialect");
          continue;
        }
        auto call=q.expressions.at(e.operands.front());
        const auto native=builtinOperator(source,call.name,call.nameQuoted,call.operands.size());
        const bool resolved=!call.routineResolved || (call.routineSchema=="pg_catalog" &&
            call.routineKind==RoutineKind::Aggregate && call.routineVolatility==FunctionVolatility::Immutable);
        if(call.kind!=ExprKind::Call || !native || native->name!="COUNT" || !resolved ||
           call.distinct || call.operands.size()!=1) {
          issue(errors,e.origin,"only native COUNT without DISTINCT has an implemented FILTER-to-CASE rewrite");
          continue;
        }
        auto argument=call.operands.front();
        const auto &original=q.expressions.at(argument);
        const bool star=original.kind==ExprKind::Star && original.name.empty();
        if(!star && !portableCountOperand(q,model.types.get(original.sqlType),original) &&
           !exactSignedIntegerLiteral(q,argument,false)) {
          issue(errors,e.origin,"COUNT FILTER needs a portable scalar argument");
          continue;
        }
        if(q.expressions.size()+appended.size()+3>options.language.limits.nodes) {
          issue(errors,e.origin,"target expression node limit exceeded");
          continue;
        }
        const auto put=[&](ModelExpression value) {
          const auto next=static_cast<ExprId>(q.expressions.size()+appended.size());
          appended.push_back(std::move(value));
          return next;
        };
        if(star) {
          ModelExpression one;
          one.kind=ExprKind::Number;one.type=ValueType::Integer;one.value="1";one.origin=original.origin;
          argument=put(std::move(one));
        }
        ModelExpression null;
        null.kind=ExprKind::Null;null.type=ValueType::Null;null.origin=e.origin;
        ModelExpression conditional;
        conditional.kind=ExprKind::Case;conditional.value="searched";conditional.origin=e.origin;
        conditional.operands={e.operands[1],argument,put(std::move(null))};
        call.origin=e.origin;
        call.name={target==Dialect::SQLServer?"COUNT_BIG":"COUNT"};
        call.nameQuoted={false};
        call.operands={put(std::move(conditional))};
        change(Change::AggregateFilter,e.origin,"COUNT(…) FILTER (WHERE …)",
               call.name.front()+"(CASE WHEN … THEN … ELSE NULL END)");
        e=std::move(call);
      }
      if (e.kind == ExprKind::Boolean && target != Dialect::PostgreSQL &&
          target != Dialect::SQLite)
        issue(errors, e.origin,
              "target SQL boolean representation needs context-aware mapping");
    }
    q.expressions.insert(q.expressions.end(),std::make_move_iterator(appended.begin()),std::make_move_iterator(appended.end()));
    if (!family) {
      auto aliases = [&](ExprId root, bool inspectNested) {
        std::vector<ExprId> work;
        if (root)
          work.push_back(root);
        std::set<ExprId> seen;
        while (!work.empty()) {
          const auto id = work.back();
          work.pop_back();
          if (!id || id >= q.expressions.size() || !seen.insert(id).second)
            continue;
          auto &e = q.expressions[id];
          if (e.outputColumn && *e.outputColumn < q.projections.size()) {
            const auto ref = q.projections[*e.outputColumn].expression;
            if (ref == id) {
              issue(errors, e.origin, "cyclic output alias reference");
              continue;
            }
            const auto origin = e.origin;
            e = q.expressions.at(ref);
            e.origin = origin;
          } else if (e.kind == ExprKind::Name && !e.binding && !e.localSymbol &&
                     e.name.size() == 1 &&
                     std::any_of(q.projections.begin(), q.projections.end(),
                                 [&](const auto &p) {
                                   return !p.alias.empty() &&
                                          p.alias == e.name[0];
                                 }))
            issue(errors, e.origin,
                  "column/output-alias precedence needs complete source "
                  "metadata before conversion");
          if (inspectNested)
            work.insert(work.end(), e.operands.begin(), e.operands.end());
        }
      };
      const bool targetGroupAlias = mysqlFamily(target) ||
                                    target == Dialect::PostgreSQL ||
                                    target == Dialect::SQLite;
      const bool targetHavingAlias =
          mysqlFamily(target) || target == Dialect::SQLite;
      if (!targetGroupAlias)
        for (auto id : q.groupBy) {
          const auto &e = q.expressions[id];
          if ((mysqlFamily(source) || source == Dialect::PostgreSQL ||
               source == Dialect::SQLite) &&
              e.kind == ExprKind::Number && !e.outputColumn)
            issue(errors, e.origin,
                  "GROUP BY position needs a known expanded SELECT shape "
                  "before conversion");
          aliases(id, true);
        }
      if (!targetHavingAlias)
        aliases(q.having, true);
    }
    preserveNullOrdering(q.orderBy, q.origin);
    for (auto &window : q.namedWindows) {
      preserveNullOrdering(window.specification.ordering, window.origin);
      preserveExclusion(window.specification,window.origin);
    }
    if (q.ddl) {
      auto &d = *q.ddl;
      if (source != target) {
        if (d.object == ObjectKind::TableGroup)
          issue(errors, q.origin, "OceanBase TABLEGROUP requires an explicit target mapping");
        for (const auto &option : d.distribution)
          issue(errors, option.origin, std::string(distributionOptionName(option.kind)) +
                                        " requires an explicit target mapping");
        if (d.indexScope != IndexScope::Unspecified)
          issue(errors, q.origin, "OceanBase index scope requires an explicit target mapping");
        const auto checkConstraint = [&](const auto &constraint) {
          if (constraint.clustering != PrimaryKeyClustering::Unspecified)
            issue(errors, constraint.origin, "TiDB primary key clustering requires an explicit target mapping");
        };
        const auto checkColumn = [&](const auto &column) {
          if (column.autoRandom)
            issue(errors, column.autoRandom->origin, "TiDB AUTO_RANDOM requires an explicit target mapping");
          for (const auto &constraint : column.constraints) checkConstraint(constraint);
        };
        for (const auto &column : d.definitions) checkColumn(column);
        for (const auto &constraint : d.constraints) checkConstraint(constraint);
        for (const auto &action : d.actions) {
          if (action.column) checkColumn(*action.column);
          if (action.constraint) checkConstraint(*action.constraint);
        }
      }
      if (d.partition && source != target)
        issue(errors, d.partition->origin, "physical partition routing requires explicit target partition mapping");
      mappedName(d.name,q.origin);
      mappedName(d.indexedTable,q.origin);
      mappedName(d.likeTable,q.origin);
      for (auto &n : d.names)
        mappedName(n,q.origin);
      for (auto &r : d.renames) {
        mappedName(r.first,q.origin);
        mappedName(r.second,q.origin);
      }
      for (auto &t : d.truncateTargets)
        mappedName(t.name,q.origin);
      if (!family) {
        const bool sourceCommit =
            mysqlFamily(source) || source == Dialect::Oracle;
        const bool targetCommit =
            mysqlFamily(target) || target == Dialect::Oracle;
        if (sourceCommit != targetCommit && !options.allowDdlTransactionChange)
          issue(errors, q.origin,
                "DDL transaction behavior changes (implicit commit vs "
                "transactional DDL); explicit review is required");
        if (sourceCommit != targetCommit)
          change(Change::DdlCommit,q.origin,sourceCommit?"Implicit commit":"Transactional DDL",
                 targetCommit?"Implicit commit":"Transactional DDL");
        if (d.object == ObjectKind::MaterializedView &&
            target != Dialect::PostgreSQL && target != Dialect::Oracle)
          issue(errors, q.origin,
                "target has no equivalent native materialized view; refusing "
                "to downgrade it to an ordinary view/table");
        if (d.object == ObjectKind::Sequence)
          issue(errors, q.origin,
                "sequence bounds/cache/gap/ownership defaults need an explicit "
                "migration policy");
        if (d.object == ObjectKind::Database || d.object == ObjectKind::Schema)
          issue(errors, q.origin,
                "database/schema ownership and namespace semantics require an "
                "explicit migration plan");
        if (!d.engine.empty() || !d.characterSet.empty() ||
            !d.collation.empty() || !d.tablespace.empty() ||
            !d.onCommit.empty() || d.tableComment || d.temporary ||
            d.unlogged || !d.likeTable.empty())
          issue(errors, q.origin,
                "physical/charset/collation/temporary-table options require "
                "explicit target mappings");
        auto constraint = [&](auto &k) {
          mappedName(k.referencedTable,k.origin);
          if (k.deferrable || k.initiallyDeferred)
            if (target != Dialect::PostgreSQL && target != Dialect::Oracle)
              issue(errors, k.origin,
                    "target cannot preserve deferrable constraints");
        };
        auto column = [&](auto &col) {
          mappedType(col.type);
          if (col.identity != IdentityKind::None)
            issue(errors, col.origin,
                  "identity/auto-increment explicit-value and zero/sequence "
                  "semantics need an explicit policy");
          if (col.comment || !col.collation.empty() ||
              !col.characterSet.empty() || col.generated)
            issue(errors, col.origin,
                  "column comments/collation/generated-column semantics "
                  "require explicit mapping");
          for (auto &k : col.constraints)
            constraint(k);
        };
        for (auto &col : d.definitions)
          column(col);
        for (auto &k : d.constraints)
          constraint(k);
        for (auto &a : d.actions) {
          if (a.column)
            column(*a.column);
          if (a.type)
            mappedType(*a.type);
          if (a.constraint)
            constraint(*a.constraint);
        }
        if (q.kind == StatementKind::Truncate)
          issue(errors, q.origin,
                "TRUNCATE identity, foreign-key and transaction effects "
                "require explicit target policy");
        if (q.kind == StatementKind::Alter)
          for (auto &a : d.actions)
            if (a.kind == AlterKind::ModifyColumn ||
                a.kind == AlterKind::ChangeColumn)
              issue(errors, a.origin,
                    "MySQL full column replacement cannot be converted without "
                    "the previous table definition");
        if (d.withNoData && target != Dialect::PostgreSQL)
          issue(errors, q.origin,
                "WITH NO DATA has no equivalent unreadable/unpopulated state "
                "in this target");
      }
    }
  }
  if (!errors.empty())
    return false;
  model.sourceDialect = target;
  return true;
}
bool lowerTarget(ModelProgram &model, Dialect target, const ConversionOptions &options,
                 std::vector<Diagnostic> &errors) {
  return lowerTarget(model,target,options,errors,{});
}
bool lowerTypeTarget(BasicTypeSpec<std::vector<std::string>> &type,
                     Dialect source, Dialect target,
                     std::vector<Diagnostic> &errors) {
  const auto count = errors.size();
  mapType(type, source, target, errors);
  return errors.size() == count;
}
ConversionResult convertSql(Dialect source, Dialect target, std::string text,
                            ConversionOptions options) {
  ConversionResult result;
  result.source = source;
  result.target = target;
  auto targetLanguage = options.language;
  targetLanguage.extension = SqlExtension::None;
  try {
    auto normalized = normalizeConversionScript(source, text, options.language,
                                                result.diagnostics);
    if (!result.diagnostics.empty())
      return result;
    Compilation tokens(source, normalized, options.language, "<conversion>");
    lex(tokens, true, true);
    if (!tokens.diagnostics.empty()) {
      result.diagnostics = tokens.diagnostics;
      return result;
    }
    std::vector<Token> stream;
    for (auto t : tokens.tokens)
      if (t.kind != TokenKind::Comment)
        stream.push_back(t);
    ConversionCatalog catalog(options.catalog);
    ScriptContext context(source, &catalog, options.names);
    std::size_t cursor = 0, count = 0;
    std::ostringstream report;
    report << "{\"source\":\"" << dialectName(source) << "\",\"target\":\""
           << dialectName(target) << "\",\"statements\":[";
    bool comma = false;
    std::string generated;
    bool sqlServerBatchClosed=false,insertedSqlServerBatch=false;
    while (cursor < stream.size() && stream[cursor].kind != TokenKind::End) {
      if (statementSeparator(stream[cursor])) {
        ++cursor;
        continue;
      }
      if (++count > options.language.limits.statements) {
        issue(result.diagnostics, stream[cursor].range,
              "conversion statement limit exceeded");
        break;
      }
      auto begin = stream[cursor].range.begin;
      std::size_t end = begin;
      std::string sql, model;
      bool needsSqlServerBatch=false,closesSqlServerBatch=false;
      if (isRoutineStart(source, stream, cursor)) {
        auto routine =
            compileRoutinePrefix(source, normalized.substr(begin),
                                 options.language, &context, options.names);
        if (!routine.ok()) {
          result.diagnostics = routine.diagnostics;
          shift(result.diagnostics, begin);
          break;
        }
        end = begin + routine.ast.origin.end;
        if (auto error = rememberRoutine(context, routine.ast)) {
          result.diagnostics.push_back(*error);
          shift(result.diagnostics, begin);
          break;
        }
        model = dumpRoutineModel(routine, options.generation.maxBytes,
                                 options.language.shouldStop);
        auto emitted = emitRoutine(routine.model, target, options);
        if (!emitted.ok()) {
          result.diagnostics = emitted.diagnostics;
          shift(result.diagnostics, begin);
          break;
        }
        sql = emitted.sql;
        auto validated = compileRoutine(target, sql, targetLanguage);
        if (!validated.ok()) {
          result.diagnostics = validated.diagnostics;
          for (auto &d : result.diagnostics) {
            d.range = {begin, end};
            d.message = "target routine validation failed: " + d.message;
          }
          break;
        }
        if(source!=target) {
          const ConversionChanges change{&result,count,begin,end};
          const auto &from=routine.ast, &to=validated.ast;
          change(ConversionChangeKind::RoutineBody, from.origin,
                 dialectName(source),dialectName(target));
          change(ConversionChangeKind::Qualifier, from.origin,joinNames(from.name),joinNames(to.name));
          if(from.function && to.function)
            change(ConversionChangeKind::Type,from.result.origin,
                   typeSpelling(from.result),typeSpelling(to.result));
          for(std::size_t i=0;i<std::min(from.parameters.size(),to.parameters.size());++i) {
            const auto &input=from.symbols.at(from.parameters[i]);
            const auto &output=to.symbols.at(to.parameters[i]);
            change(ConversionChangeKind::Type,input.type.origin,
                   typeSpelling(input.type),typeSpelling(output.type));
          }
        }
        if (mysqlFamily(target)) {
          sql = mysqlClientDefinition(std::move(sql));
        } else if (target == Dialect::SQLServer) {
          sql += "\nGO";
          needsSqlServerBatch=true;
          closesSqlServerBatch=true;
        } else if (target == Dialect::Oracle)
          sql += "\n/";
      } else {
        const auto next = statementEndIndex(source, stream, cursor);
        end = next < stream.size() ? stream[next].range.end : normalized.size();
        auto c = compile(source, normalized.substr(begin, end - begin),
                         options.language, &context, options.names);
        if (!c->ok()) {
          result.diagnostics = c->diagnostics;
          shift(result.diagnostics, begin);
          break;
        }
        for (auto root : c->model.roots)
          if (auto error = context.observe(c->model, root))
            result.diagnostics.push_back(*error);
        if (!result.diagnostics.empty()) {
          shift(result.diagnostics, begin);
          break;
        }
        model = dumpModel(*c);
        auto program = c->model;
        const ConversionChanges change{&result,count,begin,end};
        if (!lowerTarget(program, target, options, result.diagnostics,change)) {
          shift(result.diagnostics, begin);
          break;
        }
        auto emitted = generateSql(program, target, options.generation);
        if (!emitted.ok()) {
          result.diagnostics = emitted.diagnostics;
          shift(result.diagnostics, begin);
          break;
        }
        auto verified = compile(target, emitted.sql, targetLanguage);
        if (!verified->ok()) {
          result.diagnostics = verified->diagnostics;
          for (auto &d : result.diagnostics) {
            d.range = {begin, end};
            d.message = "target grammar validation failed: " + d.message;
          }
          break;
        }
        sql = emitted.sql;
        if (target==Dialect::SQLServer && program.roots.size()==1) {
          const auto &root=program.queries.at(program.roots.front()-1);
          needsSqlServerBatch=root.ddl && sqlServerDefinitionRequiresBatch(root.kind,root.ddl->object);
        }
        if(mysqlFamily(target) && program.roots.size()==1) {
          const auto &trigger=program.queries.at(program.roots.front()-1).trigger;
          if(trigger && trigger->compound)sql=mysqlClientDefinition(std::move(sql));
        }
        if(target==Dialect::Oracle && program.roots.size()==1) {
          const auto &package=program.queries.at(program.roots.front()-1).package;
          if(package && !package->drop)sql+="\n/";
        }
      }
      if (end <= begin) {
        issue(result.diagnostics, {begin, begin},
              "conversion made no progress");
        break;
      }
      while (cursor < stream.size() && stream[cursor].range.begin < end)
        ++cursor;
      if (needsSqlServerBatch && !closesSqlServerBatch) {
        auto next=cursor;
        while (next<stream.size() && statementSeparator(stream[next])) ++next;
        if (next<stream.size() && stream[next].kind!=TokenKind::End) {
          sql+="\nGO";
          closesSqlServerBatch=true;
          insertedSqlServerBatch=true;
        }
      }
      if (comma)
        report << ',';
      comma = true;
      if (model.size() > options.generation.maxBytes ||
          static_cast<std::size_t>(report.tellp()) >
              options.generation.maxBytes - model.size()) {
        issue(result.diagnostics, {begin, end},
              "conversion report byte limit exceeded");
        break;
      }
      report << "{\"sourceRange\":{\"begin\":" << begin << ",\"end\":" << end
             << "},\"model\":" << model << "}";
      const std::string separator=generated.empty()?"":
          needsSqlServerBatch && !sqlServerBatchClosed?"\nGO\n\n":"\n\n";
      if (separator=="\nGO\n\n") insertedSqlServerBatch=true;
      if (generated.size()>options.generation.maxBytes ||
          separator.size()>options.generation.maxBytes-generated.size() ||
          sql.size()>options.generation.maxBytes-generated.size()-separator.size()) {
        issue(result.diagnostics, {begin, end},
              "conversion output byte limit exceeded");
        break;
      }
      generated+=separator;
      generated += sql;
      sqlServerBatchClosed=closesSqlServerBatch;
    }
    report << "]}";
    if (!count && result.diagnostics.empty())
      issue(result.diagnostics, {}, "no SQL statements to convert");
    if (result.diagnostics.empty()) {
      std::stable_sort(result.changes.begin(), result.changes.end(),
        [](const auto& left,const auto& right){return left.origin.begin<right.origin.begin;});
      result.changes.erase(std::unique(result.changes.begin(),result.changes.end(),
          [](const auto& left,const auto& right){
            return left.statement==right.statement && left.kind==right.kind &&
                   left.origin.begin==right.origin.begin && left.origin.end==right.origin.end &&
                   left.before==right.before && left.after==right.after;
          }),result.changes.end());
      result.sql = std::move(generated);
      result.modelJson = report.str();
      result.targetValidated = true;
      if (insertedSqlServerBatch)
        result.notes.push_back("SQL Server definition batches use GO client separators; execute with a batch-aware client.");
      if (options.allowDdlTransactionChange)
        result.notes.push_back(
            "DDL transaction behavior differences were explicitly allowed; "
            "review implicit commits before execution.");
      result.notes.push_back(
          "Object existence is not required for conversion. Source metadata "
          "is optional enrichment; unresolved shapes/types can still prevent "
          "rewrites that require proof. No objects have been created.");
      result.notes.push_back(
          "Target grammar validated offline. Server versions, catalog "
          "identities, collation, privileges and runtime data still require "
          "target-side review. SQL has not been executed.");
    }
  } catch (const CompilationAborted &) {
    result.diagnostics.push_back(
        {DiagnosticCode::Stopped,
         {},
         "conversion stopped by cancellation or resource limits"});
  } catch (const std::exception &e) {
    result.diagnostics.push_back(
        {DiagnosticCode::InvalidQueryShape, {}, e.what()});
  }
  if (!result.diagnostics.empty()) {
    result.sql.clear();
    result.modelJson.clear();
    result.changes.clear();
    result.changesTruncated=false;
    result.targetValidated = false;
  }
  return result;
}
} // namespace navidog::sql::compiler
