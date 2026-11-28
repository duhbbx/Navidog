#include "navidog/sql/compiler/completion.hpp"
#include "conversion_internal.hpp"
#include "navidog/sql/compiler/conversion.hpp"
#include "navidog/sql/compiler/script_context.hpp"
#include "parser.hpp"
#include "sqlserver_keywords.hpp"
#include "navidog/sql/compiler/operator_catalog.hpp"
#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
namespace navidog::sql::compiler {
namespace {
const std::string marker = "__nvd_completion__";
bool mysql(Dialect d) {
  return isMySqlDialect(d);
}
bool wordByte(unsigned char c) {
  return c >= 128 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '$' || c == '#';
}
std::string fold(std::string s) {
  for (auto &c : s)
    if (c >= 'a' && c <= 'z')
      c -= 32;
  return s;
}
bool fuzzyIdentifier(CompletionKind kind) {
  return kind == CompletionKind::Column || kind == CompletionKind::Table ||
         kind == CompletionKind::View || kind == CompletionKind::MaterializedView;
}
int fuzzyPenalty(std::string candidate, std::string needle) {
  candidate.erase(std::remove(candidate.begin(), candidate.end(), '_'), candidate.end());
  needle.erase(std::remove(needle.begin(), needle.end(), '_'), needle.end());
  if (needle.empty() || needle.size() > candidate.size())
    return -1;
  if (candidate.rfind(needle, 0) == 0)
    return 25;
  if (candidate.find(needle) != std::string::npos)
    return 40;
  std::size_t offset = 0;
  for (std::size_t begin = 0; begin < needle.size();) {
    auto end = begin + 1;
    while (end < needle.size() && (static_cast<unsigned char>(needle[end]) & 0xc0) == 0x80)
      ++end;
    const auto character = std::string_view(needle).substr(begin, end - begin);
    offset = candidate.find(character, offset);
    if (offset == std::string::npos)
      return -1;
    offset += character.size();
    begin = end;
  }
  return 60;
}
std::optional<int> nameMatch(std::string_view label,const std::string &wanted,bool fuzzy) {
  const auto display=fold(std::string(label));
  const auto last=display.find_last_of('.');
  const auto name=std::string_view(display).substr(last==std::string::npos?0:last+1);
  if (name==wanted) return 20;
  if (wanted.empty() || name.rfind(wanted,0)==0 || display.rfind(wanted,0)==0) return 0;
  if (fuzzy) {
    const auto penalty=fuzzyPenalty(std::string(name),wanted);
    if (penalty>=0) return -penalty;
  }
  return {};
}
bool betterCandidate(const CompletionItem &a, const CompletionItem &b) {
  if (a.priority != b.priority)
    return a.priority > b.priority;
  const auto first = fold(a.label), second = fold(b.label);
  return first != second ? first < second : a.detail < b.detail;
}
std::string quote(Dialect d, const std::string &name) {
  const char a = mysql(d)                  ? '`'
                 : d == Dialect::SQLServer ? '['
                                           : '"',
             b = d == Dialect::SQLServer ? ']' : a;
  std::string s(1, a);
  for (char c : name) {
    s += c;
    if (c == b)
      s += c;
  }
  return s + b;
}
bool ident(const Token &t) {
  return t.kind == TokenKind::Word || t.kind == TokenKind::Identifier;
}
const char *valueTypeName(ValueType t) {
  switch (t) {
  case ValueType::Integer:
    return "integer";
  case ValueType::Decimal:
    return "decimal";
  case ValueType::String:
    return "string";
  case ValueType::Boolean:
    return "boolean";
  case ValueType::Null:
    return "null";
  case ValueType::Binary:
    return "binary";
  default:
    return "type unknown";
  }
}
struct VisibleRelation {
  std::string alias;
  ModelTable table;
  bool incomingRow = false;
};
class Completer {
  Dialect dialect;
  std::string source, code, prefix, missingBodyDelimiter;
  std::size_t cursor, base = 0;
  const CatalogSnapshot *catalog;
  NameResolutionContext names;
  CompletionOptions options;
  CompletionResult result;
  std::vector<Token> tokens, before;
  std::vector<std::size_t> clientSeparators;
  std::vector<std::string> qualifier;
  std::vector<VisibleRelation> visible;
  std::vector<std::string> ctes;
  std::unique_ptr<Compilation> parsed;
  std::unique_ptr<ScriptContext> declarations;
  std::unique_ptr<ScriptContext> alterActions;
  RoutineCompilation routine;
  RoutineCompletionSite routineSite;
  bool afterRoutineEnd = false;
  const NameResolutionContext *routineQueryContext = nullptr;
  const ModelStatement *cursorQuery = nullptr;
  const ModelStatement *usingQuery = nullptr;
  const ModelRelation *usingJoin = nullptr;
  bool quotedPrefix = false, parameterPrefix = false, routineHeader = false,castTypeSlot=false;
  bool jsonDocument = false, sqlServerOutput = false;
  std::size_t unpivotInputClosings = 0;
  enum class PivotSite { None, Aggregate, Argument, For, Axis, In };
  PivotSite pivotSite=PivotSite::None;
  std::size_t pivotAxisWidth=1;
  std::optional<std::size_t> windowOpening;
  enum class WindowReference { None, Direct, Copy };
  WindowReference windowReference=WindowReference::None;
  enum class CasePhase { Subject, Condition, Result, Fallback };
  struct CaseFrame {
    std::size_t depth, anchor;
    CasePhase phase=CasePhase::Subject;
    bool pendingBetween=false;
  };
  std::vector<CaseFrame> caseFrames;
  std::vector<const ModelProgram *> programs;
  std::set<std::string> dedup;
  enum class Expect {
    Statement,
    QueryStart,
    Expression,
    Relation,
    Procedure,
    Type,
    CreateObject,
    Constraint, WriteColumn, ReferenceColumn, ColumnList, JoinColumn, OutputColumn,
    CatalogObject, ObjectDeclaration, InspectionKeyword, FixedKeywords, VariableTarget, SelectContinuation, CursorName,
    ScalarVariable
  };
  Expect expect = Expect::Expression;
  enum class AliasClause { None, OrderBy, GroupBy };
  AliasClause aliasClause = AliasClause::None;
  std::vector<std::string> inspectionWords, keywordChoices;
  CatalogObjectKind objectKind=CatalogObjectKind::Table;
  std::vector<std::string> columnListTarget, listColumns;
  const char *columnListDetail="column of the indexed table";
  std::set<std::string> assignedColumns;

