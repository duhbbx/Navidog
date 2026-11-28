#include "navidog/sql.hpp"
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unordered_set>

namespace navidog::sql {
static bool wordStart(unsigned char c) { return std::isalpha(c) || c == '_' || c >= 128; }
static bool wordPart(unsigned char c) { return wordStart(c) || std::isdigit(c) || c == '$'; }
LexResult lex(std::string_view s, Dialect dialect) {
    LexResult out;
    std::size_t i = 0;
    auto emit = [&](TokenKind k, std::size_t start) { out.tokens.push_back({k, start, i}); };
    while (i < s.size()) {
        if (std::isspace(static_cast<unsigned char>(s[i]))) { ++i; continue; }
        const auto start = i;
        if ((s[i] == '-' && i + 1 < s.size() && s[i + 1] == '-' &&
             (dialect != Dialect::MySQL || i + 2 == s.size() ||
              std::isspace(static_cast<unsigned char>(s[i + 2])))) ||
            (dialect == Dialect::MySQL && s[i] == '#')) {
            while (i < s.size() && s[i] != '\n') ++i;
            emit(TokenKind::Comment, start);
        } else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            i += 2; int depth = 1;
            while (i < s.size() && depth) {
                if ((dialect == Dialect::PostgreSQL || dialect == Dialect::SQLServer) && s.substr(i, 2) == "/*") { ++depth; i += 2; }
                else if (s.substr(i, 2) == "*/") { --depth; i += 2; }
                else ++i;
            }
            if (depth) out.diagnostics.push_back({start, "注释缺少结束符 */"});
            emit(TokenKind::Comment, start);
        } else if (dialect == Dialect::PostgreSQL && s[i] == '$') {
            auto end = i + 1;
            while (end < s.size() && (wordStart(s[end]) || (end > i + 1 && std::isdigit(static_cast<unsigned char>(s[end]))))) ++end;
            if (end < s.size() && s[end] == '$') {
                const auto tag = s.substr(i, end - i + 1);
                auto close = s.find(tag, end + 1);
                if (close == std::string_view::npos) { i = s.size(); out.diagnostics.push_back({start, "美元引号字符串未结束"}); }
                else i = close + tag.size();
                emit(TokenKind::String, start);
            } else { ++i; emit(TokenKind::Symbol, start); }
        } else if (s[i] == '\'' || s[i] == '"' ||
                   ((dialect == Dialect::SQLite || dialect == Dialect::MySQL) && s[i] == '`') ||
                   ((dialect == Dialect::SQLite || dialect == Dialect::SQLServer) && s[i] == '[')) {
            const char opening = s[i++], closing = opening == '[' ? ']' : opening;
            bool closed = false;
            const bool pgEscape = dialect == Dialect::PostgreSQL && opening == '\'' && start > 0 &&
                (s[start - 1] == 'e' || s[start - 1] == 'E') &&
                (start == 1 || !wordPart(s[start - 2]));
            while (i < s.size()) {
                if (s[i] == '\\' && (dialect == Dialect::MySQL || pgEscape) && i + 1 < s.size()) { i += 2; continue; }
                if (s[i++] == closing) {
                    if ((opening != '[' || dialect == Dialect::SQLServer) && i < s.size() && s[i] == closing) { ++i; continue; }
                    closed = true; break;
                }
            }
            if (!closed) out.diagnostics.push_back({start, "引号未闭合"});
            emit(opening == '\'' || (opening == '"' && dialect == Dialect::MySQL) ? TokenKind::String : TokenKind::Identifier, start);
        } else if (dialect == Dialect::Oracle && (s[i] == 'q' || s[i] == 'Q') && i + 2 < s.size() && s[i+1] == '\'') {
            const char opening = s[i+2];
            const char closing = opening == '[' ? ']' : opening == '{' ? '}' : opening == '(' ? ')' : opening == '<' ? '>' : opening;
            i += 3; bool closed = false;
            while (i + 1 < s.size()) {
                if (s[i] == closing && s[i+1] == '\'') { i += 2; closed = true; break; }
                ++i;
            }
            if (!closed) { i = s.size(); out.diagnostics.push_back({start, "Oracle q 引号未闭合"}); }
            emit(TokenKind::String, start);
        } else if (wordStart(s[i])) {
            while (i < s.size() && wordPart(s[i])) ++i;
            emit(TokenKind::Word, start);
        } else if (std::isdigit(static_cast<unsigned char>(s[i]))) {
            while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '.')) ++i;
            emit(TokenKind::Number, start);
        } else {
            ++i;
            if (i < s.size() && (s.substr(start, 2) == "<=" || s.substr(start, 2) == ">=" ||
                s.substr(start, 2) == "<>" || s.substr(start, 2) == "!=" || s.substr(start, 2) == "||" ||
                s.substr(start, 2) == "::" || s.substr(start, 2) == "->")) {
                ++i;
                if (s.substr(start, 2) == "->" && i < s.size() && s[i] == '>') ++i;
            }
            emit(TokenKind::Symbol, start);
        }
    }
    return out;
}
bool isCreateTrigger(std::string_view source,Dialect dialect) {
    const auto parsed=lex(source,dialect);if(!parsed.diagnostics.empty())return false;
    bool first=true;int count=0;
    for(auto t:parsed.tokens){if(t.kind==TokenKind::Comment)continue;auto text=std::string(source.substr(t.begin,t.end-t.begin));std::transform(text.begin(),text.end(),text.begin(),[](unsigned char c){return std::toupper(c);});
        if(first){if(text!="CREATE")return false;first=false;continue;}
        if(text==";" || ++count>40)return false;
        if(t.kind==TokenKind::Word && text=="TRIGGER")return true;
    }return false;
}
bool isSingleStatement(std::string_view s, Dialect d, std::string& error) {
    auto result = lex(s, d);
    if (!result.diagnostics.empty()) { error = result.diagnostics.front().message; return false; }
    bool content = false, ended = false;
    for (auto t : result.tokens) {
        if (t.kind == TokenKind::Comment) {
            // Executable MySQL comments need semantic parsing before statement counting.
            if (d == Dialect::MySQL && s.substr(t.begin, 3) == "/*!") { error = "当前版本不执行 MySQL 可执行注释，请展开后运行"; return false; }
            continue;
        }
        if (t.kind == TokenKind::Symbol && s[t.begin] == ';') { ended = true; continue; }
        if (ended) { error = "每次运行一条 SQL；可选中需要运行的语句"; return false; }
        content = true;
    }
    if (!content) error = "请先输入 SQL";
    return content;
}
std::string format(std::string_view source, Dialect dialect) {
    const auto parsed = lex(source, dialect);
    if (!parsed.diagnostics.empty()) throw std::runtime_error(parsed.diagnostics.front().message);
    // Keep non-whitespace token bytes AND their boundaries unchanged. In particular,
    // do not turn '--' into a comment, separate E'...', or merge '-' tokens.
    std::unordered_set<std::string> clauses = {"SELECT", "FROM", "WHERE", "GROUP", "HAVING", "ORDER", "LIMIT", "OFFSET", "JOIN", "LEFT", "RIGHT", "INNER", "UNION", "VALUES", "SET"};
    std::string out; std::size_t previousEnd = 0; bool first = true;
    for (auto token : parsed.tokens) {
        auto text = source.substr(token.begin, token.end - token.begin);
        auto gap = source.substr(previousEnd, token.begin - previousEnd);
        std::string upper(text); std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });
        if (!first && !gap.empty()) {
            if (gap.find('\n') != std::string_view::npos || (token.kind == TokenKind::Word && clauses.find(upper)!=clauses.end())) out += '\n';
            else out += ' ';
        }
        out.append(text); previousEnd = token.end; first = false;
    }
    // Verify formatting did not alter tokenization (line comments are sensitive).
    auto after = lex(out, dialect);
    bool same = parsed.tokens.size() == after.tokens.size() && after.diagnostics.empty();
    for (std::size_t n = 0; same && n < parsed.tokens.size(); ++n) {
        auto a = parsed.tokens[n], b = after.tokens[n];
        same = a.kind == b.kind && source.substr(a.begin, a.end - a.begin) == std::string_view(out).substr(b.begin, b.end - b.begin);
    }
    return same ? out : std::string(source);
}
}
