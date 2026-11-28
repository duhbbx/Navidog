#pragma once
#include "../parser.hpp"

namespace navidog::sql::compiler {
class MySQLParser : public Parser {
public:
  using Parser::Parser;

protected:
  Statement inspectionStatement() override;
  Statement triggerStatement() override;
  DmlGrammar dmlGrammar() const override;
  DdlGrammar ddlGrammar() const override;
  void pagination(SelectStatement &) override;
  void lockingRead(SelectStatement &) override;
  bool fullJoin() const override { return false; }
  std::pair<std::string, int> binaryOperator() const override;
  int precedence(std::string_view op) const override;
  bool contextualIdentifier(std::string_view word) const override { return word=="QUICK"; }
};
class PostgreSQLParser final : public Parser {
public:
  using Parser::Parser;

private:
  Statement vacuumStatement() override;
  Statement copyStatement() override;
  int optionInteger(int max);
  bool optionBoolean();
  ExprId paginationValue(bool restricted=false);
  bool paginationPrimary(std::size_t begin,ExprId) const;
  bool nameToken() const override;
  bool functionNameToken() const override;
  bool typeNameToken() const override;
  bool bareProjectionAliasToken() const override;
  ExprId postfix(ExprId) override;
  DmlGrammar dmlGrammar() const override;
  DdlGrammar ddlGrammar() const override;
  void pagination(SelectStatement &) override;
  bool nullOrdering() const override { return true; }
  std::pair<std::string, int> binaryOperator() const override;
  int precedence(std::string_view op) const override;
};
class OracleParser final : public Parser {
public:
  using Parser::Parser;

private:
  Statement packageStatement() override;
  DmlGrammar dmlGrammar() const override;
  DdlGrammar ddlGrammar() const override;
  void pagination(SelectStatement &) override;
  ExprId paginationLiteralExpression();
  bool tableAliasWithAS() const override { return false; }
  bool requiresFrom() const override {
    return true;
  } // Oracle 12c/19c grammar baseline.
  bool nullOrdering() const override { return true; }
  std::pair<std::string, int> binaryOperator() const override;
};
class SQLiteParser final : public Parser {
public:
  using Parser::Parser;

private:
  Statement pragmaStatement() override;
  void createVirtualTable(DdlSyntax &) override;
  Statement triggerStatement() override;
  DmlGrammar dmlGrammar() const override;
  DdlGrammar ddlGrammar() const override;
  void pagination(SelectStatement &) override;
  bool nullOrdering() const override { return true; }
  std::pair<std::string, int> binaryOperator() const override;
  int precedence(std::string_view op) const override;
};
class SQLServerParser final : public Parser {
public:
  using Parser::Parser;

private:
  bool nameToken() const override;
  bool functionNameToken() const override;
  bool contextualIdentifier(std::string_view) const override;
  void selectPrefix(SelectStatement &) override;
  DmlGrammar dmlGrammar() const override;
  DdlGrammar ddlGrammar() const override;
  void pagination(SelectStatement &) override;
  ExprId limitValue() override;
  std::pair<std::string, int> binaryOperator() const override;
  int precedence(std::string_view op) const override;
};
// Distinct product frontends share the supported MySQL-compatible grammar.
// MariaDB extensions and OceanBase Oracle mode are deliberately not advertised.
class MariaDBParser final : public MySQLParser {
public:
  explicit MariaDBParser(Compilation &);
private:
  int precedence(std::string_view op) const override;
};
class OceanBaseParser final : public MySQLParser {
public:
  explicit OceanBaseParser(Compilation &);

private:
  DdlGrammar ddlGrammar() const override;
  Statement inspectionStatement() override;
};
class TiDBParser final : public MySQLParser {
public:
  using MySQLParser::MySQLParser;

private:
  bool nameToken() const override;
  void validateColumnType(const BasicTypeSpec<QualifiedName> &) override;
  DdlGrammar ddlGrammar() const override;
  Statement inspectionStatement() override;
  Statement triggerStatement() override;
};
} // namespace navidog::sql::compiler