  void stop() {
    if (options.language.shouldStop && options.language.shouldStop())
      throw CompilationAborted{};
  }
  std::size_t skipHorizontal(std::size_t at) const {
    while (at < source.size() && (source[at] == ' ' || source[at] == '\t'))
      ++at;
    return at;
  }
  std::string canonical(const Token &t) const {
    return canonicalIdentifier(dialect, t.value,
                               t.kind == TokenKind::Identifier);
  }
  std::string key(const std::string &n) const {
    return mysql(dialect) || dialect == Dialect::SQLServer ? fold(n) : n;
  }
  std::string spelling(const std::string &n, bool force = false) const {
    static const std::set<std::string> reserved = {
        "SELECT",   "FROM",       "WHERE",   "ORDER",   "GROUP",  "BY",
        "TABLE",    "VIEW",       "INDEX",   "CREATE",  "DROP",   "ALTER",
        "FUNCTION", "PROCEDURE",  "TRIGGER", "USER",    "ROLE",   "KEY",
        "PRIMARY",  "REFERENCES", "CHECK",   "DEFAULT", "NULL",   "AS",
        "JOIN",     "LEFT",       "RIGHT",   "INNER",   "OUTER",  "CROSS",
        "VALUES",   "SET",        "INSERT",  "UPDATE",  "DELETE", "BEGIN",
        "END",      "WHEN",       "CASE",    "THEN",    "ELSE",   "LIMIT",
        "OFFSET",   "TOP",        "WITH",    "UNION",   "ALL",    "IN",
        "ON",       "AND",        "OR",      "NOT",     "RETURN", "RETURNING",
        "OUTPUT"};
    bool simple = !n.empty() && !(n[0] >= '0' && n[0] <= '9');
    for (unsigned char c : n)
      if (!wordByte(c))
        simple = false;
    const bool nativeReserved=(dialect==Dialect::SQLServer && sqlServerReservedWord(fold(n))) ||
        (dialect==Dialect::TiDB && (fold(n)=="SSL" || fold(n)=="REQUIRE"));
    return force || !simple || nativeReserved || reserved.count(fold(n)) ||
                   canonicalIdentifier(dialect, n, false) != n
               ? quote(dialect, n)
               : n;
  }
  void add(std::string label, std::string insert, std::string detail,
           CompletionKind kind, int priority,
           std::size_t caret = std::string::npos, std::size_t selection = 0,
           SourceRange replacement = {std::size_t(-1), std::size_t(-1)},
           std::string_view filterText = {}) {
    stop();
    auto wanted = fold(prefix);
    if (parameterPrefix && !wanted.empty() && wanted[0] == '@')
      wanted.erase(0, 1);
    const auto match=nameMatch(filterText.empty()?std::string_view(label):filterText,wanted,
                               fuzzyIdentifier(kind) && !parameterPrefix);
    if (!match) return;
    priority+=*match;
    if (!dedup
             .insert(std::to_string(int(kind)) + ":" + insert +
                     ((kind == CompletionKind::Function ||
                       kind == CompletionKind::Procedure || kind==CompletionKind::Index || kind==CompletionKind::Trigger)
                          ? ":" + detail
                          : ""))
             .second)
      return;
    if (result.items.size() >= options.maxItems * 8 + 64) {
      // A long list of earlier fuzzy matches must not crowd out a later
      // exact match. Keep the best candidates without growing the buffer.
      std::stable_sort(result.items.begin(), result.items.end(), betterCandidate);
      result.items.resize(options.maxItems);
      result.truncated = true;
    }
    if (replacement.begin == std::size_t(-1))
      replacement = result.replacement;
    if (caret == std::string::npos)
      caret = insert.size();
    result.items.push_back({std::move(label), std::move(insert),
                            std::move(detail), kind, replacement, caret,
                            selection, priority});
  }
  bool opaqueAtCursor(const Token &t) const {
    if (t.kind != TokenKind::String && t.kind != TokenKind::Comment)
      return false;
    if (t.range.begin < cursor && cursor < t.range.end)
      return true;
    return t.kind == TokenKind::Comment && t.range.begin < cursor &&
           cursor == t.range.end && source.compare(t.range.begin, 2, "/*") != 0;
  }
  static bool wordAt(const std::vector<Token> &list,std::size_t i,std::string_view spelling) {
    return i<list.size() && list[i].kind==TokenKind::Word && fold(list[i].value)==spelling;
  }
  static bool isSymbol(const Token &token,std::string_view spelling) {
    return token.kind==TokenKind::Symbol && token.value==spelling;
  }
  std::optional<std::size_t> triggerKeyword(const std::vector<Token> &list,std::size_t start) const {
    if ((dialect!=Dialect::SQLite && (!mysql(dialect) || dialect==Dialect::TiDB)) ||
        !wordAt(list,start,"CREATE")) return {};
    auto i=start+1;
    if (dialect==Dialect::SQLite && (wordAt(list,i,"TEMP") || wordAt(list,i,"TEMPORARY"))) ++i;
    if (mysql(dialect) && wordAt(list,i,"DEFINER")) {
      // The ordinary parser validates the account specification. Here we
      // only locate the trigger introducer inside its bounded header.
      for (++i;i<list.size() && i-start<=12 && !isSymbol(list[i],";");++i)
        if (wordAt(list,i,"TRIGGER")) return i;
      return {};
    }
    if (wordAt(list,i,"TRIGGER")) return i;
    return {};
  }
  std::size_t triggerTerminator(const std::vector<Token> &list,std::size_t at) {
    std::size_t blocks=0,cases=0;
    for (;at<list.size();++at) {
      if ((at&63u)==0) stop();
      const auto &token=list[at];
      if (token.kind==TokenKind::End || (isSymbol(token,";") && !blocks)) break;
      if (wordAt(list,at,"BEGIN")) ++blocks;
      else if (wordAt(list,at,"CASE")) ++cases;
      else if (wordAt(list,at,"END")) {
        if (wordAt(list,at+1,"CASE")) {
          if (cases) --cases;
          ++at;
        } else if (wordAt(list,at+1,"IF") || wordAt(list,at+1,"LOOP") ||
                   wordAt(list,at+1,"WHILE") || wordAt(list,at+1,"REPEAT")) {
          ++at;
        } else if (cases) --cases;
        else if (blocks) --blocks;
      }
    }
    return at;
  }
  void scriptPrelude() {
    // Keep byte coordinates unchanged: hide complete neighbouring statements
    // in this private analysis copy, and retain their declarations in an
    // operation-local catalog. Nothing here edits or executes the editor SQL.
    std::vector<Diagnostic> errors;
    std::vector<std::size_t> separators;
    auto normalized = normalizeConversionScript(dialect, source, options.language,
                                                 errors, &separators);
    if (!errors.empty())
      return; // Incomplete quotes use the recovery lexer below.
    Compilation lexed(dialect, normalized, options.language,
                      "<completion script>");
    lex(lexed, true, true);
    if (!lexed.diagnostics.empty())
      return;
    std::vector<Token> significant;
    for (const auto &t : lexed.tokens)
      if (t.kind != TokenKind::Comment)
        significant.push_back(t);
    auto context = std::make_unique<ScriptContext>(dialect, catalog, names);
    std::size_t keepBegin = 0, keepEnd = normalized.size(), count = 0;
    bool inherited = false;
    for (std::size_t i = 0;
         i < significant.size() && significant[i].kind != TokenKind::End;) {
      stop();
      if (significant[i].range.begin >= cursor)
        break;
      if (significant[i].value == ";") {
        keepBegin = significant[i++].range.end;
        continue;
      }
      if (++count >
          std::min<std::size_t>(options.language.limits.statements, 256))
        throw CompilationAborted{};
      const auto begin = significant[i].range.begin;
      const bool isRoutine = isRoutineStart(dialect, significant, i);
      std::size_t end = begin;
      if (isRoutine) {
        auto previous = parseRoutineForCompletion(dialect, normalized.substr(begin), options.language);
        if (previous.ast.origin.end == 0)
          break;
        end = begin + previous.ast.origin.end;
        if (end > cursor) {
          keepEnd = end;
          break;
        }
        if (previous.ok())
          previous = compileRoutinePrefix(dialect, normalized.substr(begin),
                                          options.language, context.get(), names);
        if (previous.ok()) {
          RoutineDefinition def;
          def.procedure = !previous.ast.function;
          def.minArguments = def.maxArguments = previous.ast.parameters.size();
          if(previous.ast.function)def.sqlType=std::make_shared<const SqlType>(resolveDeclaredType(previous.ast.result,dialect,TypeUse::Signature).type);
          def.resultType = previous.ast.result.arrayDimensions
                               ? ValueType::Unknown
                               : typeCategory(previous.ast.result.name, dialect);
          for (auto id : previous.ast.parameters)
            if (previous.ast.symbols.at(id).initializer.end >
                previous.ast.symbols.at(id).initializer.begin)
              --def.minArguments;
          if (context->declareRoutine(previous.ast.name, std::move(def),
                                      {begin, end}, previous.ast.ifNotExists))
            break;
        }
      } else {
        const auto trigger=triggerKeyword(significant,i);
        auto next=trigger?triggerTerminator(significant,*trigger+1):i;
        while (next < significant.size() &&
               significant[next].kind != TokenKind::End &&
               significant[next].value != ";")
          ++next;
        end = next < significant.size() ? significant[next].range.end
                                        : normalized.size();
        if (end > cursor || next >= significant.size() ||
            significant[next].kind == TokenKind::End) {
          keepEnd = end;
          break;
        }
        auto previous = compile(dialect, normalized.substr(begin, end - begin),
                                options.language, context.get(), names);
        if (previous->ok()) {
          for (auto root : previous->model.roots)
            if (context->observe(previous->model, root)) {
              context->invalidate();
              break;
            }
        } else {
          // An unrecognized command may change schemas/session state. Do not
          // expose stale catalog suggestions past that uncertainty.
          context->invalidate();
        }
      }
      inherited = true;
      keepBegin = end;
      while (i < significant.size() && significant[i].range.begin < end)
        ++i;
    }
    if (keepBegin > cursor || keepEnd < cursor)
      return;
    auto mask = [&](std::size_t begin, std::size_t end) {
      for (auto i = begin; i < end; ++i)
        if (normalized[i] != '\n' && normalized[i] != '\r')
          normalized[i] = ' ';
    };
    mask(0, keepBegin);
    mask(keepEnd, normalized.size());
    clientSeparators = std::move(separators);
    source = std::move(normalized);
    if (inherited) {
      declarations = std::move(context);
      catalog = declarations.get();
    }
  }
  bool scan() {
    if (source.size() > options.language.limits.sourceBytes) {
      result.diagnostics.push_back({DiagnosticCode::ResourceLimit,
                                    {},
                                    "completion source limit exceeded"});
      return false;
    }
    if (cursor > source.size() ||
        (cursor < source.size() &&
         (static_cast<unsigned char>(source[cursor]) & 0xc0) == 0x80)) {
      result.diagnostics.push_back(
          {DiagnosticCode::InvalidEncoding,
           {cursor, cursor},
           "completion cursor must be on a UTF-8 boundary"});
      return false;
    }
    scriptPrelude();
    Compilation outer(dialect, source, options.language, "<completion>");
    lex(outer, true, true);
    std::vector<Token> significant;
    for (const auto &t : outer.tokens)
      if (t.kind != TokenKind::Comment && t.kind != TokenKind::End)
        significant.push_back(t);
    routineHeader = dialect != Dialect::TiDB && isRoutineStart(dialect, significant);
    code = source;
    if (dialect == Dialect::PostgreSQL && routineHeader) {
      // An unfinished dollar body is lexically opaque to the outer SQL
      // parser. Only recover a dollar quote immediately after the routine AS,
      // and leave ordinary strings/comments inside the body opaque.
      for (const auto &diagnostic : outer.diagnostics) {
        const auto start = diagnostic.range.begin;
        if (diagnostic.message != "unterminated dollar string" || start >= cursor)
          continue;
        const auto beforeBody = std::lower_bound(significant.begin(), significant.end(), start,
            [](const Token &token, std::size_t at) { return token.range.begin < at; });
        if (beforeBody == significant.begin() || std::prev(beforeBody)->kind != TokenKind::Word ||
            fold(std::prev(beforeBody)->value) != "AS")
          continue;
        const auto tagEnd = source.find('$', start + 1);
        if (tagEnd == std::string::npos || tagEnd >= cursor)
          continue;
        missingBodyDelimiter = source.substr(start, tagEnd - start + 1);
        base = tagEnd + 1;
        code = source.substr(base);
        break;
      }
    }
    for (const auto &t : outer.tokens)
      if (opaqueAtCursor(t)) {
        if (dialect == Dialect::PostgreSQL && routineHeader &&
            t.kind == TokenKind::String && source[t.range.begin] == '$') {
          auto tagEnd = source.find('$', t.range.begin + 1);
          if (tagEnd == std::string::npos)
            break;
          const auto delimiter =
              source.substr(t.range.begin, tagEnd - t.range.begin + 1);
          base = tagEnd + 1;
          const auto end = t.range.end - delimiter.size();
          if (cursor < base || cursor > end) {
            result.suppressed = true;
            return false;
          }
          code = source.substr(base, end - base);
          break;
        }
        result.suppressed = true;
        return false;
      }
    Compilation lexed(dialect, code, options.language, "<completion code>");
    lex(lexed, true, true);
    for (auto t : lexed.tokens) {
      t.range.begin += base;
      t.range.end += base;
      tokens.push_back(std::move(t));
    }
    result.replacement = {cursor, cursor};
    for (const auto &t : tokens) {
      if (t.range.begin > cursor || t.range.end < cursor ||
          (t.range.begin == cursor && t.kind != TokenKind::Identifier))
        continue;
      if (opaqueAtCursor(t) ||
          (t.kind == TokenKind::Number && t.range.begin < cursor)) {
        result.suppressed = true;
        return false;
      }
      if (ident(t) || t.kind == TokenKind::Parameter ||
          t.kind == TokenKind::Variable) {
        result.replacement = t.range;
        quotedPrefix = t.kind == TokenKind::Identifier;
        parameterPrefix = t.kind == TokenKind::Parameter;
        break;
      }
    }
    // An unfinished identifier quote is intentionally recoverable; strings and
    // comments remain opaque, including their unfinished forms.
    for (const auto &d : lexed.diagnostics) {
      const auto at = base + d.range.begin;
      if (at >= cursor)
        continue;
      const auto ch = source[at];
      const bool identifierQuote =
          (ch == '`' && (mysql(dialect) || dialect == Dialect::SQLite)) ||
          (ch == '"' &&
           (!mysql(dialect) || options.language.mysqlAnsiQuotes)) ||
          (ch == '[' &&
           (dialect == Dialect::SQLServer || dialect == Dialect::SQLite));
      if (identifierQuote) {
        quotedPrefix = true;
        result.replacement = {at, base + code.size()};
        break;
      }
      if (dialect == Dialect::PostgreSQL && routineHeader && base && ch == '$' &&
          at + 1 == cursor && d.message != "unterminated dollar string")
        continue;
      if (ch == '\'' || ch == '"' || ch == '$' ||
          (at + 1 < source.size() && source[at + 1] == '\'' &&
           (ch == 'q' || ch == 'Q' || ch == 'n' || ch == 'N' || ch == 'e' ||
            ch == 'E')) ||
          source.compare(at, 2, "/*") == 0 ||
          source.compare(at, 2, "--") == 0 || ch == '#') {
        result.suppressed = true;
        return false;
      }
      if (d.code == DiagnosticCode::InvalidEncoding) {
        result.diagnostics.push_back(d);
        return false;
      }
    }
    if (result.replacement.begin == cursor) {
      auto begin = cursor;
      while (begin > base &&
             wordByte(static_cast<unsigned char>(source[begin - 1])))
        --begin;
      auto end = cursor;
      while (end < base + code.size() &&
             wordByte(static_cast<unsigned char>(source[end])))
        ++end;
      if (dialect == Dialect::SQLServer && begin > base && source[begin - 1] == '@') {
        --begin;
        parameterPrefix = true;
      }
      result.replacement = {begin, end};
    }
    prefix = source.substr(result.replacement.begin,
                           cursor - result.replacement.begin);
    if (quotedPrefix && !prefix.empty()) {
      const char close = prefix[0] == '[' ? ']' : prefix[0];
      prefix.erase(0, 1);
      std::string unescaped;
      for (std::size_t i = 0; i < prefix.size(); ++i) {
        unescaped += prefix[i];
        if (prefix[i] == close && i + 1 < prefix.size() &&
            prefix[i + 1] == close)
          ++i;
      }
      prefix = std::move(unescaped);
      if (!prefix.empty() && prefix.back() == close)
        prefix.pop_back();
    }
    for (const auto &t : tokens)
      if (t.kind != TokenKind::Comment && t.kind != TokenKind::End &&
          t.range.end <= result.replacement.begin)
        before.push_back(t);
    if (!before.empty() && before.back().value == ".") {
      auto i = before.size() - 1;
      while (i > 0 && ident(before[i - 1])) {
        qualifier.insert(qualifier.begin(), canonical(before[i - 1]));
        if (i < 2 || before[i - 2].value != ".")
          break;
        i -= 2;
      }
    }
    return true;
  }
  std::optional<std::size_t> openColumnList() const {
    if (parameterPrefix || !qualifier.empty() || before.empty()) return {};
    auto i = before.size();
    while (i >= 2 && before[i - 1].kind == TokenKind::Symbol &&
           before[i - 1].value == "," && ident(before[i - 2]))
      i -= 2;
    if (i && before[i - 1].kind == TokenKind::Symbol && before[i - 1].value == "(")
      return i - 1;
    return {};
  }
  bool joinColumnSlot() const {
    const auto open = openColumnList();
    return open && *open && before[*open - 1].kind == TokenKind::Word &&
           fold(before[*open - 1].value) == "USING";
  }
  bool unpivotNullOptions() const {
    return dialect==Dialect::Oracle || (dialect==Dialect::PostgreSQL && options.language.calciteExtensions);
  }
  bool unpivotOpenAt(std::size_t open) const {
    return open && isSymbol(before[open],"(") &&
        (wordAt(before,open-1,"UNPIVOT") || (unpivotNullOptions() && open>=3 &&
         wordAt(before,open-3,"UNPIVOT") &&
         (wordAt(before,open-2,"INCLUDE") || wordAt(before,open-2,"EXCLUDE")) &&
         wordAt(before,open-1,"NULLS")));
  }
  std::optional<std::size_t> unpivotNameList(std::size_t end) const {
    if (!end) return {};
    auto i=end-1;
    if (ident(before[i])) return i;
    if (!unpivotNullOptions() || !isSymbol(before[i],")")) return {};
    while (i>1 && ident(before[i-1])) {
      i-=2;
      if (isSymbol(before[i],"(")) return i;
      if (!isSymbol(before[i],",")) break;
    }
    return {};
  }
  bool unpivotHasFor(std::size_t forToken) const {
    const auto measures=unpivotNameList(forToken);
    return measures && *measures && unpivotOpenAt(*measures-1);
  }
  bool unpivotHasIn(std::size_t inToken) const {
    const auto labels=unpivotNameList(inToken);
    return labels && *labels && wordAt(before,*labels-1,"FOR") && unpivotHasFor(*labels-1);
  }
  std::size_t unpivotInputDepth() {
    if ((!unpivotNullOptions() && dialect!=Dialect::SQLServer) || before.size()<6 ||
        parameterPrefix || !qualifier.empty() ||
        (!isSymbol(before.back(),"(") && !isSymbol(before.back(),","))) return 0;
    const auto open=openColumnList();
    if (!open || !*open) return 0;
    if (wordAt(before,*open-1,"IN") && unpivotHasIn(*open-1)) return 2;
    if (!unpivotNullOptions() || wordAt(before,*open-1,"AS")) return 0;
    const auto outer=openingParenthesis(*open);
    return outer && *outer && wordAt(before,*outer-1,"IN") && unpivotHasIn(*outer-1)?3:0;
  }
  PivotSite pivotCompletionSite() {
    const bool literalBuckets=unpivotNullOptions();
    if ((!literalBuckets && dialect!=Dialect::SQLServer) || parameterPrefix || !qualifier.empty()) return PivotSite::None;
    const auto nearest=openColumnList();
    if (!nearest || !*nearest) return PivotSite::None;
    const auto pivotOpen=[&](auto opening) { return opening && wordAt(before,opening-1,"PIVOT"); };
    const auto end=before.size();
    if (dialect==Dialect::PostgreSQL && wordAt(before,*nearest-1,"FOR")) {
      const auto outer=openingParenthesis(*nearest);
      if (outer && pivotOpen(*outer)) {
        auto slot=*nearest+1;
        for (;slot+1<end && ident(before[slot]) && isSymbol(before[slot+1],",");slot+=2) {
          stop();
          ++pivotAxisWidth;
        }
        if (slot==end) return PivotSite::Axis;
      }
    }
    if (*nearest+1==end && *nearest>=3 && pivotOpen(*nearest-2) &&
        before[*nearest-1].kind==TokenKind::Word) return PivotSite::Argument;
    if (!pivotOpen(*nearest)) return PivotSite::None;
    auto at=*nearest+1;
    if (at==end) return PivotSite::Aggregate;
    if (at+1>=end || before[at].kind!=TokenKind::Word || !isSymbol(before[at+1],"(")) return PivotSite::None;
    at+=2;
    for (;at<end && !isSymbol(before[at],")");++at) {
      if ((at&63u)==0) stop();
      if (!ident(before[at]) && !isSymbol(before[at],".") && !isSymbol(before[at],"*")) return PivotSite::None;
    }
    if (at==end) return PivotSite::None;
    ++at;
    if (literalBuckets && at<end && !wordAt(before,at,"FOR")) {
      if (wordAt(before,at,"AS")) ++at;
      if (at==end || !ident(before[at])) return PivotSite::None;
      ++at;
    }
    if (at==end) return PivotSite::For;
    if (!wordAt(before,at++,"FOR")) return PivotSite::None;
    if (at==end) return PivotSite::Axis;
    if (dialect==Dialect::PostgreSQL && unpivotNameList(end)==at)
      return PivotSite::In;
    return at+1==end && ident(before[at])?PivotSite::In:PivotSite::None;
  }
  std::string pivotCompletionSuffix() {
    if (pivotSite!=PivotSite::Argument && pivotSite!=PivotSite::Axis) return {};
    const auto tail=std::lower_bound(tokens.begin(),tokens.end(),result.replacement.end,
        [](const Token &token,std::size_t at) { return token.range.begin<at; });
    const auto next=std::find_if(tail,tokens.end(),[](const auto &token) { return token.kind!=TokenKind::Comment; });
    if (next!=tokens.end() && next->kind!=TokenKind::End && !isSymbol(*next,";")) return {};
    std::string result=pivotSite==PivotSite::Argument?") FOR nvd_pivot_axis":"";
    if (dialect==Dialect::PostgreSQL) {
      const auto opening=openColumnList();
      if (pivotSite==PivotSite::Axis && opening && *opening && wordAt(before,*opening-1,"FOR"))
        result+=")";
      result+=" IN (";
      if (pivotAxisWidth>1) result+="(";
      for (std::size_t i=0;i<pivotAxisWidth;++i) result+=i?", NULL":"NULL";
      if (pivotAxisWidth>1) result+=")";
      result+=" AS nvd_bucket)) nvd_pivot_result";
    } else {
      result+=unpivotNullOptions()?" IN ('nvd' AS nvd_bucket)) nvd_pivot_result":
                                      " IN ([nvd_bucket])) AS nvd_pivot_result";
    }
    return result;
  }
  bool namedWindowDefinition(std::size_t opening) {
    if ((dialect!=Dialect::PostgreSQL && dialect!=Dialect::SQLite && !mysql(dialect)) ||
        opening<3 || !wordAt(before,opening-1,"AS") || !ident(before[opening-2])) return false;
    auto name=opening-2;
    while (name && !wordAt(before,name-1,"WINDOW")) {
      stop();
      if (name<3 || !isSymbol(before[name-1],",") || !isSymbol(before[name-2],")")) return false;
      const auto preceding=openingParenthesis(name-2);
      if (!preceding || *preceding<3 || !wordAt(before,*preceding-1,"AS") || !ident(before[*preceding-2]))
        return false;
      name=*preceding-2;
    }
    return name && wordAt(before,name-1,"WINDOW");
  }
  std::optional<std::size_t> activeWindowOpening() {
    auto opening=openColumnList();
    while (opening && *opening) {
      if (wordAt(before,*opening-1,"OVER") || namedWindowDefinition(*opening)) return opening;
      opening=openingParenthesis(*opening);
    }
    return {};
  }
  WindowReference windowReferenceAtCursor() const {
    if (parameterPrefix || !qualifier.empty() || before.size()<2) return WindowReference::None;
    const auto end=before.size();
    if (wordAt(before,end-1,"OVER") && isSymbol(before[end-2],")")) return WindowReference::Direct;
    if (windowOpening && *windowOpening+1==end && *windowOpening && wordAt(before,*windowOpening-1,"OVER"))
      return WindowReference::Copy;
    return WindowReference::None;
  }
  std::string expressionSuffix() {
    const bool pg=dialect==Dialect::PostgreSQL;
    auto object=1u;
    if (wordAt(before,0,"CREATE"))
      while (wordAt(before,object,"TEMP") || wordAt(before,object,"TEMPORARY") ||
             wordAt(before,object,"UNLOGGED") || wordAt(before,object,"GLOBAL") ||
             wordAt(before,object,"LOCAL")) ++object;
    const bool tableDdl=(wordAt(before,0,"CREATE") || wordAt(before,0,"ALTER")) &&
                        wordAt(before,object,"TABLE");
    if ((!pg && !tableDdl && !windowOpening) || source.find_first_of("[(",base)>=result.replacement.begin) return {};
    std::vector<std::size_t> openings;
    for (std::size_t i=0;i<before.size();++i) {
      if ((i&63u)==0) stop();
      const auto &token=before[i];
      if (token.kind!=TokenKind::Symbol) continue;
      if (token.value=="(" || token.value=="[") {
        if (openings.size()>=options.language.limits.nesting) return {};
        openings.push_back(i);
      }
      else if (token.value==")" || token.value=="]") {
        if (openings.empty() || before[openings.back()].value!=(token.value==")"?"(":"[")) return {};
        openings.pop_back();
      }
    }
    std::string suffix;
    if (windowOpening) {
      for (auto i=openings.rbegin();i!=openings.rend();++i) {
        suffix+=before[*i].value=="["?']':')';
        if (*i==*windowOpening) return suffix;
      }
      return {};
    }
    if (std::any_of(openings.begin(),openings.end(),[&](auto i) {
          return isSymbol(before[i],"(") &&
              ((tableDdl && i && wordAt(before,i-1,"CHECK")) ||
               (pg && i>=2 && wordAt(before,i-2,"WITHIN") && wordAt(before,i-1,"GROUP")));
        })) {
      for (auto i=openings.rbegin();i!=openings.rend();++i) suffix+=before[*i].value=="["?']':')';
      return suffix;
    }
    if (!pg) return {};
    for (auto it=openings.rbegin();it!=openings.rend();++it) {
      if (before[*it].value=="[") {
        suffix+=']';
        continue;
      }
      if (*it<2 || before[*it-1].kind!=TokenKind::Word || before[*it-2].kind!=TokenKind::Symbol) break;
      const auto quantifier=fold(before[*it-1].value);
      const auto &op=before[*it-2].value;
      if ((quantifier!="ANY" && quantifier!="SOME" && quantifier!="ALL") ||
          (op!="=" && op!="<>" && op!="!=" && op!="<" && op!=">" && op!="<=" && op!=">=")) break;
      suffix+=')';
    }
    return suffix;
  }
  void findCaseFrames() {
    std::size_t depth=0;
    for (std::size_t i=0;i<before.size();++i) {
      if ((i&63u)==0) stop();
      const auto &token=before[i];
      if (isSymbol(token,"(") || isSymbol(token,"[")) {
        ++depth;
        continue;
      }
      if (isSymbol(token,")") || isSymbol(token,"]")) {
        if (depth) --depth;
        while (!caseFrames.empty() && caseFrames.back().depth>depth) caseFrames.pop_back();
        continue;
      }
      if (isSymbol(token,";")) {
        caseFrames.clear();
        continue;
      }
      auto *active=!caseFrames.empty() && caseFrames.back().depth==depth?&caseFrames.back():nullptr;
      if (wordAt(before,i,"CASE") && i) {
        const auto &previous=before[i-1];
        static const std::set<std::string> operators{
            "(", "[", ",", "=", ":=", "+", "-", "*", "/", "||", ">", "<", ">=", "<=", "<>", "!="};
        static const std::set<std::string> expressionAfter{
            "SELECT","DISTINCT","ALL","WHEN","WHERE","AND","OR","NOT","ON","HAVING","RETURN","RETURNING","DEFAULT"};
        const bool beginsExpression=(previous.kind==TokenKind::Symbol && operators.count(previous.value)) ||
            (previous.kind==TokenKind::Word && expressionAfter.count(fold(previous.value))) ||
            (active && active->anchor==i-1 &&
             (active->phase==CasePhase::Result || active->phase==CasePhase::Fallback));
        if (beginsExpression && caseFrames.size()<options.language.limits.nesting)
          caseFrames.push_back({depth,i});
        continue;
      }
      if (!active) continue;
      if (wordAt(before,i,"END")) {
        caseFrames.pop_back();
      } else if (wordAt(before,i,"WHEN") &&
                 (active->phase==CasePhase::Subject || active->phase==CasePhase::Result)) {
        *active={depth,i,CasePhase::Condition};
      } else if (wordAt(before,i,"THEN") && active->phase==CasePhase::Condition) {
        *active={depth,i,CasePhase::Result};
      } else if (wordAt(before,i,"ELSE") && active->phase==CasePhase::Result) {
        *active={depth,i,CasePhase::Fallback};
      } else if (active->phase==CasePhase::Condition && wordAt(before,i,"BETWEEN")) {
        active->pendingBetween=true;
      } else if (wordAt(before,i,"AND")) {
        active->pendingBetween=false;
      }
    }
    if (caseFrames.empty() || caseFrames.back().depth!=depth) caseFrames.clear();
    else {
      const auto outer=std::find_if(caseFrames.begin(),caseFrames.end(),[&](const auto &item) {
        return item.depth==depth;
      });
      caseFrames.erase(caseFrames.begin(),outer);
    }
  }
  auto nextCompletionToken() const {
    const auto start=std::lower_bound(tokens.begin(),tokens.end(),result.replacement.end,
        [](const Token &token,std::size_t at) { return token.range.begin<at; });
    return std::find_if(start,tokens.end(),[](const Token &token) { return token.kind!=TokenKind::Comment; });
  }
  std::string caseRecoverySuffix() const {
    if (caseFrames.empty()) return {};
    const auto next=nextCompletionToken();
    if (next!=tokens.end() && next->kind!=TokenKind::End && !isSymbol(*next,";") &&
        !isSymbol(*next,")") && !isSymbol(*next,",") &&
        !(next->kind==TokenKind::Word && (fold(next->value)=="FROM" || fold(next->value)=="AS")))
      return {};
    std::string suffix;
    for (auto frame=caseFrames.rbegin();frame!=caseFrames.rend();++frame) {
      if (frame->phase==CasePhase::Subject) suffix+=" WHEN NULL THEN NULL";
      else if (frame->phase==CasePhase::Condition) {
        if (frame->pendingBetween) suffix+=" AND NULL";
        suffix+=" THEN NULL";
      }
      suffix+=" END";
    }
    return suffix;
  }
  bool postgresOpenSubscriptTarget() {
    std::size_t depth=0;
    bool update=false, target=false;
    for (std::size_t i=0;i<before.size();++i) {
      if ((i&63u)==0) stop();
      const auto &token=before[i];
      if (isSymbol(token,"(") || isSymbol(token,"[")) ++depth;
      else if ((isSymbol(token,")") || isSymbol(token,"]")) && depth) --depth;
      else if (!depth) {
        if (wordAt(before,i,"UPDATE")) update=true;
        else if (update && (wordAt(before,i,"SET") || isSymbol(token,","))) target=true;
        else if (isSymbol(token,"=")) target=false;
        else if (isSymbol(token,";") || wordAt(before,i,"FROM") || wordAt(before,i,"RETURNING") ||
                 wordAt(before,i,"WHERE")) update=target=false;
      }
    }
    return update && target;
  }
  std::string_view sqlServerOutputSuffix() {
    if (dialect!=Dialect::SQLServer || before.empty() ||
        (!wordAt(before,0,"WITH") && !wordAt(before,0,"INSERT") && !wordAt(before,0,"MERGE"))) return {};
    bool insert=wordAt(before,0,"INSERT"), merge=wordAt(before,0,"MERGE"), output=false;
    std::size_t depth=0;
    for (std::size_t i=1;i<before.size();++i) {
      if ((i&63u)==0) stop();
      if (isSymbol(before[i],"(")) ++depth;
      else if (isSymbol(before[i],")") && depth) --depth;
      else if (!depth) {
        if (!insert && !merge) {
          if (wordAt(before,i,"UPDATE") || wordAt(before,i,"DELETE") || wordAt(before,i,"SELECT")) return {};
          insert=wordAt(before,i,"INSERT");
          merge=wordAt(before,i,"MERGE");
        }
        if ((insert || merge) && wordAt(before,i,"OUTPUT")) output=true;
        if (output && (wordAt(before,i,"VALUES") || wordAt(before,i,"SELECT") ||
                       wordAt(before,i,"DEFAULT") || wordAt(before,i,"EXEC") ||
                       wordAt(before,i,"EXECUTE") || isSymbol(before[i],";"))) return {};
      }
    }
    if (!output || depth) return {};
    const auto next=std::find_if(tokens.begin(),tokens.end(),[&](const auto &token) {
      return token.kind!=TokenKind::Comment && token.range.begin>=result.replacement.end;
    });
    if (next==tokens.end() || next->kind==TokenKind::End) return merge?";":" DEFAULT VALUES";
    if (insert && isSymbol(*next,";")) return " DEFAULT VALUES";
    return {};
  }
  void scopes() {
    const auto localBegin = result.replacement.begin - base,
               localEnd = result.replacement.end - base;
    const bool tsqlVariable = dialect == Dialect::SQLServer && parameterPrefix &&
        !prefix.empty() && prefix.front() == '@';
    const auto replacementMarker = tsqlVariable ? "@" + marker : marker;
    const bool usingSlot = joinColumnSlot();
    unpivotInputClosings=unpivotInputDepth();
    pivotSite=pivotCompletionSite();
    windowOpening=activeWindowOpening();
    windowReference=windowReferenceAtCursor();
    findCaseFrames();
    auto replacementToken = replacementMarker;
    if (windowReference==WindowReference::Direct) replacementToken="()";
    if (windowReference==WindowReference::Copy) replacementToken.clear();
    const auto suffix=expressionSuffix();
    if (usingSlot || !suffix.empty()) {
      const bool target=dialect==Dialect::PostgreSQL && !suffix.empty() && suffix.back()==']' &&
                        postgresOpenSubscriptTarget();
      const auto next = std::find_if(tokens.begin(), tokens.end(), [&](const auto &t) {
        return t.kind != TokenKind::Comment && t.range.begin >= result.replacement.end;
      });
      // Complete an open list in the analysis copy, before a separator or comment.
      if (next == tokens.end() || next->kind == TokenKind::End ||
          (next->kind == TokenKind::Symbol && next->value == ";")) {
        if (usingSlot) replacementToken += ")";
        else {
          replacementToken+=suffix;
          if (target) replacementToken+=" = NULL";
        }
      } else if (!suffix.empty() && !usingSlot && next->kind==TokenKind::Word &&
                 (fold(next->value)=="FROM" || (target && (fold(next->value)=="WHERE" || fold(next->value)=="RETURNING")))) {
        replacementToken+=suffix;
        if (target) replacementToken+=" = NULL";
      }
    }
    if (unpivotInputClosings) {
      const auto tail=std::lower_bound(tokens.begin(),tokens.end(),result.replacement.end,
          [](const Token &token,std::size_t at) { return token.range.begin<at; });
      const auto next=std::find_if(tail,tokens.end(),[](const auto &token) { return token.kind!=TokenKind::Comment; });
      if (next==tokens.end() || next->kind==TokenKind::End || isSymbol(*next,";")) {
        replacementToken.append(unpivotInputClosings,')');
        if (dialect!=Dialect::Oracle) replacementToken+=" AS nvd_unpivot_result";
      }
    }
    replacementToken+=pivotCompletionSuffix();
    replacementToken+=sqlServerOutputSuffix();
    replacementToken+=caseRecoverySuffix();
    auto patched = source;
    patched.replace(result.replacement.begin,
                    result.replacement.end - result.replacement.begin, replacementToken);
    const char *documentSuffix = "";
    if (dialect == Dialect::MySQL && result.replacement.end == source.size()) {
      std::size_t closed = 0;
      for (std::size_t i = before.size(); i > 1; --i) {
        const auto &token = before[i-1];
        if (token.kind != TokenKind::Symbol) continue;
        if (token.value == ")") {
          ++closed;
        } else if (token.value == "(") {
          if (closed) {
            --closed;
          } else {
            if (before[i-2].kind == TokenKind::Word && fold(before[i-2].value) == "JSON_TABLE")
              documentSuffix = ", '$' COLUMNS (nvd_json_value INT PATH '$')) AS nvd_json_table";
            break;
          }
        } else if (!closed && (token.value == "," || token.value == ";")) {
          break;
        }
      }
    }
    if (routineHeader) {
      patched += documentSuffix;
      // This suffix exists only in the completion copy. Recover declarations
      // from the partial AST even if the body has no END yet.
      if (!missingBodyDelimiter.empty())
        patched += missingBodyDelimiter;
      if (mysql(dialect)) {
        routineSite.offset = result.replacement.begin;
        if (!before.empty()) {
          const auto &last = before.back();
          afterRoutineEnd = last.kind == TokenKind::Word && fold(last.value) == "END" &&
                            skipHorizontal(last.range.end) >= result.replacement.begin;
          if (afterRoutineEnd)
            routineSite.offset = last.range.begin;
        }
      }
      routine = compileRoutineForCompletion(dialect, std::move(patched), options.language,
                                            catalog, names, result.replacement.begin,
                                            mysql(dialect) ? &routineSite : nullptr);
      for (const auto &n : routine.model.nodes)
        if (!n.code.empty())
          programs.push_back(&n.code);
    }
    if (!routineHeader) {
      auto fragment = code;
      fragment.replace(localBegin, localEnd - localBegin, replacementToken);
      fragment += documentSuffix;
      parsed = compile(dialect, std::move(fragment), options.language, catalog, names);
    }
    if (parsed && !declarations && !parsed->model.empty()) {
      auto context = std::make_unique<ScriptContext>(dialect, catalog, names);
      bool used = false;
      for (auto root : parsed->model.roots) {
        const auto &q = parsed->model.queries.at(root - 1);
        if (q.origin.end > localBegin)
          continue;
        if (q.ddl || q.transaction || q.role || q.kind == StatementKind::Call) {
          if (auto error = context->observe(parsed->model, root)) {
            result.diagnostics.push_back(*error);
            break;
          }
          used = true;
        }
      }
      if (used) {
        declarations = std::move(context);
        catalog = declarations.get();
      }
    }
    const auto *ordinary = parsed ? &parsed->model : nullptr;
    if (ordinary && !ordinary->empty())
      programs.push_back(ordinary);
    const ModelProgram *program = nullptr;
    const ModelStatement *chosen = nullptr;
    const auto identifierMarker = canonicalIdentifier(dialect, marker, false);
    if (usingSlot) {
      std::size_t matches = 0;
      for (const auto *p : programs)
        for (const auto &q : p->queries)
          for (const auto &join : q.relations) {
            stop();
            const auto count = std::count(join.usingColumns.begin(), join.usingColumns.end(), identifierMarker);
            if (!count) continue;
            matches += count;
            if (matches == 1) {
              usingQuery = &q;
              usingJoin = &join;
            }
          }
      if (matches != 1) {
        usingQuery = nullptr;
        usingJoin = nullptr;
      }
    }
    if (usingJoin) {
      result.parsedScope = true;
      return;
    }
    for (const auto *p : programs)
      for (const auto &q : p->queries)
        for (const auto &e : q.expressions)
          if ((e.kind == ExprKind::Name &&
               std::find(e.name.begin(), e.name.end(), identifierMarker) !=
                   e.name.end()) ||
              (e.kind == ExprKind::Parameter && e.value == replacementMarker)) {
            program = p;
            chosen = &q;
            break;
          }
    if (!chosen)
      for (const auto *p : programs)
        for (const auto &q : p->queries) {
          const auto at = p == ordinary ? localBegin : result.replacement.begin;
          if (q.origin.begin <= at && q.origin.end >= at &&
            (!chosen || q.origin.end - q.origin.begin <
                            chosen->origin.end - chosen->origin.begin)) {
            program = p;
            chosen = &q;
          }
        }
    cursorQuery = chosen;
    std::set<std::string> shadowed;
    std::size_t ancestors = 0;
    const std::vector<std::size_t> *visibleParent = nullptr;
    if (chosen && program != ordinary)
      routineQueryContext = &chosen->context;
    const auto at = program == ordinary ? localBegin : result.replacement.begin;
    if (dialect==Dialect::SQLServer && chosen) {
      const auto covers=[&](const auto &projection) {
        const auto &range=chosen->expressions.at(projection.expression).origin;
        return range.begin<=at && at<range.end;
      };
      sqlServerOutput=std::any_of(chosen->returning.begin(),chosen->returning.end(),covers);
      if (!sqlServerOutput && chosen->outputInto) {
        const auto &projections=chosen->outputInto->projections;
        sqlServerOutput=std::any_of(projections.begin(),projections.end(),covers);
      }
    }
    std::vector<std::size_t> joinTables;
    bool joinScope = false;
    for (auto *q = chosen;
         q && ancestors++ < options.language.limits.nesting;) {
      result.parsedScope = true;
      const auto *visibleHere = visibleParent;
      if (q == chosen) {
        for (const auto &table : q->tables) {
          if (!table.jsonTable) continue;
          const auto range = q->expressions.at(table.jsonTable->document).origin;
          if (range.begin <= at && at <= range.end) {
            visibleHere = &table.jsonTable->documentTables;
            jsonDocument = true;
            break;
          }
        }
      }
      if (q == chosen && !jsonDocument) {
        for (std::size_t i = 0; i < q->relations.size(); ++i) {
          stop();
          const auto condition = q->relations[i].condition;
          if (!condition) continue;
          const auto range = q->expressions.at(condition).origin;
          if (range.begin > at || range.end <= at) continue;
          joinTables = relationLeaves(*q, RelationId(i + 1), [this] { stop(); });
          visibleHere = &joinTables;
          joinScope = true;
          break;
        }
      }
      for (std::size_t i = 0; i < q->tables.size(); ++i) {
        if (visibleHere && std::find(visibleHere->begin(), visibleHere->end(), i) == visibleHere->end())
          continue;
        const auto &t = q->tables[i];
        if (dialect==Dialect::SQLServer) {
          const bool inOutput=q==chosen && sqlServerOutput;
          if ((t.pseudo && (t.alias=="inserted" || t.alias=="deleted") && !inOutput) ||
              (inOutput && q->targetTable==i)) continue;
        }
        const bool incomingRow = q->conflict.incomingTable == i;
        if (incomingRow && q == chosen &&
            std::none_of(q->conflict.assignments.begin(), q->conflict.assignments.end(),
                [&](const auto &assignment) {
                  const auto range = q->expressions.at(assignment.value).origin;
                  return range.begin <= at && at <= range.end;
                }))
          continue;
        const auto alias =
            t.alias.empty() ? (t.name.empty() ? std::string{} : t.name.back())
                            : t.alias;
        if (alias.empty() || alias == identifierMarker ||
            !shadowed.insert(key(alias)).second)
          continue;
        visible.push_back({alias, t, incomingRow});
      }
      for (const auto &cte : q->ctes)
        if (cte.query && cte.query <= program->queries.size() &&
            program->queries[cte.query - 1].origin.end < at)
          ctes.push_back(cte.name);
      if (!q->scope.parent || *q->scope.parent == 0 ||
          *q->scope.parent > program->queries.size())
        break;
      visibleParent = q->scope.parentTables ? &*q->scope.parentTables : nullptr;
      q = &program->queries[*q->scope.parent - 1];
    }
    if (chosen && chosen->inspection) return;
    // A token-based partial scope is a fallback for syntactically unfinished
    // queries. It never escapes the nearest SELECT parenthesis scope.
    if (visible.empty() && !jsonDocument && !joinScope)
      partialScope();
  }
  void partialScope() {
    int level = 0, atLevel = 0;
    for (const auto &t : tokens) {
      if (t.range.begin >= cursor)
        break;
      if (t.kind == TokenKind::Symbol && t.value == "(")
        ++atLevel;
      else if (t.kind == TokenKind::Symbol && t.value == ")")
        --atLevel;
    }
    level = 0;
    std::size_t start = 0, end = tokens.size();
    std::vector<int> levels(tokens.size());
    for (std::size_t i = 0; i < tokens.size(); ++i) {
      levels[i] = level;
      if (tokens[i].kind == TokenKind::Symbol && tokens[i].value == "(")
        ++level;
      else if (tokens[i].kind == TokenKind::Symbol && tokens[i].value == ")")
        --level;
      if (tokens[i].range.begin < cursor && levels[i] == atLevel &&
          ((tokens[i].kind == TokenKind::Word && fold(tokens[i].value) == "SELECT") ||
           (tokens[i].kind == TokenKind::Symbol && tokens[i].value == ";")))
        start = i;
      if (tokens[i].range.begin >= cursor &&
          (levels[i] < atLevel ||
           (levels[i] == atLevel && tokens[i].kind == TokenKind::Symbol &&
            tokens[i].value == ";"))) {
        end = i;
        break;
      }
    }
    static const std::set<std::string> reserved = {
        "WHERE",  "JOIN",      "LEFT",      "RIGHT", "INNER",  "OUTER",
        "CROSS",  "FULL",      "ON",        "GROUP", "HAVING", "ORDER",
        "LIMIT",  "OFFSET",    "RETURNING", "SET",   "VALUES", "UNION",
        "EXCEPT", "INTERSECT", "END"};
    for (std::size_t i = start; i < end; ++i) {
      stop();
      if (levels[i] != atLevel || tokens[i].kind != TokenKind::Word)
        continue;
      auto u = fold(tokens[i].value);
      if (u != "FROM" && u != "JOIN" && u != "UPDATE" && u != "INTO")
        continue;
      auto j = i + 1;
      if (j >= end || !ident(tokens[j]) ||
          reserved.count(fold(tokens[j].value)))
        continue;
      ModelTable table;
      table.name.push_back(canonical(tokens[j++]));
      while (j + 1 < end && tokens[j].value == "." && ident(tokens[j + 1])) {
        table.name.push_back(canonical(tokens[j + 1]));
        j += 2;
      }
      if (j < end && fold(tokens[j].value) == "AS")
        ++j;
      if (j < end && ident(tokens[j]) && !reserved.count(fold(tokens[j].value)))
        table.alias = canonical(tokens[j]);
      auto alias = table.alias.empty() ? table.name.back() : table.alias;
      if (alias == marker)
        continue;
      bool duplicate = false;
      for (const auto &r : visible)
        if (key(r.alias) == key(alias))
          duplicate = true;
      if (!duplicate)
        visible.push_back({alias, std::move(table)});
    }
  }
  bool windowReferenceGrammar() {
    if (windowReference==WindowReference::None) return false;
    const bool copied=windowReference==WindowReference::Copy;
    bool partitions=false,ordering=false;
    if (copied) {
      auto i=std::lower_bound(tokens.begin(),tokens.end(),result.replacement.end,
          [](const Token &token,std::size_t at) { return token.range.begin<at; });
      std::size_t depth=0;
      for (;i!=tokens.end();++i) {
        stop();
        if (isSymbol(*i,"(")) { ++depth; continue; }
        if (isSymbol(*i,")")) {
          if (!depth) break;
          --depth;
          continue;
        }
        if (!depth && i->kind==TokenKind::Word) {
          const auto word=fold(i->value);
          if (word=="FROM" || word=="WHERE" || word=="WINDOW") break;
          partitions|=word=="PARTITION";
          ordering|=word=="ORDER";
        }
      }
    }
    const bool named=dialect==Dialect::PostgreSQL || dialect==Dialect::SQLite || mysql(dialect);
    if (named && cursorQuery && !partitions)
      for (const auto &window:cursorQuery->namedWindows) {
        stop();
        if (copied && (!window.specification.frameUnit.empty() ||
                       (ordering && !window.specification.ordering.empty()))) continue;
        add(window.name,spelling(window.name,quotedPrefix),"named window in this SELECT",CompletionKind::Alias,155);
      }
    if (copied) {
      if (quotedPrefix) { expect=Expect::FixedKeywords; return true; }
      return false; // Inline clauses remain available at the same cursor site.
    }
    expect=Expect::FixedKeywords;
    if (!quotedPrefix && options.snippets)
      for (const std::string clause:{"PARTITION BY","ORDER BY"}) {
        const auto specification="("+clause+" )";
        add(clause,specification,"inline window specification",CompletionKind::Keyword,140,specification.size()-1);
      }
    return true;
  }
  bool windowGrammar() {
    if (!windowOpening || quotedPrefix || parameterPrefix || !qualifier.empty() ||
        openColumnList()!=windowOpening) return false;
    const auto start=*windowOpening+1,n=before.size();
    const auto word=[&](std::size_t i,std::string_view value) { return wordAt(before,i,value); };
    const auto offer=[&](std::vector<std::string> choices) {
      keywordChoices=std::move(choices);
      expect=Expect::FixedKeywords;
      return true;
    };
    const bool exclusionSupported=dialect==Dialect::PostgreSQL || dialect==Dialect::SQLite;
    const bool nullPlacement=exclusionSupported || dialect==Dialect::Oracle;
    if (start==n) return offer({"PARTITION BY","ORDER BY"});
    const auto last=n-1;
    std::optional<std::size_t> frame,separator;
    bool ordered=false,partitioned=false,exclusion=false;
    std::size_t depth=0;
    for (auto i=start;i<n;++i) {
      if ((i&63u)==0) stop();
      if (isSymbol(before[i],"(")) { ++depth; continue; }
      if (isSymbol(before[i],")") && depth) { --depth; continue; }
      if (depth) continue;
      ordered|=word(i,"ORDER") && word(i+1,"BY");
      partitioned|=word(i,"PARTITION") && word(i+1,"BY");
      if (!frame && (word(i,"ROWS") || word(i,"RANGE") || word(i,"GROUPS"))) frame=i;
      if (frame && word(i,"AND")) separator=i;
      exclusion|=word(i,"EXCLUDE");
    }
    if (exclusion) {
      if (!exclusionSupported || !frame) return false;
      if (word(last,"EXCLUDE")) return offer({"NO OTHERS","CURRENT ROW","GROUP","TIES"});
      if (last>start && word(last-1,"EXCLUDE") && word(last,"NO")) return offer({"OTHERS"});
      if (last>start && word(last-1,"EXCLUDE") && word(last,"CURRENT")) return offer({"ROW"});
      return false;
    }
    const auto finishedExpression=[&] {
      const auto &token=before[last];
      if (token.kind==TokenKind::Number || token.kind==TokenKind::String || token.kind==TokenKind::Parameter ||
          isSymbol(token,")") || isSymbol(token,"]")) return true;
      if (!ident(token)) return false;
      for (const auto *operatorWord:{"BY","AND","OR","IS","LIKE","NOT","WHEN","THEN","ELSE","COLLATE"})
        if (word(last,operatorWord)) return false;
      return true;
    };
    if (!frame) {
      if (word(last,"PARTITION") || word(last,"ORDER")) return offer({"BY"});
      if (word(last,"NULLS")) return ordered && nullPlacement?offer({"FIRST","LAST"}):false;
      if (!finishedExpression()) return false;
      if (!ordered)
        return (partitioned || (n==start+1 && ident(before[start])))?offer({"ORDER BY"}):false;
      const bool hasNullPlacement=word(last,"FIRST") || word(last,"LAST");
      std::vector<std::string> choices;
      if (!hasNullPlacement && !word(last,"ASC") && !word(last,"DESC"))
        choices={"ASC","DESC"};
      if (!hasNullPlacement && nullPlacement) {
        choices.push_back("NULLS FIRST");
        choices.push_back("NULLS LAST");
      }
      choices.push_back("ROWS");
      choices.push_back("RANGE");
      if (exclusionSupported) choices.push_back("GROUPS");
      return offer(std::move(choices));
    }

    const bool between=word(*frame+1,"BETWEEN");
    const bool ending=between && separator.has_value();
    const auto begin=ending?*separator+1:*frame+1+std::size_t(between);
    const bool startFollowing=ending && *separator>*frame+1 && word(*separator-1,"FOLLOWING");
    const bool startCurrent=ending && *separator>*frame+1 && word(*separator-1,"ROW");
    if (begin==n) {
      if (ending)
        return startFollowing?offer({"UNBOUNDED FOLLOWING"}):offer({"CURRENT ROW","UNBOUNDED FOLLOWING"});
      if (between) return offer({"UNBOUNDED PRECEDING","CURRENT ROW"});
      return offer({"BETWEEN","UNBOUNDED PRECEDING","CURRENT ROW"});
    }
    if (last==begin && word(last,"UNBOUNDED")) return offer({ending?"FOLLOWING":"PRECEDING"});
    if (last==begin && word(last,"CURRENT")) return offer({"ROW"});
    if (word(last,"PRECEDING") || word(last,"FOLLOWING") || word(last,"ROW")) {
      if (between && !ending) return offer({"AND"});
      if (exclusionSupported) return offer({"EXCLUDE"});
      return false;
    }
    if (finishedExpression()) {
      if (dialect==Dialect::SQLServer && word(*frame,"RANGE")) return offer({});
      if (!between) return offer({"PRECEDING"});
      if (ending && (startFollowing || startCurrent)) return offer({"FOLLOWING"});
      return offer({"PRECEDING","FOLLOWING"});
    }
    return false;
  }
  bool fetchGrammar() {
    const bool sqlServer = dialect == Dialect::SQLServer;
    if ((!sqlServer && dialect != Dialect::PostgreSQL && dialect != Dialect::Oracle) ||
        before.size() < 3) return false;
    const auto word = [&](std::size_t i, std::string_view text) {
      return before[i].kind == TokenKind::Word && fold(before[i].value) == text;
    };
    auto index = before.size() - 1;
    const bool afterWith = word(index, "WITH");
    if (afterWith && sqlServer) return false;
    index -= afterWith;
    if (index < 2 || (!word(index, "ROWS") && !word(index, "ROW"))) return false;
    --index;
    if (before[index].kind == TokenKind::Number || before[index].kind == TokenKind::Parameter) {
      if (index == 0) return false;
      --index;
    }
    if (index < 1 || (!word(index, "FIRST") && !word(index, "NEXT")) || !word(index - 1, "FETCH"))
      return false;
    keywordChoices = afterWith ? std::vector<std::string>{"TIES"} :
                     sqlServer ? std::vector<std::string>{"ONLY"} : std::vector<std::string>{"ONLY", "WITH TIES"};
    expect = Expect::FixedKeywords;
    return true;
  }
  bool selectIntoGrammar() {
    if (!routineHeader || before.empty() ||
        (!mysql(dialect) && dialect != Dialect::PostgreSQL && dialect != Dialect::Oracle)) return false;
    std::size_t start = before.size();
    while (start > 0 && !(before[start - 1].kind == TokenKind::Symbol && before[start - 1].value == ";"))
      --start;
    const auto last = fold(before.back().value);
    if (last != "INTO" && last != "STRICT" && last != "," && last != ".") return false;
    bool query = false, targets = false;
    unsigned depth = 0;
    for (std::size_t i = start; i < before.size(); ++i) {
      if ((i & 255u) == 0) stop();
      const auto &token = before[i];
      if (token.kind == TokenKind::Symbol && token.value == "(") { ++depth; continue; }
      if (token.kind == TokenKind::Symbol && token.value == ")" && depth) { --depth; continue; }
      if (depth || token.kind != TokenKind::Word) continue;
      const auto word = fold(token.value);
      if (!query && (word == "INSERT" || word == "UPDATE" || word == "DELETE" || word == "MERGE"))
        return false;
      if (word == "SELECT") query = true;
      if (query && word == "INTO" && i > start && fold(before[i - 1].value) != "AS" &&
          before[i - 1].value != ".") targets = true;
      else if (targets && (word == "FROM" || word == "WHERE" || word == "GROUP" ||
                           word == "HAVING" || word == "ORDER" || word == "LIMIT" ||
                           word == "OFFSET" || word == "FETCH" || word == "FOR"))
        return false;
    }
    if (!targets) return false;
    expect = Expect::VariableTarget;
    return true;
  }
  bool pgDiagnosticsGrammar() {
    if (!routineHeader || dialect != Dialect::PostgreSQL || before.empty()) return false;
    const auto word = [&](std::size_t i, std::string_view value) {
      return i < before.size() && before[i].kind == TokenKind::Word && fold(before[i].value) == value;
    };
    const auto count = before.size();
    auto start = count;
    while (start > 0 && !word(start - 1, "GET")) {
      const auto &t = before[--start];
      if (t.kind == TokenKind::Symbol && t.value == ";") return false;
    }
    if (!start) return false;
    if (start > 1) {
      const auto previous = start - 2;
      if (!word(previous, "BEGIN") && !word(previous, "THEN") && !word(previous, "ELSE") &&
          !word(previous, "LOOP") &&
          !(before[previous].kind == TokenKind::Symbol && before[previous].value == ";"))
        return false;
    }
    if (start == count) {
      keywordChoices = {"DIAGNOSTICS", "CURRENT DIAGNOSTICS", "STACKED DIAGNOSTICS"};
      expect = Expect::FixedKeywords;
      return true;
    }
    const bool stacked = word(start, "STACKED");
    if (stacked || word(start, "CURRENT")) {
      ++start;
      if (start == count) {
        keywordChoices = {"DIAGNOSTICS"};
        expect = Expect::FixedKeywords;
        return true;
      }
    }
    if (!word(start, "DIAGNOSTICS")) return false;
    if (start + 1 == count || before.back().value == "," || before.back().value == ".") {
      expect = Expect::VariableTarget;
      return true;
    }
    if (before.back().kind != TokenKind::Symbol ||
        (before.back().value != "=" && before.back().value != ":=")) return false;
    const auto area = stacked ? PgDiagnosticArea::Stacked : PgDiagnosticArea::Current;
    for (const auto &field : pgDiagnosticFields)
      if (field.available(area)) keywordChoices.push_back(field.spelling);
    expect = Expect::FixedKeywords;
    return true;
  }
  bool temporalGrammar() {
    if(dialect==Dialect::SQLServer || dialect==Dialect::SQLite || before.empty())return false;
    auto word=[&](std::size_t i){return i<before.size() && before[i].kind==TokenKind::Word?fold(before[i].value):std::string{};};
    auto keywords=[&](std::vector<std::string> words){keywordChoices=std::move(words);expect=Expect::FixedKeywords;return true;};
    std::vector<std::size_t> open;
    for(std::size_t i=0;i<before.size();++i) {
      if((i&255u)==0)stop();
      if(before[i].value=="("){if(open.size()>=256)return false;open.push_back(i);}
      else if(before[i].value==")" && !open.empty())open.pop_back();
    }
    if(!open.empty() && open.back()>0 && word(open.back()-1)=="EXTRACT") {
      const auto start=open.back()+1;
      if(start==before.size()) {
        std::vector<std::string> fields={"YEAR","MONTH","DAY","HOUR","MINUTE","SECOND"};
        if(dialect==Dialect::PostgreSQL)for(auto f:{"CENTURY","DECADE","DOW","DOY","EPOCH","ISODOW","ISOYEAR","JULIAN","MICROSECONDS","MILLENNIUM","MILLISECONDS","QUARTER","TIMEZONE","TIMEZONE_HOUR","TIMEZONE_MINUTE","WEEK"})fields.push_back(f);
        else if(dialect==Dialect::Oracle)for(auto f:{"TIMEZONE_HOUR","TIMEZONE_MINUTE","TIMEZONE_REGION","TIMEZONE_ABBR"})fields.push_back(f);
        else for(int i=1;i<=static_cast<int>(IntervalUnit::SecondMicrosecond);++i)fields.push_back(intervalUnitName(static_cast<IntervalUnit>(i)));
        return keywords(std::move(fields));
      }
      if(start+1==before.size())return keywords({"FROM"});
      if(word(start+1)=="FROM"){expect=Expect::Expression;return true;}
    }
    // This is a bounded recovery for the qualifier slot, never a substitute
    // for parsing the expression. Incomplete expressions keep ordinary names.
    const auto low=before.size()>256?before.size()-256:0;
    for(std::size_t n=before.size();n>low;--n) {
      const auto head=n-1;
      if(before[head].value==";")break;
      const auto boundary=word(head);
      if(boundary=="SELECT" || boundary=="FROM" || boundary=="WHERE" || boundary=="GROUP" || boundary=="ORDER" || boundary=="HAVING" || boundary=="LIMIT" || boundary=="AS" || boundary=="WHEN" || boundary=="THEN" || boundary=="ELSE" || boundary=="ON")break;
      if(word(head)!="INTERVAL")continue;
      auto i=head+1;if(i>=before.size())return false;
      if(mysql(dialect)) {
        int nesting=0;for(std::size_t j=i;j<before.size();++j){if(before[j].value=="(")++nesting;else if(before[j].value==")")--nesting;}
        const auto &last=before.back();
        for(int u=1;u<=static_cast<int>(IntervalUnit::SecondMicrosecond);++u)
          if(word(before.size()-1)==intervalUnitName(static_cast<IntervalUnit>(u)))return false;
        if(nesting!=0 || (last.kind!=TokenKind::Number && last.kind!=TokenKind::String && !ident(last) && last.value!=")"))return false;
        std::vector<std::string> units;for(int u=1;u<=static_cast<int>(IntervalUnit::SecondMicrosecond);++u)units.push_back(intervalUnitName(static_cast<IntervalUnit>(u)));
        return keywords(std::move(units));
      }
      if(before[i].value=="(") {if(i+2>=before.size() || before[i+2].value!=")")return false;i+=3;}
      if(i>=before.size() || before[i].kind!=TokenKind::String)return false;
      ++i;
      if(i==before.size())return keywords({"YEAR","MONTH","DAY","HOUR","MINUTE","SECOND","AS","FROM"});
      IntervalField leading=IntervalField::None;
      for(int f=1;f<=static_cast<int>(IntervalField::Second);++f)if(word(i)==intervalFieldName(static_cast<IntervalField>(f)))leading=static_cast<IntervalField>(f);
      if(leading==IntervalField::None)return false;
      ++i;
      if(i<before.size() && before[i].value=="(") {while(i<before.size() && before[i].value!=")")++i;if(i==before.size())return false;++i;}
      if(i==before.size())return leading==IntervalField::Second?false:keywords({"TO","AS","FROM"});
      if(word(i)=="TO" && i+1==before.size()) {
        std::vector<std::string> fields;
        for(int f=static_cast<int>(leading)+1;f<=static_cast<int>(IntervalField::Second);++f) {
          if(static_cast<int>(leading)<=static_cast<int>(IntervalField::Month) && f>static_cast<int>(IntervalField::Month))break;
          fields.push_back(intervalFieldName(static_cast<IntervalField>(f)));
        }
        return keywords(std::move(fields));
      }
      return false;
    }
    return false;
  }
  std::optional<std::size_t> openingParenthesis(std::size_t beforeEnd) {
    std::size_t depth=0;
    for (auto i=beforeEnd;i>0;) {
      if ((i&63u)==0) stop();
      const auto &token=before[--i];
      if (token.kind!=TokenKind::Symbol) continue;
      if (token.value==")") ++depth;
      else if (token.value=="(") {
        if (!depth) return i;
        --depth;
      }
    }
    return {};
  }
  std::optional<std::size_t> openColumnList() { return openingParenthesis(before.size()); }
  void excludeEarlierListColumns(std::size_t opening) {
    bool first=true;
    std::size_t depth=0;
    for (std::size_t i=opening+1;i<before.size();++i) {
      if ((i&63u)==0) stop();
      const auto &token=before[i];
      if (!depth && first && ident(token)) {
        assignedColumns.insert(key(canonical(token)));
        first=false;
      }
      if (token.kind!=TokenKind::Symbol) continue;
      if (token.value=="(") ++depth;
      else if (token.value==")" && depth) --depth;
      else if (!depth && token.value==",") first=true;
    }
  }
  bool conflictColumnGrammar() {
    if ((dialect!=Dialect::PostgreSQL && dialect!=Dialect::SQLite) || before.size()<7 ||
        parameterPrefix || !qualifier.empty()) return false;
    const auto word=[&](std::size_t i,std::string_view value) {
      return before[i].kind==TokenKind::Word && fold(before[i].value)==value;
    };
    if ((!word(0,"INSERT") && !word(0,"WITH")) || before.back().kind!=TokenKind::Symbol ||
        (before.back().value!="(" && before.back().value!=",")) return false;
    const auto opening=openColumnList();
    if (!opening || *opening<2 || !word(*opening-2,"ON") || !word(*opening-1,"CONFLICT"))
      return false;
    const auto readTarget=[&](const ModelStatement &statement) {
      if (statement.kind!=StatementKind::Insert || statement.conflict.targets.empty() || !statement.target)
        return false;
      if (statement.target->definition)
        for (const auto &column:statement.target->definition->columns) listColumns.push_back(column.name);
      const auto markerName=canonicalIdentifier(dialect,marker,false);
      for (auto id:statement.conflict.targets) {
        const auto &expression=statement.expressions.at(id);
        if (expression.kind==ExprKind::Name && expression.name.size()==1 && expression.name.front()!=markerName)
          assignedColumns.insert(key(expression.name.front()));
      }
      return true;
    };
    if (!cursorQuery || !readTarget(*cursorQuery)) {
      const auto endpoint=before[*opening].range.end;
      auto sql=source.substr(base,endpoint-base)+marker+") DO NOTHING";
      const auto parsedConflict=compile(dialect,std::move(sql),options.language,catalog,names);
      if (!parsedConflict->syntaxValid || parsedConflict->model.roots.size()!=1 ||
          !readTarget(parsedConflict->model.at(parsedConflict->model.roots.front()-1))) return false;
    }
    excludeEarlierListColumns(*opening);
    expect=Expect::ColumnList;
    return true;
  }
  bool conflictAssignmentGrammar() {
    const bool duplicateKey=mysql(dialect);
    if ((!duplicateKey && dialect!=Dialect::PostgreSQL && dialect!=Dialect::SQLite) ||
        before.size()<7 || parameterPrefix || (duplicateKey?qualifier.size()>1:!qualifier.empty())) return false;
    const auto word=[&](std::size_t i,std::string_view value) {
      return i<before.size() && before[i].kind==TokenKind::Word && fold(before[i].value)==value;
    };
    const auto symbol=[&](std::size_t i,std::string_view value) {
      return i<before.size() && before[i].kind==TokenKind::Symbol && before[i].value==value;
    };
    if (!word(0,"INSERT") && !word(0,"WITH") && !(duplicateKey && word(0,"REPLACE"))) return false;
    if (!symbol(before.size()-1,",") && !symbol(before.size()-1,".") &&
        !word(before.size()-1,duplicateKey?"UPDATE":"SET")) return false;
    std::size_t depth=0;
    bool sawConflict=false;
    std::optional<std::size_t> start;
    for (std::size_t i=1;i<before.size();++i) {
      if ((i&63u)==0) stop();
      if (symbol(i,"(")) { ++depth; continue; }
      if (symbol(i,")") && depth) { --depth; continue; }
      if (depth) continue;
      if (duplicateKey && word(i,"ON") && word(i+1,"DUPLICATE") && word(i+2,"KEY") && word(i+3,"UPDATE")) {
        start=i+4;
        break;
      }
      if (!duplicateKey) {
        sawConflict|=word(i,"ON") && word(i+1,"CONFLICT");
        if (sawConflict && word(i,"DO") && word(i+1,"UPDATE") && word(i+2,"SET")) {
          start=i+3;
          break;
        }
      }
    }
    if (!start) return false;
    auto assignment=*start;
    std::set<std::string> assigned;
    for (std::size_t i=*start;i<before.size();++i) {
      if ((i&63u)==0) stop();
      if (symbol(i,"(")) ++depth;
      else if (symbol(i,")") && depth) --depth;
      else if (!depth && (word(i,"WHERE") || word(i,"RETURNING") || symbol(i,";"))) return false;
      else if (!depth && symbol(i,",")) {
        auto field=assignment;
        if (duplicateKey && field+2<i && ident(before[field]) && symbol(field+1,".")) field+=2;
        if (field+1>=i || !ident(before[field]) || !symbol(field+1,"=")) return false;
        assigned.insert(key(canonical(before[field])));
        assignment=i+1;
      }
    }
    if (depth || (assignment!=before.size() && (!duplicateKey || qualifier.size()!=1 ||
        assignment+2!=before.size() || !ident(before[assignment]) || !symbol(assignment+1,".")))) return false;
    const auto readTarget=[&](const ModelStatement &statement) {
      const auto action=duplicateKey?ConflictClause::Action::DuplicateKey:ConflictClause::Action::Update;
      if (statement.kind!=StatementKind::Insert || statement.conflict.action!=action || !statement.target ||
          statement.target->name.empty()) return false;
      const auto &target=*statement.target;
      if (!qualifier.empty() && key(qualifier.front())!=key(target.alias.empty()?target.name.back():target.alias))
        return false;
      columnListTarget=target.name;
      const auto markerName=canonicalIdentifier(dialect,marker,false);
      for (const auto &update:statement.conflict.assignments)
        for (const auto id:update.targets) {
          const auto &field=statement.expressions.at(id);
          if (!field.name.empty() && field.name.back()!=markerName) assigned.insert(key(field.name.back()));
        }
      return true;
    };
    if (!cursorQuery || !readTarget(*cursorQuery)) {
      const auto endpoint=before[*start-1].range.end;
      auto sql=source.substr(base,endpoint-base)+" "+marker+"=0";
      const auto parsedUpdate=compile(dialect,std::move(sql),options.language,catalog,names);
      if (!parsedUpdate->syntaxValid || parsedUpdate->model.roots.size()!=1 ||
          !readTarget(parsedUpdate->model.at(parsedUpdate->model.roots.front()-1))) return false;
    }
    assignedColumns=std::move(assigned);
    expect=Expect::WriteColumn;
    return true;
  }
  bool indexColumnGrammar() {
    if (before.size()<6 || parameterPrefix || !qualifier.empty()) return false;
    const auto symbol=[&](std::size_t i,std::string_view spelling) {
      return before[i].kind==TokenKind::Symbol && before[i].value==spelling;
    };
    const auto word=[&](std::size_t i,std::string_view spelling) {
      return before[i].kind==TokenKind::Word && fold(before[i].value)==spelling;
    };
    if (!word(0,"CREATE")) return false;
    const bool inlineTable=mysql(dialect) && (word(1,"TABLE") ||
        (word(1,"TEMPORARY") && word(2,"TABLE")));
    const bool standalone=word(1,"INDEX") || (word(1,"UNIQUE") && word(2,"INDEX"));
    if (!inlineTable && !standalone) return false;
    const auto last=before.size()-1;
    const bool includeAvailable=dialect==Dialect::PostgreSQL || dialect==Dialect::SQLServer;
    if (standalone && includeAvailable && !quotedPrefix && symbol(last,")")) {
      const auto parsedIndex=compile(dialect,source.substr(base,before[last].range.end-base),
                                     options.language,catalog,names);
      if (!parsedIndex->syntaxValid || parsedIndex->model.roots.size()!=1) return false;
      const auto &statement=parsedIndex->model.at(parsedIndex->model.roots.front()-1);
      if (!statement.ddl || statement.ddl->object!=ObjectKind::Index || statement.ddl->predicate)
        return false;
      keywordChoices={"WHERE"};
      if (statement.ddl->includeColumns.empty()) keywordChoices.insert(keywordChoices.begin(),"INCLUDE");
      expect=Expect::FixedKeywords;
      return true;
    }
    if (!symbol(last,"(") && !symbol(last,",")) return false;
    const auto opening=openColumnList();
    if (!opening || *opening<4) return false;
    auto head=*opening;
    const bool includeList=standalone && includeAvailable && word(head-1,"INCLUDE");
    if (inlineTable) {
      if (head>=3 && word(head-2,"USING") && (word(head-1,"BTREE") || word(head-1,"HASH")))
        head-=2;
      const auto indexWord=[&](std::size_t i) { return word(i,"KEY") || word(i,"INDEX"); };
      if (head>=2 && indexWord(head-1)) --head;
      else if (head>=3 && indexWord(head-2) && ident(before[head-1])) head-=2;
      else return false;
      if (head && word(head-1,"UNIQUE")) --head;
      if (!head || (!symbol(head-1,",") && !symbol(head-1,"("))) return false;
    }
    std::size_t listDepth=0;
    for (std::size_t i=0;i<head;++i) {
      if ((i&63u)==0) stop();
      if (symbol(i,"(")) ++listDepth;
      else if (symbol(i,")")) {
        if (!listDepth) return false;
        --listDepth;
      }
    }
    if (listDepth!=(inlineTable?1u:0u)) return false;

    const auto expectedObject=inlineTable?ObjectKind::Table:ObjectKind::Index;
    const auto fromDefinition=[&](const ModelStatement &table) {
      if (table.kind!=StatementKind::Create || !table.ddl || table.ddl->object!=expectedObject) return;
      if (!table.tables.empty() && table.tables.front().definition) {
        for (const auto &column:table.tables.front().definition->columns)
          listColumns.push_back(column.name);
      } else if (inlineTable)
        for (const auto &column:table.ddl->definitions) listColumns.push_back(column.name);
      if (includeList) {
        for (const auto &part:table.ddl->keys) {
          if (!part.expression || part.expression>=table.expressions.size()) continue;
          const auto &expression=table.expressions[part.expression];
          if (expression.kind==ExprKind::Name && expression.name.size()==1)
            assignedColumns.insert(key(expression.name.front()));
        }
        for (const auto &column:table.ddl->includeColumns)
          assignedColumns.insert(key(column));
      }
    };
    if (cursorQuery) fromDefinition(*cursorQuery);
    if (listColumns.empty() && (!inlineTable || symbol(head-1,","))) {
      const auto *snapshot=declarations?static_cast<const CatalogSnapshot *>(declarations.get()):catalog;
      const auto end=inlineTable?before[head-1].range.begin:before[*opening].range.end;
      auto partial=source.substr(base,end-base);
      partial+=inlineTable?")":"__nvd_index_column__)";
      const auto definition=compile(dialect,std::move(partial),options.language,snapshot,names);
      if (!inlineTable && !definition->syntaxValid) return false;
      if (definition->syntaxValid && !definition->model.roots.empty())
        fromDefinition(definition->model.at(definition->model.roots.front()-1));
    }

    excludeEarlierListColumns(*opening);
    expect=Expect::ColumnList;
    return true;
  }
  bool constraintColumnGrammar() {
    if (before.size()<6 || parameterPrefix || !qualifier.empty() ||
        (before.back().value!="(" && before.back().value!=",")) return false;
    const auto word=[&](std::size_t i,std::string_view spelling) {
      return i<before.size() && before[i].kind==TokenKind::Word && fold(before[i].value)==spelling;
    };
    const auto symbol=[&](std::size_t i,std::string_view spelling) {
      return i<before.size() && before[i].kind==TokenKind::Symbol && before[i].value==spelling;
    };
    if (!word(0,"CREATE")) return false;
    std::size_t at=1;
    while (word(at,"TEMP") || word(at,"TEMPORARY") || word(at,"UNLOGGED") || word(at,"GLOBAL")) ++at;
    if (!word(at++,"TABLE")) return false;
    if (word(at,"IF") && word(at+1,"NOT") && word(at+2,"EXISTS")) at+=3;
    if (at>=before.size() || !ident(before[at++])) return false;
    while (symbol(at,".")) {
      ++at;
      if (at>=before.size() || !ident(before[at++])) return false;
    }
    if (!symbol(at,"(")) return false;
    const auto list=openColumnList();
    if (!list || *list<=at+1) return false;
    auto head=*list-1;
    if (word(head,"KEY")) {
      --head;
      if (!word(head,"PRIMARY") && !word(head,"FOREIGN") &&
          !(mysql(dialect) && word(head,"UNIQUE"))) return false;
    } else if (!word(head,"UNIQUE")) return false;

    std::size_t item=at+1,depth=1;
    for (auto i=item;i<head;++i) {
      if ((i&63u)==0) stop();
      if (symbol(i,"(")) ++depth;
      else if (symbol(i,")")) {
        if (depth==1) return false;
        --depth;
      } else if (depth==1 && symbol(i,",")) item=i+1;
    }
    if (depth!=1 || (item!=head && !(item+2==head && word(item,"CONSTRAINT") && ident(before[item+1]))))
      return false;

    const auto readColumns=[&](const ModelStatement &statement) {
      if (!statement.ddl || statement.kind!=StatementKind::Create || statement.ddl->object!=ObjectKind::Table)
        return;
      for (const auto &column:statement.ddl->definitions) listColumns.push_back(column.name);
    };
    if (cursorQuery) readColumns(*cursorQuery);
    if (listColumns.empty() && item>at+1) {
      const auto end=before[item-1].range.begin;
      const auto prefix=compile(dialect,source.substr(base,end-base)+")",options.language,catalog,names);
      if (prefix->syntaxValid && prefix->model.roots.size()==1)
        readColumns(prefix->model.at(prefix->model.roots.front()-1));
    }
    excludeEarlierListColumns(*list);
    auto next=std::lower_bound(tokens.begin(),tokens.end(),result.replacement.end,
        [](const Token &token,std::size_t offset) { return token.range.begin<offset; });
    const auto skipComments=[&] {
      while (next!=tokens.end() && next->kind==TokenKind::Comment) ++next;
    };
    skipComments();
    while (next!=tokens.end() && next->kind==TokenKind::Symbol && next->value==",") {
      stop();
      ++next;
      skipComments();
      if (next==tokens.end() || !ident(*next)) break;
      assignedColumns.insert(key(canonical(*next++)));
      skipComments();
    }
    columnListDetail="column declared in this table";
    expect=Expect::ColumnList;
    return true;
  }
  bool writeColumnGrammar() {
    if (before.size() < 3 || parameterPrefix) return false;
    const auto word = [&](std::size_t i, std::string_view spelling) {
      return i < before.size() && before[i].kind == TokenKind::Word &&
             fold(before[i].value) == spelling;
    };
    const auto symbol = [&](std::size_t i, std::string_view spelling) {
      return i < before.size() && before[i].kind == TokenKind::Symbol &&
             before[i].value == spelling;
    };
    std::size_t statement = 0, depth = 0;
    for (std::size_t i = 0; i < before.size(); ++i) {
      if (symbol(i, "(")) ++depth;
      else if (symbol(i, ")") && depth) --depth;
      else if (symbol(i, ";") && !depth) statement = i + 1;
    }
    if (word(statement, "WITH")) {
      depth = 0;
      for (++statement; statement < before.size(); ++statement) {
        if (symbol(statement, "(")) ++depth;
        else if (symbol(statement, ")") && depth) --depth;
        else if (!depth && (word(statement, "INSERT") || word(statement, "UPDATE"))) break;
      }
    }
    const bool insert = word(statement, "INSERT") || (mysql(dialect) && word(statement, "REPLACE"));
    const bool update = word(statement, "UPDATE");
    if (!insert && !update) return false;
    std::size_t at = statement + 1;
    if (mysql(dialect)) {
      while (word(at, "LOW_PRIORITY") || word(at, "HIGH_PRIORITY") ||
             word(at, "DELAYED") || word(at, "IGNORE")) ++at;
    }
    if (insert && word(at, "INTO")) ++at;
    if (dialect == Dialect::PostgreSQL && word(at, "ONLY")) ++at;
    if (at == before.size() || !ident(before[at])) return false;
    std::vector<std::string> target{canonical(before[at++])};
    while (symbol(at, ".") && at + 1 < before.size() && ident(before[at + 1])) {
      target.push_back(canonical(before[at + 1]));
      at += 2;
    }
    std::string alias = target.back();
    if (word(at, "AS") && at + 1 < before.size() && ident(before[at + 1])) {
      alias = canonical(before[at + 1]);
      at += 2;
    } else if (update && at + 1 < before.size() && ident(before[at]) && word(at + 1, "SET")) {
      alias = canonical(before[at++]);
    }
    std::set<std::string> assigned;
    if (insert) {
      if (!qualifier.empty() || !symbol(at++, "(")) return false;
      while (at < before.size()) {
        if (!ident(before[at]) || !symbol(at + 1, ",")) return false;
        assigned.insert(key(canonical(before[at])));
        at += 2;
      }
    } else {
      if (!word(at++, "SET")) return false;
      std::size_t start = at;
      depth = 0;
      for (; at < before.size(); ++at) {
        if (symbol(at, "(") || symbol(at,"[")) ++depth;
        else if ((symbol(at, ")") || symbol(at,"]")) && depth) --depth;
        else if (!depth && (word(at, "WHERE") || word(at, "FROM") ||
                            word(at, "RETURNING") || word(at, "OUTPUT") ||
                            word(at, "ORDER") || word(at, "LIMIT"))) return false;
        else if (!depth && symbol(at, ",")) {
          // Only record syntactically simple assignments. Tuple assignment
          // and qualified multi-table UPDATE stay with ordinary completion.
          std::size_t column = start;
          if (mysql(dialect) && ident(before[start]) && symbol(start + 1, "."))
            column += 2;
          if (column + 1 >= at || !ident(before[column]) || !symbol(column + 1, "="))
            return false;
          assigned.insert(key(canonical(before[column])));
          start = at + 1;
        }
      }
      if (start != before.size()) {
        if (!mysql(dialect) || qualifier.size() != 1 || start + 2 != before.size() ||
            key(qualifier.front()) != key(alias)) return false;
      }
    }
    if (cursorQuery && cursorQuery->targetReference &&
        cursorQuery->targetReference->name == target &&
        cursorQuery->kind == (insert ? StatementKind::Insert : StatementKind::Update)) {
      const auto identifierMarker = canonicalIdentifier(dialect, marker, false);
      const auto record = [&](const std::string &column) {
        if (column != identifierMarker) assigned.insert(key(column));
      };
      if (insert)
        for (const auto &column : cursorQuery->columns) record(column);
      else
        for (const auto &assignment : cursorQuery->assignments)
          for (auto id : assignment.targets) {
            const auto &column = cursorQuery->expressions.at(id);
            if (!column.name.empty() && (column.name.size() == 1 || key(column.name.front()) == key(alias)))
              record(column.name.back());
          }
      if (cursorQuery->target) target = cursorQuery->target->name;
    }
    columnListTarget = std::move(target);
    assignedColumns = std::move(assigned);
    expect = Expect::WriteColumn;
    return true;
  }
  bool outputIntoGrammar() {
    if (dialect!=Dialect::SQLServer || parameterPrefix) return false;
    std::optional<std::size_t> destination;
    bool dml=false, output=false, merge=false;
    std::size_t depth=0;
    for (std::size_t i=0;i<before.size();++i) {
      if ((i&63u)==0) stop();
      const auto &token=before[i];
      if (isSymbol(token,"(")) ++depth;
      else if (isSymbol(token,")") && depth) --depth;
      else if (!depth) {
        if (isSymbol(token,";") ||
            (!merge && (wordAt(before,i,"SELECT") || wordAt(before,i,"VALUES") || wordAt(before,i,"DEFAULT"))) ||
            (destination && (wordAt(before,i,"WHERE") || wordAt(before,i,"FROM")))) {
          dml=output=merge=false;
          destination.reset();
        } else if (wordAt(before,i,"MERGE")) {
          dml=merge=true;
        } else if (wordAt(before,i,"INSERT") || wordAt(before,i,"UPDATE") || wordAt(before,i,"DELETE")) {
          dml=true;
        } else if (dml && wordAt(before,i,"OUTPUT")) {
          output=true;
          destination.reset();
        } else if (output && wordAt(before,i,"INTO")) {
          output=false;
          destination=i+1;
        }
      }
    }
    if (!destination) return false;
    auto at=*destination;
    std::vector<std::string> target;
    while (at<before.size() && ident(before[at])) {
      target.push_back(canonical(before[at++]));
      if (at==before.size() || !isSymbol(before[at],".")) break;
      ++at;
    }
    if (at==before.size()) {
      if (!target.empty() && qualifier.empty()) return false;
      objectKind=CatalogObjectKind::Table;
      expect=Expect::CatalogObject;
      return true;
    }
    const auto open=openColumnList();
    if (target.empty() || !open || at!=*open) return false;
    for (auto i=at+1;i<before.size();i+=2)
      assignedColumns.insert(key(canonical(before[i])));
    bool comma=false;
    for (const auto &token:tokens) {
      if (token.range.begin<result.replacement.end || token.kind==TokenKind::Comment) continue;
      stop();
      if (isSymbol(token,",")) comma=true;
      else if (comma && ident(token)) {
        assignedColumns.insert(key(canonical(token)));
        comma=false;
      } else break;
    }
    columnListTarget=std::move(target);
    expect=Expect::WriteColumn;
    return true;
  }
  bool referenceColumnGrammar() {
    const auto open = openColumnList();
    if (!open || *open < 2) return false;
    const auto word = [&](std::size_t i, std::string_view spelling) {
      return i < before.size() && before[i].kind == TokenKind::Word && fold(before[i].value) == spelling;
    };
    if (!word(0,"CREATE") && !word(0,"ALTER")) return false;
    std::size_t table = 1;
    if (word(0,"CREATE"))
      while (word(table,"GLOBAL") || word(table,"TEMPORARY") || word(table,"TEMP") || word(table,"UNLOGGED"))
        ++table;
    if (!word(table,"TABLE")) return false;
    std::vector<std::string> target;
    auto at = *open;
    while (at > table + 1 && ident(before[at - 1])) {
      target.push_back(canonical(before[--at]));
      if (at <= table + 1 || before[at - 1].kind != TokenKind::Symbol || before[at - 1].value != ".")
        break;
      --at;
    }
    if (target.empty() || !at || !word(at - 1,"REFERENCES")) return false;
    std::reverse(target.begin(), target.end());
    columnListTarget = std::move(target);
    for (auto i = *open + 1; i < before.size(); i += 2)
      assignedColumns.insert(key(canonical(before[i])));
    expect = Expect::ReferenceColumn;
    return true;
  }
  void targetColumns() {
    if (!catalog) return;
    const auto *snapshot=alterActions?static_cast<const CatalogSnapshot *>(alterActions.get()):catalog;
    const auto table = snapshot->findTable(names, columnListTarget);
    if (!table) return;
    std::size_t visited = 0;
    for (const auto &column : table->columns) {
      stop();
      if (++visited > options.maxCatalogEntries) { result.truncated = true; break; }
      if (assignedColumns.count(key(column.name))) continue;
      const bool primary = expect == Expect::ReferenceColumn && table->primaryKeyKnown &&
          std::any_of(table->primaryKey.begin(), table->primaryKey.end(), [&](const auto &field) {
            return key(field) == key(column.name);
          });
      add(column.name, spelling(column.name, quotedPrefix),
          columnListTarget.back() + " · " + valueTypeName(column.type) + (primary ? " · primary key" : ""),
          CompletionKind::Column, primary ? 145 : 130);
    }
  }

