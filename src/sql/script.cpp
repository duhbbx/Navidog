#include "navidog/sql.hpp"
#include <algorithm>
#include <cctype>
#include <stdexcept>
namespace navidog::sql {
namespace {
std::string upper(std::string_view text){std::string out(text);std::transform(out.begin(),out.end(),out.begin(),[](unsigned char c){return std::toupper(c);});return out;}
bool white(std::string_view text){return std::all_of(text.begin(),text.end(),[](unsigned char c){return std::isspace(c);});}
std::vector<std::string> prefix(std::string_view source,Dialect dialect){auto parsed=lex(source,dialect);std::vector<std::string> words;for(auto token:parsed.tokens){if(token.kind==TokenKind::Comment)continue;words.push_back(upper(source.substr(token.begin,token.end-token.begin)));if(words.size()==12)break;}return words;}
}
bool isCompoundDefinition(std::string_view source,Dialect dialect) {
    const auto words = prefix(source, dialect);
    if (words.empty()) return false;
    const auto& operation = words.front();
    if (dialect == Dialect::Oracle && (operation == "BEGIN" || operation == "DECLARE")) return true;
    const bool alterMySqlEvent = dialect == Dialect::MySQL && operation == "ALTER";
    if (operation != "CREATE" && !(dialect == Dialect::SQLServer && operation == "ALTER") && !alterMySqlEvent)
        return false;
    for (std::size_t i = 1; i < words.size(); ++i) {
        const auto& kind = words[i];
        if (dialect == Dialect::MySQL && kind == "EVENT") return true;
        if (kind == "PROCEDURE" || (dialect == Dialect::SQLServer && kind == "PROC") ||
            kind == "FUNCTION" || kind == "TRIGGER" ||
            (dialect == Dialect::Oracle && (kind == "PACKAGE" || kind == "TYPE"))) return !alterMySqlEvent;
        if (kind == "TABLE" || kind == "VIEW" || kind == "INDEX" || kind == "DATABASE" || kind == "SCHEMA")
            return false;
    }
    return false;
}
bool isPackageSpecification(std::string_view source,Dialect dialect) {
    if(dialect!=Dialect::Oracle)return false;
    const auto words=prefix(source,dialect);
    if(words.size()<3 || words.front()!="CREATE")return false;
    std::size_t next=1;
    if(words.size()>3 && words[1]=="OR" && words[2]=="REPLACE")next+=2;
    if(next<words.size() && (words[next]=="EDITIONABLE" || words[next]=="NONEDITIONABLE"))++next;
    return words.size()>next+1 && words[next]=="PACKAGE" && words[next+1]!="BODY";
}
std::string_view statementSource(std::string_view source,StatementSpan span,Dialect dialect){
    if(span.begin>span.end || span.end>source.size())throw std::invalid_argument("invalid SQL statement span");
    auto end=span.end;
    if(dialect==Dialect::SQLServer && end<source.size() && source[end]==';')++end;
    return source.substr(span.begin,end-span.begin);
}
std::vector<StatementSpan> splitStatements(std::string_view source,Dialect dialect){
    if(source.size()>4*1024*1024)throw std::runtime_error("SQL 脚本超过 4 MB");auto parsed=lex(source,dialect);if(!parsed.diagnostics.empty())throw std::runtime_error(parsed.diagnostics.front().message);
    std::vector<StatementSpan> result;std::size_t start=0,index=0;std::string delimiter=";";bool leading=true;int triggerDepth=0;bool triggerBody=false,sqliteTrigger=false;
    auto content=[&](std::size_t a,std::size_t b){auto part=lex(source.substr(a,b-a),dialect);return std::any_of(part.tokens.begin(),part.tokens.end(),[](auto token){return token.kind!=TokenKind::Comment;});};
    auto append=[&](std::size_t end){if(content(start,end)){if(result.size()>=200)throw std::runtime_error("每次最多运行 200 条 SQL");result.push_back({start,end,isCompoundDefinition(source.substr(start,end-start),dialect)});}triggerDepth=0;triggerBody=false;sqliteTrigger=false;};
    for(std::size_t i=0;i<source.size();){
        bool atLine=leading;if(source[i]=='\n')leading=true;else if(!std::isspace(static_cast<unsigned char>(source[i])))leading=false;
        while(index<parsed.tokens.size() && parsed.tokens[index].end<=i)++index;
        if(index<parsed.tokens.size() && parsed.tokens[index].begin==i){auto token=parsed.tokens[index];if(token.kind==TokenKind::Comment || token.kind==TokenKind::String || token.kind==TokenKind::Identifier){i=token.end;continue;}}
        if(atLine && (source[i]=='/' || source[i]=='D' || source[i]=='d' || source[i]=='G' || source[i]=='g')){auto lineEnd=source.find('\n',i);if(lineEnd==std::string_view::npos)lineEnd=source.size();auto line=source.substr(i,lineEnd-i);auto word=upper(line);
            if(dialect==Dialect::MySQL && word.rfind("DELIMITER",0)==0 && line.size()>9 && std::isspace(static_cast<unsigned char>(line[9]))){if(content(start,i))throw std::runtime_error("DELIMITER 前的 SQL 缺少结束符");auto value=line.substr(10);while(!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))value.remove_prefix(1);while(!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))value.remove_suffix(1);if(value.empty() || value.size()>16 || value.find_first_of(" \t\r\n'\"`")!=std::string_view::npos)throw std::runtime_error("DELIMITER 结束符无效");delimiter=std::string(value);start=lineEnd;i=lineEnd;continue;}
            bool slash=dialect==Dialect::Oracle && !line.empty() && line[0]=='/' && white(line.substr(1));bool go=dialect==Dialect::SQLServer && word.size()>=2 && word.substr(0,2)=="GO" && white(line.substr(2));if(slash || go){append(i);start=lineEnd;i=lineEnd;continue;}
        }
        auto part=source.substr(start,i-start+1);
        if(dialect==Dialect::SQLite && !sqliteTrigger && index<parsed.tokens.size() && parsed.tokens[index].begin==i && parsed.tokens[index].kind==TokenKind::Word){auto token=parsed.tokens[index];if(upper(source.substr(token.begin,token.end-token.begin))=="TRIGGER")sqliteTrigger=isCompoundDefinition(source.substr(start,token.end-start),dialect);}
        if(sqliteTrigger && index<parsed.tokens.size() && parsed.tokens[index].begin==i && parsed.tokens[index].kind==TokenKind::Word){auto token=parsed.tokens[index];auto word=upper(source.substr(token.begin,token.end-token.begin));if(word=="BEGIN"){++triggerDepth;triggerBody=true;}else if(word=="CASE" && triggerBody)++triggerDepth;else if(word=="END" && triggerBody && triggerDepth>0)--triggerDepth;}
        if(source.substr(i,delimiter.size())==delimiter){
            bool compound=isCompoundDefinition(part,dialect);
            if((dialect==Dialect::Oracle || dialect==Dialect::SQLServer) && compound){i+=delimiter.size();continue;}
            if(sqliteTrigger && triggerDepth>0){i+=delimiter.size();continue;}
            if(dialect==Dialect::MySQL && delimiter==";" && compound){auto prefixWords=lex(part,dialect).tokens;bool begin=false;for(auto token:prefixWords)if(token.kind==TokenKind::Word && upper(part.substr(token.begin,token.end-token.begin))=="BEGIN")begin=true;if(begin)throw std::runtime_error("MySQL 复合定义请使用 DELIMITER，例如 DELIMITER $$ … END$$ 再恢复 DELIMITER ;");}
            append(i);i+=delimiter.size();start=i;continue;
        }++i;
    }append(source.size());if(result.empty())throw std::runtime_error("请先输入 SQL");return result;
}
}
