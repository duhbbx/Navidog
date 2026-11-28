#pragma once
#include "compiler.hpp"
namespace navidog::sql::compiler {
enum class CompletionKind {
  Keyword,
  Snippet,
  Schema,
  Table,
  View,
  Column,
  Alias,
  Function,
  Procedure,
  Variable,
  Type,
  Database, MaterializedView, Index, Sequence, Trigger, Synonym, Package, Event, Cursor, Label, PreparedStatement
};
struct CompletionItem {
  std::string label, insertText, detail;
  CompletionKind kind = CompletionKind::Keyword;
  SourceRange replacement;
  std::size_t cursor = 0, selection = 0; // UTF-8 offsets in insertText.
  int priority = 0;
};
struct CompletionOptions {
  LanguageOptions language;
  std::size_t maxItems = 128, maxCatalogEntries = 4096;
  bool snippets = true;
  bool columnListOnly = false; // Punctuation in a recognized list or SQL clause.
};
struct CompletionResult {
  std::vector<CompletionItem> items;
  std::vector<Diagnostic> diagnostics;
  SourceRange replacement;
  bool suppressed = false, truncated = false, parsedScope = false,
       metadataFresh = false;
};
// Read-only completion against an immutable, in-memory catalog. UTF-8 cursor
// and replacement ranges; no connection, execution, or persistence.
CompletionResult completeSql(Dialect, std::string, std::size_t cursor,
                             const CatalogSnapshot * = nullptr,
                             NameResolutionContext = {},
                             CompletionOptions = {});
const char *completionKindName(CompletionKind);
} // namespace navidog::sql::compiler
