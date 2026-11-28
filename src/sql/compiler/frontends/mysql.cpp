#include "frontends.hpp"
namespace navidog::sql::compiler {
Parser::DdlGrammar MySQLParser::ddlGrammar() const {
  DdlGrammar g;
  g.mysql = true;
  return g;
}
Parser::DmlGrammar MySQLParser::dmlGrammar() const {
  DmlGrammar g;
  g.mysql = true;
  g.defaultValues = false;
  g.qualifiedAssignment = true;
  return g;
}
void MySQLParser::pagination(SelectStatement &s) {
  limitOffset(s, true, false);
}
void MySQLParser::lockingRead(SelectStatement &s) {
  if (context_.dialect != Dialect::MySQL || context_.options.extension != SqlExtension::None)
    return;
  while (at("FOR") || at("LOCK")) {
    if (s.setOperation)
      fail("MySQL requires parentheses around a SELECT with row locks in a set operation");
    if (s.valuesQuery)
      fail("MySQL row locking requires SELECT");
    syntaxNode();
    RowLockSyntax clause;
    clause.origin.begin = current().range.begin;
    if (eat("FOR")) {
      if (!eat("UPDATE")) {
        expect("SHARE");
        clause.strength = RowLockStrength::Share;
      }
      if (eat("OF")) {
        do {
          syntaxNode();
          auto target = qualifiedName();
          if (target.size() > 2) fail("MySQL lock targets allow only a database qualifier");
          clause.targets.push_back(std::move(target));
        } while (eat(","));
      }
      if (eat("NOWAIT")) clause.wait = RowLockWait::NoWait;
      else if (eat("SKIP")) {
        expect("LOCKED");
        clause.wait = RowLockWait::SkipLocked;
      }
      if (at("NOWAIT") || at("SKIP"))
        fail("use only one wait option for a MySQL locking clause");
    } else {
      expect("LOCK");
      expect("IN");
      expect("SHARE");
      expect("MODE");
      clause.strength = RowLockStrength::Share;
      clause.legacyShare = true;
    }
    clause.origin.end = previous().range.end;
    if (!s.rowLocks.empty() && (clause.targets.empty() || s.rowLocks.front().targets.empty()))
      fail("a MySQL lock without OF cannot be combined with another locking clause");
    s.rowLocks.push_back(std::move(clause));
  }
}
Statement MySQLParser::triggerStatement() {
  Statement result;
  result.kind=StatementKind::Trigger;
  auto trigger=std::make_shared<TriggerSyntax>();
  trigger->drop=eat("DROP");
  if(!trigger->drop) {
    expect("CREATE");
    if (eat("DEFINER")) trigger->definer = storedObjectDefiner();
  }
  expect("TRIGGER");
  if(eat("IF")) {
    if(!trigger->drop)expect("NOT");
    expect("EXISTS");
    trigger->ifGuard=true;
  }
  trigger->name=qualifiedName();
  if(trigger->name.size()>2)fail("MySQL trigger names allow only a database qualifier");
  if(!trigger->drop) {
    if(eat("BEFORE"))trigger->timing=TriggerTiming::Before;
    else {expect("AFTER");trigger->timing=TriggerTiming::After;}
    if(eat("INSERT"))trigger->event=TriggerEvent::Insert;
    else if(eat("UPDATE"))trigger->event=TriggerEvent::Update;
    else {expect("DELETE");trigger->event=TriggerEvent::Delete;}
    expect("ON");
    trigger->target=qualifiedName();
    if(trigger->target.size()>2)fail("MySQL trigger targets allow only a database qualifier");
    trigger->targetOrigin={trigger->target.front().range.begin,trigger->target.back().range.end};
    expect("FOR");expect("EACH");expect("ROW");
    trigger->explicitRow=true;
    if(eat("FOLLOWS"))trigger->order=TriggerOrder::Follows;
    else if(eat("PRECEDES"))trigger->order=TriggerOrder::Precedes;
    if(trigger->order!=TriggerOrder::None)trigger->relativeTrigger=namePart();
    trigger->compound=eat("BEGIN");
    do {
      if(trigger->body.size()>=context_.options.limits.statements)
        fail("MySQL trigger step limit exceeded",DiagnosticCode::ResourceLimit);
      Statement step;
      const auto begin=current().range.begin;
      if(eat("SET")) {
        step.kind=StatementKind::TriggerSet;
        do {
          syntaxNode();
          const auto target=primary();
          const auto &name=context_.ast.expression(target);
          const bool row=name.kind==ExprKind::Name && name.name.size()==2 &&
              asciiUpper(context_.ast.identifier(name.name.front().identifier))=="NEW";
          const bool session=name.kind==ExprKind::Variable && name.value.size()>1 &&
                             name.value[0]=='@' && name.value[1]!='@';
          if(!row && !session)
            fail("trigger SET requires NEW.column or a user session variable",DiagnosticCode::UnsupportedSyntax);
          expect("=");
          const auto value=expression();
          requireScalar(value);
          step.assignments.push_back({{target},value});
        } while(eat(","));
        step.range={begin,previous().range.end};
      } else if(at("INSERT") || at("UPDATE") || at("DELETE")) {
        step=statement();
      } else fail("MySQL trigger step requires SET, INSERT, UPDATE or DELETE in this frontend",
                  DiagnosticCode::UnsupportedSyntax);
      if(trigger->compound)expect(";");
      trigger->body.push_back(context_.ast.addQuery(std::move(step)));
    } while(trigger->compound && !eat("END"));
  }
  result.trigger=std::move(trigger);
  return result;
}
std::pair<std::string, int> MySQLParser::binaryOperator() const {
  if (at("REGEXP") || at("RLIKE"))
    return {"REGEXP",precedence("LIKE")};
  if (at("XOR"))
    return {"XOR",15};
  if (at("&&"))
    return {"AND", 20};
  if (at("||"))
    return context_.options.mysqlPipesAsConcat
               ? std::make_pair(std::string("CONCAT"), 100)
               : std::make_pair(std::string("OR"), 10);
  if (at("<=>"))
    return {"NULL_SAFE_EQUAL", 50};
  if (at("%") || at("MOD"))
    return {"%", 90};
  if (at("DIV"))
    return {"DIV", 90};
  return Parser::binaryOperator();
}
int MySQLParser::precedence(std::string_view op) const {
  // Comparisons consume predicates on their right; predicates in turn consume
  // bit_expr/simple_expr. These production boundaries outrank the Bison token
  // precedence table (e.g. a=b IN(c) means a=(b IN(c))).
  if (op == "IS") return 50;
  if (op == "BETWEEN") return 57;
  if (op == "IN" || op == "LIKE") return 60;
  return Parser::precedence(op);
}
} // namespace navidog::sql::compiler
