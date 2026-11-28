#include "navidog/sql/compiler/formatter.hpp"
#include "conversion_internal.hpp"
#include "parser.hpp"
#include <algorithm>
#include <iterator>
#include <map>
#include <numeric>
#include <stdexcept>
#include <unordered_set>

namespace navidog::sql::compiler {
namespace {
struct Failure {
  DiagnosticCode code;
  std::string message;
  SourceRange range;
};
bool mysql(Dialect d) {
  return isMySqlDialect(d);
}
std::string trim(std::string_view s) {
  const auto a = s.find_first_not_of(" \t\r\n");
  return a == std::string_view::npos
             ? std::string{}
             : std::string(s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1));
}
std::string upper(std::string_view text) { return asciiUpper(text); }
unsigned width(std::string_view text, unsigned tab = 4) {
  unsigned result = 0;
  for (std::size_t i = 0; i < text.size();) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c == '\t') {
      result += tab - result % tab;
      ++i;
      continue;
    }
    unsigned cp = c, bytes = 1;
    if (c >= 0xf0) {
      cp = c & 7;
      bytes = 4;
    } else if (c >= 0xe0) {
      cp = c & 15;
      bytes = 3;
    } else if (c >= 0xc0) {
      cp = c & 31;
      bytes = 2;
    }
    for (unsigned j = 1; j < bytes && i + j < text.size(); ++j)
      cp = (cp << 6) | (static_cast<unsigned char>(text[i + j]) & 63);
    i += bytes;
    if (cp >= 0x300 && cp <= 0x36f)
      continue;
    result +=
        (cp >= 0x1100 &&
         (cp <= 0x115f || (cp >= 0x2e80 && cp <= 0xa4cf) ||
          (cp >= 0xac00 && cp <= 0xd7af) || (cp >= 0xf900 && cp <= 0xfaff) ||
          (cp >= 0xff01 && cp <= 0xff60) || (cp >= 0x1f300 && cp <= 0x1faff)))
            ? 2
            : 1;
  }
  return result;
}
struct FToken {
  TokenKind kind = TokenKind::Symbol;
  SourceRange range;
  std::string raw, up;
  int opaque = 0; // 1 = DELIMITER directive, 2 = custom statement separator, 3 = module argument
  bool identifier = false, changedBody = false, diagnosticKeyword = false;
  bool is(std::string_view value) const {
    return opaque == 0 && !identifier && kind == TokenKind::Word && up == value;
  }
  bool symbol(std::string_view value) const {
    return opaque == 0 && kind == TokenKind::Symbol && raw == value;
  }
};
void stop(const FormatOptions &options) {
  if ((options.shouldStop && options.shouldStop()) ||
      (options.language.shouldStop && options.language.shouldStop()))
    throw Failure{DiagnosticCode::Stopped, "SQL formatting stopped", {}};
}
std::vector<FToken> tokenize(Dialect d, const std::string &source,
                             const FormatOptions &options) {
  stop(options);
  if (source.size() > options.language.limits.sourceBytes)
    throw Failure{DiagnosticCode::ResourceLimit,
                  "SQL formatting source limit exceeded",
                  {}};
  Compilation first(d, source, options.language, "<format>");
  try {
    lex(first, true, true);
  } catch (const CompilationAborted &) {
  }
  for (const auto &error : first.diagnostics)
    if (error.code == DiagnosticCode::InvalidEncoding ||
        error.code == DiagnosticCode::ResourceLimit ||
        error.code == DiagnosticCode::Stopped)
      throw Failure{error.code, error.message, error.range};
  std::vector<FToken> opaque;
  std::string masked = source;
  auto protectedAt = [&](std::size_t pos) {
    auto it = std::upper_bound(first.tokens.begin(), first.tokens.end(), pos,
                               [](auto offset, const Token &token) {
                                 return offset < token.range.begin;
                               });
    if (it == first.tokens.begin())
      return false;
    --it;
    return pos < it->range.end && (it->kind == TokenKind::String ||
                                   it->kind == TokenKind::Identifier ||
                                   it->kind == TokenKind::Comment);
  };
  struct Segment {
    std::size_t begin, end;
    std::string delimiter;
  };
  std::vector<Segment> segments;
  std::string delimiter = ";";
  std::size_t segmentBegin = 0;
  if (mysql(d))
    for (std::size_t begin = 0; begin < source.size();) {
      auto end = source.find('\n', begin);
      if (end == std::string::npos)
        end = source.size();
      auto contentEnd = end;
      if (contentEnd > begin && source[contentEnd - 1] == '\r')
        --contentEnd;
      auto text =
          trim(std::string_view(source).substr(begin, contentEnd - begin));
      if (text.size() > 9 &&
          upper(std::string_view(text).substr(0, 9)) == "DELIMITER" &&
          (text[9] == ' ' || text[9] == '\t') &&
          !protectedAt(begin + source.substr(begin, contentEnd - begin)
                                   .find_first_not_of(" \t"))) {
        auto next = trim(std::string_view(text).substr(9));
        if (next.empty() || next.size() > 16 ||
            next.find_first_of(" \t\r\n") != std::string::npos)
          throw Failure{DiagnosticCode::UnsupportedSyntax,
                        "unsupported DELIMITER directive",
                        {begin, contentEnd}};
        segments.push_back({segmentBegin, begin, delimiter});
        opaque.push_back({TokenKind::Word,
                          {begin, contentEnd},
                          source.substr(begin, contentEnd - begin),
                          {},
                          1});
        std::fill(masked.begin() + begin, masked.begin() + contentEnd, ' ');
        delimiter = next;
        segmentBegin = end < source.size() ? end + 1 : end;
      }
      begin = end < source.size() ? end + 1 : end;
    }
  segments.push_back({segmentBegin, source.size(), delimiter});
  if (mysql(d))
    for (const auto &segment : segments)
      if (segment.delimiter != ";")
        for (auto pos = source.find(segment.delimiter, segment.begin);
             pos != std::string::npos &&
             pos + segment.delimiter.size() <= segment.end;
             pos =
                 source.find(segment.delimiter, pos + segment.delimiter.size()))
          if (!protectedAt(pos)) {
            opaque.push_back({TokenKind::Symbol,
                              {pos, pos + segment.delimiter.size()},
                              segment.delimiter,
                              {},
                              2});
            std::fill(masked.begin() + pos,
                      masked.begin() + pos + segment.delimiter.size(), ' ');
          }
  Compilation tokens(d, std::move(masked), options.language, "<format>");
  try {
    lex(tokens, true, false);
  } catch (const CompilationAborted &) {
  }
  for (const auto &error : tokens.diagnostics)
    throw Failure{error.code, error.message, error.range};
  std::vector<FToken> result;
  result.reserve(tokens.tokens.size() + opaque.size());
  for (const auto &t : tokens.tokens)
    if (t.kind != TokenKind::End)
      result.push_back(
          {t.kind, t.range,
           source.substr(t.range.begin, t.range.end - t.range.begin),
           t.kind == TokenKind::Word ? upper(t.value) : std::string{}});
  result.insert(result.end(), opaque.begin(), opaque.end());
  std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
    return a.range.begin < b.range.begin;
  });
  if (result.size() > options.language.limits.tokens)
    throw Failure{
        DiagnosticCode::ResourceLimit, "SQL format token limit exceeded", {}};
  return result;
}
bool preserveModuleArguments(std::vector<FToken> &tokens, const Compilation &parsed) {
  std::vector<const VirtualTableArgument *> opaque;
  for (const auto &stmt : parsed.statements) {
    if (!stmt.ddl || !stmt.ddl->virtualTable) continue;
    for (const auto &arg : stmt.ddl->virtualTable->arguments)
      if (!arg.payload.empty()) opaque.push_back(&arg);
  }
  if (opaque.empty()) return false;
  std::vector<FToken> preserved;
  preserved.reserve(tokens.size());
  auto first = tokens.begin();
  for (const auto *arg : opaque) {
    for (; first != tokens.end() && first->range.end <= arg->origin.begin; ++first)
      preserved.push_back(std::move(*first));
    while (first != tokens.end() && first->range.end <= arg->origin.end) ++first;
    preserved.push_back({TokenKind::Word, arg->origin, arg->payload, {}, 3, true});
  }
  preserved.insert(preserved.end(), std::make_move_iterator(first), std::make_move_iterator(tokens.end()));
  tokens = std::move(preserved);
  return true;
}
class Writer {
  const FormatStyle &style;
  const FormatOptions &options;

public:
  std::string text, eol;
  unsigned column = 0;
  bool atStart = true;
  struct Alignment {
    std::size_t offset;
    unsigned group;
  };
  std::vector<Alignment> alignment;
  Writer(const FormatStyle &style, const FormatOptions &options,
         std::string eol)
      : style(style), options(options), eol(std::move(eol)) {}
  void raw(std::string_view value) {
    if (value.size() > options.maxOutputBytes - text.size())
      throw Failure{DiagnosticCode::ResourceLimit,
                    "SQL format output limit exceeded",
                    {}};
    text.append(value);
    const auto line = value.find_last_of('\n');
    if (line != std::string_view::npos) {
      column = width(value.substr(line + 1), style.tabWidth);
      atStart = line + 1 == value.size();
    } else {
      column += width(value, style.tabWidth);
      atStart = false;
    }
  }
  void indent(unsigned amount) {
    if (amount > 4096)
      throw Failure{DiagnosticCode::ResourceLimit,
                    "SQL format indentation limit exceeded",
                    {}};
    if (atStart) {
      if (style.useTabs) {
        raw(std::string(amount / style.tabWidth, '\t'));
        raw(std::string(amount % style.tabWidth, ' '));
      } else
        raw(std::string(amount, ' '));
    }
  }
  void space() {
    if (!atStart && !text.empty() && text.back() != ' ' &&
        text.back() != '\t' && text.back() != '\n')
      raw(" ");
  }
  void line(unsigned empty = 0) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
      text.pop_back();
    if (!text.empty() && text.back() != '\n')
      raw(eol);
    atStart = true;
    column = 0;
    for (unsigned i = 0; i < empty; ++i)
      raw(eol);
    atStart = true;
    column = 0;
  }
  void mark(unsigned group) { alignment.push_back({text.size(), group}); }
  void align() {
    if (alignment.empty())
      return;
    std::vector<std::size_t> starts{0};
    for (std::size_t i = 0; i < text.size(); ++i)
      if (text[i] == '\n')
        starts.push_back(i + 1);
    struct Item {
      std::size_t offset, line;
      unsigned col, group;
    };
    std::vector<Item> items;
    for (const auto &mark : alignment) {
      const auto line = std::size_t(
          std::upper_bound(starts.begin(), starts.end(), mark.offset) -
          starts.begin() - 1);
      items.push_back({mark.offset, line,
                       width(std::string_view(text).substr(
                                 starts[line], mark.offset - starts[line]),
                             style.tabWidth),
                       mark.group});
    }
    std::vector<std::pair<std::size_t, unsigned>> pads;
    for (std::size_t i = 0; i < items.size();) {
      auto end = i + 1;
      unsigned max = items[i].col;
      while (end < items.size() && items[end].group == items[i].group &&
             items[end].line == items[end - 1].line + 1) {
        max = std::max(max, items[end].col);
        ++end;
      }
      bool fits = true;
      for (auto j = i; j < end; ++j) {
        const auto finish = items[j].line + 1 < starts.size()
                                ? starts[items[j].line + 1]
                                : text.size();
        if (max - items[j].col > 80 ||
            (style.columnLimit &&
             width(std::string_view(text).substr(
                       starts[items[j].line], finish - starts[items[j].line]),
                   style.tabWidth) +
                     max - items[j].col >
                 style.columnLimit))
          fits = false;
      }
      if (fits && end - i > 1)
        for (auto j = i; j < end; ++j)
          if (max > items[j].col)
            pads.push_back({items[j].offset, max - items[j].col});
      i = end;
    }
    std::string aligned;
    std::size_t previous = 0;
    for (const auto &pad : pads) {
      if (aligned.size() + pad.first - previous + pad.second >
          options.maxOutputBytes)
        throw Failure{DiagnosticCode::ResourceLimit,
                      "SQL format alignment output limit exceeded",
                      {}};
      aligned.append(text, previous, pad.first - previous);
      aligned.append(pad.second, ' ');
      previous = pad.first;
    }
    if (aligned.size() + text.size() - previous > options.maxOutputBytes)
      throw Failure{DiagnosticCode::ResourceLimit,
                    "SQL format alignment output limit exceeded",
                    {}};
    aligned.append(text, previous, std::string::npos);
    text = std::move(aligned);
  }
};
enum class Kind {
  Unknown,
  Select,
  Insert,
  Update,
  Delete,
  Create,
  Alter,
  Merge,
  Other
};
enum class Clause {
  None,
  Select,
  From,
  Where,
  Group,
  Order,
  Having,
  Set,
  Values,
  Returning,
  Other
};
enum class ParenKind {
  Expression,
  Arguments,
  Ddl,
  InsertColumns,
  RoutineParameters,
  Values,
  Query
};
struct State {
  Kind kind = Kind::Unknown;
  Clause clause = Clause::None;
  unsigned base = 0;
  std::size_t depth = 0;
  unsigned group = 0;
  bool join = false, listVertical = false;
  std::string ddl;
};
struct Paren {
  ParenKind kind;
  bool multiline;
  unsigned base;
  std::optional<State> state;
};
struct Block {
  std::string kind;
  unsigned base;
  bool closesDeclaration = false, handler = false, proceduralCase = false;
};
struct Declaration {
  unsigned base;
  bool package = false;
};
FormatResult runFormat(Dialect, std::string, const FormatStyle &, FormatOptions,
                       unsigned);