  bool alterTableGrammar() {
    const auto word=[&](std::size_t i,std::string_view spelling) {
      return i<before.size() && before[i].kind==TokenKind::Word && fold(before[i].value)==spelling;
    };
    const auto symbol=[&](std::size_t i,std::string_view spelling) {
      return i<before.size() && before[i].kind==TokenKind::Symbol && before[i].value==spelling;
    };
    const auto identifier=[&](std::size_t i) { return i<before.size() && ident(before[i]); };
    const auto keywords=[&](std::vector<std::string> choices) {
      if (!quotedPrefix && !parameterPrefix && qualifier.empty()) keywordChoices=std::move(choices);
      expect=Expect::FixedKeywords;
      return true;
    };
    if (!word(0,"ALTER") || !word(1,"TABLE") || parameterPrefix) return false;
    const bool pg=dialect==Dialect::PostgreSQL, ms=dialect==Dialect::SQLServer;
    const bool maria=dialect==Dialect::MariaDB, tidb=dialect==Dialect::TiDB;
    const bool tableGuard=pg || maria, dropGuard=tableGuard || ms || tidb;
    const bool family=mysql(dialect), ora=dialect==Dialect::Oracle, sqlite=dialect==Dialect::SQLite;
    std::size_t at=2;
    if (tableGuard && word(at,"IF")) {
      if (at+1==before.size()) return keywords({"EXISTS"});
      if (!word(at+1,"EXISTS")) return false;
      at+=2;
    } else if (tableGuard && at==before.size() && !quotedPrefix && qualifier.empty()) {
      add("IF EXISTS","IF EXISTS","ALTER TABLE clause",CompletionKind::Keyword,135);
    }
    std::vector<std::string> target;
    while (identifier(at)) {
      target.push_back(canonical(before[at++]));
      if (!symbol(at,".")) break;
      ++at;
      if (!identifier(at)) return false;
    }
    if (target.empty()) return false;
    auto action=at;
    std::size_t depth=0;
    for (std::size_t i=action;i<before.size();++i) {
      if ((i&63u)==0) stop();
      if (symbol(i,"(")) ++depth;
      else if (symbol(i,")") && depth) --depth;
      else if (!depth && symbol(i,",")) action=i+1;
    }
    if (action==before.size() && qualifier.empty() && !quotedPrefix) {
      std::vector<std::string> choices{ms?"ADD":"ADD COLUMN","DROP COLUMN"};
      if (!sqlite) choices.push_back("ADD CONSTRAINT");
      if (pg || ms || family) choices.push_back("ALTER COLUMN");
      if (!ms) choices.push_back("RENAME COLUMN");
      if (family || ora) choices.push_back(ora?"MODIFY":"MODIFY COLUMN");
      if (family) choices.push_back("CHANGE COLUMN");
      return keywords(std::move(choices));
    }
    const auto columns=[&](std::vector<std::string> choices) {
      if (!qualifier.empty()) return false;
      if (catalog && action>at) {
        const auto end=before[action-1].range.begin;
        const auto completed=compile(dialect,source.substr(base,end-base),options.language,catalog,names);
        if (completed->ok() && completed->model.roots.size()==1) {
          auto overlay=std::make_unique<ScriptContext>(dialect,catalog,names);
          if (!overlay->observe(completed->model,completed->model.roots.front()))
            alterActions=std::move(overlay);
        }
      }
      if (!quotedPrefix)
        for (const auto &choice:choices)
          add(choice,choice,"ALTER TABLE clause",CompletionKind::Keyword,135);
      columnListTarget=std::move(target);
      expect=Expect::WriteColumn;
      return true;
    };
    const bool drop=word(action,"DROP"), alter=word(action,"ALTER"), rename=word(action,"RENAME");
    const bool modify=word(action,"MODIFY"), change=word(action,"CHANGE");
    const bool addColumn=word(action,"ADD");
    if ((!drop && !alter && !rename && !modify && !change && !addColumn) ||
        (alter && !pg && !ms && !family) || (rename && ms) ||
        (modify && !ora && !family) || (change && !family)) return false;
    auto slot=action+1;
    const bool explicitColumn=word(slot,"COLUMN");
    if (explicitColumn) ++slot;
    const bool addGuard=tableGuard || (tidb && explicitColumn);
    if (addColumn && addGuard && word(slot,"IF")) {
      if (slot+1==before.size()) return keywords({"NOT EXISTS"});
      if (!word(slot+1,"NOT")) return false;
      if (slot+2==before.size()) return keywords({"EXISTS"});
      if (!word(slot+2,"EXISTS")) return false;
      slot+=3;
      if (slot==before.size()) {
        expect=Expect::ObjectDeclaration;
        return true;
      }
    }
    if (drop && dropGuard && word(slot,"IF")) {
      if (slot+1==before.size()) return keywords({"EXISTS"});
      if (!word(slot+1,"EXISTS")) return false;
      slot+=2;
    }
    if (drop && slot==before.size()) {
      if (explicitColumn)
        return columns(dropGuard && slot==action+2?std::vector<std::string>{"IF EXISTS"}:std::vector<std::string>{});
      if (sqlite && !explicitColumn) return keywords({"COLUMN"});
      if (slot!=action+1) return columns({});
      std::vector<std::string> choices{"COLUMN","CONSTRAINT"};
      if (family) choices.push_back("PRIMARY KEY");
      if (dropGuard) choices.push_back("IF EXISTS");
      return columns(std::move(choices));
    }
    if (drop && !explicitColumn && (word(slot,"CONSTRAINT") || word(slot,"PRIMARY")))
      return false;
    if (rename && !explicitColumn) {
      if (slot==before.size())
        return pg?columns({"COLUMN","TO"}):keywords({"COLUMN","TO"});
      if (word(slot,"TO")) {
        expect=Expect::ObjectDeclaration;
        return true;
      }
    }
    if (modify && ora && symbol(slot,"(")) {
      ++slot;
      // Oracle grouped MODIFY entries start after the opening parenthesis or
      // the last comma in that same group, outside type/default expressions.
      std::size_t nesting=1;
      for (auto i=slot;i<before.size() && nesting;++i) {
        if (symbol(i,"(")) ++nesting;
        else if (symbol(i,")")) --nesting;
        else if (nesting==1 && symbol(i,",")) slot=i+1;
      }
    }
    if (slot==before.size() && (alter || rename || modify || change)) {
      if (alter && ms && !explicitColumn) return keywords({"COLUMN"});
      return columns(!explicitColumn && !ora?std::vector<std::string>{"COLUMN"}:std::vector<std::string>{});
    }
    if (!identifier(slot)) {
      if (addColumn && slot==before.size()) {
        if (addGuard)
          return explicitColumn?keywords({"IF NOT EXISTS"}):keywords({"COLUMN","CONSTRAINT","IF NOT EXISTS"});
        if (!explicitColumn)
          return ms?keywords({"CONSTRAINT"}):sqlite?keywords({"COLUMN"}):keywords({"COLUMN","CONSTRAINT"});
        expect=Expect::ObjectDeclaration;
        return true;
      }
      return false;
    }
    if (slot+1==before.size() && qualifier.empty()) {
      if (drop && pg) return keywords({"CASCADE","RESTRICT"});
      if (rename && (explicitColumn || pg)) return keywords({"TO"});
      if (alter && pg) return keywords({"TYPE","SET DATA TYPE","SET DEFAULT","DROP DEFAULT","SET NOT NULL","DROP NOT NULL"});
      if (alter && family) return keywords({"SET DEFAULT","DROP DEFAULT"});
      if (change) { expect=Expect::ObjectDeclaration; return true; }
    }
    if (alter && (pg || family) && (word(slot+1,"SET") || word(slot+1,"DROP"))) {
      if (slot+2==before.size()) {
        if (!pg) return keywords({"DEFAULT"});
        return word(slot+1,"SET")?keywords({"DATA TYPE","DEFAULT","NOT NULL"}):keywords({"DEFAULT","NOT NULL"});
      }
      if (pg && slot+3==before.size() && word(slot+1,"SET") && word(slot+2,"DATA"))
        return keywords({"TYPE"});
      if (pg && slot+3==before.size() && word(slot+2,"NOT")) return keywords({"NULL"});
    }
    auto type=slot+1;
    if (change && identifier(type)) ++type;
    if ((alter && ms) || modify || addColumn || (change && type==slot+2)) {
      if (type+2*qualifier.size()!=before.size()) return false;
      if (addColumn && (word(slot,"CONSTRAINT") || word(slot,"PRIMARY") || word(slot,"FOREIGN") ||
                        word(slot,"CHECK") || word(slot,"UNIQUE"))) return false;
      expect=Expect::Type;
      return true;
    }
    if (addColumn || (rename && word(slot+1,"TO"))) {
      expect=Expect::ObjectDeclaration;
      return true;
    }
    return false;
  }

