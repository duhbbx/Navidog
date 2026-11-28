#pragma once
#include <QToolButton>
#include <QFrame>
#include <QLabel>
#include <QScreen>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPointer>
#include <QResizeEvent>
#include <QListView>
#include <QListWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QClipboard>
#include <QPushButton>
#include <QMenu>
#include <QTreeWidget>
#include <QTableView>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <functional>
#include <QApplication>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStyleOption>
#include <QTabBar>
#include <QTabWidget>
#include "navidog/database_types.hpp"
// Native Qt controls, central theme and consistent defaults. No browser runtime.
namespace navidog::ui {
// Non-activating hint placement shared by editor completion and diagnostics.
// Keep the entire caret line clear, including when Qt flips a popup upwards.
inline bool placeEditorHint(QWidget *hint,const QRect &caret,int gap) {
    auto *screen=QGuiApplication::screenAt(caret.center());
    if(!screen)screen=QGuiApplication::primaryScreen();
    if(!screen)return false;
    const auto available=screen->availableGeometry().adjusted(8,8,-8,-8);
    const int below=qMax(0,available.bottom()-caret.bottom()-gap);
    const int above=qMax(0,caret.top()-gap-available.top());
    const bool down=below>=hint->height() || below>=above;
    const int room=down?below:above;
    if(room<28){hint->hide();return false;}
    hint->resize(qMin(hint->width(),available.width()),qMin(hint->height(),room));
    const int x=qBound(available.left(),caret.left(),qMax(available.left(),available.right()-hint->width()+1));
    const int y=down?caret.bottom()+gap+1:caret.top()-gap-hint->height();
    hint->move(x,y);return true;
}
class DiagnosticCard final : public QFrame {
    QLabel *title_,*body_,*footer_;
    QTimer expiry_;
public:
    explicit DiagnosticCard(QWidget *owner):QFrame(owner,Qt::ToolTip|Qt::FramelessWindowHint|Qt::WindowTransparentForInput) {
        setObjectName("sqlDiagnosticCard");setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_ShowWithoutActivating);setAttribute(Qt::WA_TransparentForMouseEvents);
        auto *layout=new QVBoxLayout(this);layout->setContentsMargins(15,12,15,11);layout->setSpacing(7);
        title_=new QLabel(this);title_->setObjectName("diagnosticCardTitle");
        body_=new QLabel(this);body_->setObjectName("diagnosticCardBody");body_->setWordWrap(true);
        footer_=new QLabel(this);footer_->setObjectName("diagnosticCardFooter");footer_->setWordWrap(true);
        for(auto *label:{title_,body_,footer_}){label->setTextFormat(Qt::PlainText);label->setTextInteractionFlags(Qt::NoTextInteraction);layout->addWidget(label);}
        setStyleSheet("QFrame#sqlDiagnosticCard{background:#ffffff;border:1px solid #d8e1ea;border-radius:9px;}"
            "QLabel{background:transparent;border:0;color:#263b50;font-size:12px;}"
            "QLabel#diagnosticCardTitle{font-size:13px;font-weight:600;}"
            "QLabel#diagnosticCardFooter{color:#718095;font-size:11px;}");
        expiry_.setSingleShot(true);connect(&expiry_,&QTimer::timeout,this,&QWidget::hide);
    }
    void present(const QString &title,const QString &body,const QString &footer,bool error,const QRect &caret) {
        title_->setText((error?QString::fromUtf8("●  "):QString::fromUtf8("ℹ  "))+title);
        title_->setStyleSheet(error?"color:#c03948":"color:#a56a24");
        body_->setText(body.left(1600));footer_->setText(footer);
        auto *screen=QGuiApplication::screenAt(caret.center());
        const int width=qMax(160,qMin(440,screen?screen->availableGeometry().width()-32:440));
        setFixedWidth(width);adjustSize();
        if(placeEditorHint(this,caret,14)){show();expiry_.start(12000);}
    }
};
inline QIcon databaseIcon(navidog::DatabaseKind kind){
    using K=navidog::DatabaseKind;QColor color;QString label;
    switch(kind){case K::MySQL:color=QColor("#388db3");label="M";break;case K::MariaDB:color=QColor("#86634b");label="m";break;case K::PostgreSQL:color=QColor("#426c97");label="P";break;case K::Oracle:color=QColor("#cf5553");label="O";break;case K::SQLServer:color=QColor("#9c5b63");label="T";break;case K::SQLite:color=QColor("#459bb9");label="S";break;case K::Dameng:color=QColor("#287e81");label="DM";break;case K::TiDB:color=QColor("#986346");label="Ti";break;case K::OceanBaseOracle:case K::OceanBase:color=QColor("#7469ad");label="B";break;default:color=QColor("#748491");label="?";break;}
    QPixmap pixmap(64,64);pixmap.fill(Qt::transparent);QPainter p(&pixmap);p.setRenderHint(QPainter::Antialiasing);p.scale(2,2);p.setPen(QPen(color.darker(120),1));p.setBrush(color.lighter(155));p.drawRect(QRectF(5,8,22,17));p.drawEllipse(QRectF(5,20,22,9));p.setBrush(color);p.drawEllipse(QRectF(5,4,22,10));p.setPen(color.darker(170));p.setFont(QFont("Arial",11,QFont::Bold));p.drawText(QRectF(5,11,22,17),Qt::AlignCenter,label);
    return QIcon(pixmap);
}
#ifdef Q_OS_MACOS
bool showNativeContextMenu(QMenu*,QWidget*,const QPoint&);
#endif
inline QIcon objectIcon(const QString& kind) {
    QPixmap pixmap(64,64);pixmap.fill(Qt::transparent);QPainter p(&pixmap);p.setRenderHint(QPainter::Antialiasing);p.scale(2,2);
    const QColor blue("#329bd5"),green("#45a475"),orange("#dd9363"),ink("#607789");
    if(kind=="table" || kind=="tables" || kind=="view" || kind=="views" || kind=="newTable" || kind=="alterTable") {
        auto color=kind.contains("view")?orange:blue;p.setPen(Qt::NoPen);p.setBrush(color.lighter(155));p.drawRoundedRect(QRectF(3,5,26,23),3,3);p.setBrush(color);p.drawRoundedRect(QRectF(3,5,26,8),3,3);p.drawRect(QRectF(3,9,26,4));p.setPen(QPen(Qt::white,1));for(int x:{12,21})p.drawLine(x,14,x,27);p.drawLine(4,20,28,20);
        if(kind=="newTable"){p.setPen(QPen(green,3));p.drawLine(25,20,25,30);p.drawLine(20,25,30,25);}else if(kind=="alterTable"){p.setPen(QPen(orange.darker(130),3));p.drawLine(20,28,29,19);}
    } else if(kind=="functions" || kind=="function") {p.setPen(blue);p.setFont(QFont("Georgia",21,QFont::Normal,true));p.drawText(QRectF(1,1,31,29),Qt::AlignCenter,"ƒ");}
    else if(kind=="procedures" || kind=="procedure"){p.setPen(QPen(ink,2));p.setBrush(orange.lighter(145));p.drawRoundedRect(QRectF(6,3,20,26),2,2);for(int y:{10,16,22})p.drawLine(10,y,22,y);}
    else if(kind=="query"){p.setPen(Qt::NoPen);p.setBrush(blue.lighter(170));p.drawRoundedRect(QRectF(5,2,23,28),2,2);p.setPen(QPen(blue,2));p.drawLine(9,9,23,9);p.drawLine(9,14,22,14);p.drawLine(9,19,17,19);p.setPen(QPen(green,2.5));p.drawLine(23,20,23,29);p.drawLine(19,25,28,25);}
    else if(kind=="refresh"){p.setPen(QPen(ink,3));p.drawArc(QRectF(6,5,20,21),35*16,285*16);p.setBrush(ink);p.setPen(Qt::NoPen);QPolygonF arrow;arrow<<QPointF(24,3)<<QPointF(28,12)<<QPointF(19,10);p.drawPolygon(arrow);}
    else if(kind=="column"){p.setPen(QPen(ink,2));p.setFont(QFont("Menlo",17));p.drawText(QRectF(0,0,32,32),Qt::AlignCenter,"#");}
    else if(kind=="index" || kind=="indexes" || kind=="primaryKey" || kind=="redisKey" || kind=="redisKeys"){p.setPen(QPen(orange.darker(120),2.5));p.setBrush(orange.lighter(165));p.drawEllipse(QRectF(3,7,11,11));p.drawLine(14,13,29,13);p.drawLine(24,13,24,19);p.drawLine(28,13,28,17);}
    else if(kind=="foreignKey" || kind=="foreignKeys"){p.setPen(QPen(green,2.5));p.setBrush(Qt::NoBrush);p.drawRoundedRect(QRectF(3,4,13,11),3,3);p.drawRoundedRect(QRectF(16,18,13,11),3,3);p.drawLine(9,15,9,23);p.drawLine(9,23,16,23);}
    else if(kind=="check" || kind=="checks"){p.setPen(QPen(green,3));p.drawLine(6,16,13,23);p.drawLine(13,23,27,8);}
    else if(kind=="trigger" || kind=="triggers"){p.setBrush(orange);p.setPen(Qt::NoPen);QPolygonF bolt;bolt<<QPointF(18,2)<<QPointF(6,18)<<QPointF(15,18)<<QPointF(12,30)<<QPointF(27,12)<<QPointF(18,12);p.drawPolygon(bolt);}
    else if(kind=="schema"){p.setPen(QPen(ink,2));p.setBrush(blue.lighter(160));p.drawRoundedRect(QRectF(11,2,10,8),1,1);p.drawLine(16,10,16,18);p.drawLine(6,18,26,18);p.drawLine(6,18,6,22);p.drawLine(26,18,26,22);p.drawRoundedRect(QRectF(1,22,10,8),1,1);p.drawRoundedRect(QRectF(21,22,10,8),1,1);}
    else {p.setPen(QPen(ink,1));p.setBrush(blue.lighter(160));p.drawRect(QRectF(5,8,22,17));p.drawEllipse(QRectF(5,20,22,9));p.setBrush(blue);p.drawEllipse(QRectF(5,4,22,10));if(kind=="connection"){p.setPen(QPen(green,3));p.drawLine(25,19,25,30);p.drawLine(20,25,30,25);}}
    return QIcon(pixmap);
}
class Button : public QToolButton {
public:
    Button(const QString& text, const QString& name = {}, QWidget* parent=nullptr) : QToolButton(parent) {
        setText(text); setObjectName(name); setCursor(Qt::PointingHandCursor); setToolButtonStyle(Qt::ToolButtonTextOnly);
    }
};
// Compact command row with reachable overflow actions at narrow window sizes.
class OverflowStrip : public QWidget {
    QHBoxLayout* row_;
    Button* more_;
    QMenu* menu_;
    QList<QToolButton*> buttons_;
    QWidget* status_=nullptr;
    bool pending_=false;
    void schedule(){if(pending_)return;pending_=true;QTimer::singleShot(0,this,[this]{pending_=false;fit();});}
    void fit(){
        if(buttons_.isEmpty())return;
        const auto margin=row_->contentsMargins();const int available=qMax(0,width()-margin.left()-margin.right());const int gap=qMax(0,row_->spacing());
        int needed=0;for(auto* button:buttons_)needed+=button->sizeHint().width()+gap;
        int statusWidth=status_?qMin(180,status_->sizeHint().width())+gap:0;needed+=statusWidth;
        const bool overflow=needed>available;if(overflow)needed+=more_->sizeHint().width()+gap;
        int hidden=0;for(int i=buttons_.size()-1;i>=0;--i){const bool hide=needed>available;if(hide){needed-=buttons_[i]->sizeHint().width()+gap;++hidden;}buttons_[i]->setVisible(!hide);}
        if(status_){status_->setVisible(needed<=available);status_->setMaximumWidth(180);}
        more_->setVisible(hidden>0);
    }
protected:
    void resizeEvent(QResizeEvent* e)override{QWidget::resizeEvent(e);schedule();}
    bool event(QEvent* e)override{if(e->type()==QEvent::LayoutRequest)schedule();return QWidget::event(e);}
public:
    explicit OverflowStrip(QWidget* parent=nullptr):QWidget(parent){
        setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);row_=new QHBoxLayout(this);row_->setSizeConstraint(QLayout::SetNoConstraint);
        more_=new Button("更多 ▾","queryToolbarMore",this);menu_=new QMenu(more_);more_->setMenu(menu_);more_->setPopupMode(QToolButton::InstantPopup);more_->hide();
        connect(menu_,&QMenu::aboutToShow,this,[this]{
            menu_->clear();
            for(auto* button:buttons_)if(button->isHidden()){
                auto* action=button->menu()?menu_->addMenu(button->menu()):menu_->addAction(button->text());
                action->setText(button->text());action->setIcon(button->icon());
                action->setEnabled(button->isEnabled());action->setToolTip(button->toolTip());
                if(!button->menu()){
                    QPointer<QToolButton> target(button);
                    connect(action,&QAction::triggered,this,[target]{if(target)target->click();});
                }
            }
        });
    }
    QHBoxLayout* row()const{return row_;}
    void complete(QList<QToolButton*> buttons,QWidget* status=nullptr){buttons_=std::move(buttons);status_=status;if(status_)status_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);row_->addWidget(more_);schedule();}
    QSize minimumSizeHint()const override{return {100,QWidget::minimumSizeHint().height()};}
};
class WorkspaceTabStyle final : public QProxyStyle {
    static QStyle* freshBase() {
        auto* base=QStyleFactory::create(QApplication::style()->objectName());
        return base?base:QStyleFactory::create("Fusion");
    }
public:
    WorkspaceTabStyle():QProxyStyle(freshBase()){}
    int styleHint(StyleHint hint,const QStyleOption* option=nullptr,const QWidget* widget=nullptr,QStyleHintReturn* result=nullptr) const override {
        if(hint==QStyle::SH_TabBar_CloseButtonPosition)return QTabBar::RightSide;
        return QProxyStyle::styleHint(hint,option,widget,result);
    }
    void drawPrimitive(PrimitiveElement element,const QStyleOption* option,QPainter* painter,const QWidget* widget=nullptr) const override {
        if(element!=QStyle::PE_IndicatorTabClose || !option || !painter) {
            QProxyStyle::drawPrimitive(element,option,painter,widget);return;
        }
        // The macOS style hides the selected-tab glyph against our light theme.
        // Keep Qt's native button and hit testing, with a visible themed glyph.
        painter->save();painter->setRenderHint(QPainter::Antialiasing);
        const auto center=QRectF(option->rect).center();
        if(option->state & QStyle::State_MouseOver) {
            painter->setPen(Qt::NoPen);painter->setBrush(QColor("#dce6ef"));
            painter->drawRoundedRect(QRectF(center.x()-8,center.y()-8,16,16),3,3);
        }
        painter->setPen(QPen(QColor(option->state & QStyle::State_Enabled?"#526579":"#9ca9b6"),1.4,Qt::SolidLine,Qt::RoundCap));
        painter->drawLine(center+QPointF(-3.5,-3.5),center+QPointF(3.5,3.5));
        painter->drawLine(center+QPointF(-3.5,3.5),center+QPointF(3.5,-3.5));painter->restore();
    }
};
class WorkspaceTabBar final : public QTabBar {
public:
    explicit WorkspaceTabBar(QWidget* parent=nullptr):QTabBar(parent) {
        auto* closeStyle=new WorkspaceTabStyle;closeStyle->setParent(this);setStyle(closeStyle);
        setExpanding(false);setMovable(true);
    }
};
class WorkspaceTabs final : public QTabWidget {
public:
    explicit WorkspaceTabs(QWidget* parent=nullptr):QTabWidget(parent) {
        setTabBar(new WorkspaceTabBar(this));setTabsClosable(true);setMovable(true);tabBar()->setExpanding(false);
    }
};
class CategoryList : public QListWidget {
public:
    explicit CategoryList(QWidget* parent=nullptr):QListWidget(parent){
        setSelectionMode(QAbstractItemView::SingleSelection);setUniformItemSizes(true);
        setStyleSheet("QListWidget { border: 1px solid #d3dbe3; background: #f3f6f9; } QListWidget::item { padding: 12px 10px; } QListWidget::item:selected { background: #d9e9f7; color: #234d70; }");
    }
};
class TextInput : public QLineEdit {
public:
    using QLineEdit::QLineEdit;
};
class MultiLineInput : public QPlainTextEdit {
public:
    explicit MultiLineInput(QWidget* parent=nullptr):QPlainTextEdit(parent) {
        setStyleSheet("QPlainTextEdit { background:#fff; border:1px solid #b7c6d5; "
                      "border-radius:4px; padding:5px 7px; } "
                      "QPlainTextEdit:focus { border-color:#2473aa; }");
    }
};
class Select : public QComboBox {
public:
    using QComboBox::QComboBox;
protected:
    void paintEvent(QPaintEvent* event)override{
        QComboBox::paintEvent(event);QStyleOptionComboBox option;initStyleOption(&option);
        const auto rect=style()->subControlRect(QStyle::CC_ComboBox,&option,QStyle::SC_ComboBoxArrow,this);
        QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);painter.setPen(QPen(isEnabled()?QColor("#52677d"):QColor("#a1adba"),1.5,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
        auto c=QRectF(rect).center();QPolygonF points;points<<QPointF(c.x()-3.5,c.y()-1.5)<<QPointF(c.x(),c.y()+2)<<QPointF(c.x()+3.5,c.y()-1.5);painter.drawPolyline(points);
    }
};
class ContextSelect : public Select {
public:
    using Select::Select;
    std::function<void()> opening;
    void showPopup() override {if(opening)opening();Select::showPopup();}
};
class DecimalInput : public QDoubleSpinBox { public: using QDoubleSpinBox::QDoubleSpinBox; };
class NumberInput : public QSpinBox { public: using QSpinBox::QSpinBox; };
class CheckBox : public QCheckBox { public: using QCheckBox::QCheckBox; };
class PushButton : public QPushButton { public: using QPushButton::QPushButton; };
class Dialog : public QDialog {
public: explicit Dialog(QWidget* parent=nullptr) : QDialog(parent) { setMinimumWidth(490); }
};
class DatabaseErrorDialog final : public Dialog {
    QPlainTextEdit* details_;
public:
    explicit DatabaseErrorDialog(QWidget* parent) : Dialog(parent) {
        setObjectName("databaseErrorDialog");
        setWindowModality(Qt::NonModal);
        resize(650, 290);

        auto* layout = new QVBoxLayout(this);
        details_ = new QPlainTextEdit(this);
        details_->setObjectName("databaseErrorDetails");
        details_->setAccessibleName("错误详情");
        details_->setReadOnly(true);
        details_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        layout->addWidget(details_, 1);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, this);
        buttons->button(QDialogButtonBox::Ok)->setText("确定");
        auto* copy = new PushButton("复制完整错误", this);
        copy->setObjectName("copyDatabaseError");
        buttons->addButton(copy, QDialogButtonBox::ActionRole);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(this, &QDialog::finished, details_, &QPlainTextEdit::clear);
        connect(copy, &QPushButton::clicked, this, [this] {
            QApplication::clipboard()->setText(details_->toPlainText());
        });
    }