class Layout {
  Dialect dialect;
  const std::string &source;
  const FormatStyle &style;
  FormatOptions options;
  unsigned nesting;
  std::vector<FToken> &tokens;
  std::vector<std::size_t> matching;
  std::vector<std::size_t> flatWidths, clauseEnds;
  Writer out;
  State state;
  std::vector<Paren> parens;
  std::vector<Block> blocks;
  std::vector<Declaration> declarations;
  std::map<std::size_t, unsigned> between;
  unsigned indent = 0, groups = 1;
  std::size_t visits = 0;
  bool routine = false, routineHeader = false, routinePackage = false,
       statementStart = true, branch = false, beginTail = false,
       endTail = false;
  unsigned routineBase = 0;
  bool supported;
  void tick() {
    if (++visits > options.maxVisits)
      throw Failure{
          DiagnosticCode::ResourceLimit, "SQL format work limit exceeded", {}};
    if ((visits & 255) == 0)
      stop(options);
  }
  bool top() const { return parens.size() == state.depth; }
  bool is(std::size_t i, std::string_view word) const {
    return i < tokens.size() && tokens[i].is(word);
  }
  bool sym(std::size_t i, std::string_view word) const {
    return i < tokens.size() && tokens[i].symbol(word);
  }
  bool next(std::size_t i, std::string_view word) const {
    auto j = i + 1;
    while (j < tokens.size() && tokens[j].kind == TokenKind::Comment)
      ++j;
    return is(j, word) || sym(j, word);
  }
  std::size_t previousCodeToken(std::size_t i) const {
    while (i && tokens[i - 1].kind == TokenKind::Comment) --i;
    return i ? i - 1 : tokens.size();
  }
  bool fromClauseWord(std::size_t i) const {
    if (!is(i, "FROM")) return false;
    const auto distinct = previousCodeToken(i);
    if (!is(distinct, "DISTINCT")) return true;
    auto before = previousCodeToken(distinct);
    if (is(before, "NOT")) before = previousCodeToken(before);
    return !is(before, "IS");
  }
  bool loopJumpCondition(std::size_t i) const {
    auto previous = previousCodeToken(i);
    if (is(previous, "EXIT") || is(previous, "CONTINUE")) return true;
    if (previous >= tokens.size() ||
        (tokens[previous].kind != TokenKind::Word && tokens[previous].kind != TokenKind::Identifier)) return false;
    previous = previousCodeToken(previous);
    return is(previous, "EXIT") || is(previous, "CONTINUE");
  }
  unsigned blockIndent() const {
    if (blocks.empty())
      return declarations.empty() ? 0 : declarations.back().base + style.indentWidth;
    const auto &block = blocks.back();
    return block.base + style.indentWidth *
        (block.proceduralCase && style.indentCaseBranches ? 2 : 1);
  }
  unsigned currentBodyIndent() const {
    return !parens.empty() && parens.back().multiline
               ? parens.back().base + style.continuationIndentWidth
               : blockIndent();
  }
  bool keywordAllowed(const FToken &token) const {
    return token.kind == TokenKind::Word && !token.identifier &&
           (supported || dialect == Dialect::PostgreSQL ||
            dialect == Dialect::Oracle);
  }
  std::string spelling(const FToken &token) const {
    static const std::unordered_set<std::string> keywords = {
        "OVER", "WINDOW", "FILTER", "WITHIN", "PARTITION", "RANGE", "GROUPS", "UNBOUNDED", "PRECEDING", "FOLLOWING", "EXCLUDE", "CURRENT", "NULLS", "OTHERS", "TIES", "NO",
        "SELECT",    "FROM",        "WHERE",     "GROUP",      "BY",
        "HAVING",    "ORDER",       "ASC",       "DESC",       "LIMIT",
        "OFFSET",    "FETCH",       "FIRST",     "NEXT",       "ROWS",
        "ROW",       "ARRAY",       "ONLY",       "WITH",      "RECURSIVE", "AS",
        "PARTITIONS", "LINEAR", "HASH", "ALGORITHM", "PIVOT", "UNPIVOT", "INCLUDE",
        "UNION",     "ALL",         "DISTINCT",  "INTERSECT",  "EXCEPT",
        "JOIN",      "STRAIGHT_JOIN", "LEFT",    "RIGHT",      "FULL",       "INNER",
        "OUTER",     "CROSS",       "NATURAL",   "ON",         "USING",
        "AND",       "OR",          "NOT",       "IN",         "EXISTS",
        "BETWEEN",   "SYMMETRIC",   "ASYMMETRIC", "LIKE",      "ILIKE", "IS", "NULL", "UNKNOWN",
        "TRUE",      "FALSE",       "CASE",      "WHEN",       "THEN",
        "ELSE",      "END",         "INSERT",    "INTO",       "VALUES",
        "DEFAULT",   "UPDATE",      "SET",       "DELETE",     "RETURNING",
        "OUTPUT",    "CONFLICT",    "DO",        "NOTHING",    "DUPLICATE",
        "KEY",       "IGNORE",      "CREATE",    "OR",         "REPLACE",
        "ALTER",     "DROP",        "TRUNCATE",  "RENAME",     "TO",
        "TABLE",     "VIEW",        "INDEX",     "SCHEMA",     "DATABASE",
        "SEQUENCE",  "TEMP",        "TEMPORARY", "UNIQUE",     "PRIMARY",
        "FOREIGN",   "REFERENCES",  "CHECK",     "CONSTRAINT", "GENERATED",
        "ALWAYS",    "IDENTITY",    "STORED",    "VIRTUAL",    "ADD",
        "COLUMN",    "MODIFY",      "CHANGE",    "IF",         "CASCADE",
        "RESTRICT",  "ENGINE",      "CHARSET",   "COLLATE",    "COMMENT",
        "LANGUAGE",  "FUNCTION",    "PROCEDURE", "PROC",       "PACKAGE",
        "BODY",      "TRIGGER",     "BEGIN",     "DECLARE",    "RETURN",
        "RETURNS",   "SONAME",      "AGGREGATE", "ELSIF",      "ELSEIF",    "LOOP",       "WHILE",
        "FOR",       "REVERSE",     "EACH",      "DO",        "REPEAT", "UNTIL", "EXIT", "CONTINUE", "LEAVE", "ITERATE",
        "EXCEPTION", "RAISE",       "EXECUTE",   "EXEC",       "CALL",
        "MERGE",     "MATCHED",     "TOP",       "PERCENT",    "TRY",
        "CATCH",     "TRANSACTION", "TRAN",      "COMMIT",     "ROLLBACK",
        "SAVEPOINT", "RELEASE",     "START",     "WORK",       "CHAIN",
        "PREPARE",   "EXECUTE",     "DEALLOCATE", "ROLE", "NONE", "PRAGMA",
        "VACUUM", "ANALYZE", "ANALYSE", "FREEZE", "VERBOSE", "DISABLE_PAGE_SKIPPING",
        "SKIP_LOCKED", "INDEX_CLEANUP", "PROCESS_TOAST", "PARALLEL", "AUTO", "OFF",
        "COPY", "STDIN", "STDOUT", "FORMAT", "HEADER", "DELIMITER", "ENCODING",
        "QUOTE", "ESCAPE", "FORCE_QUOTE", "FORCE_NOT_NULL", "FORCE_NULL",
        "JSON_TABLE","COLUMNS",     "NESTED",    "PATH",       "ORDINALITY",
        "ISOLATION", "LEVEL",       "DEFERRED",  "IMMEDIATE",  "EXCLUSIVE",
        "CONSISTENT","SNAPSHOT",    "READ",      "WRITE",      "SERIALIZABLE",
        "REPEATABLE","UNCOMMITTED", "COMMITTED", "INOUT",      "OUT", "IN",
        "OF",        "BEFORE",      "AFTER",     "INSTEAD",    "ENABLE",
        "EXTRACT", "YEAR", "QUARTER", "MONTH", "WEEK", "DAY", "HOUR", "MINUTE", "SECOND", "MICROSECOND", "YEAR_MONTH", "DAY_HOUR", "DAY_MINUTE", "DAY_SECOND", "HOUR_MINUTE", "HOUR_SECOND", "MINUTE_SECOND", "DAY_MICROSECOND", "HOUR_MICROSECOND", "MINUTE_MICROSECOND", "SECOND_MICROSECOND", "EPOCH", "DOW", "DOY", "ISODOW", "ISOYEAR", "TIMEZONE", "TIMEZONE_HOUR", "TIMEZONE_MINUTE", "TIMEZONE_REGION", "TIMEZONE_ABBR", "DATE", "TIME", "TIMESTAMP", "INTERVAL", "ZONE", "WITHOUT", "LOCAL", "DISABLE", "SHOW", "DESCRIBE", "DATABASES", "SCHEMAS", "TABLES", "COLUMNS", "FIELDS", "INDEXES", "KEYS", "STATUS", "VARIABLES", "GLOBAL", "SESSION", "PROCESSLIST", "EXTENDED", "EVENT"};
    const bool arithmeticKeyword = mysql(dialect) && (token.up == "DIV" || token.up == "MOD");
    static const std::unordered_set<std::string> tidbKeywords = {
        "AUTO_RANDOM", "AUTO_RANDOM_BASE", "SHARD_ROW_ID_BITS", "PRE_SPLIT_REGIONS",
        "CLUSTERED", "NONCLUSTERED", "PLACEMENT", "POLICY", "ADMIN", "DDL", "JOBS"};
    static const std::unordered_set<std::string> oceanbaseKeywords = {
        "TABLEGROUP", "TABLEGROUPS", "SHARDING"};
    const bool distributedKeyword = (dialect == Dialect::TiDB && tidbKeywords.count(token.up)) ||
        (dialect == Dialect::OceanBase && oceanbaseKeywords.count(token.up));
    if (style.keywordCase == KeywordCase::Preserve || !keywordAllowed(token) ||
        (!token.diagnosticKeyword && !arithmeticKeyword && !distributedKeyword && !keywords.count(token.up)))
      return token.raw;
    if (style.keywordCase == KeywordCase::Upper)
      return token.up;
    auto text = token.up;
    for (auto &c : text)
      if (c >= 'A' && c <= 'Z')
        c += 32;
    return text;
  }
  bool nonStructuralSymbol(const FToken &t) const {
    return t.kind == TokenKind::Symbol && !t.opaque && t.raw != "(" &&
           t.raw != ")" && t.raw != "," && t.raw != ";" && t.raw != "." &&
           t.raw != "[" && t.raw != "]";
  }
  bool wantSpace(std::size_t i) const {
    if (i == 0 || out.atStart)
      return false;
    const auto &a = tokens[i - 1];
    const auto &b = tokens[i];
    // A standalone MySQL @ joins the user and host of an account name.
    if (mysql(dialect) && (a.symbol("@") || b.symbol("@")))
      return false;
    const bool gap = b.range.begin > a.range.end;
    if (nonStructuralSymbol(a) && nonStructuralSymbol(b))
      return gap;
    if (b.symbol(".") || a.symbol(".") || b.symbol("::") || a.symbol("::") ||
        b.symbol(",") || b.symbol(";") || b.opaque == 2)
      return false;
    if (b.symbol(")") || a.symbol("("))
      return style.spacesInParentheses && !(a.symbol("(") && b.symbol(")"));
    if (b.symbol("[") || b.symbol("]") || a.symbol("["))
      return false;
    if (dialect==Dialect::PostgreSQL && (a.symbol(":") || b.symbol(":")))
      return false;
    if (a.symbol(","))
      return style.spaceAfterComma;
    if (b.symbol("(")) {
      const bool function = a.kind == TokenKind::Identifier ||
                            a.kind == TokenKind::Word || a.symbol(")");
      if (function && !a.is("IN") && !a.is("VALUES") && !a.is("EXISTS") &&
          !a.is("AS") && !a.is("TABLE") && !a.is("CHECK") && !a.is("VACUUM") && !a.is("COPY"))
        return mysql(dialect) ? gap : false;
      return true;
    }
    if (nonStructuralSymbol(a) || nonStructuralSymbol(b)) {
      // Unary +/- must stay adjacent to their operand unless it was a
      // separate operator token sequence in the original source.
      if ((a.symbol("-") || a.symbol("+")) &&
          (i < 2 || tokens[i - 2].symbol("(") || tokens[i - 2].symbol(",") ||
           nonStructuralSymbol(tokens[i - 2])))
        return false;
      return style.spacesAroundOperators;
    }
    return true;
  }
  void emit(std::size_t i, std::string text = {}) {
    if (text.empty())
      text = spelling(tokens[i]);
    out.indent(indent);
    if (wantSpace(i))
      out.space();
    out.raw(text);
  }
  void newLine(unsigned target, unsigned empty = 0) {
    out.line(empty);
    indent = target;
  }
  bool vertical(ListLayout layout, std::size_t begin, std::size_t end,
                unsigned base) const {
    return layout == ListLayout::Vertical ||
           (layout == ListLayout::Fit && style.columnLimit &&
            base + flatWidths[std::min(end, tokens.size())] -
                    flatWidths[begin] >
                style.columnLimit);
  }
  std::size_t clauseEnd(std::size_t begin) const { return clauseEnds[begin]; }
  ListLayout currentList() const {
    if (!parens.empty() && parens.size() > state.depth) {
      switch (parens.back().kind) {
      case ParenKind::Ddl:
        return style.ddlColumns;
      case ParenKind::InsertColumns:
        return style.insertColumns;
      case ParenKind::RoutineParameters:
        return style.routineParameters;
      default:
        return style.functionArguments;
      }
    }
    switch (state.clause) {
    case Clause::Select:
    case Clause::Returning:
      return style.selectList;
    case Clause::Set:
      return style.updateAssignments;
    case Clause::Values:
      return style.valuesRows;
    default:
      return state.kind == Kind::Alter ? style.ddlColumns : ListLayout::Fit;
    }
  }
  bool listMultiline(std::size_t i) const {
    if (!parens.empty() && parens.size() > state.depth)
      return parens.back().multiline;
    return state.listVertical || vertical(currentList(), i, clauseEnd(i),
                                          state.base + style.indentWidth);
  }
  bool joinStart(std::size_t i) const {
    return is(i, "JOIN") || (is(i, "STRAIGHT_JOIN") && state.clause != Clause::Select) ||
           ((is(i, "LEFT") || is(i, "RIGHT") || is(i, "FULL") ||
             is(i, "INNER") || is(i, "CROSS") || is(i, "NATURAL")) &&
            (next(i, "JOIN") || next(i, "OUTER") || next(i, "LEFT") ||
             next(i, "RIGHT") || next(i, "FULL")));
  }
  bool joinTail(std::size_t i) const {
    return i > 0 &&
           (is(i - 1, "LEFT") || is(i - 1, "RIGHT") || is(i - 1, "FULL") ||
            is(i - 1, "INNER") || is(i - 1, "CROSS") || is(i - 1, "NATURAL") ||
            is(i - 1, "OUTER"));
  }
  void startClause(std::size_t i, Clause clause, bool multilineList = false) {
    if (style.breakBeforeClauses || clause == Clause::Select)
      newLine(state.base);
    state.clause = clause;
    state.listVertical = multilineList;
    state.group = groups++;
    emit(i);
    if (multilineList)
      newLine(state.base + style.indentWidth);
    else
      indent = state.base;
  }
  void parenthesis(std::size_t i) {
    const auto end = matching[i];
    ParenKind kind = ParenKind::Expression;
    ListLayout layout = style.functionArguments;
    const bool query = next(i, "SELECT") || next(i, "WITH");
    if (query)
      kind = ParenKind::Query;
    else if (routineHeader && state.ddl != "TABLE" && state.ddl != "INDEX") {
      kind = ParenKind::RoutineParameters;
      layout = style.routineParameters;
      routineHeader = false;
    } else if ((state.kind == Kind::Create || state.kind == Kind::Alter) &&
               state.ddl == "TABLE" && parens.size() == state.depth) {
      kind = ParenKind::Ddl;
      layout = style.ddlColumns;
    } else if (state.kind == Kind::Insert && state.clause != Clause::Values &&
               top()) {
      kind = ParenKind::InsertColumns;
      layout = style.insertColumns;
    } else if (state.clause == Clause::Values && top()) {
      kind = ParenKind::Values;
      layout = style.functionArguments;
    } else if (i > 0 &&
               (tokens[i - 1].kind == TokenKind::Word ||
                tokens[i - 1].kind == TokenKind::Identifier) &&
               !tokens[i - 1].is("IN") && !tokens[i - 1].is("WHERE") &&
               !tokens[i - 1].is("AND") && !tokens[i - 1].is("OR"))
      kind = ParenKind::Arguments;
    const bool multi = query ? style.indentSubqueries
                             : vertical(layout, i, end + 1, out.column + 1);
    const auto base = indent;
    emit(i);
    Paren frame{kind, multi, base, {}};
    if (query) {
      frame.state = state;
      state = {};
      state.base =
          base + (style.indentSubqueries ? style.continuationIndentWidth : 0);
      state.depth = parens.size() + 1;
      state.group = groups++;
    }
    parens.push_back(frame);
    if (multi)
      newLine(base + (kind == ParenKind::Ddl ||
                              kind == ParenKind::RoutineParameters ||
                              kind == ParenKind::InsertColumns
                          ? style.indentWidth
                          : style.continuationIndentWidth));
  }
  std::optional<std::string> embeddedBody(std::size_t i) {
    auto &token = tokens[i];
    if (!style.formatRoutineBodies || dialect != Dialect::PostgreSQL ||
        nesting >= 2 || !routine || token.kind != TokenKind::String ||
        token.raw.empty() || token.raw[0] != '$')
      return {};
    std::string language;
    for (auto j = i; j > 0;) {
      --j;
      tick();
      if (tokens[j].symbol(";"))
        break;
      if (tokens[j].is("LANGUAGE") && j + 1 < tokens.size())
        language = upper(tokens[j + 1].raw);
    }
    for (auto j = i + 1; j < tokens.size() && !tokens[j].symbol(";"); ++j) {
      tick();
      if (tokens[j].is("LANGUAGE") && j + 1 < tokens.size())
        language = upper(tokens[j + 1].raw);
    }
    if (language != "SQL" && language != "PLPGSQL")
      return {};
    const auto end = token.raw.find('$', 1);
    if (end == std::string::npos)
      return {};
    const auto tag = token.raw.substr(0, end + 1);
    if (token.raw.size() < tag.size() * 2 ||
        token.raw.substr(token.raw.size() - tag.size()) != tag)
      return {};
    auto body = token.raw.substr(tag.size(), token.raw.size() - 2 * tag.size());
    auto formatted = runFormat(dialect, body, style, options, nesting + 1);
    if (!formatted.ok())
      return {};
    auto text = tag + out.eol + formatted.sql;
    while (text.size() >= out.eol.size() &&
           text.compare(text.size() - out.eol.size(), out.eol.size(),
                        out.eol) == 0)
      text.resize(text.size() - out.eol.size());
    text += out.eol + tag;
    token.changedBody = true;
    return text;
  }

public:
  Layout(Dialect d, const std::string &source, const FormatStyle &style,
         FormatOptions options, unsigned nesting, std::vector<FToken> &tokens,
         bool supported)
      : dialect(d), source(source), style(style), options(std::move(options)),
        nesting(nesting), tokens(tokens),
        matching(tokens.size(), tokens.size()), flatWidths(tokens.size() + 1),
        clauseEnds(tokens.size(), tokens.size()),
        out(style, this->options,
            style.lineEnding == LineEnding::CRLF ||
                    (style.lineEnding == LineEnding::Preserve &&
                     source.find("\r\n") != std::string::npos)
                ? "\r\n"
                : "\n"),
        supported(supported) {
    std::vector<std::size_t> stack;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
      tick();
      flatWidths[i + 1] =
          flatWidths[i] + width(tokens[i].raw, style.tabWidth) + 1;
      if (tokens[i].symbol("(")) {
        if (stack.size() >= this->options.language.limits.nesting)
          throw Failure{DiagnosticCode::ResourceLimit,
                        "SQL format parenthesis nesting limit exceeded",
                        tokens[i].range};
        stack.push_back(i);
      } else if (tokens[i].symbol(")")) {
        if (stack.empty())
          throw Failure{DiagnosticCode::ExpectedToken,
                        "unmatched closing parenthesis", tokens[i].range};
        matching[i] = stack.back();
        matching[stack.back()] = i;
        stack.pop_back();
      }
    }
    // Suffix boundary cache: each nested range is skipped once, avoiding
    // rescanning the whole SELECT list at every comma.
    for (std::size_t i = tokens.size(); i > 0;) {
      --i;
      tick();
      auto j = i + 1;
      if (j < tokens.size()) {
        if (tokens[j].symbol("(") && matching[j] < tokens.size())
          clauseEnds[i] = clauseEnds[matching[j]];
        else if (tokens[j].symbol(")") || tokens[j].symbol(";") ||
                 fromClauseWord(j) || tokens[j].is("WHERE") ||
                 tokens[j].is("GROUP") || tokens[j].is("ORDER") ||
                 tokens[j].is("HAVING") || tokens[j].is("WINDOW") || tokens[j].is("RETURNING") ||
                 tokens[j].is("OUTPUT") || tokens[j].is("VALUES") ||
                 tokens[j].is("UNION"))
          clauseEnds[i] = j;
        else
          clauseEnds[i] = clauseEnds[j];
      }
    }
    if (!stack.empty())
      throw Failure{DiagnosticCode::ExpectedToken,
                    "missing closing parenthesis", tokens[stack.back()].range};
  }
  std::string format() {
    for (std::size_t i = 0; i < tokens.size(); ++i) {
      tick();
      auto &t = tokens[i];
      const auto gap = i ? std::string_view(source).substr(
                               tokens[i - 1].range.end,
                               t.range.begin - tokens[i - 1].range.end)
                         : std::string_view{};
      if (t.opaque == 1) {
        newLine(0);
        out.raw(t.raw);
        newLine(0);
        state = {};
        routine = routineHeader = false;
        statementStart = true;
        continue;
      }
      if (t.opaque == 2) {
        emit(i, t.raw);
        newLine(0, style.blankLinesBetweenStatements);
        state = {};
        routine = routineHeader = false;
        statementStart = true;
        continue;
      }
      if (t.opaque == 3) {
        emit(i, t.raw);
        continue;
      }
      if (t.kind == TokenKind::Comment) {
        const bool line = t.raw.rfind("--", 0) == 0 || t.raw.rfind('#', 0) == 0;
        const bool originalNewline =
            gap.find('\n') != std::string_view::npos || i == 0;
        if (originalNewline && !out.atStart)
          newLine(indent);
        if (!out.atStart)
          out.space();
        out.indent(indent);
        out.raw(t.raw);
        if (line || t.raw.find('\n') != std::string::npos)
          newLine(indent);
        else
          out.space();
        continue;
      }
      // PostgreSQL concatenates adjacent string literals only across a newline.
      if (dialect == Dialect::PostgreSQL && i && t.kind == TokenKind::String &&
          tokens[i - 1].kind == TokenKind::String &&
          gap.find('\n') != std::string_view::npos)
        newLine(indent);
      if (style.maxEmptyLines && gap.find('\n') != std::string_view::npos &&
          out.atStart) {
        const auto lines = std::count(gap.begin(), gap.end(), '\n');
        if (lines > 1) {
          unsigned have = 0;
          for (std::size_t j = out.text.size();
               j > 0 && (out.text[j - 1] == '\n' || out.text[j - 1] == '\r');
               --j)
            if (out.text[j - 1] == '\n')
              ++have;
          if (have < 2)
            for (unsigned n = 0; n < std::min<unsigned>(style.maxEmptyLines,
                                                        unsigned(lines - 1));
                 ++n)
              out.line(1);
        }
      }
      if (t.symbol("(")) {
        parenthesis(i);
        statementStart = false;
        continue;
      }
      if (t.symbol(")")) {
        const auto frame = parens.back();
        parens.pop_back();
        if (frame.multiline)
          newLine(frame.base);
        else if (style.spacesInParentheses && i && !tokens[i - 1].symbol("("))
          out.space();
        emit(i);
        indent = frame.base;
        if (frame.state)
          state = *frame.state;
        statementStart = false;
        continue;
      }
      if (t.symbol(",")) {
        const bool multi = listMultiline(i);
        const auto target =
            !parens.empty() && parens.size() > state.depth
                ? parens.back().base +
                      (parens.back().kind == ParenKind::Ddl ||
                               parens.back().kind ==
                                   ParenKind::RoutineParameters ||
                               parens.back().kind == ParenKind::InsertColumns
                           ? style.indentWidth
                           : style.continuationIndentWidth)
                : state.base + style.indentWidth;
        if (multi && style.commaPosition == CommaPosition::Leading)
          newLine(target);
        emit(i);
        if (multi && style.commaPosition == CommaPosition::Trailing)
          newLine(target);
        else if (style.spaceAfterComma)
          out.space();
        statementStart = false;
        continue;
      }
      if (t.symbol(";")) {
        emit(i);
        if (!blocks.empty() || !declarations.empty()) {
          newLine(blockIndent());
          state = {};
          state.base = indent;
          state.depth = parens.size();
          statementStart = true;
        } else {
          newLine(0, style.blankLinesBetweenStatements);
          state = {};
          routine = routineHeader = false;
          statementStart = true;
        }
        between.clear();
        continue;
      }
      const bool lineOnly =
          ((dialect == Dialect::SQLServer && t.is("GO")) ||
           (dialect == Dialect::Oracle && t.symbol("/"))) &&
          [&] {
            const auto before = i ? tokens[i - 1].range.end : 0;
            const auto after = i + 1 < tokens.size() ? tokens[i + 1].range.begin
                                                     : source.size();
            const auto left =
                std::string_view(source).substr(before, t.range.begin - before);
            const auto right = std::string_view(source).substr(
                t.range.end, after - t.range.end);
            return (!i || left.find('\n') != std::string_view::npos) &&
                   (i + 1 == tokens.size() ||
                    right.find('\n') != std::string_view::npos);
          }();
      if ((dialect == Dialect::SQLServer && t.is("GO") && lineOnly) ||
          (dialect == Dialect::Oracle && t.symbol("/") && lineOnly)) {
        newLine(0);
        emit(i);
        newLine(0, style.blankLinesBetweenStatements);
        state = {};
        statementStart = true;
        continue;
      }
      if (endTail) {
        emit(i);
        endTail = false;
        continue;
      }
      if (beginTail) {
        emit(i);
        beginTail = false;
        newLine(blockIndent());
        statementStart = true;
        continue;
      }
      if (t.is("END") && (!blocks.empty() || !declarations.empty())) {
        bool expressionCase = false;
        if (!blocks.empty()) {
          const auto block = blocks.back();
          blocks.pop_back();
          newLine(block.base);
          expressionCase = block.kind == "CASE" && !block.proceduralCase;
          if (block.closesDeclaration && !declarations.empty())
            declarations.pop_back();
        } else {
          newLine(declarations.back().base);
          declarations.pop_back();
        }
        emit(i);
        if (next(i, "IF") || next(i, "LOOP") || next(i, "WHILE") ||
            next(i, "REPEAT") || next(i, "CASE") || next(i, "TRY") ||
            next(i, "CATCH"))
          endTail = true;
        if (!expressionCase) {
          state = {};
          state.base = indent;
        }
        statementStart = false;
        continue;
      }
      if (t.is("BEGIN") && (routine || nesting || dialect == Dialect::Oracle || dialect == Dialect::SQLServer ||
                           (dialect == Dialect::MariaDB && next(i, "NOT"))) &&
          !next(i, "TRAN") && !next(i, "TRANSACTION") &&
          !next(i, "WORK") && !next(i, ";")) {
        const auto base = routineHeader && state.ddl == "TRIGGER" ? routineBase
                          : !declarations.empty() && blocks.empty() ? declarations.back().base : indent;
        newLine(base);
        emit(i);
        blocks.push_back({"BEGIN", base, !declarations.empty()});
        if (next(i, "TRY") || next(i, "CATCH"))
          beginTail = true;
        else
          newLine(base + style.indentWidth);
        state = {};
        state.base = base + style.indentWidth;
        state.depth = parens.size();
        statementStart = true;
        continue;
      }
      if (t.is("DECLARE") &&
          (dialect == Dialect::Oracle ||
           (dialect == Dialect::PostgreSQL && nesting > 0)) &&
          blocks.empty()) {
        newLine(indent);
        emit(i);
        declarations.push_back({indent, false});
        newLine(indent + style.indentWidth);
        state = {};
        statementStart = true;
        continue;
      }
      if (t.is("EXCEPTION") && !blocks.empty()) {
        newLine(blocks.back().base);
        emit(i);
        blocks.back().handler = true;
        newLine(blocks.back().base + style.indentWidth);
        statementStart = true;
        continue;
      }
      if (t.is("CASE")) {
        const bool procedure = statementStart && !blocks.empty() && dialect != Dialect::SQLServer &&
                               (routine || nesting || dialect == Dialect::Oracle);
        if (procedure)
          newLine(blockIndent());
        emit(i);
        blocks.push_back({"CASE", indent, false, false, procedure});
        if (!procedure && style.indentCaseBranches)
          newLine(indent + style.indentWidth);
        statementStart = false;
        continue;
      }
      if ((t.is("ELSE") || (t.is("WHEN") && !loopJumpCondition(i))) && !blocks.empty() &&
          (blocks.back().kind == "CASE" || blocks.back().handler)) {
        newLine(blocks.back().base +
                (style.indentCaseBranches ? style.indentWidth : 0));
        emit(i);
        statementStart = t.is("ELSE") && blocks.back().proceduralCase;
        if (statementStart) {
          newLine(blockIndent());
          state = {};
          state.base = indent;
          state.depth = parens.size();
        }
        continue;
      }
      if ((t.is("ELSE") || t.is("ELSIF") || t.is("ELSEIF")) &&
          !blocks.empty() && blocks.back().kind == "IF") {
        newLine(blocks.back().base);
        emit(i);
        branch = true;
        if (t.is("ELSE")) {
          newLine(blocks.back().base + style.indentWidth);
          branch = false;
          statementStart = true;
        }
        continue;
      }
      if (t.is("IF") && statementStart && !next(i, "(")) {
        newLine(blockIndent());
        emit(i);
        branch = false;
        statementStart = false;
        continue;
      }
      if (t.is("THEN") && (blocks.empty() || blocks.back().kind != "CASE" ||
                           blocks.back().proceduralCase)) {
        emit(i);
        if (!blocks.empty() && blocks.back().proceduralCase) {
          newLine(blockIndent());
        } else if (!blocks.empty() && blocks.back().handler) {
          newLine(blocks.back().base + 2 * style.indentWidth);
        } else if (state.kind != Kind::Merge) {
          if (!branch)
            blocks.push_back({"IF", indent});
          branch = false;
          newLine(blocks.back().base + style.indentWidth);
        }
        statementStart = true;
        state = {};
        state.base = indent;
        state.depth = parens.size();
        continue;
      }
      if (mysql(dialect) && t.symbol(":") && !blocks.empty() &&
          (next(i, "REPEAT") || next(i, "LOOP") || next(i, "WHILE"))) {
        emit(i);
        statementStart = true;
        continue;
      }
      if (t.is("LOOP") || (t.is("DO") && !statementStart && !blocks.empty()) ||
          (t.is("REPEAT") && statementStart)) {
        emit(i);
        blocks.push_back({t.up, indent});
        newLine(indent + style.indentWidth);
        state = {};
        state.base = indent;
        state.depth = parens.size();
        statementStart = true;
        continue;
      }
      if (t.is("UNTIL") && !blocks.empty() && blocks.back().kind == "REPEAT") {
        newLine(blocks.back().base);
        emit(i);
        continue;
      }
      if (statementStart) {
        indent = std::max(indent, blockIndent());
        state.base = indent;
        state.depth = parens.size();
        state.group = groups++;
        if (t.is("SELECT") || t.is("WITH"))
          state.kind = Kind::Select;
        else if (t.is("INSERT") || (mysql(dialect) && t.is("REPLACE")))
          state.kind = Kind::Insert;
        else if (t.is("UPDATE"))
          state.kind = Kind::Update;
        else if (t.is("DELETE"))
          state.kind = Kind::Delete;
        else if (t.is("CREATE"))
          state.kind = Kind::Create;
        else if (t.is("ALTER"))
          state.kind = Kind::Alter;
        else if (t.is("MERGE"))
          state.kind = Kind::Merge;
        else
          state.kind = Kind::Other;
        statementStart = false;
      }
      if ((state.kind == Kind::Create || state.kind == Kind::Alter) &&
          (t.is("TABLE") || t.is("INDEX") || t.is("VIEW") || t.is("FUNCTION") ||
           t.is("PROCEDURE") || t.is("PROC") || t.is("PACKAGE") ||
           t.is("TRIGGER")) &&
          state.ddl.empty()) {
        state.ddl = t.up;
        if (t.is("FUNCTION") || t.is("PROCEDURE") || t.is("PROC") ||
            t.is("PACKAGE") || t.is("TRIGGER")) {
          routine = routineHeader = true;
          routinePackage = t.is("PACKAGE");
          routineBase = state.base;
        }
      }
      if (!declarations.empty() && declarations.back().package &&
          (t.is("PROCEDURE") || t.is("FUNCTION")) && top()) {
        routine = routineHeader = true;
        routinePackage = false;
        routineBase = indent;
        state.ddl = t.up;
      }
      if (routine && (t.is("AS") || t.is("IS")) &&
          (dialect == Dialect::Oracle || dialect == Dialect::SQLServer) &&
          top()) {
        newLine(routineBase);
        emit(i);
        declarations.push_back({routineBase, routinePackage});
        newLine(routineBase + style.indentWidth);
        state = {};
        state.base = indent;
        state.depth = parens.size();
        routineHeader = false;
        continue;
      }
      if (t.kind == TokenKind::String) {
        if (auto body = embeddedBody(i)) {
          if (!out.atStart)
            out.space();
          out.indent(indent);
          out.raw(*body);
          continue;
        }
      }
      if (top() && t.is("SELECT")) {
        if (state.kind != Kind::Select)
          newLine(state.base);
        state.kind = Kind::Select;
        startClause(i, Clause::Select,
                    vertical(style.selectList, i, clauseEnd(i), state.base));
        continue;
      }
      if (top() && joinStart(i) && !joinTail(i) &&
          (state.kind == Kind::Select || state.kind == Kind::Update ||
           state.kind == Kind::Delete)) {
        if (style.breakBeforeJoin)
          newLine(state.base);
        state.join = true;
        state.clause = Clause::From;
        emit(i);
        continue;
      }
      if (top() &&
          ((fromClauseWord(i) && state.kind != Kind::Delete) || t.is("WHERE") ||
           (t.is("GROUP") && next(i, "BY")) ||
           (t.is("ORDER") && next(i, "BY")) || t.is("HAVING") || t.is("WINDOW") ||
           t.is("LIMIT") || t.is("OFFSET") || t.is("FETCH") ||
           t.is("RETURNING") || t.is("OUTPUT") ||
           (t.is("VALUES") && state.kind == Kind::Insert) ||
           (t.is("SET") && state.kind == Kind::Update) || t.is("UNION") ||
           t.is("INTERSECT") || t.is("EXCEPT") || (t.is("ON") && state.join) ||
           (t.is("USING") && state.kind == Kind::Merge) ||
           (t.is("ON") && state.kind == Kind::Merge) ||
           (t.is("WHEN") && state.kind == Kind::Merge))) {
        Clause clause = t.is("FROM")     ? Clause::From
                        : t.is("WHERE")  ? Clause::Where
                        : t.is("HAVING") ? Clause::Having
                        : t.is("SET")    ? Clause::Set
                        : t.is("VALUES") ? Clause::Values
                        : t.is("RETURNING") || t.is("OUTPUT")
                            ? Clause::Returning
                        : t.is("ON")    ? Clause::Where
                        : t.is("GROUP") ? Clause::Group
                        : t.is("ORDER") ? Clause::Order
                                        : Clause::Other;
        const bool multiline =
            (clause == Clause::Values &&
             vertical(style.valuesRows, i, clauseEnd(i), state.base)) ||
            (clause == Clause::Set &&
             vertical(style.updateAssignments, i, clauseEnd(i), state.base)) ||
            (clause == Clause::Returning &&
             vertical(style.selectList, i, clauseEnd(i), state.base));
        startClause(i, clause, multiline);
        continue;
      }
      if (t.is("BETWEEN"))
        ++between[parens.size()];
      if (t.is("AND") && between[parens.size()])
        --between[parens.size()];
      else if (style.breakBeforeBooleanOperators &&
               (t.is("AND") || t.is("OR")) &&
               (state.clause == Clause::Where ||
                state.clause == Clause::Having))
        newLine(top() ? state.base + style.continuationIndentWidth
                      : currentBodyIndent());
      if (style.columnLimit &&
          out.column + width(t.raw, style.tabWidth) + 1 > style.columnLimit &&
          i && (tokens[i - 1].symbol(",") || t.is("AND") || t.is("OR")))
        newLine(state.base + style.continuationIndentWidth);
      if ((style.alignAliases && t.is("AS") && state.clause == Clause::Select &&
           top()) ||
          (style.alignAssignments && t.symbol("=") &&
           state.clause == Clause::Set && top())) {
        out.indent(indent);
        if (wantSpace(i))
          out.space();
        out.mark(state.group);
        out.raw(spelling(t));
      } else
        emit(i);
    }
    out.align();
    while (!out.text.empty() &&
           (out.text.back() == ' ' || out.text.back() == '\t' ||
            out.text.back() == '\r' || out.text.back() == '\n'))
      out.text.pop_back();
    if (style.finalNewline && !out.text.empty())
      out.raw(out.eol);
    return std::move(out.text);
  }
};
FormatResult runFormat(Dialect dialect, std::string source,
                       const FormatStyle &style, FormatOptions options,
                       unsigned depth) {
  FormatResult result;
  try {
    // Programmatic callers receive the same option bounds as file users.
    parseFormatStyle(dumpFormatStyle(style));
    stop(options);
    auto originalStop = options.language.shouldStop;
    auto formatStop = options.shouldStop;
    options.language.shouldStop = [originalStop, formatStop] {
      return (originalStop && originalStop()) || (formatStop && formatStop());
    };
    auto tokens = tokenize(dialect, source, options);
    auto language = options.language;
    language.shouldStop = [options] {
      return (options.shouldStop && options.shouldStop()) ||
             (options.language.shouldStop && options.language.shouldStop());
    };
    auto ast = compile(dialect, source, language);
    result.syntaxSupported = ast->syntaxValid;
    for (const auto &error : ast->diagnostics)
      if (error.code == DiagnosticCode::ResourceLimit ||
          error.code == DiagnosticCode::Stopped)
        throw Failure{error.code, error.message, error.range};
    const bool moduleArguments = dialect == Dialect::SQLite && preserveModuleArguments(tokens, *ast);
    std::unordered_set<std::size_t> names;
    for (auto range : ast->identifierRanges)
      names.insert(range.begin);
    for(ExprId id=1;id<=ast->ast.size();++id) {
      const auto &expression=ast->ast.expression(id);
      if(expression.kind!=ExprKind::TypedLiteral || !expression.castType || expression.castType->name.size()!=1)continue;
      const auto &part=expression.castType->name[0];const auto spelling=asciiUpper(ast->ast.identifier(part.identifier));
      if(!part.quoted && (spelling=="DATE" || spelling=="TIME" || spelling=="TIMESTAMP" || spelling=="INTERVAL"))names.erase(part.range.begin);
    }
    for (auto &token : tokens)
      token.identifier = names.count(token.range.begin);
    if (depth && dialect == Dialect::PostgreSQL) {
      for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (!tokens[i].is("GET")) continue;
        auto previous = i;
        while (previous && tokens[previous - 1].kind == TokenKind::Comment) --previous;
        if (previous) {
          const auto &token = tokens[previous - 1];
          if (!token.symbol(";") && !token.is("BEGIN") && !token.is("THEN") &&
              !token.is("ELSE") && !token.is("LOOP")) continue;
        }
        auto area = i + 1;
        while (area < tokens.size() && tokens[area].kind == TokenKind::Comment) ++area;
        auto header = area;
        if (area < tokens.size() && (tokens[area].is("CURRENT") || tokens[area].is("STACKED"))) {
          do { ++header; } while (header < tokens.size() && tokens[header].kind == TokenKind::Comment);
        }
        if (header == tokens.size() || !tokens[header].is("DIAGNOSTICS")) continue;
        tokens[i].diagnosticKeyword = tokens[header].diagnosticKeyword = true;
        if (area != header) tokens[area].diagnosticKeyword = true;
        bool field = false;
        for (i = header + 1; i < tokens.size() && !tokens[i].symbol(";"); ++i) {
          auto &token = tokens[i];
          if (token.kind == TokenKind::Comment) continue;
          if (field) token.diagnosticKeyword = std::any_of(std::begin(pgDiagnosticFields), std::end(pgDiagnosticFields),
              [&](const auto &value) { return token.is(value.spelling); });
          field = token.symbol("=") || token.symbol(":=");
        }
      }
    }
    Layout layout(dialect, source, style, options, depth, tokens,
                  result.syntaxSupported);
    result.sql = layout.format();
    stop(options);
    auto after = tokenize(dialect, result.sql, options);
    if (moduleArguments) {
      auto checked = compile(dialect, result.sql, language);
      for (const auto &error : checked->diagnostics)
        if (error.code == DiagnosticCode::ResourceLimit || error.code == DiagnosticCode::Stopped)
          throw Failure{error.code, error.message, error.range};
      preserveModuleArguments(after, *checked);
    }
    if (tokens.size() != after.size())
      throw Failure{
          DiagnosticCode::InvalidQueryShape,
          "formatting would change SQL token boundaries; text left unchanged",
          {}};
    for (std::size_t i = 0; i < tokens.size(); ++i) {
      const auto &a = tokens[i];
      const auto &b = after[i];
      bool equal = a.kind == b.kind && a.opaque == b.opaque &&
                   (a.raw == b.raw ||
                    (a.kind == TokenKind::Word && !a.identifier &&
                     upper(a.raw) == upper(b.raw)) ||
                    a.changedBody);
      if (!equal)
        throw Failure{DiagnosticCode::InvalidQueryShape,
                      "formatting would change a literal, comment or token; "
                      "text left unchanged",
                      a.range};
      result.mapping.push_back({a.range, b.range});
    }
  } catch (const Failure &error) {
    result.sql.clear();
    result.mapping.clear();
    result.diagnostics.push_back({error.code, error.range, error.message});
  } catch (const std::exception &error) {
    result.sql.clear();
    result.mapping.clear();
    result.diagnostics.push_back(
        {DiagnosticCode::InvalidQueryShape, {}, error.what()});
  }
  return result;
}
} // namespace
FormatResult formatSql(Dialect dialect, std::string source,
                       const FormatStyle &style, FormatOptions options) {
  return runFormat(dialect, std::move(source), style, std::move(options), 0);
}
} // namespace navidog::sql::compiler