  bool triggerRowGrammar() {
    if (!catalog || parameterPrefix || qualifier.size()!=1 || before.empty()) return false;
    const auto row=fold(qualifier.front());
    if (row!="NEW" && row!="OLD") return false;
    if (std::any_of(visible.begin(),visible.end(),[&](const auto &relation) {
          return !relation.table.pseudo && fold(relation.alias)==row;
        })) return false;
    std::optional<std::size_t> trigger;
    auto start=before.size();
    while (start && !trigger) {
      --start;
      if ((start&63u)==0) stop();
      trigger=triggerKeyword(before,start);
    }
    if (!trigger || triggerTerminator(before,*trigger+1)<before.size()) return false;
    auto at=*trigger+1;
    while (at<before.size() && !wordAt(before,at,"ON")) {
      if ((at&63u)==0) stop();
      ++at;
    }
    if (++at>=before.size() || !ident(before[at++])) return false;
    while (at<before.size() && isSymbol(before[at],".")) {
      if (++at>=before.size() || !ident(before[at++])) return false;
    }
    const bool sqlite=dialect==Dialect::SQLite;
    if (wordAt(before,at,"FOR") && wordAt(before,at+1,"EACH") && wordAt(before,at+2,"ROW")) at+=3;
    else if (!sqlite) return false;
    if (!sqlite && (wordAt(before,at,"PRECEDES") || wordAt(before,at,"FOLLOWS"))) {
      if (++at>=before.size() || !ident(before[at++])) return false;
    }
    if (at>=before.size()-1) return false;
    const auto begin=before[start].range.begin,end=before[at-1].range.end;
    const auto header=source.substr(begin,end-begin);
    const auto parsedHeader=compile(dialect,header+(sqlite?" BEGIN SELECT 1; END":" SET @nvd_completion = 1"),
                                    options.language,catalog,names);
    if (!parsedHeader->syntaxValid || parsedHeader->model.roots.size()!=1) return false;
    const auto &statement=parsedHeader->model.at(parsedHeader->model.roots.front()-1);
    if (!statement.trigger || statement.trigger->drop) return false;
    expect=Expect::ColumnList;

    if (!sqlite) {
      auto step=at;
      for (auto i=at;i<before.size();++i) {
        if ((i&63u)==0) stop();
        if (isSymbol(before[i],";") || wordAt(before,i,"BEGIN")) step=i+1;
      }
      if (wordAt(before,step,"SET")) {
        bool assigned=false;
        std::size_t depth=0;
        for (auto i=step+1;i<before.size();++i) {
          if ((i&63u)==0) stop();
          if (isSymbol(before[i],"(")) ++depth;
          else if (isSymbol(before[i],")") && depth) --depth;
          else if (!depth && isSymbol(before[i],"=")) assigned=true;
          else if (!depth && isSymbol(before[i],",")) assigned=false;
        }
        if (!assigned && (row=="OLD" || statement.trigger->timing!=TriggerTiming::Before)) return true;
      }
    }
    const auto image=std::find_if(statement.tables.begin(),statement.tables.end(),[&](const auto &table) {
      return table.pseudo && fold(table.alias)==row;
    });
    if (image==statement.tables.end() || !image->definition) return true;
    std::size_t visited=0;
    for (const auto &column:image->definition->columns) {
      if (++visited>options.maxCatalogEntries) {result.truncated=true; break;}
      add(column.name,spelling(column.name,quotedPrefix),row+" · "+valueTypeName(column.type),CompletionKind::Column,145);
    }
    return true;
  }

