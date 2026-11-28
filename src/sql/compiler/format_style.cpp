#include "navidog/sql/compiler/formatter.hpp"
#include <sstream>
#include <stdexcept>
#include <unordered_set>
namespace navidog::sql::compiler {
namespace {
std::string trim(std::string_view value) {
  const auto first = value.find_first_not_of(" \t\r");
  if (first == std::string_view::npos)
    return {};
  return std::string(
      value.substr(first, value.find_last_not_of(" \t\r") - first + 1));
}
std::string boolean(bool value) { return value ? "true" : "false"; }
std::string list(ListLayout value) {
  return value == ListLayout::Inline     ? "Inline"
         : value == ListLayout::Vertical ? "Vertical"
                                         : "Fit";
}
} // namespace
FormatStyle formatPreset(std::string_view name) {
  FormatStyle style;
  if (name == "Readable" || name == "Default")
    return style;
  if (name == "Compact") {
    style.columnLimit = 120;
    style.breakBeforeClauses = false;
    style.breakBeforeJoin = false;
    style.breakBeforeBooleanOperators = false;
    style.selectList = style.insertColumns = style.updateAssignments =
        style.functionArguments = style.routineParameters = ListLayout::Fit;
    style.valuesRows = ListLayout::Inline;
    style.alignAssignments = false;
    style.blankLinesBetweenStatements = 0;
    return style;
  }
  if (name == "Expanded") {
    style.indentWidth = 4;
    style.continuationIndentWidth = 4;
    style.selectList = style.insertColumns = style.updateAssignments =
        style.ddlColumns = style.routineParameters = ListLayout::Vertical;
    style.alignAliases = true;
    return style;
  }
  throw std::invalid_argument("unknown SQL format preset: " +
                              std::string(name));
}
FormatStyle parseFormatStyle(std::string_view config, FormatStyle style) {
  if (config.size() > 65536)
    throw std::invalid_argument("SQL format config exceeds 64 KiB");
  std::istringstream input{std::string(config)};
  std::vector<std::pair<std::string, std::string>> entries;
  std::unordered_set<std::string> seen;
  for (std::string line; std::getline(input, line);) {
    auto text = trim(line);
    if (text.empty() || text[0] == '#' || text == "---" || text == "...")
      continue;
    auto colon = text.find(':');
    if (colon == std::string::npos)
      throw std::invalid_argument("expected Key: Value in SQL format config");
    auto key = trim(text.substr(0, colon)),
         value = trim(text.substr(colon + 1));
    if (auto hash = value.find(" #"); hash != std::string::npos)
      value = trim(value.substr(0, hash));
    if (!seen.insert(key).second)
      throw std::invalid_argument("duplicate SQL format option: " + key);
    entries.push_back({key, value});
  }
  for (const auto &entry : entries)
    if (entry.first == "BasedOnStyle")
      style = formatPreset(entry.second);
  auto unsignedValue = [&](const std::string &value, unsigned max,
                           unsigned min) {
    if (value.empty() ||
        value.find_first_not_of("0123456789") != std::string::npos)
      throw std::invalid_argument(
          "expected unsigned integer in SQL format config");
    const auto number = std::stoul(value);
    if (number < min || number > max)
      throw std::invalid_argument("SQL format option is out of range");
    return unsigned(number);
  };
  auto boolValue = [](const std::string &value) {
    if (value == "true")
      return true;
    if (value == "false")
      return false;
    throw std::invalid_argument("expected true or false in SQL format config");
  };
  auto listValue = [](const std::string &value) {
    if (value == "Inline")
      return ListLayout::Inline;
    if (value == "Fit")
      return ListLayout::Fit;
    if (value == "Vertical")
      return ListLayout::Vertical;
    throw std::invalid_argument("expected Inline, Fit or Vertical");
  };
  for (const auto &[key, value] : entries) {
    if (key == "BasedOnStyle")
      continue;
#define UINT_OPTION(name, field, min, max)                                     \
  if (key == name) {                                                           \
    style.field = unsignedValue(value, max, min);                              \
    continue;                                                                  \
  }
    UINT_OPTION("IndentWidth", indentWidth, 1, 8)
    UINT_OPTION("ContinuationIndentWidth", continuationIndentWidth, 0,
                16) UINT_OPTION("TabWidth", tabWidth, 1, 8)
        UINT_OPTION("ColumnLimit", columnLimit, 0, 240) UINT_OPTION(
            "BlankLinesBetweenStatements", blankLinesBetweenStatements, 0,
            3) UINT_OPTION("MaxEmptyLines", maxEmptyLines, 0, 3)
#undef UINT_OPTION
#define BOOL_OPTION(name, field)                                               \
  if (key == name) {                                                           \
    style.field = boolValue(value);                                            \
    continue;                                                                  \
  }
            BOOL_OPTION("UseTabs", useTabs) BOOL_OPTION("BreakBeforeClauses",
                                                        breakBeforeClauses)
                BOOL_OPTION("BreakBeforeJoin", breakBeforeJoin) BOOL_OPTION(
                    "BreakBeforeBooleanOperators",
                    breakBeforeBooleanOperators) BOOL_OPTION("IndentSubqueries",
                                                             indentSubqueries)
                    BOOL_OPTION(
                        "IndentCaseBranches",
                        indentCaseBranches) BOOL_OPTION("SpacesAroundOperators",
                                                        spacesAroundOperators)
                        BOOL_OPTION(
                            "SpaceAfterComma",
                            spaceAfterComma) BOOL_OPTION("SpacesInParentheses",
                                                         spacesInParentheses)
                            BOOL_OPTION(
                                "AlignAliases",
                                alignAliases) BOOL_OPTION("AlignAssignments",
                                                          alignAssignments)
                                BOOL_OPTION(
                                    "FormatRoutineBodies",
                                    formatRoutineBodies) BOOL_OPTION("FinalNewl"
                                                                     "ine",
                                                                     finalNewline)
#undef BOOL_OPTION
#define LIST_OPTION(name, field)                                               \
  if (key == name) {                                                           \
    style.field = listValue(value);                                            \
    continue;                                                                  \
  }
                                    LIST_OPTION(
                                        "SelectList",
                                        selectList) LIST_OPTION("InsertColumns",
                                                                insertColumns)
                                        LIST_OPTION("ValuesRows", valuesRows)
                                            LIST_OPTION("UpdateAssignments",
                                                        updateAssignments)
                                                LIST_OPTION("DdlColumns",
                                                            ddlColumns)
                                                    LIST_OPTION(
                                                        "RoutineParameters",
                                                        routineParameters)
                                                        LIST_OPTION(
                                                            "FunctionArguments",
                                                            functionArguments)
#undef LIST_OPTION
                                                            if (key ==
                                                                "KeywordCase") {
      if (value == "Upper")
        style.keywordCase = KeywordCase::Upper;
      else if (value == "Lower")
        style.keywordCase = KeywordCase::Lower;
      else if (value == "Preserve")
        style.keywordCase = KeywordCase::Preserve;
      else
        throw std::invalid_argument("invalid KeywordCase");
      continue;
    }
    if (key == "CommaPosition") {
      if (value == "Trailing")
        style.commaPosition = CommaPosition::Trailing;
      else if (value == "Leading")
        style.commaPosition = CommaPosition::Leading;
      else
        throw std::invalid_argument("invalid CommaPosition");
      continue;
    }
    if (key == "LineEnding") {
      if (value == "Preserve")
        style.lineEnding = LineEnding::Preserve;
      else if (value == "LF")
        style.lineEnding = LineEnding::LF;
      else if (value == "CRLF")
        style.lineEnding = LineEnding::CRLF;
      else
        throw std::invalid_argument("invalid LineEnding");
      continue;
    }
    throw std::invalid_argument("unknown SQL format option: " + key);
  }
  if (style.columnLimit != 0 && style.columnLimit < 40)
    throw std::invalid_argument("ColumnLimit must be 0 or 40..240");
  return style;
}
std::string dumpFormatStyle(const FormatStyle &s) {
  std::ostringstream out;
  out << "# Navidog SQL format style; flat Key: Value "
         "configuration\nBasedOnStyle: Readable\n";
#define WRITE(name, field) out << name << ": " << s.field << '\n';
  WRITE("IndentWidth", indentWidth)
      WRITE("ContinuationIndentWidth", continuationIndentWidth)
          WRITE("TabWidth", tabWidth) WRITE("ColumnLimit", columnLimit) WRITE(
              "BlankLinesBetweenStatements",
              blankLinesBetweenStatements) WRITE("MaxEmptyLines", maxEmptyLines)
#undef WRITE
#define WRITE(name, field) out << name << ": " << boolean(s.field) << '\n';
              WRITE("UseTabs", useTabs) WRITE("BreakBeforeClauses",
                                              breakBeforeClauses)
                  WRITE("BreakBeforeJoin", breakBeforeJoin) WRITE(
                      "BreakBeforeBooleanOperators",
                      breakBeforeBooleanOperators) WRITE("IndentSubqueries",
                                                         indentSubqueries)
                      WRITE("IndentCaseBranches", indentCaseBranches) WRITE(
                          "SpacesAroundOperators", spacesAroundOperators)
                          WRITE("SpaceAfterComma", spaceAfterComma) WRITE(
                              "SpacesInParentheses", spacesInParentheses)
                              WRITE("AlignAliases", alignAliases) WRITE(
                                  "AlignAssignments",
                                  alignAssignments) WRITE("FormatRoutineBodies",
                                                          formatRoutineBodies)
                                  WRITE("FinalNewline", finalNewline)
#undef WRITE
#define WRITE(name, field) out << name << ": " << list(s.field) << '\n';
                                      WRITE("SelectList", selectList) WRITE(
                                          "InsertColumns", insertColumns)
                                          WRITE("ValuesRows", valuesRows) WRITE(
                                              "UpdateAssignments",
                                              updateAssignments)
                                              WRITE("DdlColumns", ddlColumns)
                                                  WRITE("RoutineParameters",
                                                        routineParameters)
                                                      WRITE("FunctionArguments",
                                                            functionArguments)
#undef WRITE
                                                          out
      << "KeywordCase: "
      << (s.keywordCase == KeywordCase::Upper   ? "Upper"
          : s.keywordCase == KeywordCase::Lower ? "Lower"
                                                : "Preserve")
      << "\nCommaPosition: "
      << (s.commaPosition == CommaPosition::Leading ? "Leading" : "Trailing")
      << "\nLineEnding: "
      << (s.lineEnding == LineEnding::LF     ? "LF"
          : s.lineEnding == LineEnding::CRLF ? "CRLF"
                                             : "Preserve")
      << '\n';
  return out.str();
}
} // namespace navidog::sql::compiler
