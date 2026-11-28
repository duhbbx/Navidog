#include "table_object_model.hpp"
#include <QColor>
using O=navidog::TableObjectKind;using K=navidog::DatabaseKind;
int TableObjectModel::columnCount(const QModelIndex& p)const {if(p.isValid())return 0;return kind==O::Index?7:kind==O::ForeignKey?10:3;}
QVariant TableObjectModel::headerData(int n,Qt::Orientation orientation,int role)const{if(role!=Qt::DisplayRole)return {};if(orientation==Qt::Vertical)return n+1;
    if(kind==O::Index)return QStringList{"名称","字段 / 表达式","唯一","索引方法","筛选条件","注释","完整定义（高级）"}.value(n);
    if(kind==O::ForeignKey)return QStringList{"名称","字段","引用数据库 / Schema","引用表","引用字段","删除时","更新时","可延迟","初始延迟","启用 / 验证"}.value(n);
    if(kind==O::Check)return QStringList{"名称","表达式","启用 / 验证"}.value(n);
    return QStringList{"名称","定义摘要","启用"}.value(n);
}
QVariant TableObjectModel::data(const QModelIndex& i,int role)const{if(!i.isValid() || i.row()>=objects.size())return {};const auto& o=objects[i.row()];int c=i.column();if(role==Qt::ToolTipRole)return o.protectedReason.isEmpty()?o.sql:o.protectedReason;
    if(role==Qt::ForegroundRole && !o.protectedReason.isEmpty())return QColor("#8a7c66");
    if(role==Qt::CheckStateRole){if(kind==O::Index && c==2)return o.unique?Qt::Checked:Qt::Unchecked;if(kind==O::ForeignKey && c>=7)return (c==7?o.deferrable:c==8?o.initiallyDeferred:o.enabled)?Qt::Checked:Qt::Unchecked;if((kind==O::Check || kind==O::Trigger) && c==2)return o.enabled?Qt::Checked:Qt::Unchecked;}
    if(role!=Qt::DisplayRole && role!=Qt::EditRole)return {};if(c==0)return o.name;
    if(kind==O::Index){if(c==1)return o.expression;if(c==3)return o.method;if(c==4)return o.predicate;if(c==5)return o.comment;if(c==6)return o.sql;}
    if(kind==O::ForeignKey){if(c==1)return navidog::formatIdentifierList(o.columns,driver);if(c==2)return o.referenceSchema;if(c==3)return o.referenceTable;if(c==4)return navidog::formatIdentifierList(o.referenceColumns,driver);if(c==5)return o.onDelete;if(c==6)return o.onUpdate;}
    if(kind==O::Check && c==1)return o.expression;if(kind==O::Trigger && c==1)return o.sql.simplified();return {};
}
Qt::ItemFlags TableObjectModel::flags(const QModelIndex& i)const {auto f=QAbstractTableModel::flags(i);if(!i.isValid() || i.row()>=objects.size())return f;auto& o=objects[i.row()];int c=i.column();if(!o.protectedReason.isEmpty())return f;
    if(kind==O::Index){if(!o.sql.isEmpty() && c!=0 && c!=6)return f;if(c==2)return o.constraint?f:f|Qt::ItemIsUserCheckable;if(c==3 && o.constraint)return f;if(c==4 && driver!=K::SQLite && driver!=K::PostgreSQL && driver!=K::SQLServer)return f;if(c==5 && !navidog::isMySqlFamily(driver) && driver!=K::PostgreSQL)return f;}
    if(kind==O::ForeignKey && c>=7){if(c<9 && driver!=K::SQLite && driver!=K::PostgreSQL && driver!=K::Oracle)return f;if(c==9 && (driver==K::SQLite || navidog::isMySqlFamily(driver)))return f;return f|Qt::ItemIsUserCheckable;}
    if(kind==O::ForeignKey && c==6 && driver==K::Oracle)return f;
    if((kind==O::Check || kind==O::Trigger) && c==2){if(driver==K::SQLite || (kind==O::Trigger && (navidog::isMySqlFamily(driver))))return f;return f|Qt::ItemIsUserCheckable;}
    if(kind==O::Trigger && c==1)return f;
    return f|Qt::ItemIsEditable;
}
bool TableObjectModel::setData(const QModelIndex& i,const QVariant& v,int role){if(!i.isValid() || i.row()>=objects.size())return false;auto f=flags(i);int c=i.column();auto& o=objects[i.row()];if(role==Qt::CheckStateRole){if(!(f&Qt::ItemIsUserCheckable))return false;bool checked=v.toInt()==Qt::Checked;if(kind==O::Index)o.unique=checked;else if(kind==O::ForeignKey && c==7)o.deferrable=checked;else if(kind==O::ForeignKey && c==8){o.initiallyDeferred=checked;if(checked)o.deferrable=true;}else o.enabled=checked;
    }else if(role==Qt::EditRole){if(!(f&Qt::ItemIsEditable))return false;auto text=v.toString();try{if(c==0)o.name=text.trimmed();else if(kind==O::Index){if(c==1)o.expression=text.trimmed();if(c==3)o.method=text.trimmed();if(c==4)o.predicate=text.trimmed();if(c==5)o.comment=text;if(c==6)o.sql=text.trimmed();}
        else if(kind==O::ForeignKey){if(c==1)o.columns=navidog::parseIdentifierList(text);if(c==2)o.referenceSchema=text.trimmed();if(c==3)o.referenceTable=text.trimmed();if(c==4)o.referenceColumns=navidog::parseIdentifierList(text);if(c==5)o.onDelete=text.trimmed();if(c==6)o.onUpdate=text.trimmed();o.sql.clear();}
        else if(kind==O::Check){if(c==1)o.expression=text.trimmed();o.sql.clear();}
    }catch(const std::exception&){return false;}}else return false;
    if(kind==O::Check || kind==O::ForeignKey || (o.constraint && c!=6))o.sql.clear();emit dataChanged(index(i.row(),0),index(i.row(),columnCount()-1));return true;
}
void TableObjectModel::resetObjects(const QVector<navidog::TableObject>& values){beginResetModel();objects.clear();for(auto& o:values)if(o.kind==kind)objects<<o;endResetModel();}
void TableObjectModel::append(const navidog::TableObject& o){if(objects.size()>=512)return;auto r=objects.size();beginInsertRows({},r,r);objects<<o;endInsertRows();}
void TableObjectModel::remove(int r){if(r<0 || r>=objects.size())return;beginRemoveRows({},r,r);objects.removeAt(r);endRemoveRows();}
QStringList TableObjectModel::choices(int c)const {if(kind==O::ForeignKey && (c==5 || c==6)){if(driver==K::Oracle)return {"NO ACTION","CASCADE","SET NULL"};if(driver==K::SQLServer)return {"NO ACTION","CASCADE","SET NULL","SET DEFAULT"};if(navidog::isMySqlFamily(driver))return {"NO ACTION","RESTRICT","CASCADE","SET NULL"};return {"NO ACTION","RESTRICT","CASCADE","SET NULL","SET DEFAULT"};}
    if(kind==O::Index && c==3){if(navidog::isMySqlFamily(driver))return {"BTREE","HASH","FULLTEXT","SPATIAL"};if(driver==K::PostgreSQL)return {"btree","hash","gin","gist","spgist","brin"};if(driver==K::SQLServer)return {"NONCLUSTERED","CLUSTERED"};}return {};
}