  bool distributionGrammar() {
    if ((dialect != Dialect::TiDB && dialect != Dialect::OceanBase) || before.size() < 2 ||
        quotedPrefix || parameterPrefix || !qualifier.empty()) return false;
    const auto word = [&](std::size_t i) {
      return i < before.size() && before[i].kind == TokenKind::Word ?
             fold(before[i].value) : std::string{};
    };
    const auto offer = [&](std::vector<std::string> choices) {
      keywordChoices = std::move(choices);
      expect = Expect::FixedKeywords;
      return true;
    };
    const bool tidb = dialect == Dialect::TiDB;
    const auto first = word(0), last = word(before.size() - 1);
    if (tidb && first == "ADMIN") {
      if (before.size() == 2 && last == "SHOW") return offer({"DDL"});
      if (before.size() == 3 && last == "DDL") return offer({"JOBS"});
      if (word(3) == "JOBS" &&
          (before.size() == 4 || (before.size() == 5 && before.back().kind == TokenKind::Number)))
        return offer({"WHERE"});
      return false;
    }
    const bool create = first == "CREATE", alter = first == "ALTER";
    if (!create && !alter) return false;
    if (!tidb && create && word(1) == "TABLEGROUP" && before.size() > 2) {
      if (last == "SHARDING") return offer({"= 'NONE'", "= 'PARTITION'", "= 'ADAPTIVE'"});
      if (before.back().value == "=") return offer({"'NONE'", "'PARTITION'", "'ADAPTIVE'"});
      if (ident(before.back())) return offer({"SHARDING"});
      return false;
    }
    if (!tidb && create && (word(1) == "INDEX" || (word(1) == "UNIQUE" && word(2) == "INDEX")) &&
        before.back().value == ")")
      return offer({"LOCAL", "GLOBAL"});
    if (word(1) != "TABLE" || before.size() < 3) return false;
    if (last == "PLACEMENT" && tidb) return offer({"POLICY"});
    if (last == "SET" && !tidb && alter) return offer({"TABLEGROUP"});
    if (alter && (before.size() == 3 ||
        (before.size() == 5 && before[3].value == ".")) && ident(before.back()))
      return tidb ? offer({"PLACEMENT POLICY", "SHARD_ROW_ID_BITS", "AUTO_RANDOM_BASE", "ADD", "DROP", "MODIFY", "RENAME"})
                  : offer({"SET TABLEGROUP", "ADD", "DROP", "MODIFY", "RENAME"});
    if (!create) return false;

    std::vector<std::size_t> opens;
    std::optional<std::size_t> closed;
    for (std::size_t i = 2; i < before.size(); ++i) {
      if (before[i].kind != TokenKind::Symbol) continue;
      if (before[i].value == "(") opens.push_back(i);
      if (before[i].value == ")" && !opens.empty()) {
        if (i + 1 == before.size()) closed = opens.back();
        opens.pop_back();
      }
    }
    if (closed && opens.empty())
      return tidb ? offer({"PLACEMENT POLICY", "SHARD_ROW_ID_BITS", "PRE_SPLIT_REGIONS",
                           "AUTO_RANDOM_BASE", "ENGINE", "PARTITION BY", "AS"})
                  : offer({"TABLEGROUP", "PARTITION BY", "AS"});
    if (tidb && opens.size() == 1) {
      const auto key = closed ? *closed : before.size();
      if (key >= 2 && word(key - 2) == "PRIMARY" && word(key - 1) == "KEY")
        return offer({"CLUSTERED", "NONCLUSTERED", "AUTO_RANDOM"});
      if (last == "BIGINT" || last == "INT8" || last == "UNSIGNED")
        return offer({"AUTO_RANDOM", "PRIMARY KEY", "NOT NULL", "DEFAULT"});
    }
    return false;
  }

