#include "result_grid.hpp"
#include "components.hpp"
#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSet>
#include <QShortcut>
#include <QTableView>
#include <QTabWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <stdexcept>
namespace navidog::ui {
namespace {
constexpr qint64 maxCopyCells=10000,maxCopyChars=2*1024*1024;
QString valueText(const QVariant& value){
    if(!value.isValid() || value.isNull())return "NULL";
    return value.userType()==QMetaType::QByteArray?QString::fromLatin1(value.toByteArray().toHex()):value.toString();
}
QString tsvField(QString value){
    if(value.contains('\t') || value.contains('\n') || value.contains('\r') || value.contains('"')){value.replace('"',"\"\"");return '"'+value+'"';}return value;
}
void copySelection(QTableView* grid,bool headers){
    try{auto text=resultSelectionText(grid,headers);if(!text.isNull())QApplication::clipboard()->setText(text);}
    catch(const std::exception& e){QMessageBox::information(grid,"选择范围过大",QString::fromUtf8(e.what()));}
}
void inspectCell(QTableView* grid,const QModelIndex& index){
    if(!index.isValid())return;
    const auto value=index.data(Qt::UserRole);const auto name=grid->model()->headerData(index.column(),Qt::Horizontal,Qt::UserRole).toString();
    Dialog dialog(grid);dialog.setObjectName("resultCellDialog");dialog.setWindowTitle(name+QString(" · 第 %1 行").arg(index.row()+1));dialog.resize(760,530);
    auto* layout=new QVBoxLayout(&dialog);auto* info=new QLabel;info->setTextFormat(Qt::PlainText);
    const bool binary=value.userType()==QMetaType::QByteArray,isNull=!value.isValid() || value.isNull();
    info->setText(isNull?"NULL":binary?QString("二进制 · %1 字节").arg(value.toByteArray().size()):QString("文本 · %1 字符").arg(value.toString().size()));layout->addWidget(info);
    auto* tabs=new QTabWidget;layout->addWidget(tabs,1);
    auto add=[&](QString title,QString text,QString object){auto* editor=new QPlainTextEdit;editor->setObjectName(object);editor->setReadOnly(true);editor->setLineWrapMode(QPlainTextEdit::NoWrap);editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));editor->setPlainText(text);tabs->addTab(editor,title);};
    add(binary?"十六进制":"文本",binary?QString::fromLatin1(value.toByteArray().toHex(' ')):valueText(value),"resultCellText");
    if(!isNull && !binary){QJsonParseError error;const auto json=QJsonDocument::fromJson(value.toString().toUtf8(),&error);if(error.error==QJsonParseError::NoError && !json.isNull())add("JSON",QString::fromUtf8(json.toJson(QJsonDocument::Indented)),"resultCellJson");}
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Close);auto* copy=buttons->addButton("复制原值",QDialogButtonBox::ActionRole);QObject::connect(copy,&QPushButton::clicked,&dialog,[value]{QApplication::clipboard()->setText(valueText(value));});QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);layout->addWidget(buttons);dialog.exec();
}
}
QString resultSelectionText(QTableView* grid,bool includeHeaders,int valueRole){
    if(!grid || !grid->model() || !grid->selectionModel())return {};
    qint64 count=0;for(const auto& range:grid->selectionModel()->selection()){count+=qint64(range.height())*range.width();if(count>maxCopyCells)throw std::runtime_error("最多复制 10000 个单元格，请缩小选择范围或使用导出。");}
    auto indexes=grid->selectionModel()->selectedIndexes();if(indexes.isEmpty() && grid->currentIndex().isValid())indexes<<grid->currentIndex();if(indexes.isEmpty())return {};
    std::sort(indexes.begin(),indexes.end(),[](auto a,auto b){return a.row()==b.row()?a.column()<b.column():a.row()<b.row();});
    QSet<int> selectedColumns,selectedRows;for(auto index:indexes){selectedColumns.insert(index.column());selectedRows.insert(index.row());}
    if(qint64(selectedRows.size())*selectedColumns.size()>maxCopyCells)throw std::runtime_error("不连续选择展开后超过 10000 个单元格，请缩小选择范围或使用导出。");
    auto columns=selectedColumns.values();std::sort(columns.begin(),columns.end(),[grid](int a,int b){return grid->horizontalHeader()->visualIndex(a)<grid->horizontalHeader()->visualIndex(b);});QString text="";
    auto append=[&](const QString& value){if(text.size()+value.size()>maxCopyChars)throw std::runtime_error("复制内容超过 4 MiB，请缩小选择范围或使用导出。");text+=value;};
    if(includeHeaders){bool first=true;for(auto column:columns){if(!first)append("\t");append(tsvField(grid->model()->headerData(column,Qt::Horizontal,Qt::UserRole).toString()));first=false;}append("\n");}
    int position=0;bool firstRow=true;
    while(position<indexes.size()){
        if(!firstRow)append("\n");firstRow=false;const int row=indexes[position].row();bool firstColumn=true;QHash<int,QModelIndex> cells;
        while(position<indexes.size() && indexes[position].row()==row){cells.insert(indexes[position].column(),indexes[position]);++position;}
        for(auto column:columns){if(!firstColumn)append("\t");firstColumn=false;
            if(cells.contains(column))append(tsvField(valueText(cells[column].data(valueRole))));
        }
    }
    return text;
}
void configureResultGrid(ObjectGrid* grid){
    grid->setAlternatingRowColors(true);grid->setSelectionBehavior(QAbstractItemView::SelectItems);grid->setSelectionMode(QAbstractItemView::ExtendedSelection);grid->setEditTriggers(QAbstractItemView::NoEditTriggers);grid->setWordWrap(false);
    grid->verticalHeader()->setDefaultSectionSize(24);grid->verticalHeader()->setMinimumWidth(38);auto* header=grid->horizontalHeader();header->setSectionResizeMode(QHeaderView::Interactive);header->setDefaultSectionSize(155);header->setStretchLastSection(false);header->setDefaultAlignment(Qt::AlignLeft|Qt::AlignVCenter);header->setSectionsMovable(true);
    auto* copy=new QShortcut(QKeySequence::Copy,grid);copy->setContext(Qt::WidgetWithChildrenShortcut);QObject::connect(copy,&QShortcut::activated,grid,[grid]{copySelection(grid,false);});
    auto* inspect=new QShortcut(QKeySequence(Qt::Key_F2),grid);inspect->setContext(Qt::WidgetWithChildrenShortcut);QObject::connect(inspect,&QShortcut::activated,grid,[grid]{inspectCell(grid,grid->currentIndex());});
    QObject::connect(grid,&QTableView::doubleClicked,grid,[grid](const QModelIndex& index) {
        grid->afterDoubleClick(index,[grid](const QModelIndex& activated) { inspectCell(grid,activated); });
    });
    grid->setContextMenuPolicy(Qt::CustomContextMenu);QObject::connect(grid,&QWidget::customContextMenuRequested,grid,[grid](QPoint point){
        auto index=grid->indexAt(point);if(!index.isValid())return;if(!grid->selectionModel()->isSelected(index))grid->setCurrentIndex(index);
        ContextMenu menu(grid);menu.addAction("查看单元格",grid,[grid,index]{inspectCell(grid,index);})->setShortcut(QKeySequence(Qt::Key_F2));menu.addSeparator();menu.addAction("复制",grid,[grid]{copySelection(grid,false);})->setShortcut(QKeySequence::Copy);menu.addAction("复制并包含列名",grid,[grid]{copySelection(grid,true);});menu.addSeparator();menu.addAction("全选",grid,&QTableView::selectAll);menu.execAt(grid->viewport()->mapToGlobal(point),grid);
    });
}
}
