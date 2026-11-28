#include "navidog/sql/optimizer/interpreter.hpp"
#include <algorithm>
#include <charconv>

namespace navidog::sql::optimizer {
namespace {
constexpr std::array<std::uint32_t,10> powers{
    1,10,100,1000,10000,100000,1000000,10000000,100000000,1000000000};
constexpr std::uint64_t base=powers.back();

bool parseUnit(std::string_view text,std::uint32_t &unit) {
  const auto *last=text.data()+text.size();
  const auto parsed=std::from_chars(text.data(),last,unit);
  return parsed.ec==std::errc{} && parsed.ptr==last;
}
void appendPadded(std::string &result,std::uint32_t unit) {
  const auto digits=std::to_string(unit);
  result.append(9-digits.size(),'0');
  result+=digits;
}
std::optional<std::string> expandScientific(std::string_view text,std::size_t marker) {
  if (text.size()>128) return {};
  auto mantissa=text.substr(0,marker);
  auto exponentText=text.substr(marker+1);
  if (mantissa.empty() || exponentText.empty()) return {};
  const bool negative=mantissa.front()=='-';
  if (negative || mantissa.front()=='+') mantissa.remove_prefix(1);
  if (exponentText.front()=='+') {
    exponentText.remove_prefix(1);
    if (exponentText.empty() || exponentText.front()<'0' || exponentText.front()>'9') return {};
  }
  int exponent=0;
  const auto end=exponentText.data()+exponentText.size();
  const auto parsed=std::from_chars(exponentText.data(),end,exponent);
  if (parsed.ec!=std::errc{} || parsed.ptr!=end || exponent < -256 || exponent>256) return {};
  const auto dot=mantissa.find('.');
  const auto left=mantissa.substr(0,dot);
  const auto right=dot==std::string_view::npos?std::string_view{}:mantissa.substr(dot+1);
  if ((left.empty() && right.empty()) ||
      left.find_first_not_of("0123456789")!=std::string_view::npos ||
      right.find_first_not_of("0123456789")!=std::string_view::npos)
    return {};
  std::string digits(left);
  digits.append(right);
  const auto decimal=static_cast<int>(left.size())+exponent;
  const auto count=static_cast<int>(digits.size());
  const auto scale=std::max(0,count-decimal);
  if (scale>LocalNumeric::maxScale) return {};
  std::string expanded=negative?"-":"";
  if (decimal<=0) {
    expanded+="0.";
    expanded.append(-decimal,'0');
    expanded+=digits;
  } else if (decimal<count) {
    expanded.append(digits,0,decimal);
    expanded+='.';
    expanded.append(digits,decimal,std::string::npos);
  } else {
    const auto first=digits.find_first_not_of('0');
    if (first==std::string::npos) return "0";
    if (decimal-static_cast<int>(first)>LocalNumeric::maxScale) return {};
    expanded+=digits;
    expanded.append(decimal-count,'0');
  }
  return expanded;
}
} // namespace

LocalNumeric::Storage LocalNumeric::integer(std::int64_t value) {
  Storage result;
  result.negative=value<0;
  auto magnitude=result.negative?std::uint64_t(0)-static_cast<std::uint64_t>(value)
                                :static_cast<std::uint64_t>(value);
  for (std::size_t i=fractionalLimbs;i<result.units.size();++i) {
    result.units[i]=static_cast<std::uint32_t>(magnitude%base);
    magnitude/=base;
  }
  return result;
}

LocalNumeric::LocalNumeric(std::int64_t value) {
  if (value) replace(integer(value));
}
LocalNumeric::LocalNumeric(Storage value) {
  if (value.scale || !value.zero()) replace(std::move(value));
}
const LocalNumeric::Storage &LocalNumeric::value() const {
  static const Storage zero;
  return data_?*data_:zero;
}
void LocalNumeric::replace(Storage result) {
  if (data_.use_count()==1) *data_=std::move(result);
  else data_=std::make_shared<Storage>(std::move(result));
}

std::optional<LocalNumeric> LocalNumeric::parse(std::string_view text) {
  if (const auto marker=text.find_first_of("eE");marker!=std::string_view::npos) {
    const auto expanded=expandScientific(text,marker);
    return expanded?parse(*expanded):std::nullopt;
  }
  Storage result;
  if (text.empty()) return {};
  const bool negative=text.front()=='-';
  if (negative || text.front()=='+') text.remove_prefix(1);
  const auto dot=text.find('.');
  auto integral=text.substr(0,dot);
  auto fractional=dot==std::string_view::npos?std::string_view{}:text.substr(dot+1);
  if ((integral.empty() && fractional.empty()) ||
      integral.find_first_not_of("0123456789")!=std::string_view::npos ||
      fractional.find_first_not_of("0123456789")!=std::string_view::npos) return {};
  integral.remove_prefix(std::min(integral.size(),integral.find_first_not_of('0')));
  if (integral.size()>digitsPerLimb*(result.units.size()-fractionalLimbs) ||
      fractional.size()>digitsPerLimb*fractionalLimbs) return {};
  result.scale=static_cast<std::uint8_t>(fractional.size());
  for (std::size_t i=fractionalLimbs;i<result.units.size() && !integral.empty();++i) {
    const auto count=std::min<std::size_t>(digitsPerLimb,integral.size());
    if (!parseUnit(integral.substr(integral.size()-count),result.units[i])) return {};
    integral.remove_suffix(count);
  }
  for (std::size_t i=fractionalLimbs;i>0 && !fractional.empty();--i) {
    const auto count=std::min<std::size_t>(digitsPerLimb,fractional.size());
    if (!parseUnit(fractional.substr(0,count),result.units[i-1])) return {};
    result.units[i-1]*=powers[digitsPerLimb-count];
    fractional.remove_prefix(count);
  }
  result.negative=negative && !result.zero();
  return LocalNumeric(std::move(result));
}

bool LocalNumeric::Storage::zero() const {
  return std::all_of(units.begin(),units.end(),[](auto unit){return unit==0;});
}

int LocalNumeric::Storage::compareMagnitude(const Storage &other) const {
  for (std::size_t i=units.size();i>0;--i) {
    if (units[i-1]!=other.units[i-1]) return units[i-1]<other.units[i-1]?-1:1;
  }
  return 0;
}

bool LocalNumeric::operator==(const LocalNumeric &other) const {
  return value().negative==other.value().negative && value().units==other.value().units;
}
bool LocalNumeric::operator<(const LocalNumeric &other) const {
  const auto &lhs=value();
  const auto &rhs=other.value();
  if (lhs.negative!=rhs.negative) return lhs.negative;
  const auto comparison=lhs.compareMagnitude(rhs);
  return lhs.negative?comparison>0:comparison<0;
}
int LocalNumeric::compare(std::int64_t other) const {
  const auto &lhs=value();
  const auto rhs=integer(other);
  if (lhs.negative!=rhs.negative) return lhs.negative?-1:1;
  const auto comparison=lhs.compareMagnitude(rhs);
  return lhs.negative?-comparison:comparison;
}

bool LocalNumeric::add(std::int64_t value) {
  return addStorage(integer(value));
}
bool LocalNumeric::add(const LocalNumeric &operand) {
  return addStorage(operand.value());
}
bool LocalNumeric::addStorage(const Storage &operand) {
  const auto &lhs=value();
  auto result=lhs;
  result.scale=std::max(lhs.scale,operand.scale);
  if (lhs.negative==operand.negative) {
    std::uint64_t carry=0;
    for (std::size_t i=0;i<result.units.size();++i) {
      const auto sum=std::uint64_t(lhs.units[i])+operand.units[i]+carry;
      result.units[i]=static_cast<std::uint32_t>(sum%base);
      carry=sum/base;
    }
    if (carry) return false;
  } else {
    const bool swap=lhs.compareMagnitude(operand)<0;
    const auto &larger=swap?operand.units:lhs.units;
    const auto &smaller=swap?lhs.units:operand.units;
    std::int64_t borrow=0;
    for (std::size_t i=0;i<result.units.size();++i) {
      const auto diff=std::int64_t(larger[i])-smaller[i]-borrow;
      borrow=diff<0;
      result.units[i]=static_cast<std::uint32_t>(diff+(borrow?static_cast<std::int64_t>(base):0));
    }
    result.negative=(swap?operand.negative:lhs.negative) && !result.zero();
  }
  replace(std::move(result));
  return true;
}

bool LocalNumeric::subtract(LocalNumeric operand) {
  auto negative=operand.value();
  if (!negative.zero()) negative.negative=!negative.negative;
  return addStorage(negative);
}

int LocalNumeric::Storage::digit(int position) const {
  if (position<0 || position>=static_cast<int>(units.size())*digitsPerLimb) return 0;
  return static_cast<int>(units[position/digitsPerLimb]/powers[position%digitsPerLimb]%10);
}

std::pair<int,int> LocalNumeric::Storage::leadingDecimalGroup() const {
  constexpr int fractionalDigits=digitsPerLimb*fractionalLimbs;
  int position=static_cast<int>(units.size())*digitsPerLimb-1;
  while (position>=0 && !digit(position)) --position;
  if (position<0) return {0,0};
  const auto exponent=position-fractionalDigits;
  const auto weight=(exponent<0?exponent-3:exponent)/4;
  const auto groupStart=fractionalDigits+4*weight;
  int leading=0;
  for (int i=3;i>=0;--i) leading=leading*10+digit(groupStart+i);
  return {weight,leading};
}

bool LocalNumeric::Storage::roundAt(int discarded,bool increase) {
  const auto limb=static_cast<std::size_t>(discarded/digitsPerLimb);
  if (limb>=units.size()) {
    units.fill(0);
    return !increase;
  }
  const auto quantum=powers[discarded%digitsPerLimb];
  std::fill(units.begin(),units.begin()+limb,0);
  units[limb]-=units[limb]%quantum;
  std::uint64_t carry=increase?quantum:0;
  for (std::size_t i=limb;i<units.size() && carry;++i) {
    const auto sum=units[i]+carry;
    units[i]=static_cast<std::uint32_t>(sum%base);
    carry=sum/base;
  }
  return !carry;
}

std::optional<LocalNumeric> LocalNumeric::withTypeModifier(int precision,int scale) const {
  if (precision<1 || precision>1000 || scale < -maxScale || scale>maxScale) return {};
  auto result=value();
  const auto discarded=maxScale-scale;
  if (!result.roundAt(discarded,result.digit(discarded-1)>=5)) return {};
  const auto edge=std::max(0,maxScale+precision-scale);
  const auto digits=static_cast<int>(result.units.size())*digitsPerLimb;
  for (int i=edge;i<digits;++i)
    if (result.digit(i)) return {};
  result.scale=static_cast<std::uint8_t>(std::max(0,scale));
  result.negative=result.negative && !result.zero();
  return LocalNumeric(std::move(result));
}

std::optional<LocalNumeric> LocalNumeric::multipliedBy(const LocalNumeric &right) const {
  const auto &lhs=value(), &rhs=right.value();
  const int scale=lhs.scale+rhs.scale;
  if (scale>maxScale) return {};
  constexpr std::size_t limbs=std::tuple_size<decltype(Storage::units)>::value;
  std::array<std::uint32_t,2*limbs> product{};
  for (std::size_t i=0;i<limbs;++i) {
    std::uint64_t carry=0;
    for (std::size_t j=0;j<limbs;++j) {
      const auto current=std::uint64_t(lhs.units[i])*rhs.units[j]+product[i+j]+carry;
      product[i+j]=static_cast<std::uint32_t>(current%base);
      carry=current/base;
    }
    product[i+limbs]=static_cast<std::uint32_t>(carry);
  }
  const auto overflow=[](auto unit) { return unit!=0; };
  if (std::any_of(product.begin()+fractionalLimbs+limbs,product.end(),overflow)) return {};
  Storage result;
  std::copy_n(product.begin()+fractionalLimbs,limbs,result.units.begin());
  result.scale=static_cast<std::uint8_t>(scale);
  result.negative=(lhs.negative!=rhs.negative) && !result.zero();
  return LocalNumeric(std::move(result));
}

std::optional<LocalNumeric::Storage> LocalNumeric::Storage::divide(
    const Storage &denominator,int scale,bool keepRemainder) const {
  if (denominator.zero() || scale<0 || scale>maxScale) return {};
  constexpr auto limbs=std::tuple_size<decltype(units)>::value;
  std::array<std::uint32_t,limbs+1> residual{}, divisor{};
  std::copy(denominator.units.begin(),denominator.units.end(),divisor.begin());
  const auto atLeastDivisor=[&] {
    for (std::size_t i=residual.size();i>0;--i)
      if (residual[i-1]!=divisor[i-1]) return residual[i-1]>divisor[i-1];
    return true;
  };
  const auto subtractDivisor=[&] {
    std::int64_t borrow=0;
    for (std::size_t i=0;i<residual.size();++i) {
      auto difference=std::int64_t(residual[i])-divisor[i]-borrow;
      borrow=difference<0;
      if (borrow) difference+=static_cast<std::int64_t>(base);
      residual[i]=static_cast<std::uint32_t>(difference);
    }
  };
  constexpr int digits=static_cast<int>(limbs)*digitsPerLimb;
  int position=digits-1;
  while (position>0 && !digit(position)) --position;
  Storage result;
  const int last=keepRemainder?0:-scale-1;
  unsigned roundingDigit=0;
  for (;position>=last;--position) {
    auto carry=static_cast<std::uint64_t>(digit(position));
    for (auto &unit:residual) {
      const auto partial=std::uint64_t(unit)*10+carry;
      unit=static_cast<std::uint32_t>(partial%base);
      carry=partial/base;
    }
    unsigned quotientDigit=0;
    while (atLeastDivisor()) {
      subtractDivisor();
      ++quotientDigit;
    }
    if (!quotientDigit || keepRemainder) continue;
    if (position==last) {
      roundingDigit=quotientDigit;
      continue;
    }
    const auto target=position+maxScale;
    if (target>=digits) return {};
    result.units[target/digitsPerLimb]+=quotientDigit*powers[target%digitsPerLimb];
  }
  if (keepRemainder) {
    std::copy_n(residual.begin(),limbs,result.units.begin());
  } else if (!result.roundAt(maxScale-scale,roundingDigit>=5)) {
    return {};
  }
  result.scale=static_cast<std::uint8_t>(scale);
  result.negative=(keepRemainder?negative:negative!=denominator.negative) && !result.zero();
  return result;
}

std::optional<LocalNumeric> LocalNumeric::dividedBy(const LocalNumeric &right) const {
  const auto &lhs=value(), &rhs=right.value();
  const auto numerator=lhs.leadingDecimalGroup();
  const auto denominator=rhs.leadingDecimalGroup();
  const auto weight=numerator.first-denominator.first-(numerator.second<=denominator.second);
  const auto scale=std::max({int(lhs.scale),int(rhs.scale),16-4*weight});
  if (auto result=lhs.divide(rhs,scale,false)) return LocalNumeric(std::move(*result));
  return {};
}

std::optional<LocalNumeric> LocalNumeric::remainder(const LocalNumeric &right) const {
  const auto &lhs=value(), &rhs=right.value();
  if (auto result=lhs.divide(rhs,std::max(lhs.scale,rhs.scale),true))
    return LocalNumeric(std::move(*result));
  return {};
}

std::optional<LocalNumeric> LocalNumeric::dividedByCount(std::uint32_t count) const {
  if (!count) return {};
  constexpr int fractionalDigits=digitsPerLimb*fractionalLimbs;
  const auto &source=value();
  const auto numerator=source.leadingDecimalGroup();
  const auto denominator=integer(count).leadingDecimalGroup();
  // PostgreSQL targets 16 significant digits using the leading base-10000
  // groups, and retains at least the source display scale.
  const auto quotientWeight=numerator.first-denominator.first-(numerator.second<=denominator.second);
  const auto scale=std::max(static_cast<int>(source.scale),16-4*quotientWeight);
  if (scale>fractionalDigits) return {};

  Storage result;
  result.scale=static_cast<std::uint8_t>(scale);
  std::uint64_t remainder=0;
  for (std::size_t i=source.units.size();i>0;--i) {
    const auto partial=remainder*base+source.units[i-1];
    result.units[i-1]=static_cast<std::uint32_t>(partial/count);
    remainder=partial%count;
  }
  const auto discarded=fractionalDigits-scale;
  const bool round=discarded?result.digit(discarded-1)>=5:remainder*2>=count;
  if (!result.roundAt(discarded,round)) return {};
  result.negative=source.negative && !result.zero();
  return LocalNumeric(std::move(result));
}

bool LocalNumeric::setDisplayScale(std::size_t scale) {
  auto result=value();
  constexpr std::size_t maximum=digitsPerLimb*fractionalLimbs;
  if (scale>maximum) return false;
  for (std::size_t i=0;i<maximum-scale;++i)
    if (result.digit(static_cast<int>(i))) return false;
  if (result.scale==scale) return true;
  result.scale=static_cast<std::uint8_t>(scale);
  replace(std::move(result));
  return true;
}

std::string LocalNumeric::decimal() const {
  const auto &number=value();
  auto highest=number.units.size()-1;
  while (highest>fractionalLimbs && !number.units[highest]) --highest;
  std::string result=number.negative?"-":"";
  result+=std::to_string(number.units[highest]);
  while (highest>fractionalLimbs) appendPadded(result,number.units[--highest]);
  if (number.scale) {
    result+='.';
    const auto start=result.size();
    for (std::size_t i=fractionalLimbs;i>0;--i) appendPadded(result,number.units[i-1]);
    result.resize(start+number.scale);
  }
  return result;
}

std::size_t LocalNumeric::hash() const {
  const auto &number=value();
  std::size_t result=number.negative;
  for (const auto unit:number.units)
    result^=unit+std::size_t{0x9e3779b9u}+(result<<6)+(result>>2);
  return result;
}
} // namespace navidog::sql::optimizer
