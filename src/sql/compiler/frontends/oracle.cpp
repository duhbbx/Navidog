#include "frontends.hpp"
namespace navidog::sql::compiler {
Statement Parser::packageStatement() {
  fail("package syntax requires the Oracle frontend",DiagnosticCode::UnsupportedSyntax);
}
Statement OracleParser::packageStatement() {
  Statement result;
  result.kind=StatementKind::Package;
  auto package=std::make_shared<PackageSyntax>();
  package->drop=eat("DROP");
  if(!package->drop) {
    expect("CREATE");
    if(eat("OR")) {expect("REPLACE");package->replace=true;}
    if(eat("EDITIONABLE"))package->edition=PackageEdition::Editionable;
    else if(eat("NONEDITIONABLE"))package->edition=PackageEdition::NonEditionable;
  }
  expect("PACKAGE");
  if(at("BODY"))fail("Oracle package bodies need their own PL/SQL grammar",DiagnosticCode::UnsupportedSyntax);
  package->name=qualifiedName();
  if(package->name.size()>2)fail("Oracle package names allow only a schema qualifier");
  if(!package->drop) {
    if(eat("AUTHID")) {
      if(eat("CURRENT_USER"))package->auth=PackageAuth::CurrentUser;
      else {expect("DEFINER");package->auth=PackageAuth::Definer;}
    }
    if(!eat("AS"))expect("IS");
    while(!at("END")) {
      syntaxNode();
      using Kind=PackageDeclarationKind;
      BasicPackageDeclaration<NamePart,QualifiedName> declaration;
      declaration.origin.begin=current().range.begin;
      const bool procedure=eat("PROCEDURE");
      const bool function=!procedure && eat("FUNCTION");
      declaration.name=namePart();
      if(procedure || function) {
        declaration.kind=procedure?Kind::Procedure:Kind::Function;
        if(eat("(")) {
          do {
            syntaxNode();
            BasicPackageParameter<NamePart,QualifiedName> parameter;
            parameter.origin.begin=current().range.begin;
            parameter.name=namePart();
            if(eat("IN"))parameter.mode=eat("OUT")?ParameterMode::InOut:ParameterMode::In;
            else if(eat("OUT"))parameter.mode=ParameterMode::Out;
            parameter.noCopy=eat("NOCOPY");
            parameter.type=typeSpec();
            if(eat("DEFAULT") || eat(":=")) {
              parameter.defaultValue=expression();
              requireScalar(parameter.defaultValue);
            }
            parameter.origin.end=previous().range.end;
            declaration.parameters.push_back(std::move(parameter));
          } while(eat(","));
          expect(")");
        }
        if(function) {
          expect("RETURN");
          declaration.type=typeSpec();
          declaration.deterministic=eat("DETERMINISTIC");
        }
      } else if(eat("EXCEPTION"))declaration.kind=Kind::Exception;
      else {
        const bool constant=eat("CONSTANT");
        declaration.kind=constant?Kind::Constant:Kind::Variable;
        declaration.type=typeSpec();
        if(eat("DEFAULT") || eat(":=")) {
          declaration.initializer=expression();
          requireScalar(declaration.initializer);
        }
      }
      expect(";");
      declaration.origin.end=previous().range.end;
      package->declarations.push_back(std::move(declaration));
    }
    expect("END");
    if(nameToken())package->endName=namePart();
  }
  result.package=std::move(package);
  return result;
}
Parser::DdlGrammar OracleParser::ddlGrammar() const {
  DdlGrammar g;
  g.oracle = true;
  return g;
}
Parser::DmlGrammar OracleParser::dmlGrammar() const {
  DmlGrammar g;
  g.returning = true;
  g.returningInto = true;
  g.defaultValues = false;
  g.multiValues = false;
  g.leadingWith = false;
  return g;
}
ExprId OracleParser::paginationLiteralExpression() {
  // Oracle 19c permits numeric expressions, NULL and signed/fractional values.
  // Keep this first extension independent of unresolved column/function scope.
  if (at("ROW") || at("ROWS") || at("ONLY") || at("WITH") || at("PERCENT") || at("OFFSET") || at("FETCH"))
    fail("expected an Oracle row-count/offset expression");
  const auto root=expression(51);
  std::vector<ExprId> work{root};
  while (!work.empty()) {
    checkStop(context_);
    const auto &value=context_.ast.expression(work.back());
    work.pop_back();
    const bool scalar=value.kind==ExprKind::Number || value.kind==ExprKind::Null || value.kind==ExprKind::Parameter;
    const bool sign=value.kind==ExprKind::Unary && (value.value=="+" || value.value=="-");
    const bool arithmetic=value.kind==ExprKind::Binary &&
        (value.value=="+" || value.value=="-" || value.value=="*" || value.value=="/");
    if (!scalar && !sign && !arithmetic)
      fail("Oracle pagination currently covers numeric literals/arithmetic, NULL and bind parameters; other expression scope requires further validation",
           DiagnosticCode::UnsupportedSyntax);
    work.insert(work.end(),value.operands.begin(),value.operands.end());
  }
  return root;
}
void OracleParser::pagination(SelectStatement &s) {
  if (eat("OFFSET")) {
    s.limit.offset=paginationLiteralExpression();
    if (!eat("ROW")) expect("ROWS");
    s.limit.syntax=LimitSyntax::Fetch;
  }
  if (eat("FETCH")) {
    if (!eat("FIRST")) expect("NEXT");
    s.limit.count=(at("ROW") || at("ROWS"))?
        node({ExprKind::Number,current().range,"1",{}, {}}):paginationLiteralExpression();
    if (at("PERCENT"))
      fail("Oracle percentage pagination requires a separate percentage/row-count model",DiagnosticCode::UnsupportedSyntax);
    if (!eat("ROW")) expect("ROWS");
    if (eat("WITH")) {expect("TIES");s.limit.withTies=true;}
    else expect("ONLY");
    s.limit.syntax=LimitSyntax::Fetch;
  }
  if (at("OFFSET") || at("FETCH")) fail("Oracle row limiting requires OFFSET before a single FETCH clause");
}
std::pair<std::string, int> OracleParser::binaryOperator() const {
  if (at("||"))
    return {"CONCAT", 80};
  return Parser::binaryOperator();
}
} // namespace navidog::sql::compiler