    void setError(const QString& title, const QString& detail) {
        setWindowTitle(title);
        details_->setPlainText(detail);
        details_->moveCursor(QTextCursor::Start);
    }
};
class FormButtons : public QDialogButtonBox {
public:
    explicit FormButtons(QWidget* parent=nullptr) : QDialogButtonBox(Save|Cancel,parent) {
        button(Save)->setText("保存"); button(Save)->setDefault(true); button(Save)->setProperty("primary",true);
        button(Cancel)->setText("取消");
    }
};
class ContextMenu : public QMenu {
public:
    explicit ContextMenu(QWidget* parent=nullptr):QMenu(parent) {
        setMinimumWidth(200);
        setSeparatorsCollapsible(true);
        setToolTipsVisible(false);
    }
    void execAt(const QPoint& globalPosition,QWidget* anchor) {
#ifdef Q_OS_MACOS
        if(QGuiApplication::platformName()=="cocoa" && showNativeContextMenu(this,anchor,globalPosition))return;
#else
        Q_UNUSED(anchor);
#endif
        exec(globalPosition);
    }
};
class ObjectTree : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;
    using QTreeWidget::indexFromItem;
    using QTreeWidget::itemFromIndex;
    void afterDoubleClick(QTreeWidgetItem* item, std::function<void(QTreeWidgetItem*)> action) {
        if(!handlingDoubleClick_){action(item);return;}
        const QPersistentModelIndex index=indexFromItem(item);
        afterRelease_=[this,index,action=std::move(action)] {
            if(index.isValid())action(itemFromIndex(index));
        };
    }