  bool inspectionGrammar() {
    if (!mysql(dialect) || before.empty()) return false;
    auto word = [&](std::size_t i) { return i < before.size() && before[i].kind == TokenKind::Word ? fold(before[i].value) : std::string{}; };
    auto keywords = [&](std::vector<std::string> words) {
      inspectionWords = std::move(words); expect = Expect::InspectionKeyword; return true;
    };
    auto database = [&] { objectKind=CatalogObjectKind::Database; expect=Expect::CatalogObject; return true; };
    const auto first=word(0);
    if (first == "DESCRIBE" || first == "DESC") {
      if (before.size() == 1 || before.back().value == ".") { expect=Expect::Relation; return true; }
      return keywords({}); // Optional column names/patterns remain ordinary typed input.
    }
    if (first != "SHOW") return false;
    for (const auto &token : before) if (token.kind==TokenKind::Word && fold(token.value)=="WHERE") return false;
    std::size_t head=1;
    while (head<before.size() && (word(head)=="EXTENDED" || word(head)=="FULL" || word(head)=="GLOBAL" || word(head)=="SESSION")) ++head;
    if (head==before.size()) {
      if (word(head-1)=="GLOBAL" || word(head-1)=="SESSION") return keywords({"VARIABLES","STATUS"});
      if (word(head-1)=="FULL") return keywords({"TABLES","COLUMNS","FIELDS","PROCESSLIST"});
      if (word(head-1)=="EXTENDED") return keywords({"FULL","TABLES","COLUMNS","FIELDS","INDEX"});
      std::vector<std::string> words{"DATABASES","SCHEMAS","TABLES","COLUMNS","FIELDS","INDEX","INDEXES","KEYS","TABLE STATUS","VARIABLES","STATUS","PROCESSLIST","CREATE","FULL","EXTENDED","GLOBAL","SESSION"};
      if (dialect == Dialect::OceanBase) words.push_back("TABLEGROUPS");
      return keywords(std::move(words));
    }
    const auto kind=word(head), last=word(before.size()-1);
    if (kind=="CREATE") {
      if (head+1==before.size()) {
        if (dialect == Dialect::TiDB) return keywords({"TABLE","VIEW","DATABASE","SCHEMA"});
        if (dialect == Dialect::OceanBase) return keywords({"TABLE","VIEW","DATABASE","SCHEMA","FUNCTION","PROCEDURE","TRIGGER","EVENT","TABLEGROUP"});
        return keywords({"TABLE","VIEW","DATABASE","SCHEMA","FUNCTION","PROCEDURE","TRIGGER","EVENT"});
      }
      static const std::map<std::string,CatalogObjectKind> kinds={
        {"TABLE",CatalogObjectKind::Table},{"VIEW",CatalogObjectKind::View},{"DATABASE",CatalogObjectKind::Database},
        {"SCHEMA",CatalogObjectKind::Database},{"FUNCTION",CatalogObjectKind::Function},{"PROCEDURE",CatalogObjectKind::Procedure},
        {"TRIGGER",CatalogObjectKind::Trigger},{"EVENT",CatalogObjectKind::Event}};
      auto found=kinds.find(word(head+1));
      if(found==kinds.end())return keywords({});
      if (found->second==CatalogObjectKind::Database && last=="IF") return keywords({"NOT EXISTS"});
      if (found->second==CatalogObjectKind::Database && last=="NOT") return keywords({"EXISTS"});
      objectKind=found->second;expect=Expect::CatalogObject;return true;
    }
    if (kind=="TABLE" && head+1==before.size()) return keywords({"STATUS"});
    const bool tables=kind=="TABLES" || kind=="TABLE";
    const bool columns=kind=="COLUMNS" || kind=="FIELDS";
    const bool indexes=kind=="INDEX" || kind=="INDEXES" || kind=="KEYS";
    if(columns || indexes) {
      std::size_t from=head+1;
      if(from==before.size())return keywords({"FROM","IN"});
      if(word(from)!="FROM" && word(from)!="IN")return keywords({});
      const auto start=++from;
      if(from==before.size()){expect=Expect::Relation;return true;}
      // Read only the qualified target's token shape; no network or execution.
      if(ident(before[from]))++from;
      while(from+1<before.size() && before[from].value=="." && ident(before[from+1]))from+=2;
      if(from<before.size() && before[from].value=="." && from+1==before.size()) {expect=Expect::Relation;return true;}
      if(from==before.size() && start<from)return keywords(indexes?std::vector<std::string>{"FROM","IN","WHERE"}:std::vector<std::string>{"FROM","IN","LIKE","WHERE"});
      if(from<before.size() && (word(from)=="FROM" || word(from)=="IN")) {
        if(from+1==before.size())return database();
        return keywords(indexes?std::vector<std::string>{"WHERE"}:std::vector<std::string>{"LIKE","WHERE"});
      }
      return keywords({});
    }
    if(tables && (last=="FROM" || last=="IN"))return database();
    if(kind=="PROCESSLIST")return keywords({});
    if(tables)return keywords({"FROM","IN","LIKE","WHERE"});
    if(kind=="DATABASES" || kind=="SCHEMAS" || kind=="VARIABLES" || kind=="STATUS")return keywords({"LIKE","WHERE"});
    return keywords({});
  }
  bool loadableFunctionGrammar() {
    if ((dialect!=Dialect::MySQL && dialect!=Dialect::MariaDB) || quotedPrefix ||
        parameterPrefix || !qualifier.empty()) return false;
    const auto word=[&](std::size_t i,std::string_view value) {
      return i<before.size() && before[i].kind==TokenKind::Word && fold(before[i].value)==value;
    };
    if (!word(0,"CREATE")) return false;
    const auto choices=[&](std::vector<std::string> values) {
      keywordChoices=std::move(values);
      expect=Expect::FixedKeywords;
      return true;
    };
    const auto typed=fold(prefix);
    const bool replace=dialect==Dialect::MariaDB && word(1,"OR") && word(2,"REPLACE");
    std::size_t at=replace?3:1;
    if (at==before.size() && typed.size()>=2 && std::string_view("AGGREGATE").find(typed)==0)
      return choices({"AGGREGATE FUNCTION"});
    const bool aggregate=word(at,"AGGREGATE");
    if (aggregate && ++at==before.size()) return choices({"FUNCTION"});
    if (!word(at++,"FUNCTION")) return false;
    if (aggregate && !replace && at==before.size()) return choices({"IF NOT EXISTS"});
    if (word(at,"IF")) {
      ++at;
      if (aggregate && at==before.size()) return choices({"NOT EXISTS"});
      if (!word(at++,"NOT")) return false;
      if (aggregate && at==before.size()) return choices({"EXISTS"});
      if (!word(at++,"EXISTS")) return false;
    }
    if (at>=before.size() || !ident(before[at++])) return false;
    if (at==before.size()) {
      if (aggregate || (!typed.empty() && std::string_view("RETURNS").find(typed)==0))
        return choices({"RETURNS"});
      return false;
    }
    if (!word(at++,"RETURNS")) return false;
    if (at==before.size()) return choices({"STRING","INTEGER","REAL","DECIMAL"});
    if (!word(at,"STRING") && !word(at,"INTEGER") && !word(at,"INT") &&
        !word(at,"REAL") && !word(at,"DECIMAL")) return false;
    if (++at==before.size()) return choices({"SONAME"});
    return word(at,"SONAME") && choices({});
  }
  bool mysqlViewHeaderGrammar() {
    if (dialect != Dialect::MySQL && dialect != Dialect::MariaDB) return false;
    const auto word = [&](std::size_t i, std::string_view text) {
      return i < before.size() && before[i].kind == TokenKind::Word && fold(before[i].value) == text;
    };
    const auto symbol = [&](std::size_t i, std::string_view text) {
      return i < before.size() && before[i].kind == TokenKind::Symbol && before[i].value == text;
    };
    if (!word(0,"CREATE")) return false;
    std::size_t at = word(1,"OR") && word(2,"REPLACE") ? 3 : 1;
    if (!word(at,"ALGORITHM") && !word(at,"DEFINER") && !word(at,"SQL")) return false;
    const auto choices = [&](std::vector<std::string> items) {
      keywordChoices = std::move(items);
      expect = Expect::FixedKeywords;
      return true;
    };
    const auto equals = [&] { return choices({"="}); };
    const bool algorithm = word(at,"ALGORITHM");
    if (algorithm) {
      if (++at == before.size()) return equals();
      if (!symbol(at++,"=")) return false;
      if (at == before.size()) return choices({"MERGE","TEMPTABLE","UNDEFINED"});
      if (!word(at,"MERGE") && !word(at,"TEMPTABLE") && !word(at,"UNDEFINED")) return false;
      if (++at == before.size()) return choices({"DEFINER","SQL SECURITY","VIEW"});
    }
    if (word(at,"DEFINER")) {
      if (++at == before.size()) return equals();
      if (!symbol(at++,"=")) return false;
      if (at == before.size()) return choices({"CURRENT_USER"});
      if (word(at,"CURRENT_USER")) {
        ++at;
        if (symbol(at,"(")) {
          if (!symbol(at + 1,")")) return false;
          at += 2;
        }
      } else {
        const auto accountPart = [&](std::size_t i) {
          return i < before.size() && (ident(before[i]) || before[i].kind == TokenKind::String);
        };
        if (!accountPart(at++)) return false;
        if (symbol(at,"@")) {
          if (!accountPart(++at)) return choices({});
          ++at;
        }
      }
      if (at == before.size()) {
        auto items = std::vector<std::string>{"SQL SECURITY","VIEW"};
        if (!algorithm) items.insert(items.end(),{"FUNCTION","PROCEDURE","TRIGGER"});
        return choices(std::move(items));
      }
    }
    if (word(at,"SQL")) {
      if (++at == before.size()) return choices({"SECURITY"});
      if (!word(at++,"SECURITY")) return false;
      if (at == before.size()) return choices({"DEFINER","INVOKER"});
      if (!word(at,"DEFINER") && !word(at,"INVOKER")) return false;
      if (++at == before.size()) return choices({"VIEW"});
    }
    if (word(at,"VIEW") && at + 1 + qualifier.size() * 2 == before.size()) {
      expect = Expect::ObjectDeclaration;
      return true;
    }
    return false;
  }
  bool objectGrammar() {
    if(before.empty())return false;
    auto word=[&](std::size_t i){return i<before.size() && before[i].kind==TokenKind::Word?fold(before[i].value):std::string{};};
    const auto first=word(0),last=word(before.size()-1);
    std::vector<std::string> calls;
    for(std::size_t i=0;i<before.size();++i) {
      if(before[i].value=="(")calls.push_back(i?word(i-1):std::string{});
      else if(before[i].value==")" && !calls.empty())calls.pop_back();
    }
    auto typeSlot=before.size();
    if(!qualifier.empty() && before.size()>=2*qualifier.size())typeSlot-=2*qualifier.size();
    const auto prior=typeSlot?word(typeSlot-1):std::string{};
    castTypeSlot=prior=="AS" && !calls.empty() && calls.back()=="CAST";
    if(castTypeSlot ||
       (before.back().value=="::") || (typeSlot && before[typeSlot-1].value=="::") ||
       (prior=="TYPE" && first=="ALTER" && word(1)=="TABLE")) {expect=Expect::Type;return true;}
    if(!qualifier.empty() && typeSlot>=2 && ident(before[typeSlot-1]) &&
        (before[typeSlot-2].value=="(" || before[typeSlot-2].value=="," || word(typeSlot-2)=="COLUMN" || word(typeSlot-2)=="DECLARE") &&
        (first=="CREATE" || first=="DECLARE" || first=="ALTER")) {expect=Expect::Type;return true;}

    if(first=="USE"){objectKind=CatalogObjectKind::Database;expect=Expect::CatalogObject;return true;}
    if(first=="SET" && before.size()>=3 && word(1)=="SEARCH_PATH" &&
       (word(2)=="TO" || before[2].value=="=")) {objectKind=CatalogObjectKind::Schema;expect=Expect::CatalogObject;return true;}
    if(before.back().value=="::"){expect=Expect::Type;return true;}
    const bool create=first=="CREATE",target=create || first=="DROP" || first=="ALTER" ||
      first=="COMMENT" || first=="TRUNCATE" || first=="RENAME" || first=="REFRESH";
    if(!target)return false;
    std::size_t at=1;
    for(;at<before.size();++at) {
      auto w=word(at);
      if(w=="OR" || w=="REPLACE" || w=="TEMP" || w=="TEMPORARY" || w=="UNIQUE" ||
          w=="CONCURRENTLY" || w=="MATERIALIZED" || w=="ON" || w=="PUBLIC" || w=="GLOBAL" || w=="UNLOGGED")continue;
      break;
    }
    if(at>=before.size())return false;
    const auto object=word(at);
    static const std::map<std::string,CatalogObjectKind> kinds={
      {"TABLE",CatalogObjectKind::Table},{"VIEW",CatalogObjectKind::View},{"INDEX",CatalogObjectKind::Index},
      {"SEQUENCE",CatalogObjectKind::Sequence},{"SCHEMA",CatalogObjectKind::Schema},{"DATABASE",CatalogObjectKind::Database},
      {"TRIGGER",CatalogObjectKind::Trigger},{"FUNCTION",CatalogObjectKind::Function},{"PROCEDURE",CatalogObjectKind::Procedure},
      {"PROC",CatalogObjectKind::Procedure},{"TYPE",CatalogObjectKind::Type},{"SYNONYM",CatalogObjectKind::Synonym},
      {"PACKAGE",CatalogObjectKind::Package},{"EVENT",CatalogObjectKind::Event}};
    auto found=kinds.find(object);if(found==kinds.end())return false;
    objectKind=found->second;
    if(object=="VIEW" && at && word(at-1)=="MATERIALIZED")objectKind=CatalogObjectKind::MaterializedView;
    // CREATE names are declarations, not expression or existing-object uses.
    // Only the name slot is handled here; column/body grammar remains separate.
    for(std::size_t i=at+1;i<before.size();++i) {
      auto w=word(i);
      if(w=="ON" && create && (object=="INDEX" || object=="TRIGGER")) {
        if(i==before.size()-1 || before.back().value=="."){expect=Expect::Relation;return true;}
        return false;
      }
      if(before[i].value=="(" || before[i].value==")" || before[i].value==";" ||
         w=="AS" || w=="IS" || w=="TO" || w=="RENAME" || w=="ADD" || w=="SET" || w=="RESET" || w=="OWNER" ||
         w=="ALTER" || w=="COLUMN" || w=="MODIFY" || w=="CHANGE" || w=="DROP" || w=="ENABLE" || w=="DISABLE" || w=="RESTART" || w=="INCREMENT")return false;
      if(before[i].value!="," && before[i].value!="." && !ident(before[i]))return false;
    }
    expect=create?Expect::ObjectDeclaration:Expect::CatalogObject;return true;
  }
  bool rowFunctionEndsAt(std::size_t end) {
    if (dialect!=Dialect::PostgreSQL || end<4 || before[end-1].kind!=TokenKind::Symbol ||
        before[end-1].value!=")") return false;
    const auto word=[&](std::size_t at,std::string_view spelling) {
      return before[at].kind==TokenKind::Word && fold(before[at].value)==spelling;
    };
    std::size_t closed=0;
    for (auto i=end; i>1 && end-i<384;) {
      stop();
      const auto &token=before[--i];
      if (token.kind!=TokenKind::Symbol) continue;
      if (token.value==")") ++closed;
      else if (token.value=="(" && --closed==0) {
        if (!word(i-1,"UNNEST") && !word(i-1,"GENERATE_SERIES")) return false;
        auto name=i-1;
        if (name>=2 && before[name-1].kind==TokenKind::Symbol &&
            before[name-1].value=="." && word(name-2,"PG_CATALOG"))
          name-=2;
        if (name && word(name-1,"LATERAL")) --name;
        if (!name) return false;
        if (word(name-1,"FROM") || word(name-1,"JOIN")) return true;
        if (before[name-1].kind!=TokenKind::Symbol || before[name-1].value!=",") return false;
        // Commas in SELECT lists do not introduce table sources.
        std::size_t depth=0;
        for (auto at=name-1;at>0 && name-at<384;) {
          const auto &token=before[--at];
          if (token.kind==TokenKind::Symbol && token.value==")") ++depth;
          if (token.kind==TokenKind::Symbol && token.value=="(") {
            if (!depth) return false;
            --depth;
          }
          if (depth || token.kind!=TokenKind::Word) continue;
          if (word(at,"FROM")) return true;
          if (word(at,"SELECT") || word(at,"WHERE") || word(at,"BY") || word(at,"HAVING")) return false;
        }
        return false;
      }
    }
    return false;
  }
  bool rowOrdinalityGrammar() {
    if (before.size()<5 || quotedPrefix || parameterPrefix || !qualifier.empty() ||
        before.back().kind!=TokenKind::Word || fold(before.back().value)!="WITH" ||
        !rowFunctionEndsAt(before.size()-1)) return false;
    const auto after=std::lower_bound(tokens.begin(),tokens.end(),result.replacement.end,
        [](const Token &token,std::size_t at) { return token.range.begin<at; });
    const auto next=std::find_if(after,tokens.end(),
        [](const Token &token) { return token.kind!=TokenKind::Comment; });
    if (next!=tokens.end() && next->kind==TokenKind::Word && fold(next->value)=="ORDINALITY")
      return false;
    keywordChoices={"ORDINALITY"};
    expect=Expect::FixedKeywords;
    return true;
  }
  bool orderedSetGrammar() {
    if (dialect!=Dialect::PostgreSQL || quotedPrefix || parameterPrefix ||
        !qualifier.empty() || before.size()<3) return false;
    const auto i=before.size()-1;
    if (wordAt(before,i,"WITHIN") && isSymbol(before[i-1],")"))
      keywordChoices=options.language.calciteExtensions?std::vector<std::string>{"GROUP","DISTINCT"}:
          std::vector<std::string>{"GROUP"};
    else if (i>=2 && isSymbol(before[i],"(") && wordAt(before,i-1,"GROUP") && wordAt(before,i-2,"WITHIN"))
      keywordChoices={"ORDER BY"};
    else if (i>=3 && wordAt(before,i,"ORDER") && isSymbol(before[i-1],"(") &&
             wordAt(before,i-2,"GROUP") && wordAt(before,i-3,"WITHIN"))
      keywordChoices={"BY"};
    else if (isSymbol(before[i],")")) {
      std::size_t depth=1, open=i;
      while (open) {
        if ((open&63u)==0) stop();
        --open;
        if (isSymbol(before[open],")")) ++depth;
        if (isSymbol(before[open],"(") && --depth==0) break;
      }
      if (!open || depth ||
          (!wordAt(before,open-1,"MODE") && !wordAt(before,open-1,"PERCENTILE_DISC") &&
           !wordAt(before,open-1,"PERCENTILE_CONT"))) return false;
      const auto end=skipHorizontal(result.replacement.end);
      const auto next=std::find_if(tokens.begin(),tokens.end(),[&](const auto &token) {
        return token.kind!=TokenKind::Comment && token.range.begin>=end;
      });
      if (next!=tokens.end() && next->kind==TokenKind::Word && fold(next->value)=="WITHIN") return false;
      const std::string clause="WITHIN GROUP (ORDER BY ";
      add("WITHIN GROUP",clause+")","ordered-set aggregate",CompletionKind::Keyword,150,clause.size());
    } else return false;
    expect=Expect::FixedKeywords;
    return true;
  }
  bool unpivotHeaderGrammar() {
    const bool nullOptions=unpivotNullOptions();
    if ((!nullOptions && dialect!=Dialect::SQLServer) || quotedPrefix || parameterPrefix ||
        !qualifier.empty() || before.size()<4) return false;
    const auto end=before.size();
    const auto keyword=[&](std::vector<std::string> choices) {
      keywordChoices=std::move(choices);
      expect=Expect::FixedKeywords;
      return true;
    };
    const auto nullMode=[&](std::size_t i) {
      return wordAt(before,i,"INCLUDE") || wordAt(before,i,"EXCLUDE");
    };
    if (nullOptions && wordAt(before,end-1,"UNPIVOT"))
      return keyword({"INCLUDE NULLS","EXCLUDE NULLS"});
    if (nullOptions && wordAt(before,end-2,"UNPIVOT") && nullMode(end-1))
      return keyword({"NULLS"});
    if (unpivotHasFor(end)) return keyword({"FOR"});
    if (unpivotHasIn(end)) return keyword({"IN"});
    return false;
  }
  bool unpivotInputGrammar() {
    if (!unpivotInputClosings) return false;
    expect=Expect::ColumnList;
    columnListDetail="input column of UNPIVOT";
    const auto markerName=canonicalIdentifier(dialect,marker,false);
    for (const auto *program:programs)
      for (const auto &query:program->queries)
        for (const auto &table:query.tables) {
          stop();
          if (!table.unpivot || !table.unpivotInput) continue;
          const auto &inputs=table.unpivot->inputs;
          const auto branch=std::find_if(inputs.begin(),inputs.end(),[&](const auto &input) {
            return std::find(input.columns.begin(),input.columns.end(),markerName)!=input.columns.end();
          });
          if (branch==inputs.end()) continue;
          const auto exclude=[&](const auto &input) {
            for (const auto &field:input.columns)
              if (field!=markerName) assignedColumns.insert(key(field));
          };
          if (table.unpivot->measures.size()==1)
            for (const auto &input:inputs) exclude(input);
          else exclude(*branch);
          for (const auto &field:table.unpivotInput->columns) listColumns.push_back(field.name);
          return true;
        }
    return true;
  }
  bool pivotGrammar() {
    if (pivotSite==PivotSite::None) return false;
    expect=Expect::FixedKeywords;
    if (pivotSite==PivotSite::For || pivotSite==PivotSite::In) {
      if (!quotedPrefix) keywordChoices={pivotSite==PivotSite::For?"FOR":"IN"};
    } else if (pivotSite==PivotSite::Aggregate) {
      const bool literalBuckets=unpivotNullOptions();
      if (!quotedPrefix)
        for (const std::string name:{"SUM","COUNT","AVG","MIN","MAX","COUNT_BIG"}) {
          if (literalBuckets && name=="COUNT_BIG") continue;
          const bool star=literalBuckets && name=="COUNT";
          const auto insert=name+(star?"(*)":"()");
          add(name,insert,"aggregate in PIVOT",CompletionKind::Function,145,name.size()+1,star?1:0);
        }
    } else {
      expect=Expect::ColumnList;
      columnListDetail=pivotSite==PivotSite::Argument?"aggregate input of PIVOT":"axis input of PIVOT";
      const auto markerName=canonicalIdentifier(dialect,marker,false);
      for (const auto *program:programs)
        for (const auto &query:program->queries)
          for (const auto &table:query.tables) {
            stop();
            if (!table.pivot || !table.pivotInput) continue;
            const auto &slot=pivotSite==PivotSite::Argument?table.pivot->argument:table.pivot->axes;
            if (std::find(slot.begin(),slot.end(),markerName)==slot.end()) continue;
            if (pivotSite==PivotSite::Axis)
              for (const auto &column:slot)
                if (column!=markerName) assignedColumns.insert(key(column));
            for (const auto &field:table.pivotInput->columns) listColumns.push_back(field.name);
            result.parsedScope=true;
            return true;
          }
    }
    return true;
  }
  bool selectContinuationGrammar() {
    if (before.size() < 2 || quotedPrefix || parameterPrefix || !qualifier.empty()) return false;
    // Read the nearest query block to the left of the cursor. Completed
    // nested expressions are opaque, and an unmatched '(' ends this block.
    std::vector<const Token *> terms;
    std::size_t closed = 0;
    bool foundSelect = false;
    for (std::size_t i = before.size(), scanned = 0; i && scanned < 384; --i, ++scanned) {
      if ((scanned & 63u) == 0) stop();
      const auto &t = before.at(i - 1);
      if (t.kind == TokenKind::Symbol && t.value == ")") {
        if (!closed) terms.push_back(&t);
        ++closed;
        continue;
      }
      if (t.kind == TokenKind::Symbol && t.value == "(") {
        if (!closed) break;
        --closed;
        continue;
      }
      if (closed) continue;
      if (t.kind == TokenKind::Symbol && t.value == ";") break;
      terms.push_back(&t);
      if (t.kind == TokenKind::Word && fold(t.value) == "SELECT") {
        foundSelect = true;
        break;
      }
    }
    if (!foundSelect || terms.size() < 2) return false;
    std::reverse(terms.begin(), terms.end());
    const auto word = [&](std::size_t i) {
      return i < terms.size() && terms[i]->kind == TokenKind::Word ? fold(terms[i]->value) : std::string{};
    };
    const auto lastIndex = terms.size() - 1;
    const auto last = word(lastIndex);
    std::size_t head = 0;
    static const std::set<std::string> clauses{
        "FROM", "JOIN", "WHERE", "ON", "GROUP", "HAVING", "ORDER",
        "LIMIT", "OFFSET", "FETCH", "WINDOW", "UNION", "EXCEPT", "INTERSECT",
        "INTO", "FOR", "RETURNING", "QUALIFY", "CONNECT", "START", "USING"};
    for (std::size_t i = 1; i <= lastIndex; ++i) {
      if (clauses.count(word(i)) && terms[i-1]->value != ".") head = i;
    }
    const auto clause = word(head);
    const auto &end = *terms.back();
    static const std::set<std::string> incomplete{
        "AS", "ALL", "DISTINCT", "TOP", "BY", "WHEN", "THEN", "ELSE", "CASE",
        "AND", "OR", "NOT", "IS", "LIKE", "ILIKE", "IN", "BETWEEN", "OVER", "FILTER"};
    const bool joinModifier = (clause == "FROM" || clause == "JOIN") &&
        (last == "LEFT" || last == "RIGHT" || last == "FULL" ||
         last == "INNER" || last == "CROSS" || last == "OUTER");
    const auto previous = word(lastIndex - 1);
    const auto &beforeEnd = *terms[lastIndex - 1];
    const bool wildcard = clause == "SELECT" && end.value == "*" &&
        (previous == "SELECT" || previous == "ALL" || previous == "DISTINCT" ||
         (beforeEnd.kind == TokenKind::Symbol && (beforeEnd.value == "." || beforeEnd.value == ",")));
    std::vector<std::string> choices;
    if ((clause == "GROUP" || clause == "ORDER") && head == lastIndex) {
      choices = {"BY"};
    } else if (joinModifier) {
      choices = {"JOIN"};
      if (last == "LEFT" || last == "RIGHT" || last == "FULL") choices.push_back("OUTER JOIN");
    } else if (head == lastIndex || incomplete.count(last) ||
               (end.kind == TokenKind::Symbol && end.value != ")" && !wildcard)) {
      return false;
    } else if (clause == "SELECT") {
      choices = {"FROM"};
      if (!wildcard && previous != "AS")
        choices.push_back("AS");
    } else if (clause == "JOIN") {
      bool requiresCondition = true;
      for (auto i = head; i && head - i < 4; --i) {
        const auto modifier = word(i - 1);
        if (modifier == "CROSS" || modifier == "NATURAL") requiresCondition = false;
        if (modifier != "LEFT" && modifier != "RIGHT" && modifier != "INNER" &&
            modifier != "FULL" && modifier != "OUTER") break;
      }
      if (requiresCondition) {
        choices = {"ON"};
        if (dialect != Dialect::SQLServer) choices.push_back("USING");
      }
    }
    if (choices.empty() && (clause == "FROM" || clause == "JOIN" || clause == "USING")) {
      choices = {"WHERE", "JOIN", "LEFT JOIN", "INNER JOIN", "GROUP BY", "ORDER BY"};
    } else if (clause == "GROUP" && word(head + 1) == "BY") {
      choices = {"HAVING", "ORDER BY"};
    } else if (clause == "ORDER" && word(head + 1) == "BY") {
      if (last != "ASC" && last != "DESC" && last != "FIRST" && last != "LAST")
        choices = {"ASC", "DESC"};
      if ((dialect == Dialect::PostgreSQL || dialect == Dialect::Oracle) &&
          last != "FIRST" && last != "LAST") {
        choices.push_back("NULLS FIRST");
        choices.push_back("NULLS LAST");
      }
    }
    bool finishedPredicate = false;
    if (clause == "WHERE" || clause == "HAVING" || clause == "ON") {
      static const std::set<std::string_view> signs{"=", "!=", "<>", "<", ">", "<=", ">=", "<=>"};
      bool pendingBetween = false;
      for (std::size_t i = head + 1; i < lastIndex; ++i) {
        const auto current = word(i);
        if (current == "BETWEEN") {
          pendingBetween = true;
          finishedPredicate = false;
        } else if (pendingBetween && current == "AND") {
          pendingBetween = false;
          finishedPredicate = true;
        } else if (current == "AND" || current == "OR") {
          finishedPredicate = false;
        } else if (current == "IS" || current == "LIKE" || current == "ILIKE" ||
                   current == "IN" || (terms[i]->kind == TokenKind::Symbol && signs.count(terms[i]->value))) {
          finishedPredicate = true;
        }
      }
      if (pendingBetween) choices = {"AND"};
      else if (finishedPredicate || last == "TRUE" || last == "FALSE") {
        choices = {"AND", "OR"};
        if (clause == "ON") choices.insert(choices.end(), {"JOIN", "LEFT JOIN", "WHERE"});
        if (clause != "HAVING") choices.push_back("GROUP BY");
        choices.push_back("ORDER BY");
      }
    }
    if (rowFunctionEndsAt(before.size()))
      choices.insert(choices.begin(),"WITH ORDINALITY");
    const bool page = !joinModifier && (clause == "FROM" || clause == "USING" ||
        ((clause == "GROUP" || clause == "ORDER") && word(head + 1) == "BY"));
    if (page) {
      if (mysql(dialect) || dialect == Dialect::PostgreSQL || dialect == Dialect::SQLite)
        choices.push_back("LIMIT");
      if (dialect == Dialect::Oracle || dialect == Dialect::PostgreSQL)
        choices.push_back("FETCH FIRST");
      if (dialect == Dialect::SQLServer && clause == "ORDER") choices.push_back("OFFSET");
    }
    const auto nextToken = std::lower_bound(tokens.begin(), tokens.end(), result.replacement.end,
        [](const Token &token, std::size_t at) { return token.range.begin < at; });
    const auto next = std::find_if(nextToken, tokens.end(), [](const Token &token) {
      return token.kind != TokenKind::Comment;
    });
    if (next != tokens.end() && next->kind == TokenKind::Word) {
      const auto nextWord = fold(next->value);
      choices.erase(std::remove_if(choices.begin(), choices.end(), [&](const auto &choice) {
        return choice.substr(0,choice.find(' ')) == nextWord;
      }), choices.end());
    }
    const auto typed = fold(prefix);
    if (choices.empty() || (!typed.empty() && std::none_of(choices.begin(), choices.end(),
        [&](const auto &choice) { return choice.rfind(typed, 0) == 0; }))) return false;
    keywordChoices = std::move(choices);
    expect = Expect::SelectContinuation;
    return true;
  }
  AliasClause aliasClauseAtCursor() const {
    if (!cursorQuery || cursorQuery->kind != StatementKind::Select ||
        !qualifier.empty() || parameterPrefix) return AliasClause::None;
    const auto identifierMarker = canonicalIdentifier(dialect, marker, false);
    const auto markerExpression = [&](ExprId id) {
      const auto &expr = cursorQuery->expressions.at(id);
      return expr.kind == ExprKind::Name && expr.name.size() == 1 &&
             expr.name.front() == identifierMarker;
    };
    for (const auto &order : cursorQuery->orderBy)
      if (markerExpression(order.expression)) return AliasClause::OrderBy;
    if (dialect == Dialect::PostgreSQL || mysql(dialect) || dialect == Dialect::SQLite)
      for (auto group : cursorQuery->groupBy)
        if (markerExpression(group)) return AliasClause::GroupBy;
    return AliasClause::None;
  }
  bool caseGrammar() {
    if (caseFrames.empty()) return false;
    expect=Expect::Expression;
    if (quotedPrefix || parameterPrefix || !qualifier.empty()) return true;
    const auto &frame=caseFrames.back();
    const auto &tail=before.back();
    static const std::set<std::string> operators{
        "AND","OR","NOT","IS","LIKE","ILIKE","BETWEEN","IN","COLLATE","FROM","DISTINCT","AT"};
    const bool complete=frame.anchor+1<before.size() &&
        ((ident(tail) && (tail.kind==TokenKind::Identifier || !operators.count(fold(tail.value)))) ||
         tail.kind==TokenKind::Number || tail.kind==TokenKind::String || tail.kind==TokenKind::Parameter ||
         isSymbol(tail,")") || isSymbol(tail,"]"));
    const auto next=nextCompletionToken();
    const auto offer=[&](const char *keyword) {
      if (next==tokens.end() || next->kind!=TokenKind::Word || fold(next->value)!=keyword)
        add(keyword,keyword,"CASE expression",CompletionKind::Keyword,175);
    };
    if (frame.phase==CasePhase::Subject && (complete || frame.anchor+1==before.size()))
      offer("WHEN");
    else if (complete && frame.phase==CasePhase::Condition && !frame.pendingBetween)
      offer("THEN");
    else if (complete && frame.phase==CasePhase::Result) {
      offer("WHEN");
      offer("ELSE");
      offer("END");
    } else if (complete && frame.phase==CasePhase::Fallback)
      offer("END");
    return true;
  }
  void grammar() {
    if (caseGrammar()) return;
    if (loadableFunctionGrammar()) return;
    if (routineSite.preparedNameTarget) {
      if (qualifier.empty() && !parameterPrefix)
        for (const auto &name:routineSite.preparedNames)
          add(name,spelling(name,quotedPrefix),"prepared in this procedure",CompletionKind::PreparedStatement,145);
      expect=Expect::FixedKeywords;
      return;
    }
    if (routineSite.labelTarget) {
      if (qualifier.empty() && !parameterPrefix)
        for (const auto &[label,loop]:routineSite.labels)
          add(label,spelling(label,quotedPrefix),loop?"loop label":"block label",CompletionKind::Label,150);
      expect=Expect::FixedKeywords;
      return;
    }
    if (routineSite.variableTarget) {
      if (!quotedPrefix && !parameterPrefix && qualifier.empty())
        keywordChoices=std::move(routineSite.keywords);
      expect=Expect::VariableTarget;
      return;
    }
    if (((!quotedPrefix && !routineSite.keywords.empty()) || !routineSite.conditionNames.empty()) &&
        !parameterPrefix && qualifier.empty()) {
      if (!quotedPrefix) keywordChoices=std::move(routineSite.keywords);
      for (const auto &condition:routineSite.conditionNames)
        add(condition,spelling(condition,quotedPrefix),"declared SQLSTATE condition",CompletionKind::Variable,145);
      expect=Expect::FixedKeywords;
      return;
    }
    if (routineSite.scalarVariable) {
      expect=Expect::ScalarVariable;
      return;
    }
    if (routineSite.cursorName) {
      expect = Expect::CursorName;
      return;
    }
    if (routineSite.statement && !quotedPrefix && !parameterPrefix &&
        qualifier.empty()) {
      if (!afterRoutineEnd) {
        expect = Expect::Statement;
        return;
      }
      if (routineSite.closesIf) {
        expect = Expect::FixedKeywords;
        endIfCompletion(true);
        return;
      }
    }
    if (unpivotInputGrammar() || unpivotHeaderGrammar() || pivotGrammar()) return;
    if (sqlServerOutput) {
      if (cursorQuery->kind==StatementKind::Merge && qualifier.empty() && !parameterPrefix && !quotedPrefix)
        add("$action","$action","MERGE action · nvarchar(10)",CompletionKind::Column,155);
      return;
    }
    if (jsonDocument) return;
    if (usingJoin) {
      expect = Expect::JoinColumn;
      return;
    }
    if (triggerRowGrammar() || constraintColumnGrammar() || indexColumnGrammar() ||
        conflictColumnGrammar() || conflictAssignmentGrammar() ||
        outputIntoGrammar() || writeColumnGrammar() || referenceColumnGrammar()) return;
    aliasClause = aliasClauseAtCursor();
    if (aliasClause == AliasClause::OrderBy && cursorQuery->setOperation) {
      expect = Expect::OutputColumn;
      return;
    }
    if(distributionGrammar())return;
    if(inspectionGrammar())return;
    if(orderedSetGrammar())return;
    if(windowReferenceGrammar() || windowGrammar())return;
    if(fetchGrammar())return;
    if(pgDiagnosticsGrammar())return;
    if(selectIntoGrammar())return;
    if(temporalGrammar())return;
    if (mysqlViewHeaderGrammar()) return;
    if (alterTableGrammar()) return;
    if(objectGrammar())return;
    if(rowOrdinalityGrammar() || selectContinuationGrammar())return;
    const auto boundary = [](const Token &token) {
      if (token.kind == TokenKind::Symbol) return token.value == ";";
      if (token.kind != TokenKind::Word) return false;
      const auto word = fold(token.value);
      return word == "BEGIN" || word == "THEN" || word == "ELSE" ||
             word == "LOOP" || word == "DO" || word == "TRY" || word == "CATCH";
    };
    if (before.empty() ||
        (before.back().kind == TokenKind::Symbol && before.back().value == ";") ||
        (routineHeader && boundary(before.back()))) {
      expect=Expect::Statement;
      return;
    }
    auto last = fold(before.back().value);
    std::string previous =
        before.size() > 1 ? fold(before[before.size() - 2].value) : "";
    if (routineHeader && (mysql(dialect) || dialect == Dialect::SQLServer) &&
        last == "SET" && before.back().kind == TokenKind::Word && before.size() > 1 &&
        boundary(before[before.size() - 2])) {
      expect = Expect::VariableTarget;
      return;
    }
    const bool arrayQuery=dialect==Dialect::PostgreSQL && previous=="ARRAY" &&
        before.size()>1 && before[before.size()-2].kind==TokenKind::Word;
    if (last=="(" && (previous=="AS" || previous=="FROM" || previous=="JOIN" || arrayQuery)) {
      expect=Expect::QueryStart;
      return;
    }
    std::string clause;
    for (const auto &t : before) {
      auto u = fold(t.value);
      if (u == "SELECT" || u == "FROM" || u == "WHERE" || u == "ON" ||
          u == "JOIN" || u == "TABLE" || u == "SET" || u == "VALUES" || u == "RETURNING" ||
          u == "CALL" || u == "EXEC" || u == "EXECUTE")
        clause = u;
    }
    if (last == "CALL" || last == "EXEC" || last == "EXECUTE" ||
        (last == "." &&
         (clause == "CALL" || clause == "EXEC" || clause == "EXECUTE")))
      expect = Expect::Procedure;
    else if (last == "CREATE" || (last == "REPLACE" && previous == "OR"))
      expect = Expect::CreateObject;
    else if (last == "FROM" || last == "JOIN" || last == "UPDATE" ||
             last == "INTO" || last == "REFERENCES" || last == "TABLE" ||
             last == "VIEW" || (last == "," && clause == "FROM"))
      expect = Expect::Relation;
    else if (last == "." && (clause == "FROM" || clause == "JOIN" || clause == "TABLE"))
      expect = Expect::Relation;
    else if (before.size() > 1 && ident(before.back()) &&
             (previous == "(" || previous == "," || previous == "DECLARE") &&
             std::any_of(before.begin(), before.end(), [](const auto &t) {
               return fold(t.value) == "CREATE" || fold(t.value) == "DECLARE";
             }))
      expect = Expect::Type;
  }
  void endIfCompletion(bool afterEnd) {
    auto range = result.replacement;
    if (afterEnd) {
      range.begin = before.back().range.begin;
    } else {
      const auto at = skipHorizontal(range.end);
      const auto next = std::lower_bound(tokens.begin(), tokens.end(), at,
          [](const Token &token, std::size_t offset) { return token.range.begin < offset; });
      if (next != tokens.end() && next->range.begin == at &&
          next->kind == TokenKind::Word && fold(next->value) == "IF")
        range.end = next->range.end;
    }
    const auto separator = skipHorizontal(range.end);
    if (separator < source.size() && source[separator] == ';' &&
        !std::binary_search(clientSeparators.begin(), clientSeparators.end(), separator))
      range.end = separator + 1;
    add("END IF;", "END IF;", "close the procedural IF block",
        CompletionKind::Keyword, 200, std::string::npos, 0, range,
        afterEnd ? "IF;" : "");
  }
  void ifStatementCompletion() {
    const auto after = skipHorizontal(result.replacement.end);
    const bool lineEnd = after == source.size() || source[after] == '\n' || source[after] == '\r';
    if (!options.snippets || !lineEnd) {
      add("IF", after == result.replacement.end ? "IF " : "IF",
          "procedural IF statement", CompletionKind::Keyword, 170);
      return;
    }
    const auto begin = result.replacement.begin;
    const auto newline = begin ? source.rfind('\n', begin - 1) : std::string::npos;
    const auto line = newline == std::string::npos ? 0 : newline + 1;
    const auto indent = source.substr(line, skipHorizontal(line) - line);
    add("IF", "IF condition THEN\n" + indent + "  \n" + indent + "END IF;",
        "procedural IF statement", CompletionKind::Snippet, 170, 3, 9);
  }
  std::optional<TableDefinition> definition(const VisibleRelation &relation) {
    if (relation.table.definition)
      return *relation.table.definition;
    if (catalog && !(sqlServerOutput && relation.table.pseudo))
      return catalog->findTable(names, relation.table.name);
    return {};
  }
  void joinColumns() {
    struct Field {
      const ColumnDefinition *definition;
      std::size_t count;
    };
    using Fields = std::map<std::string, Field>;
    const bool insensitive = mysql(dialect) || (catalog && catalog->columnNameEquals("a", "A"));
    const auto columnKey = [&](const std::string &name) { return insensitive ? fold(name) : name; };
    std::size_t inspected = 0;
    const auto collect = [&](const auto &visit, RelationId id, std::size_t depth)
        -> std::optional<Fields> {
      stop();
      if (!id || id > usingQuery->relations.size() || depth >= options.language.limits.nesting)
        return {};
      const auto &relation = usingQuery->relations[id - 1];
      if (!relation.left) {
        const auto &table = usingQuery->tables.at(relation.table);
        // A partial definition cannot prove that a name occurs only once.
        if (!table.definition || !table.definition->complete) return {};
        Fields fields;
        for (const auto &column : table.definition->columns) {
          stop();
          if (++inspected > options.maxCatalogEntries) {
            result.truncated = true;
            return {};
          }
          auto field = fields.try_emplace(columnKey(column.name), Field{&column, 0}).first;
          ++field->second.count;
        }
        return fields;
      }
      auto left = visit(visit, relation.left, depth + 1);
      auto right = visit(visit, relation.right, depth + 1);
      if (!left || !right) return {};
      std::set<std::string> merged;
      for (const auto &name : relation.usingColumns) merged.insert(columnKey(name));
      for (const auto &[name, field] : *right) {
        stop();
        const auto [existing, fresh] = left->try_emplace(name, field);
        if (!fresh && ((!relation.natural && !merged.count(name)) ||
                       existing->second.count != 1 || field.count != 1))
          existing->second.count += field.count;
      }
      return left;
    };
    auto left = collect(collect, usingJoin->left, 0);
    auto right = collect(collect, usingJoin->right, 0);
    if (!left || !right) return;
    std::set<std::string> used;
    const auto identifierMarker = canonicalIdentifier(dialect, marker, false);
    for (const auto &name : usingJoin->usingColumns)
      if (name != identifierMarker) used.insert(columnKey(name));
    for (const auto &[name, field] : *left) {
      stop();
      auto match = right->find(name);
      if (field.count != 1 || match == right->end() || match->second.count != 1 || used.count(name))
        continue;
      const auto &label = field.definition->name;
      add(label, spelling(label, quotedPrefix), "column on both sides of the join",
          CompletionKind::Column, 130);
    }
  }
  void outputAliases() {
    if (aliasClause == AliasClause::None) return;
    const bool grouping = aliasClause == AliasClause::GroupBy;
    const bool insensitive = mysql(dialect) || (catalog && catalog->columnNameEquals("a", "A"));
    const auto nameKey = [&](const std::string &name) { return insensitive ? fold(name) : name; };
    std::set<std::string> inputNames;
    std::size_t inspected = 0;
    if (grouping) {
      for (const auto &table : cursorQuery->tables) {
        if (!table.definition || !table.definition->complete) return;
        for (const auto &column : table.definition->columns) {
          stop();
          if (++inspected > options.maxCatalogEntries) {
            result.truncated = true;
            return;
          }
          inputNames.insert(nameKey(column.name));
        }
      }
    }
    std::map<std::string, std::size_t> counts;
    for (const auto &projection : cursorQuery->projections) {
      stop();
      if (!projection.alias.empty()) ++counts[nameKey(projection.alias)];
    }
    for (const auto &projection : cursorQuery->projections) {
      stop();
      if (projection.alias.empty()) continue;
      if (++inspected > options.maxCatalogEntries) { result.truncated = true; break; }
      const auto name = nameKey(projection.alias);
      if (counts.at(name) > 1 || inputNames.count(name)) continue;
      const auto &expr = cursorQuery->expressions.at(projection.expression);
      // A grouping alias that represents an aggregate or window expression
      // is not legal. Start with known direct columns, where this is provable.
      if (grouping && (expr.kind != ExprKind::Name || !expr.binding || expr.binding->scope != cursorQuery->id))
        continue;
      add(projection.alias, spelling(projection.alias, quotedPrefix),
          std::string("SELECT output · ") + valueTypeName(expr.type), CompletionKind::Column, 145);
    }
  }
  void columns() {
    struct Column {
      std::string name, alias;
      ValueType type;
    };
    std::vector<Column> columns;
    std::map<std::string, std::size_t> counts;
    std::set<std::string> locals;
    if (routineQueryContext && routineQueryContext->localPolicy != LocalNamePolicy::PreferColumns &&
        qualifier.empty())
      for (const auto &local : routineQueryContext->locals)
        if (local.name.size() == 1) locals.insert(key(local.name.front()));
    for (const auto &relation : visible) {
      stop();
      if (!qualifier.empty()) {
        const auto equal = [&](const std::string &a, const std::string &b) {
          return key(a) == key(b) || (catalog && catalog->relationNameEquals(a, b));
        };
        const bool matches = qualifier.size() == 1 ? equal(qualifier.front(), relation.alias) :
            relation.table.alias.empty() && qualifier.size() == relation.table.name.size() &&
            std::equal(qualifier.begin(), qualifier.end(), relation.table.name.begin(), equal);
        if (!matches) continue;
      }
      auto table = definition(relation);
      if (!table)
        continue;
      for (const auto &col : table->columns) {
        if (columns.size() >= options.maxCatalogEntries) {
          result.truncated = true;
          break;
        }
        columns.push_back({col.name, relation.alias, col.type});
        ++counts[key(col.name)];
      }
    }
    for (const auto &col : columns) {
      const auto slot = key(col.name);
      const bool qualified = qualifier.empty() && (sqlServerOutput || counts[slot] > 1 || locals.count(slot));
      const auto name = spelling(col.name, quotedPrefix);
      const auto label = qualified ? col.alias + "." + col.name : col.name;
      const auto insert = qualified ? spelling(col.alias) + "." + name : name;
      add(label, insert, col.alias + " · " + valueTypeName(col.type),
          CompletionKind::Column, 120);
    }
    if (qualifier.empty()) {
      const auto after=skipHorizontal(result.replacement.end);
      const bool dot=after<source.size() && source[after]=='.';
      for (const auto &relation : visible) {
        if (sqlServerOutput)
          add(relation.alias,spelling(relation.alias,quotedPrefix)+(dot?"":"."),"OUTPUT row",CompletionKind::Alias,145);
        else if (!relation.table.pseudo || relation.incomingRow)
          add(relation.alias, spelling(relation.alias, quotedPrefix),
              "query alias", CompletionKind::Alias, 90);
      }
    }
  }
  ProceduralScopeId visibleRoutineScope() const {
    if (routine.ast.scopes.size() < 2)
      return 0;
    ProceduralScopeId scope = 0;
    for (const auto &s : routine.ast.scopes)
      if (s.id && s.origin.begin <= cursor &&
          (s.origin.end <= s.origin.begin || s.origin.end >= cursor))
        if (!scope || s.origin.begin >= routine.ast.scopes[scope].origin.begin)
          scope = s.id;
    return scope;
  }
  void cursorNames() {
    std::set<std::string> shadowed;
    for (auto scope = visibleRoutineScope(); scope; scope = routine.ast.scopes[scope].parent) {
      for (auto id : routine.ast.scopes[scope].cursors) {
        stop();
        const auto &cursor = routine.ast.cursors[id];
        if (cursor.origin.end > result.replacement.begin ||
            !shadowed.insert(key(cursor.name)).second)
          continue;
        add(cursor.name, spelling(cursor.name, quotedPrefix || fold(cursor.name) == "NEXT"), "routine cursor",
            CompletionKind::Cursor, 140);
      }
    }
  }
  void variables() {
    const auto scope = visibleRoutineScope();
    std::set<std::string> shadowed;
    std::set<std::string> relationColumns, relationAliases;
    std::size_t inspected = 0;
    if (routineQueryContext && routineQueryContext->localPolicy != LocalNamePolicy::PreferLocals &&
        qualifier.empty() && expect == Expect::Expression)
      for (const auto &relation : visible) {
        stop();
        relationAliases.insert(key(relation.alias));
        if (auto table = definition(relation))
          for (const auto &column : table->columns) {
            if ((inspected++ & 127u) == 0) stop();
            relationColumns.insert(key(column.name));
          }
      }
    const bool positional = dialect == Dialect::PostgreSQL && !prefix.empty() && prefix.front() == '$';
    std::vector<std::size_t> ordinals;
    if (positional) {
      ordinals.resize(routine.ast.symbols.size());
      for (std::size_t i = 0; i < routine.ast.parameters.size(); ++i)
        ordinals[routine.ast.parameters[i]] = i + 1;
    }
    for (auto id = scope; id; id = routine.ast.scopes[id].parent) {
      const auto &s = routine.ast.scopes[id];
      for (auto symbolId : s.symbols) {
        if ((inspected++ & 127u) == 0) stop();
        const auto &symbol = routine.ast.symbols[symbolId];
        if (!qualifier.empty() && (qualifier.size() != 1 ||
            key(routineSymbolLabel(routine.ast, symbol)) != key(qualifier.back())))
          continue;
        if (!routineSymbolVisible(symbol, cursor))
          continue;
        auto label = symbol.name;
        if (positional) {
          const auto ordinal = ordinals[symbolId];
          if (!ordinal || !qualifier.empty()) continue;
          label = "$" + std::to_string(ordinal);
        }
        if (!shadowed.insert(key(label)).second)
          continue;
        if ((expect == Expect::VariableTarget || expect == Expect::Statement) &&
            !routineSymbolWritable(dialect, symbol))
          continue;
        auto text = dialect == Dialect::SQLServer
                              ? "@" + label
                              : positional ? label : spelling(label, quotedPrefix);
        if (!positional && relationColumns.count(key(label))) {
          const auto &scopeLabel = routineSymbolLabel(routine.ast, symbol);
          if (scopeLabel.empty() || relationAliases.count(key(scopeLabel))) continue;
          text = spelling(scopeLabel) + "." + text;
        }
        std::string detail = symbol.kind == SymbolKind::Parameter
                ? positional ? "routine parameter · " + symbol.name : "routine parameter"
                : symbol.kind == SymbolKind::Implicit ? "PL/pgSQL status variable"
                : symbol.kind == SymbolKind::LoopIndex ? "numeric FOR index"
                : symbol.kind == SymbolKind::ExceptionStatus ? "PL/pgSQL exception status"
                : "local variable";
        if (const auto type = generateTypeSql(symbol.type, dialect); type.ok() && !type.sql.empty())
          detail += " · " + type.sql;
        if (expect == Expect::Statement && mysql(dialect)) {
          text = "SET " + text + " = ";
          detail += " · SET";
        }
        add(std::move(label), text, std::move(detail), CompletionKind::Variable, 140);
      }
    }
  }
  bool catalogQualifierMatches(const std::string &database,const std::string &schema) const {
    return qualifier.empty() || (qualifier.size()==1 &&
        key(qualifier.front())==key(mysql(dialect)?database:schema));
  }
  bool qualifiesCatalogName(const std::string &schema) const {
    return qualifier.empty() && !schema.empty() && !mysql(dialect) && key(schema)!=key(names.schema);
  }
  std::string catalogLabel(const std::string &schema,const std::string &name) const {
    return qualifiesCatalogName(schema)?schema+"."+name:name;
  }
  CatalogNameFilter exactCatalogMatch(const std::string &wanted) {
    return [this,wanted](const std::string &database,const std::string &schema,const std::string &name) {
      stop();
      return catalogQualifierMatches(database,schema) &&
          nameMatch(catalogLabel(schema,name),wanted,false)==20;
    };
  }
  void relations() {
    if (qualifier.empty())
      for (const auto &cte : ctes)
        add(cte, spelling(cte, quotedPrefix), "CTE", CompletionKind::Table, 130);
    if (dialect==Dialect::PostgreSQL && (qualifier.empty() ||
        (qualifier.size()==1 && qualifier.front()=="pg_catalog"))) {
      const auto end=skipHorizontal(result.replacement.end);
      const bool opened=end<source.size() && source[end]=='(';
      for (const auto *function:{"generate_series","unnest"}) {
        const auto name=spelling(function,quotedPrefix);
        add(function,name+(opened?"":"()"),"PostgreSQL set-returning function",CompletionKind::Function,105,
            name.size()+(opened?0:1));
      }
    }
    if (!catalog)
      return;
    const auto wanted=fold(prefix);
    std::set<std::string> schemas;
    const auto accept=[&](const std::string &database,const std::string &schema,const std::string &table) {
      stop();
      if (!catalogQualifierMatches(database,schema)) return false;
      if (qualifier.empty() && !schema.empty() && fold(schema).rfind(wanted,0)==0) schemas.insert(schema);
      return nameMatch(catalogLabel(schema,table),wanted,true).has_value();
    };
    auto listing = prefix.empty() && qualifier.empty()
        ? catalog->listRelations(names,options.maxCatalogEntries+1)
        : catalog->matchingRelations(names,options.maxCatalogEntries+1,accept);
    if (listing.size() > options.maxCatalogEntries) {
      listing.resize(options.maxCatalogEntries);
      result.truncated = true;
      if (!wanted.empty()) {
        auto exact=catalog->matchingRelations(names,options.maxItems,exactCatalogMatch(wanted));
        listing.insert(listing.end(),std::make_move_iterator(exact.begin()),
                                     std::make_move_iterator(exact.end()));
      }
    }
    for (const auto &table : listing) {
      stop();
      if (!table.schema.empty())
        schemas.insert(table.schema);
      const auto ns = mysql(dialect) ? table.database : table.schema;
      if (!qualifier.empty() &&
          (qualifier.size() != 1 || key(qualifier.back()) != key(ns)))
        continue;
      const bool qualify = qualifiesCatalogName(table.schema);
      const auto label = catalogLabel(table.schema,table.name);
      auto insert = spelling(table.name, quotedPrefix);
      if (qualify)
        insert = spelling(table.schema) + "." + insert;
      add(label, insert, ns + (catalog->current() ? "" : " · cached"),
          table.kind == ObjectKind::Table ? CompletionKind::Table
            :table.kind==ObjectKind::MaterializedView?CompletionKind::MaterializedView:CompletionKind::View,
          110);
    }
    if (qualifier.empty())
      for (const auto &schema : schemas)
        add(schema, spelling(schema, quotedPrefix) + ".", "schema",
            CompletionKind::Schema, 60);
  }
  void objects(CatalogObjectKind kind) {
    if(!catalog)return;
    if(qualifier.empty() && kind!=CatalogObjectKind::Schema && kind!=CatalogObjectKind::Database) {
      auto spaces=catalog->listObjects(names,mysql(dialect)?CatalogObjectKind::Database:CatalogObjectKind::Schema,options.maxCatalogEntries+1);
      result.truncated|=spaces.size()>options.maxCatalogEntries;
      for(std::size_t i=0;i<std::min(spaces.size(),options.maxCatalogEntries);++i)
        add(spaces[i].name,spelling(spaces[i].name,quotedPrefix)+".","namespace",CompletionKind::Schema,60);
    }
    auto emit=[&](CatalogObjectDefinition o,CompletionKind completion){
      stop();
      if(!qualifier.empty() && (qualifier.size()!=1 || key(qualifier.back())!=key(mysql(dialect)?o.database:o.schema)))return;
      const bool qualify=qualifier.empty() && kind!=CatalogObjectKind::Schema && kind!=CatalogObjectKind::Database &&
        !o.schema.empty() && key(o.schema)!=key(names.schema) && !mysql(dialect);
      const auto label=(qualify?o.schema+".":std::string{})+o.name;
      auto insert=(qualify?spelling(o.schema)+".":std::string{})+spelling(o.name,quotedPrefix);
      add(label,insert,(o.parent.empty()?o.detail:o.parent+" · "+o.detail)+(catalog->current()?"":" · cached"),completion,125);
    };
    if(kind==CatalogObjectKind::Table || kind==CatalogObjectKind::View || kind==CatalogObjectKind::MaterializedView) {
      auto list=catalog->listRelations(names,options.maxCatalogEntries+1);
      result.truncated|=list.size()>options.maxCatalogEntries;
      for(std::size_t i=0;i<std::min(list.size(),options.maxCatalogEntries);++i) {
        const auto &t=list[i];auto k=t.kind==ObjectKind::MaterializedView?CatalogObjectKind::MaterializedView:t.kind==ObjectKind::View?CatalogObjectKind::View:CatalogObjectKind::Table;
        if(k==kind)emit({k,t.database,t.schema,t.name,{},{}},k==CatalogObjectKind::Table?CompletionKind::Table:k==CatalogObjectKind::View?CompletionKind::View:CompletionKind::MaterializedView);
      }
      return;
    }
    if(kind==CatalogObjectKind::Function || kind==CatalogObjectKind::Procedure) {
      auto list=catalog->listRoutines(names,kind==CatalogObjectKind::Procedure,options.maxCatalogEntries+1);
      result.truncated|=list.size()>options.maxCatalogEntries;
      for(std::size_t i=0;i<std::min(list.size(),options.maxCatalogEntries);++i) {
        const auto &r=list[i];emit({kind,r.database,r.schema,r.name,{},std::to_string(r.minArguments)+"…"+std::to_string(r.maxArguments)+" arguments"},kind==CatalogObjectKind::Function?CompletionKind::Function:CompletionKind::Procedure);
      }
      return;
    }
    static const std::map<CatalogObjectKind,CompletionKind> kinds={
      {CatalogObjectKind::Database,CompletionKind::Database},{CatalogObjectKind::Schema,CompletionKind::Schema},
      {CatalogObjectKind::Index,CompletionKind::Index},{CatalogObjectKind::Sequence,CompletionKind::Sequence},
      {CatalogObjectKind::Trigger,CompletionKind::Trigger},{CatalogObjectKind::Type,CompletionKind::Type},
      {CatalogObjectKind::Synonym,CompletionKind::Synonym},{CatalogObjectKind::Package,CompletionKind::Package},{CatalogObjectKind::Event,CompletionKind::Event}};
    auto found=kinds.find(kind);if(found==kinds.end())return;
    auto list=catalog->listObjects(names,kind,options.maxCatalogEntries+1);
    result.truncated|=list.size()>options.maxCatalogEntries;
    for(std::size_t i=0;i<std::min(list.size(),options.maxCatalogEntries);++i)emit(list[i],found->second);
  }
  void functions(bool procedure = false) {
    auto call = [&](const std::string &label, const std::string &name,
                    const std::string &detail, CompletionKind kind,
                    int priority) {
      auto end = result.replacement.end;
      while (end < source.size() && (source[end] == ' ' || source[end] == '\t'))
        ++end;
      const bool parentheses = (procedure && dialect==Dialect::SQLServer) || (end < source.size() && source[end] == '(');
      add(label, name + (parentheses ? "" : "()"), detail, kind, priority,
          parentheses ? name.size() : name.size() + 1);
    };
    if (catalog) {
      const auto wanted=fold(prefix);
      auto listing=prefix.empty() && qualifier.empty()
          ? catalog->listRoutines(names,procedure,options.maxCatalogEntries+1)
          : catalog->matchingRoutines(names,procedure,options.maxCatalogEntries+1,
              [&](const std::string &database,const std::string &schema,const std::string &routine) {
                stop();
                return catalogQualifierMatches(database,schema) &&
                    nameMatch(catalogLabel(schema,routine),wanted,false).has_value();
              });
      if (listing.size() > options.maxCatalogEntries) {
        listing.resize(options.maxCatalogEntries);
        result.truncated = true;
        if (!wanted.empty()) {
          auto exact=catalog->matchingRoutines(names,procedure,options.maxItems,exactCatalogMatch(wanted));
          listing.insert(listing.end(),std::make_move_iterator(exact.begin()),
                                       std::make_move_iterator(exact.end()));
        }
      }
      for (const auto &r : listing) {
        if (!procedure && dialect==Dialect::PostgreSQL && r.schema=="pg_catalog") {
          const auto name=fold(r.name);
          if (name=="MODE" || name=="PERCENTILE_DISC" || name=="PERCENTILE_CONT") continue;
        }
        if (!qualifier.empty() &&
            (qualifier.size() != 1 ||
             key(qualifier.back()) !=
                 key(mysql(dialect) ? r.database : r.schema)))
          continue;
        const bool qualify = qualifier.empty() && !r.schema.empty() &&
                             key(r.schema) != key(names.schema) &&
                             !mysql(dialect);
        const auto text = (qualify ? spelling(r.schema) + "." : "") +
                          spelling(r.name, quotedPrefix);
        call((qualify ? r.schema + "." : "") + r.name, text,
             std::to_string(r.minArguments) + ".." +
                 (r.variadic ? "*" : std::to_string(r.maxArguments)) + " args" +
                 (catalog->current() ? "" : " · cached"),
             procedure ? CompletionKind::Procedure : CompletionKind::Function,
             100);
      }
    }
    const bool nativeScope=dialect==Dialect::PostgreSQL && qualifier.size()==1 && qualifier.front()=="pg_catalog";
    if (procedure || (!qualifier.empty() && !nativeScope)) return;
    for (const auto &builtin:builtinOperators(dialect)) {
      if (builtin.requiresWithinGroup) {
        const auto end=skipHorizontal(result.replacement.end);
        const auto name=spelling(canonicalIdentifier(dialect,builtin.name,false),quotedPrefix);
        const bool mode=builtin.name=="MODE";
        const auto lead=name+(mode?"(":"(0.5")+") WITHIN GROUP (ORDER BY ";
        const bool opened=end<source.size() && source[end]=='(';
        add(builtin.name,opened?name:lead+")","PostgreSQL ordered-set aggregate",
            CompletionKind::Function,110,opened?name.size():mode?lead.size():name.size()+1,
            !opened && !mode?3:0);
      } else if (qualifier.empty()) {
        static const std::set<std::string> valueKeywords{
            "CURRENT_DATE","CURRENT_TIME","CURRENT_TIMESTAMP","CURRENT_USER","SESSION_USER","SYSTEM_USER",
            "CURRENT_CATALOG","CURRENT_ROLE","CURRENT_SCHEMA","USER","LOCALTIME","LOCALTIMESTAMP","SYSDATE","SYSTIMESTAMP"};
        const bool bare=valueKeywords.count(builtin.name) &&
            (!mysql(dialect) || builtin.name!="SYSDATE");
        if (bare)
          add(builtin.name,builtin.name,"SQL value keyword",CompletionKind::Function,60);
        else
          call(builtin.name,builtin.name,"built-in · "+std::string(dialectName(dialect))+" · "+routineKindName(builtin.kind),
               CompletionKind::Function,60);
      }
    }
  }

