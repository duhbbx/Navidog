#pragma once
#include "navidog/table_design.hpp"
#include <QAbstractTableModel>
class TableObjectModel final:public QAbstractTableModel {
public:
    navidog::TableObjectKind kind;
    navidog::DatabaseKind driver;
    QVector<navidog::TableObject> objects;
    TableObjectModel(navidog::TableObjectKind k,navidog::DatabaseKind d,QObject* p=nullptr):QAbstractTableModel(p),kind(k),driver(d){}
    int rowCount(const QModelIndex& p={})const override{return p.isValid()?0:objects.size();}
    int columnCount(const QModelIndex& p={})const override;
    QVariant data(const QModelIndex&,int role)const override;
    QVariant headerData(int,Qt::Orientation,int)const override;
    Qt::ItemFlags flags(const QModelIndex&)const override;
    bool setData(const QModelIndex&,const QVariant&,int role=Qt::EditRole)override;
    void resetObjects(const QVector<navidog::TableObject>&);
    void append(const navidog::TableObject&);
    void remove(int);
    QStringList choices(int column)const;
};