protected:
    void mousePressEvent(QMouseEvent* event) override {
        afterRelease_={};
        QTreeWidget::mousePressEvent(event);
    }
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        afterRelease_={};
        handlingDoubleClick_=event->button()==Qt::LeftButton;
        QTreeWidget::mouseDoubleClickEvent(event);
        handlingDoubleClick_=false;
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        const bool context=event->button()==Qt::RightButton
#ifdef Q_OS_MACOS
            || (event->button()==Qt::LeftButton && event->modifiers().testFlag(Qt::MetaModifier))
#endif
            ;
        QTreeWidget::mouseReleaseEvent(event);
        // Tab changes and credential dialogs can move focus. Run them after
        // the double-click gesture has delivered its final release.
        if(event->button()==Qt::LeftButton && afterRelease_) {
            QTimer::singleShot(0,this,std::move(afterRelease_));
            afterRelease_={};
        }
        // Some native/assisted input paths deliver a secondary mouse click
        // without a QContextMenuEvent. Use the same viewport coordinates on
        // every platform; consume the later native event to avoid two menus.
        if(context && contextMenuPolicy()==Qt::CustomContextMenu) {event->accept();const auto point=event->pos();QTimer::singleShot(0,this,[this,point]{if(isVisible() && contextMenuPolicy()==Qt::CustomContextMenu)emit customContextMenuRequested(point);});}
    }
    bool viewportEvent(QEvent* event) override {
        if (event->type() == QEvent::ContextMenu && contextMenuPolicy() == Qt::CustomContextMenu) {
            // QAbstractScrollArea can emit the custom menu signal directly for
            // a viewport event. Mouse releases already queue that signal.
            contextMenuEvent(static_cast<QContextMenuEvent*>(event));
            return true;
        }
        return QTreeWidget::viewportEvent(event);
    }
    void contextMenuEvent(QContextMenuEvent* event) override {
        if(contextMenuPolicy()!=Qt::CustomContextMenu){QTreeWidget::contextMenuEvent(event);return;}
        if(event->reason()==QContextMenuEvent::Keyboard)showKeyboardMenu();
        event->accept();
    }
    void keyPressEvent(QKeyEvent* event) override {
        if(event->key()==Qt::Key_Menu || (event->key()==Qt::Key_F10 && event->modifiers().testFlag(Qt::ShiftModifier))) {showKeyboardMenu();event->accept();return;}
        QTreeWidget::keyPressEvent(event);
    }
