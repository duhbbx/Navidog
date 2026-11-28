#pragma once
#include "compiler.hpp"
namespace navidog::sql::compiler {
enum class KeywordCase { Preserve, Upper, Lower };
enum class ListLayout { Inline, Fit, Vertical };
enum class CommaPosition { Trailing, Leading };
enum class LineEnding { Preserve, LF, CRLF };
struct FormatStyle {
  unsigned indentWidth = 2, continuationIndentWidth = 2, tabWidth = 4,
           columnLimit = 100;
  bool useTabs = false, breakBeforeClauses = true, breakBeforeJoin = true,
       breakBeforeBooleanOperators = true, indentSubqueries = true,
       indentCaseBranches = true, spacesAroundOperators = true,
       spaceAfterComma = true, spacesInParentheses = false,
       alignAliases = false, alignAssignments = true,
       formatRoutineBodies = true, finalNewline = false;
  unsigned blankLinesBetweenStatements = 1, maxEmptyLines = 1;
  KeywordCase keywordCase = KeywordCase::Upper;
  ListLayout selectList = ListLayout::Fit, insertColumns = ListLayout::Fit,
             valuesRows = ListLayout::Vertical,
             updateAssignments = ListLayout::Vertical,
             ddlColumns = ListLayout::Vertical,
             routineParameters = ListLayout::Vertical,
             functionArguments = ListLayout::Fit;
  CommaPosition commaPosition = CommaPosition::Trailing;
  LineEnding lineEnding = LineEnding::Preserve;
};
FormatStyle
formatPreset(std::string_view name); // Readable / Compact / Expanded
FormatStyle parseFormatStyle(std::string_view config, FormatStyle base = {});
std::string dumpFormatStyle(const FormatStyle &);
struct FormatOptions {
  LanguageOptions language;
  std::size_t maxOutputBytes = 8 * 1024 * 1024, maxVisits = 1000000;
  std::function<bool()> shouldStop;
};
struct FormatTokenMapping {
  SourceRange before, after;
};
struct FormatResult {
  std::string sql;
  std::vector<Diagnostic> diagnostics;
  std::vector<FormatTokenMapping> mapping;
  bool syntaxSupported = false;
  bool ok() const { return diagnostics.empty(); }
};
// Source-preserving layout, distinct from canonical IR generation. Unsupported
// grammar can still be laid out; token identities and literals are verified.
FormatResult formatSql(Dialect, std::string source,
                       const FormatStyle &style = {},
                       FormatOptions options = {});
} // namespace navidog::sql::compiler
