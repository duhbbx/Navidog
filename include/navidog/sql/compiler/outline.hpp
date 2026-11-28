#pragma once

#include "compiler.hpp"

namespace navidog::sql::compiler {

struct SqlOutlineNode {
  std::string label;
  SourceRange range;
  std::vector<SqlOutlineNode> children;
  bool recovered = false;
};

struct SqlOutline {
  std::vector<SqlOutlineNode> roots;
  bool partial = false;
  bool truncated = false;
};

// A source-oriented outline. Ranges are half-open UTF-8 byte offsets into
// the original script, including statements inside quoted routine bodies.
SqlOutline outlineSql(Dialect, std::string, LanguageOptions = {});

} // namespace navidog::sql::compiler
