#pragma once
#include "catalog.hpp"
namespace navidog {
struct SourceDefinition {
    CatalogNode node;
    QString sql,sqlMode;
    QVector<CatalogNode> parameters;
    bool exists=false;
};
struct SourceDesignResult {
    SourceDefinition definition;
    QString error,refreshError,warning;
    bool applied=false,deleted=false,rolledBack=false,restored=false,partial=false,cancelled=false;
};
inline bool isRoutineObject(const QString& kind) {return kind=="function" || kind=="procedure";}
inline bool isSourceRelation(const QString& kind) {return kind=="view" || kind=="materializedView";}
bool supportsSourceDesign(DatabaseKind,const QString& kind);
QString sourceTemplate(const ConnectionProfile&,const CatalogNode&);
QString rewriteSourceTarget(const QString&,const ConnectionProfile&,const CatalogNode&);
QString sourceCallSql(const ConnectionProfile&,const SourceDefinition&);
QString sourceDropSql(const ConnectionProfile&,const CatalogNode&);
SourceDefinition readSourceDefinition(Database&,const ConnectionProfile&,const CatalogNode&,const Cancellation&);
SourceDesignResult applySourceChanges(const ConnectionProfile&,const SourceDefinition&,const CatalogNode&,const QString&,bool remove,const Cancellation&);
}
Q_DECLARE_METATYPE(navidog::SourceDesignResult)