private:
    bool handlingDoubleClick_=false;
    std::function<void()> afterRelease_;
    void showKeyboardMenu() {emit customContextMenuRequested(currentItem()?visualItemRect(currentItem()).center():QPoint(8,8));}
};
template<typename View>
class DeferredItemView : public View {
public:
    using View::View;

    void afterDoubleClick(const QModelIndex& item, std::function<void(const QModelIndex&)> action) {
        if (!handlingDoubleClick_) {
            action(item);
            return;
        }
        const QPersistentModelIndex index(item);
        afterRelease_ = [this,index,action=std::move(action)] {
            if (this->isVisible() && index.isValid())
                action(index);
        };
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        afterRelease_ = {};
        View::mousePressEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        afterRelease_ = {};
        handlingDoubleClick_ = event->button() == Qt::LeftButton;
        View::mouseDoubleClickEvent(event);
        handlingDoubleClick_ = false;
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        View::mouseReleaseEvent(event);
        if (event->button() == Qt::LeftButton && afterRelease_) {
            QTimer::singleShot(0,this,std::move(afterRelease_));
            afterRelease_ = {};
        }
    }

private:
    bool handlingDoubleClick_ = false;
    std::function<void()> afterRelease_;
};

using ObjectList = DeferredItemView<QListView>;

