#include "table_report_dialog.hpp"
#include "result_model.hpp"
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsRectItem>
#include <QGraphicsSimpleTextItem>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QMessageBox>
#include <QSaveFile>
#include <QTableView>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QSet>
#include <cmath>

namespace {
QString xml(QString value){value.replace('&',"&amp;");value.replace('<',"&lt;");value.replace('>',"&gt;");value.replace('"',"&quot;");return value;}
class DistributionChart final : public QWidget {
    navidog::QueryResult values_;
public:
    explicit DistributionChart(const navidog::QueryResult& values,QWidget* parent=nullptr):QWidget(parent),values_(values){setMinimumHeight(320);}
    void paintEvent(QPaintEvent*)override {
        QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);painter.fillRect(rect(),Qt::white);if(values_.rows.isEmpty()){painter.drawText(rect(),Qt::AlignCenter,"没有可显示的分组");return;}
        double maximum=1;for(auto row:values_.rows)maximum=std::max(maximum,row.value(1).toDouble());
        const auto step=double(height()-30)/values_.rows.size();const int left=190,right=60;const auto available=std::max(1,width()-left-right);
        for(int i=0;i<values_.rows.size();++i){const auto& row=values_.rows[i];const auto y=15+i*step;auto label=row[0].isValid()?row[0].toString():"NULL";painter.setPen(QColor("#344255"));painter.drawText(QRectF(8,y,170,step),Qt::AlignVCenter|Qt::AlignRight,fontMetrics().elidedText(label,Qt::ElideRight,170));painter.setBrush(QColor("#2e83c5"));painter.setPen(Qt::NoPen);const double size=available*row.value(1).toDouble()/maximum;painter.drawRoundedRect(QRectF(left,y+step*.2,size,step*.6),2,2);painter.setPen(QColor("#344255"));painter.drawText(QRectF(left+size+6,y,50,step),Qt::AlignVCenter,row.value(1).toString());}
    }
};
}
TableReportDialog::TableReportDialog(const navidog::TableTask& task,const navidog::TableTaskResult& result,QWidget* parent,navidog::ui::ExportContext context):Dialog(parent) {
    setAttribute(Qt::WA_DeleteOnClose);setObjectName("tableReportDialog");resize(1000,720);const bool model=task.action==navidog::TableAction::ReverseModel;
    setWindowTitle((model?"结构模型 · ":"BI 分布 · ")+task.node.object);auto* layout=new QVBoxLayout(this);auto* tools=new QHBoxLayout;auto* title=new QLabel(model?"只读结构模型 · 字段、索引与外键引用":"本地 BI · 按 "+task.dimension+" 分组的记录数（前 20 组）");title->setTextFormat(Qt::PlainText);tools->addWidget(title);tools->addStretch();layout->addLayout(tools);
    if(model) {
        auto* view=new QGraphicsView;view->setObjectName("tableModelView");auto* scene=new QGraphicsScene(view);view->setScene(scene);view->setRenderHint(QPainter::Antialiasing);view->setDragMode(QGraphicsView::ScrollHandDrag);layout->addWidget(view,1);
        const auto doc=result.document;auto columns=doc.value("columns").toArray();QStringList lines;lines<<doc.value("table").toString();for(auto value:columns){auto c=value.toObject();lines<<(c.value("pk").toInt()>0?"PK ":"   ")+c.value("name").toString()+"   "+c.value("type").toString()+(c.value("nullable").toBool()?"":"   NOT NULL");}auto indexes=doc.value("indexes").toArray();if(!indexes.isEmpty()){lines<<""<<"索引";for(auto i:indexes)lines<<i.toString();}
        const double height=lines.size()*24+24,width=460;auto* box=scene->addRect(0,0,width,height,QPen(QColor("#9db2c7")),QBrush(Qt::white));Q_UNUSED(box);
        for(int i=0;i<lines.size();++i){auto* text=scene->addSimpleText(lines[i]);text->setToolTip(lines[i]);text->setText(QFontMetrics(text->font()).elidedText(lines[i],Qt::ElideRight,430));text->setPos(12,12+i*24);if(i==0){auto font=text->font();font.setBold(true);text->setFont(font);}}
        QSet<QString> targets;int index=0;for(auto value:doc.value("references").toArray()){auto ref=value.toObject();auto name=ref.value("table").toString();if(targets.contains(name))continue;targets.insert(name);const auto y=index++*115.0;scene->addLine(width,40,580,y+30,QPen(QColor("#7293ac"),1.5));scene->addRect(580,y,360,78,QPen(QColor("#9db2c7")),QBrush(QColor("#f0f6fb")));scene->addSimpleText(name)->setPos(592,y+10);scene->addSimpleText(ref.value("detail").toString().left(44))->setPos(592,y+40);}
        scene->setSceneRect(scene->itemsBoundingRect().adjusted(-20,-20,20,20));view->centerOn(240,std::min(height/2,250.0));
        auto* save=new navidog::ui::PushButton("导出 SVG…");save->setObjectName("exportTableModel");tools->addWidget(save);
        connect(save,&QPushButton::clicked,this,[this,doc,lines,height,width,context,purpose=QString("表结构模型-")+task.node.object]{auto path=navidog::ui::chooseExportFile(this,"导出结构模型",context,purpose,"SVG (*.svg)");if(path.isEmpty())return;QSaveFile file(path);if(!file.open(QIODevice::WriteOnly)){QMessageBox::warning(this,"导出失败",file.errorString());return;}auto refs=doc.value("references").toArray();double allHeight=std::max(height,refs.size()*115.0)+40;QString svg=QString("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1000\" height=\"%1\"><rect width=\"100%\" height=\"100%\" fill=\"white\"/><g font-family=\"sans-serif\" font-size=\"13\" fill=\"#253548\"><rect x=\"20\" y=\"20\" width=\"%2\" height=\"%3\" fill=\"#f7fafc\" stroke=\"#9db2c7\"/>").arg(allHeight).arg(width).arg(height);for(int i=0;i<lines.size();++i)svg+=QString("<text x=\"32\" y=\"%1\">%2</text>").arg(48+i*24).arg(xml(lines[i]));int index=0;QSet<QString> seen;for(auto value:refs){auto ref=value.toObject();auto name=ref["table"].toString();if(seen.contains(name))continue;seen.insert(name);int y=20+index++*115;svg+=QString("<path d=\"M480 60 L600 %1\" stroke=\"#7293ac\"/><rect x=\"600\" y=\"%2\" width=\"360\" height=\"78\" fill=\"#f0f6fb\" stroke=\"#9db2c7\"/><text x=\"612\" y=\"%3\">%4</text><text x=\"612\" y=\"%5\">%6</text>").arg(y+30).arg(y).arg(y+28).arg(xml(name)).arg(y+56).arg(xml(ref["detail"].toString().left(44)));}svg+="</g></svg>";auto bytes=svg.toUtf8();if(file.write(bytes)!=bytes.size() || !file.commit())QMessageBox::warning(this,"导出失败",file.errorString());});
    }else {
        auto* chart=new DistributionChart(result.result);chart->setObjectName("distributionChart");layout->addWidget(chart,2);auto* table=new QTableView;auto* data=new ResultModel(table);data->setResult(result.result);table->setModel(data);table->setEditTriggers(QAbstractItemView::NoEditTriggers);table->horizontalHeader()->setStretchLastSection(true);layout->addWidget(table,1);
        auto* save=new navidog::ui::PushButton("保存图表…");save->setObjectName("exportBiChart");tools->addWidget(save);connect(save,&QPushButton::clicked,this,[this,chart,context,purpose=QString("BI分布-")+task.node.object]{auto path=navidog::ui::chooseExportFile(this,"保存图表",context,purpose,"PNG (*.png)");if(path.isEmpty())return;QSaveFile file(path);if(!file.open(QIODevice::WriteOnly) || !chart->grab().save(&file,"PNG") || !file.commit())QMessageBox::warning(this,"保存失败","无法写入图像文件，原文件未覆盖。");});
    }
    auto* close=new navidog::ui::FormButtons;close->setStandardButtons(QDialogButtonBox::Close);close->button(QDialogButtonBox::Close)->setText("关闭");connect(close,&QDialogButtonBox::rejected,this,&QDialog::reject);layout->addWidget(close);
}
