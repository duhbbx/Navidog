#include "navidog/data_edit.hpp"
#include <algorithm>
#include <stdexcept>
namespace navidog {
namespace {
using K=DatabaseKind;
bool mysqlEditing(K kind){return kind==K::MySQL || kind==K::MariaDB;}
[[noreturn]] void fail(QString message){throw std::runtime_error(message.toStdString());}
bool isNull(const QVariant& v){return !v.isValid() || v.isNull();}
QString bind(QVector<QVariant>& args,const QVariant& value,K k) {args<<value;return (navidog::usesNativePostgres(k)?"$":k==K::Oracle?":":"?")+QString::number(args.size());}
QString bindColumn(QVector<QVariant>& args,const QVariant& value,K k,const TableColumn& column){
    auto marker=bind(args,value,k);if(k!=K::Oracle || isNull(value))return marker;
    const auto type=column.type.toUpper();
    if(type=="DATE")return "TO_DATE("+marker+", 'YYYY-MM-DD HH24:MI:SS')";
    if(type.startsWith("TIMESTAMP")){if(type.contains("WITH TIME ZONE"))return "TO_TIMESTAMP_TZ("+marker+", 'YYYY-MM-DD HH24:MI:SS.FF9 TZH:TZM')";return "TO_TIMESTAMP("+marker+", 'YYYY-MM-DD HH24:MI:SS.FF9')";}
    return marker;
}
QueryResult check(Database& db,const QString& sql,const QVector<QVariant>& args,const Cancellation& cancel,QueryLimits limits={}) {
    if(cancel->load())fail("操作已停止");auto r=db.executeBound(sql,args,limits,cancel);if(r.cancelled)fail("操作已停止");if(!r.error.isEmpty())fail(r.error);return r;
}
QStringList names(const TableDefinition& d,K k){QStringList out;for(auto c:d.columns)out<<quoteIdentifier(c.name,k);return out;}
QString keyWhere(const TableDefinition& d,const QVector<QVariant>& original,QVector<QVariant>& args,K k) {
    if(original.size()!=d.columns.size())fail("原始行不完整");QStringList clauses;
    for(auto index:primaryKeyColumns(d)){if(isNull(original[index]))fail("主键含 NULL，无法唯一定位记录");clauses<<quoteIdentifier(d.columns[index].name,k)+" = "+bindColumn(args,original[index],k,d.columns[index]);}
    if(clauses.isEmpty())fail("没有主键，无法写入记录");return clauses.join(" AND ");
}
bool equalValue(const QVariant& a,const QVariant& b){if(isNull(a) || isNull(b))return isNull(a)&&isNull(b);return a.userType()==b.userType() && a==b;}
}
bool supportsDataEditing(DatabaseKind kind){return kind==K::SQLite || usesNativePostgres(kind) || mysqlEditing(kind) || kind==K::Oracle;}
QVector<int> primaryKeyColumns(const TableDefinition& d){QVector<int> out;for(int i=0;i<d.columns.size();++i)if(d.columns[i].primaryKey)out<<i;std::stable_sort(out.begin(),out.end(),[&](int a,int b){return d.columns[a].primaryKey<d.columns[b].primaryKey;});return out;}
DataPageResult readDataPage(const ConnectionProfile& p,const DataPageRequest& request,const Cancellation& cancel) {
    DataPageResult out;out.request=request;
    try {
        if(!supportsDataEditing(p.driver))fail("此引擎暂使用只读数据预览");
        if(request.node.kind!="table" && request.node.kind!="view")fail("请选择表或视图");
        if(request.pageSize<1 || request.pageSize>1000 || request.offset<0 || request.offset>1000000000)fail("分页范围无效");
        auto db=connectDatabase(p);out.schema=readTableDefinition(*db,p,request.node,cancel,false);const auto k=p.driver;auto columns=names(out.schema,k);auto keys=primaryKeyColumns(out.schema);QVector<QVariant> args;QStringList predicates;
        const bool versioned=usesNativePostgres(k) && !isDistributedPostgres(k) && request.node.kind=="table";
        auto select=columns;if(versioned)select<<"xmin::text";
        if(request.filter!=DataFilter::None) {
            if(request.filterColumn<0 || request.filterColumn>=columns.size())fail("筛选字段无效");auto col=columns[request.filterColumn];
            switch(request.filter) {
            case DataFilter::Equal:predicates<<col+" = "+bindColumn(args,request.filterValue,k,out.schema.columns[request.filterColumn]);break;
            case DataFilter::NotEqual:predicates<<col+" <> "+bindColumn(args,request.filterValue,k,out.schema.columns[request.filterColumn]);break;
            case DataFilter::Less:predicates<<col+" < "+bindColumn(args,request.filterValue,k,out.schema.columns[request.filterColumn]);break;
            case DataFilter::Greater:predicates<<col+" > "+bindColumn(args,request.filterValue,k,out.schema.columns[request.filterColumn]);break;
            case DataFilter::IsNull:predicates<<col+" IS NULL";break;
            case DataFilter::IsNotNull:predicates<<col+" IS NOT NULL";break;
            case DataFilter::Contains:{auto text=request.filterValue.toString();text.replace("!","!!").replace("%","!%").replace("_","!_");predicates<<"CAST("+col+(mysqlEditing(k)?" AS CHAR) LIKE ":k==K::Oracle?" AS VARCHAR2(4000)) LIKE ":" AS TEXT) LIKE ")+bind(args,"%"+text+"%",k)+" ESCAPE '!'";break;}
            default:fail("筛选方式无效");
            }
        }
        if(request.sortColumn < -1 || request.sortColumn>=columns.size())fail("排序字段无效");
        out.keyset=!keys.isEmpty() && !request.descending && (request.sortColumn==-1 || request.sortColumn==keys[0]) && !request.afterKey.isEmpty();
        if(out.keyset) {
            if(request.afterKey.size()!=keys.size())fail("分页游标无效");QStringList alternatives;
            for(int i=0;i<keys.size();++i) {
                QStringList terms;for(int j=0;j<=i;++j){if(isNull(request.afterKey[j]))fail("分页主键不能为 NULL");terms<<columns[keys[j]]+(j==i?" > ":" = ")+bindColumn(args,request.afterKey[j],k,out.schema.columns[keys[j]]);}alternatives<<"("+terms.join(" AND ")+")";
            }
            predicates<<"("+alternatives.join(" OR ")+")";
        }
        QStringList order;if(request.sortColumn>=0)order<<columns[request.sortColumn]+(request.descending?" DESC":" ASC");
        for(int index:keys)if(index!=request.sortColumn)order<<columns[index]+" ASC";
        auto sql="SELECT "+select.join(", ")+" FROM "+tableQualifiedName(p,request.node);
        if(!predicates.isEmpty())sql+=" WHERE "+predicates.join(" AND ");if(!order.isEmpty())sql+=" ORDER BY "+order.join(", ");
        bool oracleOffset=false;
        if(k==K::Oracle){
            const auto offset=out.keyset?0:request.offset;auto upper=bind(args,offset+request.pageSize+1,k);
            if(offset){QString alias="__NAVIDOG_PAGE_ROW";while(columns.contains(quoteIdentifier(alias,k)))alias+="_";alias=quoteIdentifier(alias,k);sql="SELECT * FROM (SELECT q.*, ROWNUM "+alias+" FROM ("+sql+") q WHERE ROWNUM <= "+upper+") WHERE "+alias+" > "+bind(args,offset,k);oracleOffset=true;}
            else sql="SELECT * FROM ("+sql+") WHERE ROWNUM <= "+upper;
        }else{sql+=" LIMIT "+bind(args,request.pageSize+1,k);if(!out.keyset && request.offset)sql+=" OFFSET "+bind(args,request.offset,k);}
        QueryLimits limits;limits.maxRows=request.pageSize+1;out.result=check(*db,sql,args,cancel,limits);if(oracleOffset){out.result.columns.removeLast();if(!out.result.columnTypes.isEmpty())out.result.columnTypes.removeLast();for(auto& row:out.result.rows)row.removeLast();}
        out.hasMore=out.result.rows.size()>request.pageSize || out.result.truncated;
        if(out.result.rows.size()>request.pageSize)out.result.rows.removeLast();
        if(versioned){out.result.columns.removeLast();if(!out.result.columnTypes.isEmpty())out.result.columnTypes.removeLast();for(auto& row:out.result.rows){out.versions<<row.takeLast().toString();}}
        else out.versions.resize(out.result.rows.size());
        bool transactional=true;if(mysqlEditing(k) && request.node.kind=="table"){auto engine=db->executeBound("SELECT engine FROM information_schema.tables WHERE table_schema=?1 AND table_name=?2",{request.node.database.isEmpty()?p.database:request.node.database,request.node.object},{},cancel);transactional=engine.rows.size()==1 && engine.rows[0][0].toString().compare("InnoDB",Qt::CaseInsensitive)==0;}
        out.editable=transactional && !p.http.enabled && !p.readOnly && request.node.kind=="table" && !keys.isEmpty() && !out.result.cellTruncated && !out.result.truncated;
        if(!transactional)out.readOnlyReason="该表引擎不支持本工作台的事务编辑（仅支持 InnoDB）";else if(p.http.enabled)out.readOnlyReason="HTTP 隧道仅支持读取；数据编辑请使用直连或 SSH";else if(p.readOnly)out.readOnlyReason="只读连接";else if(request.node.kind=="view")out.readOnlyReason="视图仅供查看";else if(keys.isEmpty())out.readOnlyReason="未找到主键，数据只读";else if(out.result.cellTruncated || out.result.truncated)out.readOnlyReason="结果有截断，数据只读；请缩小每页行数";
        for(const auto& row:out.result.rows)for(int key:keys)if(isNull(row[key])){out.editable=false;out.readOnlyReason="主键含 NULL，当前页只读";}
    }catch(const std::exception& e){out.error=QString::fromUtf8(e.what());out.cancelled=cancel->load();}
    return out;
}
DataCommitResult commitDataChanges(const ConnectionProfile& p,const TableDefinition& schema,const QVector<DataRowChange>& changes,const Cancellation& cancel) {
    DataCommitResult out;std::unique_ptr<Database> db;bool transaction=false,committing=false;
    try {
        if(p.http.enabled)fail("HTTP 隧道不支持跨请求事务，请使用直连或 SSH 提交编辑");
        if(p.readOnly || !supportsDataEditing(p.driver) || schema.node.kind!="table" || primaryKeyColumns(schema).isEmpty())fail("当前表不允许数据写入");
        if(changes.size()>1000)fail("每次最多提交 1000 行");if(changes.isEmpty())return out;
        db=connectDatabase(p);if(mysqlEditing(p.driver)){check(*db,"SET SESSION innodb_lock_wait_timeout=5",{},cancel);check(*db,"SET TRANSACTION ISOLATION LEVEL READ COMMITTED",{},cancel);}if(p.driver==K::Oracle){db->setAutoCommit(false);check(*db,"SET TRANSACTION ISOLATION LEVEL READ COMMITTED",{},cancel);transaction=true;check(*db,"LOCK TABLE "+tableQualifiedName(p,schema.node)+" IN ROW EXCLUSIVE MODE NOWAIT",{},cancel);}else{check(*db,p.driver==K::SQLite?"BEGIN IMMEDIATE":mysqlEditing(p.driver)?"START TRANSACTION":isDistributedPostgres(p.driver)?"BEGIN":"BEGIN ISOLATION LEVEL READ COMMITTED",{},cancel);transaction=true;}
        if(mysqlEditing(p.driver)){check(*db,"SELECT 1 FROM "+tableQualifiedName(p,schema.node)+" LIMIT 0",{},cancel);auto engine=check(*db,"SELECT engine FROM information_schema.tables WHERE table_schema=?1 AND table_name=?2",{schema.node.database.isEmpty()?p.database:schema.node.database,schema.node.object},cancel);if(engine.rows.size()!=1 || engine.rows[0][0].toString().compare("InnoDB",Qt::CaseInsensitive)!=0)fail("仅允许向 InnoDB 表提交事务编辑");}
        if(readTableDefinition(*db,p,schema.node,cancel,false).columns!=schema.columns)fail("表结构已改变，请重新加载后编辑");
        const auto cols=names(schema,p.driver);const auto table=tableQualifiedName(p,schema.node);qint64 bytes=0;
        const bool versioned = usesNativePostgres(p.driver) && !isDistributedPostgres(p.driver);
        for(auto change:changes) {
            if(change.state==DataRowState::Clean)continue;
            if(change.values.size()!=schema.columns.size())fail("待提交行不完整");
            for(auto value:change.values){auto size=value.userType()==QMetaType::QByteArray?value.toByteArray().size():value.toString().size()*2;bytes+=size;if(size>131072 || bytes>32*1024*1024)fail("编辑内容超出提交上限");}
            auto touched=change.touched.values();std::sort(touched.begin(),touched.end());
            for(int index:touched)if(index<0 || index>=schema.columns.size() || (!schema.columns[index].protectedReason.isEmpty() || !schema.columns[index].generatedExpression.isEmpty()))fail("不能修改生成 / 标识字段或无效字段");
            if(change.state!=DataRowState::Inserted) {
                QVector<QVariant> args;auto where=keyWhere(schema,change.original,args,p.driver);auto sql="SELECT "+(versioned?QString("xmin::text"):cols.join(", "))+" FROM "+table+" WHERE "+where+(p.driver==K::Oracle?" FOR UPDATE NOWAIT":navidog::usesNativePostgres(p.driver) || mysqlEditing(p.driver)?" FOR UPDATE":"");
                QueryLimits limit;limit.maxRows=2;auto current=check(*db,sql,args,cancel,limit);
                if(current.truncated || current.cellTruncated || current.rows.size()!=1)fail("记录已被删除或无法唯一定位；本批修改未提交");
                if(versioned){if(change.version.isEmpty() || current.rows[0][0].toString()!=change.version)fail("记录已被其他会话修改；请重新加载后核对");}
                else {if(current.rows[0].size()!=change.original.size())fail("原始行不完整");for(int i=0;i<change.original.size();++i)if(!equalValue(current.rows[0][i],change.original[i]))fail("记录已被其他会话修改；请重新加载后核对");}
            }
            QVector<QVariant> args;QString sql;
            if(change.state==DataRowState::Inserted) {
                QStringList fields,values;for(int index:touched){fields<<cols[index];values<<bindColumn(args,change.values[index],p.driver,schema.columns[index]);}
                if(fields.isEmpty() && p.driver==K::Oracle){for(int i=0;i<schema.columns.size();++i)if(schema.columns[i].protectedReason.isEmpty()){fields<<cols[i];values<<"DEFAULT";break;}if(fields.isEmpty())fail("请至少填写一个普通字段后再插入 Oracle 记录");}
                sql="INSERT INTO "+table+(fields.isEmpty()?(mysqlEditing(p.driver)?" () VALUES ()":" DEFAULT VALUES"):" ("+fields.join(", ")+") VALUES ("+values.join(", ")+")");
            } else if(change.state==DataRowState::Modified) {
                if(touched.isEmpty())continue;QStringList fields;for(int index:touched)fields<<cols[index]+" = "+bindColumn(args,change.values[index],p.driver,schema.columns[index]);
                sql="UPDATE "+table+" SET "+fields.join(", ")+" WHERE "+keyWhere(schema,change.original,args,p.driver);
            } else if(change.state==DataRowState::Deleted)sql="DELETE FROM "+table+" WHERE "+keyWhere(schema,change.original,args,p.driver);
            else fail("行状态无效");
            auto result=check(*db,sql,args,cancel);if(result.affectedRows!=1)fail("写入影响行数不是 1；本批修改已取消");
            if(change.state==DataRowState::Inserted)++out.inserted;else if(change.state==DataRowState::Modified)++out.updated;else ++out.deleted;
        }
        committing=true;check(*db,"COMMIT",{},cancel);transaction=false;
    } catch(const std::exception& e) {
        out.error=QString::fromUtf8(e.what());out.cancelled=cancel->load();
        if(transaction && db){try{check(*db,"ROLLBACK",{},cancellation());out.rolledBack=true;}catch(...){db.reset();if(!committing)out.rolledBack=true;}}
        out.commitUnknown=committing && !out.rolledBack;out.inserted=out.updated=out.deleted=0;
    }
    return out;
}
}