class ObjectGrid : public DeferredItemView<QTableView> {
public:
    using DeferredItemView<QTableView>::DeferredItemView;

protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        const bool context=event->button()==Qt::RightButton
#ifdef Q_OS_MACOS
            || (event->button()==Qt::LeftButton && event->modifiers().testFlag(Qt::MetaModifier))
#endif
            ;
        DeferredItemView<QTableView>::mouseReleaseEvent(event);
        if(context && contextMenuPolicy()==Qt::CustomContextMenu){event->accept();const auto point=event->pos();QTimer::singleShot(0,this,[this,point]{if(isVisible() && contextMenuPolicy()==Qt::CustomContextMenu)emit customContextMenuRequested(point);});}
    }
    bool viewportEvent(QEvent* event) override {
        if (event->type() == QEvent::ContextMenu && contextMenuPolicy() == Qt::CustomContextMenu) {
            contextMenuEvent(static_cast<QContextMenuEvent*>(event));
            return true;
        }
        return QTableView::viewportEvent(event);
    }
    void contextMenuEvent(QContextMenuEvent* event) override {
        if(contextMenuPolicy()!=Qt::CustomContextMenu){QTableView::contextMenuEvent(event);return;}
        if(event->reason()==QContextMenuEvent::Keyboard)showKeyboardMenu();event->accept();
    }
    void keyPressEvent(QKeyEvent* event) override {
        if(event->key()==Qt::Key_Menu || (event->key()==Qt::Key_F10 && event->modifiers().testFlag(Qt::ShiftModifier))){showKeyboardMenu();event->accept();return;}
        QTableView::keyPressEvent(event);
    }
