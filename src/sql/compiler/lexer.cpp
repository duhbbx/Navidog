#include "parser.hpp"
#include <cctype>

namespace navidog::sql::compiler {
namespace {
bool alpha(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
         c >= 128;
}
bool digit(unsigned char c) { return c >= '0' && c <= '9'; }
bool word(unsigned char c) { return alpha(c) || digit(c) || c == '$'; }
bool space(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}
bool validUTF8(Compilation &c) {
  const auto &s = c.source.text();
  for (std::size_t i = 0; i < s.size();) {
    if ((i & 4095u) == 0)
      checkStop(c);
    const auto b = static_cast<unsigned char>(s[i]);
    std::size_t n = 0;
    std::uint32_t cp = 0;
    if (b > 0 && b < 128) {
      ++i;
      continue;
    }
    if (b >= 0xc2 && b <= 0xdf) {
      n = 2;
      cp = b & 31;
    } else if (b >= 0xe0 && b <= 0xef) {
      n = 3;
      cp = b & 15;
    } else if (b >= 0xf0 && b <= 0xf4) {
      n = 4;
      cp = b & 7;
    }
    bool good = n && i + n <= s.size();
    for (std::size_t j = 1; good && j < n; ++j) {
      auto next = static_cast<unsigned char>(s[i + j]);
      good = (next & 0xc0) == 0x80;
      cp = (cp << 6) | (next & 63);
    }
    good = good &&
           cp >= (n == 2   ? 0x80u
                  : n == 3 ? 0x800u
                           : 0x10000u) &&
           cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff);
    if (!good) {
      c.report(DiagnosticCode::InvalidEncoding, {i, i + 1},
               "expected UTF-8 without NUL bytes");
      return false;
    }
    i += n;
  }
  return true;
}
class Lexer {
public:
  explicit Lexer(Compilation &c, bool opaqueComments, bool recoverSymbols)
      : c(c), s(c.source.text()),
        mysql(isMySqlDialect(c.dialect)),
        pg(c.dialect == Dialect::PostgreSQL),
        oracle(c.dialect == Dialect::Oracle),
        mssql(c.dialect == Dialect::SQLServer),
        sqlite(c.dialect == Dialect::SQLite), opaqueComments(opaqueComments),
        recoverSymbols(recoverSymbols) {}
  void run() {
    while (i < s.size() && c.diagnostics.empty()) {
      checkStop(c);
      if (space(s[i])) {
        ++i;
        continue;
      }
      if (c.tokens.size() >= c.options.limits.tokens) {
        c.report(DiagnosticCode::ResourceLimit, {i, i}, "token limit exceeded");
        break;
      }
      const auto start = i;
      if ((s.compare(i, 2, "--") == 0 &&
           (!mysql || i + 2 == s.size() || space(s[i + 2]))) ||
          (mysql && s[i] == '#')) {
        while (i < s.size() && s[i] != '\r' && s[i] != '\n')
          ++i;
        emit(TokenKind::Comment, start);
        continue;
      }
      if (!opaqueComments && s.compare(i, 4, "/*T!") == 0 && mysql) {
        tidbFeatureComment();
        continue;
      }
      if (s.compare(i, 2, "/*") == 0) {
        if (!opaqueComments && i + 2 < s.size() &&
            (s[i + 2] == '!' || s[i + 2] == '+' ||
             (s[i + 2] == 'M' && i + 3 < s.size() && s[i + 3] == '!'))) {
          c.report(
              DiagnosticCode::UnsupportedSyntax, {i, i + 3},
              "executable comments and optimizer hints are not supported yet");
          break;
        }
        i += 2;
        std::size_t depth = 1;
        while (i < s.size() && depth) {
          if (s.compare(i, 2, "/*") == 0 && (pg || mssql)) {
            ++depth;
            i += 2;
          } else if (s.compare(i, 2, "*/") == 0) {
            --depth;
            i += 2;
          } else
            ++i;
        }
        if (depth)
          error(start, "unterminated block comment");
        else
          emit(TokenKind::Comment, start);
        continue;
      }
      if (pg && s[i] == '$') {
        if (i + 1 < s.size() && digit(s[i + 1])) {
          i += 2;
          while (i < s.size() && digit(s[i]))
            ++i;
          if (s[start + 1] == '0')
            error(start,
                  "parameter index must be positive without leading zeroes");
          else
            emit(TokenKind::Parameter, start);
          continue;
        }
        auto end = i + 1;
        if (end < s.size() && alpha(s[end]))
          while (end < s.size() && (alpha(s[end]) || digit(s[end])))
            ++end;
        if (end < s.size() && s[end] == '$') {
          const auto tag = s.substr(i, end - i + 1);
          auto close = s.find(tag, end + 1);
          if (close == std::string::npos) {
            i = s.size();
            error(start, "unterminated dollar string");
          } else {
            i = close + tag.size();
            emit(TokenKind::String, start, s.substr(end + 1, close - end - 1));
          }
          continue;
        }
      }
      if (oracle && (s[i] == 'q' || s[i] == 'Q') && i + 2 < s.size() &&
          s[i + 1] == '\'') {
        char open = s[i + 2];
        char close = open;
        if (open == '[')
          close = ']';
        else if (open == '(')
          close = ')';
        else if (open == '{')
          close = '}';
        else if (open == '<')
          close = '>';
        if (space(open) || open == '\'') {
          i += 3;
          error(start, "invalid alternative quote delimiter");
          continue;
        }
        auto end = s.find(std::string(1, close) + "'", i + 3);
        if (end == std::string::npos) {
          i = s.size();
          error(start, "unterminated alternative string");
        } else {
          i = end + 2;
          emit(TokenKind::String, start, s.substr(start + 3, end - start - 3));
        }
        continue;
      }
      if ((pg && (s[i] == 'e' || s[i] == 'E')) ||
          (mssql && (s[i] == 'n' || s[i] == 'N'))) {
        if (i + 1 < s.size() && s[i + 1] == '\'') {
          const bool escapes = pg;
          ++i;
          quoted(start, '\'', TokenKind::String, escapes);
          continue;
        }
      }
      if (s[i] == '\'') {
        quoted(start, '\'', TokenKind::String,
               mysql && !c.options.mysqlNoBackslashEscapes);
        continue;
      }
      if (s[i] == '"') {
        bool identifier = !mysql || c.options.mysqlAnsiQuotes;
        quoted(start, '"',
               identifier ? TokenKind::Identifier : TokenKind::String,
               !identifier && !c.options.mysqlNoBackslashEscapes);
        continue;
      }
      if (s[i] == '`' && (mysql || sqlite)) {
        quoted(start, '`', TokenKind::Identifier, false);
        continue;
      }
      if (s[i] == '[' && (mssql || sqlite)) {
        quoted(start, ']', TokenKind::Identifier, false);
        continue;
      }
      if (mysql && s[i] == '@') {
        ++i;
        if (rolePrefix == RolePrefix::Accounts || rolePrefix == RolePrefix::Definer) {
          emit(TokenKind::Symbol, start);
          continue;
        }
        const bool system = i < s.size() && s[i] == '@';
        if (system)
          ++i;
        const auto nameStart = i;
        while (i < s.size() && word(s[i]))
          ++i;
        if (system && i < s.size() && s[i] == '.') {
          ++i;
          while (i < s.size() && word(s[i]))
            ++i;
        }
        if (i == nameStart || s[i - 1] == '.') {
          c.report(
              DiagnosticCode::UnsupportedSyntax, {start, i},
              "quoted or incomplete MySQL variable spelling is not supported");
        } else
          emit(TokenKind::Variable, start);
        continue;
      }
      if ((s[i] == ':' && (oracle || sqlite) && i + 1 < s.size() &&
           s[i + 1] != ':' && s[i + 1] != '=') ||
          (s[i] == '@' && (mssql || sqlite)) || (s[i] == '$' && sqlite)) {
        ++i;
        while (i < s.size() && word(s[i]))
          ++i;
        if (i == start + 1)
          error(start, "expected parameter name");
        else
          emit(TokenKind::Parameter, start);
        continue;
      }
      if (s[i] == '?' && (mysql || sqlite)) {
        ++i;
        if (sqlite)
          while (i < s.size() && digit(s[i]))
            ++i;
        emit(TokenKind::Parameter, start);
        continue;
      }
      if (digit(s[i]) || (s[i] == '.' && i + 1 < s.size() && digit(s[i + 1]))) {
        while (i < s.size() && digit(s[i]))
          ++i;
        if (i < s.size() && s[i] == '.' && (!(pg || oracle) || s.compare(i, 2, "..") != 0)) {
          ++i;
          while (i < s.size() && digit(s[i]))
            ++i;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
          ++i;
          if (i < s.size() && (s[i] == '+' || s[i] == '-'))
            ++i;
          auto digits = i;
          while (i < s.size() && digit(s[i]))
            ++i;
          if (digits == i) {
            error(start, "expected exponent digits");
            continue;
          }
        }
        if (i < s.size() && alpha(s[i])) {
          error(start, "unsupported numeric literal suffix");
          continue;
        }
        emit(TokenKind::Number, start);
        continue;
      }
      if (mssql && s[i]=='$') {
        ++i;
        while (i<s.size() && word(s[i])) ++i;
        emit(TokenKind::Symbol,start);
        continue;
      }
      if (mssql && s[i]=='#') {
        ++i;
        if (i<s.size() && s[i]=='#') ++i;
        const auto name=i;
        while (i<s.size() && word(s[i])) ++i;
        if (i==name) error(start,"expected temporary table name");
        else emit(TokenKind::Word,start);
        continue;
      }
      if (alpha(s[i])) {
        ++i;
        while (i < s.size() && word(s[i]))
          ++i;
        emit(TokenKind::Word, start);
        continue;
      }
      if (pg && postgresOperator()) {
        emit(TokenKind::Symbol, start);
        continue;
      }
      bool combined = false;
      if ((pg || oracle) && s.compare(i, 2, "..") == 0) {
        i += 2;
        emit(TokenKind::Symbol, start);
        continue;
      }
      if (sqlite && s.compare(i, 2, "==") == 0) {
        i += 2;
        emit(TokenKind::Symbol, start);
        continue;
      }
      for (auto op : {"<=>", "::", "<=", ">=", "<>", "!=", "||", "&&", ":="}) {
        if (s.compare(i, std::char_traits<char>::length(op), op) == 0) {
          i += std::char_traits<char>::length(op);
          emit(TokenKind::Symbol, start);
          combined = true;
          break;
        }
      }
      if (combined)
        continue;
      if ((pg && (s[i] == '[' || s[i] == ']')) ||
          std::string_view("+-*/%=<>!(),.;:").find(s[i]) !=
              std::string_view::npos) {
        ++i;
        emit(TokenKind::Symbol, start);
        continue;
      }
      ++i;
      if (recoverSymbols)
        emit(TokenKind::Symbol, start);
      else
        error(start, "unsupported character in this dialect");
    }
    c.tokens.push_back({TokenKind::End, {i, i}, ""});
  }

private:
  Compilation &c;
  const std::string &s;
  bool mysql, pg, oracle, mssql, sqlite, opaqueComments, recoverSymbols;
  enum class RolePrefix {
    Start, Set, SetDefault, Accounts, Create, CreateOr, CreateAlgorithm,
    CreateAlgorithmValue, Definer, Other
  };
  RolePrefix rolePrefix = RolePrefix::Start;
  std::size_t i = 0, trailingSignsEnd = 0;
  void tidbFeatureComment() {
    const auto begin = i;
    const auto end = s.find("*/", i + 4);
    if (end == std::string::npos) {
      i = s.size();
      error(begin, "unterminated TiDB feature comment");
      return;
    }
    auto body = i + 4;
    i = end + 2;
    const auto unsupported = [&] {
      c.report(DiagnosticCode::UnsupportedSyntax, {begin, i},
               "TiDB feature comment needs a supported TiDB DDL option");
    };
    if (c.dialect != Dialect::TiDB) {
      // Other MySQL-compatible servers treat TiDB feature comments as inert.
      emit(TokenKind::Comment, begin);
      return;
    }
    std::string_view tag;
    if (body < end && s[body] == '[') {
      const auto close = s.find(']', body + 1);
      if (close == std::string::npos || close >= end) {
        unsupported();
        return;
      }
      tag = std::string_view(s).substr(body + 1, close - body - 1);
      body = close + 1;
    }
    const auto payload = std::string_view(s).substr(body, end - body);
    const auto first = payload.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
      unsupported();
      return;
    }
    const auto wordEnd = payload.find_first_of(" \t\r\n(=", first);
    const auto word = asciiUpper(std::string(payload.substr(first, wordEnd - first)));
    const bool clustering = word == "CLUSTERED" || word == "NONCLUSTERED";
    const bool supported =
        (tag == "auto_rand" && word == "AUTO_RANDOM") ||
        (tag == "clustered_index" && clustering) ||
        (tag == "placement" && word == "PLACEMENT") ||
        (tag.empty() && (word == "SHARD_ROW_ID_BITS" || word == "PRE_SPLIT_REGIONS" ||
                         word == "AUTO_RANDOM_BASE" || word == "PLACEMENT" ||
                         word == "AUTO_RANDOM" || clustering));
    if (!supported || payload.find("/*") != std::string_view::npos) {
      unsupported();
      return;
    }
    auto options = c.options;
    options.limits.tokens -= c.tokens.size();
    Compilation fragment(c.dialect, std::string(payload), std::move(options), c.source.name());
    lex(fragment, false, recoverSymbols);
    for (const auto &diagnostic : fragment.diagnostics)
      c.report(diagnostic.code, {body + diagnostic.range.begin, body + diagnostic.range.end},
               diagnostic.message);
    if (!c.diagnostics.empty()) return;
    for (auto &token : fragment.tokens) {
      if (token.kind == TokenKind::End) break;
      if (token.kind == TokenKind::Comment || token.value == ";") {
        unsupported();
        return;
      }
      token.range.begin += body;
      token.range.end += body;
      c.tokens.push_back(std::move(token));
    }
  }
  bool postgresOperator() {
    if (i < trailingSignsEnd) {
      ++i;
      return true;
    }
    constexpr std::string_view custom = "~!@#%^&|`?";
    constexpr std::string_view ordinary = "+-*/<=>";
    auto end = i;
    auto lastNonSign = i;
    bool keepsTrailingSigns = false;
    while (end < s.size()) {
      const auto ch = s[end];
      const bool customCharacter = custom.find(ch) != std::string_view::npos;
      if ((!customCharacter && ordinary.find(ch) == std::string_view::npos) ||
          s.compare(end, 2, "--") == 0 || s.compare(end, 2, "/*") == 0)
        break;
      if ((end & 4095u) == 0) checkStop(c);
      ++end;
      if (ch != '+' && ch != '-') lastNonSign = end;
      keepsTrailingSigns |= customCharacter;
    }
    if (end == i) return false;
    trailingSignsEnd = end;
    i = keepsTrailingSigns ? end : std::max(i + 1, lastNonSign);
    return true;
  }
  void emit(TokenKind kind, std::size_t start) {
    emit(kind, start, s.substr(start, i - start));
  }
  void emit(TokenKind kind, std::size_t start, std::string value) {
    if (mysql && kind != TokenKind::Comment) {
      if (kind == TokenKind::Symbol && value == ";") {
        rolePrefix = RolePrefix::Start;
      } else if (rolePrefix != RolePrefix::Accounts) {
        const auto keyword = kind == TokenKind::Word ? asciiUpper(value) : std::string{};
        if (rolePrefix == RolePrefix::Definer) {
          if(keyword=="TRIGGER" || keyword=="PROCEDURE" || keyword=="FUNCTION" ||
             keyword=="EVENT" || keyword=="VIEW")rolePrefix=RolePrefix::Other;
        } else if(keyword=="CREATE")
          rolePrefix=RolePrefix::Create;
        else if (rolePrefix == RolePrefix::Create && keyword == "OR")
          rolePrefix = RolePrefix::CreateOr;
        else if (rolePrefix == RolePrefix::CreateOr && keyword == "REPLACE")
          rolePrefix = RolePrefix::Create;
        else if (rolePrefix == RolePrefix::Create && keyword == "ALGORITHM")
          rolePrefix = RolePrefix::CreateAlgorithm;
        else if (rolePrefix == RolePrefix::CreateAlgorithm && kind == TokenKind::Symbol && value == "=")
          rolePrefix = RolePrefix::CreateAlgorithmValue;
        else if (rolePrefix == RolePrefix::CreateAlgorithmValue &&
                 (keyword == "MERGE" || keyword == "UNDEFINED" || keyword == "TEMPTABLE"))
          rolePrefix = RolePrefix::Create;
        else if(rolePrefix == RolePrefix::Create && keyword=="DEFINER")
          rolePrefix=RolePrefix::Definer;
        else if (rolePrefix == RolePrefix::Start && keyword == "SET")
          rolePrefix = RolePrefix::Set;
        else if (rolePrefix == RolePrefix::Set && keyword == "DEFAULT")
          rolePrefix = RolePrefix::SetDefault;
        else if ((rolePrefix == RolePrefix::Set || rolePrefix == RolePrefix::SetDefault) && keyword == "ROLE")
          rolePrefix = RolePrefix::Accounts;
        else
          rolePrefix = RolePrefix::Other;
      }
    }
    c.tokens.push_back({kind, {start, i}, std::move(value)});
  }
  void error(std::size_t start, const char *message) {
    c.report(DiagnosticCode::LexicalError, {start, i}, message);
  }
  void quoted(std::size_t start, char close, TokenKind kind, bool escapes) {
    ++i;
    std::string value;
    while (i < s.size()) {
      char ch = s[i++];
      if (ch == close) {
        if (i < s.size() && s[i] == close && !(sqlite && close == ']')) {
          value += close;
          ++i;
          continue;
        }
        if (kind == TokenKind::Identifier && value.empty())
          error(start, "empty quoted identifier");
        else
          emit(kind, start, std::move(value));
        return;
      }
      if (ch == '\\' && escapes) {
        if (i == s.size())
          break;
        ch = s[i++];
        switch (ch) {
        case 'n':
          value += '\n';
          break;
        case 'r':
          value += '\r';
          break;
        case 't':
          value += '\t';
          break;
        case 'b':
          value += '\b';
          break;
        case 'f':
          if (pg)
            value += '\f';
          else
            value += ch;
          break;
        case '0':
          if (mysql)
            value += '\0';
          else {
            error(start, "octal escapes are not supported yet");
            return;
          }
          break;
        case 'Z':
          if (mysql)
            value += char(26);
          else
            value += ch;
          break;
        case '%':
        case '_':
          if (mysql)
            value += '\\';
          value += ch;
          break;
        default:
          if (pg && (digit(ch) || ch == 'x' || ch == 'u' || ch == 'U')) {
            error(start,
                  "numeric and Unicode escape strings are not supported yet");
            return;
          }
          value += ch;
        }
      } else
        value += ch;
    }
    error(start, "unterminated quoted token");
  }
};
} // namespace
void lex(Compilation &c, bool opaqueComments, bool recoverSymbols) {
  if (validUTF8(c))
    Lexer(c, opaqueComments, recoverSymbols).run();
}
} // namespace navidog::sql::compiler
