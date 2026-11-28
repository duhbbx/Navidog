#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace navidog::sql {
enum class Dialect { SQLite, MySQL, PostgreSQL, Oracle, SQLServer };
enum class TokenKind { Word, Number, String, Identifier, Comment, Symbol };
struct Token { TokenKind kind; std::size_t begin, end; };
struct Diagnostic { std::size_t offset; std::string message; };
struct LexResult { std::vector<Token> tokens; std::vector<Diagnostic> diagnostics; };
LexResult lex(std::string_view, Dialect);
// Lexical statement guard, not a syntax validator. Database engine validates syntax.
bool isCreateTrigger(std::string_view,Dialect);
bool isPackageSpecification(std::string_view,Dialect);
bool isCompoundDefinition(std::string_view,Dialect);
struct StatementSpan { std::size_t begin,end; bool compound; };
// Retrieves a split statement, retaining an original SQL Server semicolon
// (required by MERGE). Never invents a missing terminator or rewrites SQL.
std::string_view statementSource(std::string_view,StatementSpan,Dialect);
// Splits client scripts, preserving quoted bodies; the server validates syntax.
std::vector<StatementSpan> splitStatements(std::string_view,Dialect);
bool isSingleStatement(std::string_view, Dialect, std::string& error);
// Whitespace-only lexical formatter; token spelling and comments are preserved.
std::string format(std::string_view, Dialect);
}
