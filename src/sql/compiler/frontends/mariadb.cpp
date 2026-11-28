#include "frontends.hpp"
namespace navidog::sql::compiler {
MariaDBParser::MariaDBParser(Compilation &c) : MySQLParser(c) {}
int MariaDBParser::precedence(std::string_view op) const {
  // MariaDB's recursive predicate/boolean_test productions additionally allow
  // predicate chains and comparisons after truth tests; expression() enforces
  // those dialect-specific operand categories.
  return MySQLParser::precedence(op);
}
} // namespace navidog::sql::compiler
