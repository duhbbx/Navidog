#include "parser.hpp"
#include <algorithm>
#include <array>
#include <set>

namespace navidog::sql::compiler {
namespace {
constexpr std::array<const char *,7> fields={"","YEAR","MONTH","DAY","HOUR","MINUTE","SECOND"};
constexpr std::array<const char *,21> units={"","YEAR","QUARTER","MONTH","WEEK","DAY","HOUR","MINUTE","SECOND","MICROSECOND","YEAR_MONTH","DAY_HOUR","DAY_MINUTE","DAY_SECOND","HOUR_MINUTE","HOUR_SECOND","MINUTE_SECOND","DAY_MICROSECOND","HOUR_MICROSECOND","MINUTE_MICROSECOND","SECOND_MICROSECOND"};
IntervalField field(std::string text) {
  text=asciiUpper(text);for(std::size_t i=1;i<fields.size();++i)if(text==fields[i])return static_cast<IntervalField>(i);
  return IntervalField::None;
}
IntervalUnit unit(std::string text) {
  text=asciiUpper(text);for(std::size_t i=1;i<units.size();++i)if(text==units[i])return static_cast<IntervalUnit>(i);
  return IntervalUnit::None;
}
}
const char *intervalFieldName(IntervalField f) {const auto i=static_cast<std::size_t>(f);return i<fields.size()?fields[i]:"";}
const char *intervalUnitName(IntervalUnit u) {const auto i=static_cast<std::size_t>(u);return i<units.size()?units[i]:"";}
std::string validateIntervalSpec(const IntervalSpec &s,Dialect d) {
  const bool my=isMySqlDialect(d);
  const bool pg=d==Dialect::PostgreSQL,ora=d==Dialect::Oracle;
  if(!my && !pg && !ora)return "interval syntax is unavailable in target dialect";
  if(static_cast<unsigned>(s.leading)>static_cast<unsigned>(IntervalField::Second) ||
     static_cast<unsigned>(s.trailing)>static_cast<unsigned>(IntervalField::Second) ||
     static_cast<unsigned>(s.mysqlUnit)>static_cast<unsigned>(IntervalUnit::SecondMicrosecond))return "invalid interval unit";
  if(my) {
    if(s.mysqlUnit==IntervalUnit::None || s.leading!=IntervalField::None || s.trailing!=IntervalField::None || s.leadingPrecision || s.fractionalPrecision || s.prefixPrecision)return "invalid MySQL interval qualifier";
    return {};
  }
  if(s.mysqlUnit!=IntervalUnit::None)return "MySQL interval unit in literal qualifier";
  if(s.leadingPrecision && (!ora || *s.leadingPrecision<0 || *s.leadingPrecision>9))return "invalid interval leading precision";
  if(s.fractionalPrecision && (*s.fractionalPrecision<0 || *s.fractionalPrecision>(pg?6:9)))return "invalid interval fractional precision";
  if(s.prefixPrecision && (!pg || !s.fractionalPrecision))return "invalid interval prefix precision";
  if(ora && s.leading==IntervalField::None)return "Oracle interval requires a leading field";
  if(s.trailing!=IntervalField::None &&
     (s.leading==IntervalField::None || s.trailing<=s.leading || (s.leading<=IntervalField::Month && s.trailing>IntervalField::Month)))return "invalid interval field range";
  const auto end=s.trailing==IntervalField::None?s.leading:s.trailing;
  if(s.fractionalPrecision && end!=IntervalField::Second && !(pg && s.prefixPrecision && end==IntervalField::None))return "interval fractional precision requires SECOND";
  if(ora && s.leading==IntervalField::Second && s.fractionalPrecision && !s.leadingPrecision)return "Oracle leading SECOND requires leading precision before fractional precision";
  return {};
}
bool validExtractField(std::string_view field,Dialect d) {
  const auto spelling=std::string(field);
  static const std::set<std::string> pg={"CENTURY","DAY","DECADE","DOW","DOY","EPOCH","HOUR","ISODOW","ISOYEAR","JULIAN","MICROSECONDS","MILLENNIUM","MILLISECONDS","MINUTE","MONTH","QUARTER","SECOND","TIMEZONE","TIMEZONE_HOUR","TIMEZONE_MINUTE","WEEK","YEAR"};
  static const std::set<std::string> oracle={"YEAR","MONTH","DAY","HOUR","MINUTE","SECOND","TIMEZONE_HOUR","TIMEZONE_MINUTE","TIMEZONE_REGION","TIMEZONE_ABBR"};
  if(d==Dialect::PostgreSQL)return pg.count(spelling);
  if(d==Dialect::Oracle)return oracle.count(spelling);
  if(isMySqlDialect(d))return unit(spelling)!=IntervalUnit::None;
  return false;
}
bool Parser::intervalAhead() const {
  if(!at("INTERVAL"))return false;
  if (ddlGrammar().postgres)
    return at("(",1) || (cursor_+1<tokens_.size() && tokens_[cursor_+1].kind==TokenKind::String);
  if(!dmlGrammar().mysql)return true;
  // INTERVAL(a,b,...) is also a MySQL scalar function. An expression followed
  // by a unit belongs to the temporal grammar instead of the function grammar.
  if(!at("(",1))return true;
  std::size_t depth=0;
  for(std::size_t i=cursor_+1;i<tokens_.size();++i) {
    if(tokens_[i].value=="(")++depth;
    else if(tokens_[i].value==")" && --depth==0)
      return i+1<tokens_.size() && unit(tokens_[i+1].value)!=IntervalUnit::None;
    if(tokens_[i].kind==TokenKind::End)break;
  }
  return false;
}
ExprId Parser::intervalExpression() {
  const auto begin=take().range.begin;IntervalSpec spec;
  Expr result;result.kind=ExprKind::Interval;result.range.begin=begin;
  if(dmlGrammar().mysql) {
    result.operands.push_back(expression());requireScalar(result.operands.back());
    spec.mysqlUnit=unit(current().value);
    if(spec.mysqlUnit==IntervalUnit::None)fail("expected MySQL interval unit");
    take();
  } else {
    if(!ddlGrammar().postgres && !ddlGrammar().oracle)
      fail("INTERVAL literals are unavailable in this dialect",DiagnosticCode::ExpectedToken);
    auto precision=[&](int maximum) {
      if(current().kind!=TokenKind::Number || current().value.empty() || current().value.size()>2 ||
         current().value.find_first_not_of("0123456789")!=std::string::npos)
        fail("expected an integer interval precision");
      const auto p=std::stoi(take().value);if(p>maximum)fail("interval precision is outside this dialect's supported range");return p;
    };
    if(ddlGrammar().postgres && eat("(")) {
      spec.prefixPrecision=true;spec.fractionalPrecision=precision(6);expect(")");
    }
    if(current().kind!=TokenKind::String)fail("INTERVAL literal requires a string value");
    result.value=take().value;
    spec.leading=field(current().value);
    if(spec.leading!=IntervalField::None) {
      take();
      if(ddlGrammar().oracle && eat("(")) {
        spec.leadingPrecision=precision(9);
        if(eat(",")) {
          if(spec.leading!=IntervalField::Second)fail("two precisions require a leading SECOND field");
          spec.fractionalPrecision=precision(9);
        }
        expect(")");
      }
      if(eat("TO")) {
        spec.trailing=field(current().value);
        if(spec.trailing==IntervalField::None)fail("expected a trailing interval field");
        take();
        const auto a=static_cast<int>(spec.leading),b=static_cast<int>(spec.trailing);
        if(b<=a || (a<=static_cast<int>(IntervalField::Month) && b>static_cast<int>(IntervalField::Month)))
          fail("interval trailing field must be less significant and in the same year-month / day-time group");
      }
      if(ddlGrammar().postgres && eat("(")) {
        if(spec.prefixPrecision)fail("duplicate interval fractional precision");
        if((spec.trailing==IntervalField::None?spec.leading:spec.trailing)!=IntervalField::Second)
          fail("PostgreSQL interval precision requires a SECOND field");
        spec.fractionalPrecision=precision(6);expect(")");
      } else if(ddlGrammar().oracle && spec.trailing!=IntervalField::None && eat("(")) {
        if(spec.trailing!=IntervalField::Second)fail("trailing interval precision requires SECOND");
        spec.fractionalPrecision=precision(9);expect(")");
      }
      if(spec.prefixPrecision && spec.trailing!=IntervalField::Second && spec.leading!=IntervalField::Second)
        fail("PostgreSQL interval fractional precision requires a SECOND field");
    } else if(ddlGrammar().oracle)fail("Oracle interval literal requires leading YEAR, MONTH, DAY, HOUR, MINUTE or SECOND");
  }
  if(const auto error=validateIntervalSpec(spec,context_.dialect);!error.empty())fail(error);
  result.interval=std::make_shared<const IntervalSpec>(std::move(spec));
  result.range.end=previous().range.end;return node(std::move(result));
}
void Parser::intervalTypeTail(BasicTypeSpec<QualifiedName> &type) {
  if(!ddlGrammar().postgres && !ddlGrammar().oracle)fail("INTERVAL types are unavailable in this dialect",DiagnosticCode::ExpectedToken);
  IntervalSpec spec;
  auto precision=[&](int maximum) {
    if(current().kind!=TokenKind::Number || current().value.empty() || current().value.size()>2 || current().value.find_first_not_of("0123456789")!=std::string::npos)
      fail("expected an integer interval type precision");
    const auto value=std::stoi(take().value);if(value>maximum)fail("interval type precision is outside this dialect's supported range");return value;
  };
  if(ddlGrammar().postgres && eat("(")) {spec.prefixPrecision=true;spec.fractionalPrecision=precision(6);expect(")");}
  spec.leading=field(current().value);
  if(spec.leading!=IntervalField::None) {
    if(spec.prefixPrecision)fail("PostgreSQL interval type precision must follow field qualifiers");
    take();
    if(ddlGrammar().oracle && eat("(")){spec.leadingPrecision=precision(9);expect(")");}
    if(eat("TO")) {
      spec.trailing=field(current().value);if(spec.trailing==IntervalField::None)fail("expected trailing interval type field");take();
    }
    if(eat("(")) {
      const auto end=spec.trailing==IntervalField::None?spec.leading:spec.trailing;
      if(end!=IntervalField::Second)fail("interval type fractional precision requires SECOND");
      spec.fractionalPrecision=precision(ddlGrammar().oracle?9:6);expect(")");
    }
  }
  if(ddlGrammar().oracle && !((spec.leading==IntervalField::Year && spec.trailing==IntervalField::Month) || (spec.leading==IntervalField::Day && spec.trailing==IntervalField::Second)))
    fail("Oracle interval type requires YEAR TO MONTH or DAY TO SECOND");
  if(const auto error=validateIntervalSpec(spec,context_.dialect);!error.empty())fail(error);
  type.intervalType=std::make_shared<const IntervalSpec>(std::move(spec));
}
ExprId Parser::extractExpression() {
  const auto begin=take().range.begin;
  if(ddlGrammar().sqlserver || ddlGrammar().sqlite)
    fail("EXTRACT syntax is unavailable in this dialect",DiagnosticCode::ExpectedToken);
  expect("(");
  const auto part=current();
  if(part.kind!=TokenKind::Word && part.kind!=TokenKind::String)fail("expected EXTRACT field");
  const auto spelling=asciiUpper(part.value);
  const bool valid=validExtractField(spelling,context_.dialect);
  if(!valid || (part.kind==TokenKind::String && !ddlGrammar().postgres))fail("invalid EXTRACT field for this dialect");
  take();expect("FROM");auto value=expression();requireScalar(value);expect(")");
  return node({ExprKind::Extract,{begin,previous().range.end},spelling,{}, {value}});
}
} // namespace navidog::sql::compiler