  void keywords() {
    if (sqlServerOutput) return;
    if (expect == Expect::CursorName || expect == Expect::ScalarVariable)
      return;
    if (expect == Expect::VariableTarget) {
      for (const auto &word:keywordChoices)
        add(word,word,"clause keyword",CompletionKind::Keyword,150);
      if (dialect == Dialect::PostgreSQL && fold(before.back().value) == "INTO")
        add("STRICT", "STRICT", "exactly one result row", CompletionKind::Keyword, 110);
      return;
    }
    if(expect==Expect::FixedKeywords || expect==Expect::SelectContinuation) {
      int priority=150;
      for(const auto &word:keywordChoices) {
        add(word,word,"clause keyword",CompletionKind::Keyword,priority);
        if(expect==Expect::SelectContinuation) --priority;
      }
      return;
    }
    if (expect==Expect::InspectionKeyword) {
      for (const auto &word : inspectionWords) add(word,word,"inspection keyword",CompletionKind::Keyword,130);
      return;
    }
    if(expect==Expect::CatalogObject || expect==Expect::ObjectDeclaration || expect==Expect::WriteColumn || expect==Expect::ReferenceColumn || expect==Expect::ColumnList || expect==Expect::JoinColumn || expect==Expect::OutputColumn)return;
    if (!qualifier.empty() || parameterPrefix || quotedPrefix)
      return;
    std::vector<std::string> words = {
        "SELECT",      "FROM",       "WHERE",      "JOIN",        "LEFT JOIN",
        "RIGHT JOIN",  "INNER JOIN", "CROSS JOIN", "ON",          "AS",
        "GROUP BY",    "HAVING",     "ORDER BY",   "ASC",         "DESC",
        "AND",         "OR",         "NOT",        "NULL",        "IS NULL",
        "IS NOT NULL", "IN",         "EXISTS",     "BETWEEN",     "LIKE",
        "DISTINCT",    "UNION",      "UNION ALL",  "WITH",        "CASE",
        "WHEN",        "THEN",       "ELSE",       "END",         "INSERT INTO",
        "VALUES",      "UPDATE",     "SET",        "DELETE FROM", "CREATE",
        "ALTER",       "DROP",       "TABLE",      "VIEW",        "INDEX",
        "FUNCTION",    "PROCEDURE",  "TRIGGER",    "PRIMARY KEY", "FOREIGN KEY",
        "REFERENCES",  "CHECK",      "UNIQUE",     "NOT NULL",    "DEFAULT",
        "BEGIN",       "RETURN",     "DECLARE",    "IF",          "WHILE",
        "COMMIT",      "ROLLBACK"};
    if (mysql(dialect) || dialect == Dialect::PostgreSQL ||
        dialect == Dialect::SQLite) {
      words.push_back("LIMIT");
      words.push_back("OFFSET");
    }
    if (dialect == Dialect::PostgreSQL)
      for (auto w : {"RETURNING", "ON CONFLICT", "MATERIALIZED VIEW", "LATERAL",
                     "ILIKE", "NOT ILIKE", "IS DISTINCT FROM",
                     "IS NOT DISTINCT FROM", "LANGUAGE plpgsql", "ELSIF"})
        words.push_back(w);
    if (dialect==Dialect::PostgreSQL && expect==Expect::Expression)
      add("ARRAY","ARRAY[","array constructor",CompletionKind::Keyword,95);
    if (dialect == Dialect::Oracle)
      for (auto w :
           {"DUAL", "FETCH FIRST", "CONNECT BY", "START WITH", "ROWNUM",
            "ELSIF", "EXCEPTION", "LOOP", "AUTHID CURRENT_USER"})
        words.push_back(w);
    if (dialect == Dialect::SQLServer)
      for (auto w : {"TOP", "OUTPUT", "GO", "MERGE", "CROSS APPLY",
                     "OUTER APPLY", "TRY", "CATCH", "EXEC"})
        words.push_back(w);
    if (mysql(dialect))
      for (auto w : {"AUTO_INCREMENT", "ENGINE", "DELIMITER", "ELSEIF", "LEAVE",
                     "LOOP", "CALL"})
        words.push_back(w);
    if(dialect==Dialect::PostgreSQL || dialect==Dialect::Oracle || dialect==Dialect::SQLServer)
      for(auto w:{"GROUPING SETS","ROLLUP","CUBE"})words.push_back(w);
    if(mysql(dialect))words.push_back("WITH ROLLUP");
    if (expect == Expect::Statement) {
      words={"SELECT","INSERT INTO","UPDATE","DELETE FROM","CREATE","ALTER","DROP",
             "WITH","BEGIN","COMMIT","ROLLBACK","TRUNCATE","SET"};
      if (mysql(dialect)) words.push_back("REPLACE INTO");
      if (dialect!=Dialect::SQLite && dialect!=Dialect::TiDB)
        words.push_back(dialect==Dialect::SQLServer?"EXEC":"CALL");
      if (dialect==Dialect::TiDB) words.push_back("ADMIN SHOW DDL");
      if (mysql(dialect)) for(auto word:{"DELIMITER","SHOW","DESCRIBE"}) words.push_back(word);
      if (dialect==Dialect::MySQL || dialect==Dialect::PostgreSQL) words.push_back("TABLE");
      if (dialect==Dialect::MySQL || dialect==Dialect::PostgreSQL || dialect==Dialect::SQLite) words.push_back("VALUES");
      if (dialect==Dialect::SQLServer || routineHeader) words.push_back("DECLARE");
      if (routineHeader) {
        for (auto word : {"RETURN", "WHILE"}) words.push_back(word);
        if (mysql(dialect) && routineSite.statement)
          ifStatementCompletion();
        else
          words.push_back("IF");
        if (routineSite.closesIf)
          endIfCompletion(false);
        else
          words.push_back("END");
        if (mysql(dialect)) {
          for (auto word : {"LOOP", "REPEAT", "LEAVE", "ITERATE", "SIGNAL", "RESIGNAL", "GET DIAGNOSTICS"})
            words.push_back(word);
          if (!routine.ast.function && (dialect==Dialect::MySQL || dialect==Dialect::MariaDB))
            for (auto word:{"PREPARE","EXECUTE","DEALLOCATE PREPARE"}) words.push_back(word);
          if (visibleRoutineScope())
            for (auto word : {"OPEN", "FETCH", "CLOSE"}) words.push_back(word);
        }
        else if (dialect == Dialect::PostgreSQL || dialect == Dialect::Oracle)
          for (auto word : {"FOR", "LOOP", "EXIT WHEN", "CONTINUE WHEN"}) words.push_back(word);
        if (dialect == Dialect::PostgreSQL) words.push_back("GET DIAGNOSTICS");
      }
    }
    if (expect == Expect::QueryStart) {
      words={"SELECT","WITH"};
      if (dialect==Dialect::MySQL || dialect==Dialect::PostgreSQL) words.push_back("TABLE");
      if (dialect==Dialect::MySQL || dialect==Dialect::PostgreSQL || dialect==Dialect::SQLite) words.push_back("VALUES");
    }
    if (expect == Expect::CreateObject) {
      words={"TABLE","VIEW","INDEX"};
      if (dialect!=Dialect::TiDB) {
        words.push_back("TRIGGER");
        if(dialect!=Dialect::SQLite)for(auto w:{"FUNCTION","PROCEDURE"})words.push_back(w);
      }
      if(dialect!=Dialect::SQLite) words.push_back("SCHEMA");
      if(dialect==Dialect::OceanBase) words.push_back("TABLEGROUP");
      if(mysql(dialect) || dialect==Dialect::PostgreSQL || dialect==Dialect::SQLServer)words.push_back("DATABASE");
      if(dialect==Dialect::PostgreSQL || dialect==Dialect::Oracle || dialect==Dialect::SQLServer)for(auto w:{"SEQUENCE","TYPE"})words.push_back(w);
      if(dialect==Dialect::PostgreSQL || dialect==Dialect::Oracle)words.push_back("MATERIALIZED VIEW");
      if(dialect==Dialect::Oracle || dialect==Dialect::SQLServer)words.push_back("SYNONYM");
      if(dialect==Dialect::Oracle)words.push_back("PACKAGE");
      if(mysql(dialect) && dialect!=Dialect::TiDB)words.push_back("EVENT");
      if (dialect == Dialect::MySQL || dialect == Dialect::MariaDB)
        words.insert(words.end(),{"ALGORITHM","DEFINER","SQL SECURITY"});
    }
    if (!caseFrames.empty()) {
      words={"CASE","AND","OR","NOT","NULL","IS NULL","IS NOT NULL","IN","EXISTS","BETWEEN","LIKE","TRUE","FALSE"};
      if (dialect==Dialect::PostgreSQL) words.push_back("ILIKE");
    }
    if (expect == Expect::Type) {
      words = {"INTEGER", "SMALLINT", "BIGINT", "DECIMAL",  "NUMERIC",
               "VARCHAR", "CHAR",     "DATE",   "TIMESTAMP"};
      if (mysql(dialect))
        for (auto w : {"INT", "TEXT", "DATETIME", "JSON", "DOUBLE", "BOOLEAN"})
          words.push_back(w);
      if (dialect == Dialect::PostgreSQL)
        for (auto w : {"TEXT", "JSONB", "JSON", "BOOLEAN", "UUID", "BYTEA",
                       "TIMESTAMPTZ"})
          words.push_back(w);
      if (dialect == Dialect::Oracle)
        words = {"NUMBER",      "VARCHAR2",  "NVARCHAR2", "CHAR",
                 "DATE",        "TIMESTAMP", "CLOB",      "BLOB",
                 "PLS_INTEGER", "BOOLEAN"};
      if (dialect == Dialect::SQLServer)
        for (auto w : {"NVARCHAR", "NCHAR", "BIT", "DATETIME2",
                       "UNIQUEIDENTIFIER", "VARBINARY", "MONEY"})
          words.push_back(w);
    }
    if(expect==Expect::Type && castTypeSlot && dialect==Dialect::MySQL)words=mysqlCastTypeNames();
    for (const auto &w : words)
      add(w, w, expect == Expect::Type ? "data type" : "keyword",
          expect == Expect::Type ? CompletionKind::Type
                                 : CompletionKind::Keyword,
          expect == Expect::Type ? 130 :
          expect == Expect::Statement || expect == Expect::QueryStart
            ? (w=="SELECT"?180:w=="CREATE" || w=="RETURN"?140:120) : 20);
  }
  void snippets() {
    if (!options.snippets || quotedPrefix || parameterPrefix ||
        !qualifier.empty())
      return;
    if (expect == Expect::Expression && prefix.empty() && !before.empty()) {
      const auto word = [&](std::size_t previous) -> std::string {
        if (previous >= before.size()) return {};
        const auto &token = before[before.size()-1-previous];
        return token.kind == TokenKind::Word ? fold(token.value) : std::string{};
      };
      const auto last = word(0);
      if (last == "SELECT" ||
          ((last == "ALL" || last == "DISTINCT") && word(1) == "SELECT")) {
        const auto next = std::find_if(tokens.begin(), tokens.end(),
            [&](const Token &token) {
              return token.range.begin >= cursor && token.kind != TokenKind::Comment &&
                     token.kind != TokenKind::End;
            });
        if (next == tokens.end() || next->value == ")" || next->value == ";")
          add("* FROM", "* FROM table_name", "complete the projection and relation",
              CompletionKind::Snippet, 160, 7, 10);
        add("*", "*", "all columns", CompletionKind::Snippet, 150);
      }
    }
    if(expect==Expect::Expression && (mysql(dialect) || dialect==Dialect::PostgreSQL || dialect==Dialect::Oracle)) {
      add("EXTRACT template","EXTRACT(YEAR FROM DATE '2000-01-01')","temporal field extraction",CompletionKind::Snippet,80,24,10);
      if(!mysql(dialect))add("INTERVAL literal","INTERVAL '1' DAY","typed interval literal",CompletionKind::Snippet,80,10,1);
      else add("DATE_ADD template","DATE_ADD(CURRENT_DATE, INTERVAL 1 DAY)","date arithmetic",CompletionKind::Snippet,80,32,1);
      for(const auto &entry:std::vector<std::pair<std::string,std::string>>{{"DATE","2000-01-01"},{"TIME","12:00:00"},{"TIMESTAMP","2000-01-01 12:00:00"}}) {
        if(dialect==Dialect::Oracle && entry.first=="TIME")continue;
        add(entry.first+" literal",entry.first+" '"+entry.second+"'","typed temporal literal",CompletionKind::Snippet,80,entry.first.size()+2,entry.second.size());
      }
    }
    std::size_t create = std::string::npos;
    for (const auto &t : before)
      if (fold(t.value) == "CREATE")
        create = t.range.begin;
    const bool root = before.empty() || before.back().value == ";";
    if (!root && expect != Expect::CreateObject &&
        !(create != std::string::npos && before.back().range.begin == create))
      return;
    const bool replacingCreate = create != std::string::npos && !root;
    auto range = replacingCreate ? SourceRange{create, result.replacement.end}
                                 : result.replacement;
    auto item = [&](const std::string &kind, std::string body) {
      const auto text = "CREATE " + body;
      auto name = body.find("object_name");
      add((replacingCreate ? "" : "CREATE ") + kind, text,
          "object definition template", CompletionKind::Snippet, 100, 7 + name,
          std::string("object_name").size(), range);
    };
    item("TABLE",
         dialect == Dialect::Oracle
             ? "TABLE object_name (\n  id NUMBER(10, 0) PRIMARY KEY\n);"
             : "TABLE object_name (\n  id INTEGER PRIMARY KEY\n);");
    item("VIEW", "VIEW object_name AS\nSELECT column_name\nFROM table_name;");
    item("INDEX","INDEX object_name ON table_name (column_name);");
    if(dialect==Dialect::PostgreSQL || dialect==Dialect::Oracle || dialect==Dialect::SQLServer)
      item("SEQUENCE","SEQUENCE object_name START WITH 1 INCREMENT BY 1;");
    if(dialect==Dialect::PostgreSQL || dialect==Dialect::Oracle)
      item("MATERIALIZED VIEW","MATERIALIZED VIEW object_name AS\nSELECT column_name\nFROM table_name;");
    if(mysql(dialect) || dialect==Dialect::PostgreSQL || dialect==Dialect::SQLServer)
      item("DATABASE","DATABASE object_name;");
    if(mysql(dialect) || dialect==Dialect::PostgreSQL || dialect==Dialect::SQLServer)
      item("SCHEMA","SCHEMA object_name;");

    if (dialect == Dialect::OceanBase)
      item("TABLEGROUP", "TABLEGROUP object_name SHARDING = 'NONE';");
    if (dialect == Dialect::PostgreSQL) {
      item("FUNCTION",
           "FUNCTION object_name(p_value INTEGER) RETURNS INTEGER\nLANGUAGE "
           "plpgsql AS $body$\nBEGIN\n  RETURN p_value;\nEND;\n$body$;");
      item("PROCEDURE", "PROCEDURE object_name(IN p_value INTEGER)\nLANGUAGE "
                        "plpgsql AS $body$\nBEGIN\n  NULL;\nEND;\n$body$;");
      item("TRIGGER", "TRIGGER object_name BEFORE INSERT ON table_name\nFOR "
                      "EACH ROW EXECUTE FUNCTION trigger_function();");
    } else if (mysql(dialect) && dialect != Dialect::TiDB) {
      item("FUNCTION", "FUNCTION object_name(p_value INTEGER) RETURNS "
                       "INTEGER\nDETERMINISTIC\nRETURN p_value;");
      item("PROCEDURE",
           "PROCEDURE object_name(IN p_value INTEGER)\nSELECT p_value;");
      item("TRIGGER", "TRIGGER object_name BEFORE INSERT ON table_name\nFOR "
                      "EACH ROW SET NEW.column_name = NEW.column_name;");
    } else if (dialect == Dialect::Oracle) {
      item("FUNCTION", "FUNCTION object_name(p_value NUMBER) RETURN "
                       "NUMBER\nAS\nBEGIN\n  RETURN p_value;\nEND;");
      item(
          "PROCEDURE",
          "PROCEDURE object_name(p_value IN NUMBER)\nAS\nBEGIN\n  NULL;\nEND;");
      item("TRIGGER", "TRIGGER object_name BEFORE INSERT ON table_name\nFOR "
                      "EACH ROW\nBEGIN\n  NULL;\nEND;");
    } else if (dialect == Dialect::SQLServer) {
      item("FUNCTION", "FUNCTION object_name(@p_value INTEGER) RETURNS "
                       "INTEGER\nAS\nBEGIN\n  RETURN @p_value;\nEND;");
      item("PROCEDURE", "PROCEDURE object_name @p_value INTEGER\nAS\nBEGIN\n  "
                        "SET NOCOUNT ON;\nEND;");
      item("TRIGGER", "TRIGGER object_name ON dbo.table_name AFTER "
                      "INSERT\nAS\nBEGIN\n  SET NOCOUNT ON;\nEND;");
    } else if (dialect == Dialect::SQLite)
      item("TRIGGER", "TRIGGER object_name AFTER INSERT ON table_name\nBEGIN\n "
                      " SELECT 1;\nEND;");
  }

public:
  Completer(Dialect d, std::string text, std::size_t at,
            const CatalogSnapshot *c, NameResolutionContext context,
            CompletionOptions o)
      : dialect(d), source(std::move(text)), cursor(at), catalog(c),
        names(std::move(context)), options(std::move(o)) {
    names.dialect = d;
    options.maxItems =
        std::max<std::size_t>(1, std::min<std::size_t>(512, options.maxItems));
    options.maxCatalogEntries = std::max<std::size_t>(
        1, std::min<std::size_t>(8192, options.maxCatalogEntries));
  }
  CompletionResult run() {
    try {
      if (!scan())
        return result;
      stop();
      scopes();
      grammar();
      const bool windowList=windowOpening && openColumnList()==windowOpening &&
          (*windowOpening+1==before.size() || isSymbol(before.back(),","));
      if (options.columnListOnly && expect != Expect::JoinColumn &&
          expect != Expect::WriteColumn && expect != Expect::ReferenceColumn && expect != Expect::ColumnList &&
          !windowList && pivotSite!=PivotSite::Aggregate)
        return result;
      result.metadataFresh = catalog && catalog->current();
      if (expect == Expect::JoinColumn) joinColumns();
      else if (expect == Expect::WriteColumn || expect == Expect::ReferenceColumn) targetColumns();
      else if (expect == Expect::ColumnList) {
        std::size_t visited=0;
        for (const auto &column:listColumns) {
          stop();
          if (++visited>options.maxCatalogEntries) { result.truncated=true; break; }
          if (!assignedColumns.count(key(column)))
            add(column,spelling(column,quotedPrefix),columnListDetail,CompletionKind::Column,145);
        }
      }
      else if (expect == Expect::OutputColumn) outputAliases();
      else if(expect==Expect::CatalogObject)objects(objectKind);
      else if(expect==Expect::Type && !(castTypeSlot && dialect==Dialect::MySQL))objects(CatalogObjectKind::Type);
      else if (expect == Expect::Relation)
        relations();
      else if (expect == Expect::Procedure)
        functions(true);
      else if (expect == Expect::CursorName)
        cursorNames();
      else if (expect == Expect::VariableTarget || expect == Expect::ScalarVariable)
        variables();
      else if (expect == Expect::Statement) {
        if (routineHeader && (dialect == Dialect::PostgreSQL || dialect == Dialect::Oracle ||
                              (mysql(dialect) && !prefix.empty() && qualifier.empty())))
          variables();
      } else if (expect != Expect::Type && expect != Expect::CreateObject && expect != Expect::QueryStart && expect != Expect::ObjectDeclaration && expect != Expect::InspectionKeyword && expect != Expect::FixedKeywords &&
                 expect != Expect::SelectContinuation) {
        outputAliases();
        columns();
        variables();
        functions();
      }
      keywords();
      snippets();
      std::stable_sort(result.items.begin(), result.items.end(), betterCandidate);
      if (result.items.size() > options.maxItems) {
        result.truncated = true;
        result.items.resize(options.maxItems);
      }
    } catch (const CompilationAborted &) {
      result.items.clear();
      result.diagnostics.push_back(
          {DiagnosticCode::Stopped,
           {},
           "completion cancelled or resource budget exhausted"});
    } catch (const std::exception &e) {
      result.items.clear();
      result.diagnostics.push_back(
          {DiagnosticCode::InvalidQueryShape, {}, e.what()});
    }
    return result;
  }
};
} // namespace
CompletionResult completeSql(Dialect d, std::string source, std::size_t cursor,
                             const CatalogSnapshot *catalog,
                             NameResolutionContext names,
                             CompletionOptions options) {
  return Completer(d, std::move(source), cursor, catalog, std::move(names),
                   std::move(options))
      .run();
}
const char *completionKindName(CompletionKind k) {
  switch (k) {
  case CompletionKind::Keyword:
    return "keyword";
  case CompletionKind::Snippet:
    return "snippet";
  case CompletionKind::Schema:
    return "schema";
  case CompletionKind::Table:
    return "table";
  case CompletionKind::View:
    return "view";
  case CompletionKind::Column:
    return "column";
  case CompletionKind::Alias:
    return "alias";
  case CompletionKind::Function:
    return "function";
  case CompletionKind::Procedure:
    return "procedure";
  case CompletionKind::Cursor:
    return "cursor";
  case CompletionKind::Label:
    return "label";
  case CompletionKind::PreparedStatement:
    return "prepared-statement";
  case CompletionKind::Variable:
    return "variable";
  case CompletionKind::Type:
    return "type";
  case CompletionKind::Database:return "database";
  case CompletionKind::MaterializedView:return "materializedView";
  case CompletionKind::Index:return "index";
  case CompletionKind::Sequence:return "sequence";
  case CompletionKind::Trigger:return "trigger";
  case CompletionKind::Synonym:return "synonym";
  case CompletionKind::Package:return "package";
  case CompletionKind::Event:return "event";
  }
  return "unknown";
}
} // namespace navidog::sql::compiler
