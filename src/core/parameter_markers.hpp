#pragma once
#include "navidog/sql.hpp"
#include <QByteArray>
#include <QVariant>
#include <QVector>
#include <stdexcept>
namespace navidog {
struct PositionalParameters {QByteArray sql;QVector<QVariant> values;};
inline PositionalParameters mysqlParameters(const QString& query,const QVector<QVariant>& parameters){auto input=query.toUtf8();auto parsed=sql::lex(input.toStdString(),sql::Dialect::MySQL);PositionalParameters out;int previous=0,sequential=0;for(std::size_t i=0;i<parsed.tokens.size();++i){auto t=parsed.tokens[i];if(t.kind!=sql::TokenKind::Symbol || input[int(t.begin)]!='?')continue;int end=t.end;while(end<input.size() && input[end]>='0' && input[end]<='9')++end;bool numbered=false;int parameter=input.mid(t.end,end-t.end).toInt(&numbered);if(!numbered)parameter=++sequential;if(parameter<1 || parameter>parameters.size())throw std::runtime_error("绑定参数数量不匹配");out.sql+=input.mid(previous,t.begin-previous);out.sql+='?';previous=end;out.values<<parameters[parameter-1];while(i+1<parsed.tokens.size() && parsed.tokens[i+1].begin<std::size_t(end))++i;}out.sql+=input.mid(previous);if(out.values.size()!=parameters.size() && out.values.isEmpty())throw std::runtime_error("绑定参数数量不匹配");return out;}
}