private:
    void showKeyboardMenu(){emit customContextMenuRequested(currentIndex().isValid()?visualRect(currentIndex()).center():QPoint(8,8));}
};
inline void applyTheme(QWidget* widget) {
    widget->setStyleSheet(R"(
        QMainWindow, QDialog { background:#f8fafc; color:#253548; }
        QWidget { font-family:"PingFang SC","Segoe UI","Noto Sans CJK SC",sans-serif; font-size:12px; color:#253548; }
        QToolButton, QPushButton { padding:4px 9px; border:1px solid transparent; border-radius:3px; background:transparent; }
        QToolButton:hover, QPushButton:hover { background:#e7edf5; border-color:#d1dbe7; }
        QToolButton:disabled, QPushButton:disabled { color:#a1adba; }
        QToolButton#runQuery { background:#295f9b; color:white; padding:4px 14px; }
        QToolButton#runQuery:hover { background:#214e80; }
        QToolButton#runQuery:disabled { background:#99b1cb; }
        QToolButton#stopQuery { color:#ad4d42; }
        QToolButton#stopQuery:disabled { color:#a1adba; }
        QLineEdit, QComboBox, QSpinBox { border:1px solid #ccd7e3; border-radius:3px; background:white; padding:3px 7px; min-height:18px; }
        QLineEdit:focus, QComboBox:focus { border-color:#628cba; }
        QComboBox { padding-right:25px; }
        QComboBox::drop-down { border:0; width:22px; }
        QComboBox::down-arrow { image:none; width:0; height:0; }
        QCheckBox { spacing:7px; }
        QTreeWidget { border:0; background:#f1f5f9; outline:0; }
        QTreeWidget::item { padding:2px 3px; }
        QTreeWidget::item:selected { background:#dce8f5; color:#244f7e; }
        QTreeWidget::item:hover { background:#e6eef7; }
        QTabWidget::tab-bar { alignment:left; }
        QToolButton#closeWorkspaceTab { font-size:15px; min-width:14px; padding:0px 2px; margin:0px; }
        QWidget#globalToolbar { background:#f0f1f3; border-bottom:1px solid #cbd2d9; }
        QWidget#globalToolbar QToolButton { border-radius:2px; min-width:54px; padding:4px 9px; }
        QWidget#globalToolbar QToolButton[categorySelected="true"] { background:#cfe6f6; border-color:#b2d2e8; }
        QWidget#globalToolbar QToolButton:hover { background:#dcebf6; border-color:#b8d6ee; }
        QToolButton[primary="true"] { background:#295f9b; color:white; }
        QToolButton[primary="true"]:disabled { background:#aebdca; }
        QPushButton[primary="true"] { background:#295f9b; color:white; }
        QPushButton[primary="true"]:hover { background:#214e80; }
        QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled { background:#edf2f7; color:#8795a5; }
        QMenu { background:#f7f7f7; border:1px solid #cccccc; border-radius:7px; padding:5px; font-size:13px; }
        QMenu::item { padding:3px 24px 3px 10px; margin:0px 1px; border-radius:3px; color:#252525; }
        QMenu::item:selected { background:#2878d8; color:white; }
        QMenu::item:disabled { color:#999999; }
        QMenu::separator { height:1px; background:#d7d7d7; margin:5px 8px; }
        QTabWidget::pane { border:0; background:white; }
        QTabBar::tab { background:#edf2f7; padding:5px 12px; border-right:1px solid #dce4ec; min-width:65px; }
        QTabBar::tab:selected { background:white; color:#245c99; border-top:2px solid #3674b8; padding-top:3px; }
        QPlainTextEdit { background:white; border:0; selection-background-color:#dceafa; padding:10px 4px; }
        QPlainTextEdit#sqlEditor, QPlainTextEdit#sourceEditor, QPlainTextEdit#databaseConsoleOutput, QLineEdit#databaseConsoleInput { font-family:"Menlo","Consolas",monospace; font-size:13px; }
        QTableView { background:white; alternate-background-color:#f8fafc; border:0; gridline-color:#e5ebf1; selection-background-color:#dceafa; selection-color:#253548; }
        QHeaderView::section { background:#edf2f7; color:#52677d; border:0; border-right:1px solid #dce4ec; border-bottom:1px solid #d5dfe9; padding:4px 7px; }
        QSplitter::handle { background:#dce4ed; height:4px; width:1px; }
        QStatusBar { background:#eaf0f6; border-top:1px solid #d5dfe9; }
        QStatusBar::item { border:0; }
        QLabel#muted { color:#7e8fa2; }
        QLabel#brand { font-size:18px; font-weight:600; letter-spacing:-0.2px; }
        QLabel#sectionTitle { font-weight:600; }
        QWidget#appHeader { background:#ffffff; border-bottom:1px solid #dce4ed; }
        QWidget#queryViewToolbar, QWidget#objectsToolbar, QWidget#queryToolbar, QWidget#tableToolbar, QWidget#dataToolbar, QWidget#dataFilterToolbar, QWidget#resultHeader { background:#f2f4f6; border-bottom:1px solid #dce4ed; }
        QScrollBar:vertical { width:10px; background:#f4f7fa; }
        QScrollBar::handle:vertical { background:#c3cfdb; min-height:30px; border-radius:4px; margin:2px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }
    )");
}
}
