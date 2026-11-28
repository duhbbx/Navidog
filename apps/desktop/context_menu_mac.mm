#import <AppKit/AppKit.h>
#include "components.hpp"
#include <QAction>
#include <QPointer>
#include <vector>

@interface NVDContextMenuTarget : NSObject {
@public NSInteger selected;
}
- (void)choose:(NSMenuItem*)sender;
@end
@implementation NVDContextMenuTarget
- (instancetype)init { self=[super init];if(self)selected=-1;return self; }
- (void)choose:(NSMenuItem*)sender { selected=[sender tag]; }
@end

namespace navidog::ui {
namespace {
NSString* text(const QString& value) {return [NSString stringWithUTF8String:value.toUtf8().constData()];}
void shortcut(NSMenuItem* item,const QKeySequence& sequence) {
    if(sequence.isEmpty())return;
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
    const int combined=sequence[0].toCombined();
#else
    const int combined=sequence[0];
#endif
    const int key=combined & ~int(Qt::KeyboardModifierMask);NSEventModifierFlags modifiers=0;
    if(combined & Qt::ControlModifier)modifiers|=NSEventModifierFlagCommand;
    if(combined & Qt::MetaModifier)modifiers|=NSEventModifierFlagControl;
    if(combined & Qt::ShiftModifier)modifiers|=NSEventModifierFlagShift;
    if(combined & Qt::AltModifier)modifiers|=NSEventModifierFlagOption;
    if(key>=Qt::Key_F1 && key<=Qt::Key_F35){unichar code=NSF1FunctionKey+key-Qt::Key_F1;[item setKeyEquivalent:[NSString stringWithCharacters:&code length:1]];}
    else if(key>0 && key<0x10000)[item setKeyEquivalent:text(QString(QChar(key)).toLower())];
    [item setKeyEquivalentModifierMask:modifiers];
}
NSMenu* buildMenu(QMenu* source,NVDContextMenuTarget* target,std::vector<QPointer<QAction>>& actions) {
    NSMenu* native=[[NSMenu alloc]initWithTitle:@""];[native setAutoenablesItems:NO];
    for(auto* action:source->actions()) {
        if(!action->isVisible())continue;
        if(action->isSeparator()){[native addItem:[NSMenuItem separatorItem]];continue;}
        NSMenuItem* item=[[NSMenuItem alloc]initWithTitle:text(action->text()) action:nil keyEquivalent:@""];
        [item setEnabled:action->isEnabled()];
        if(auto* submenu=action->menu()) {
            auto* child=buildMenu(submenu,target,actions);[item setSubmenu:child];[child release];
        }else {
            [item setTag:static_cast<NSInteger>(actions.size())];actions.emplace_back(action);[item setTarget:target];[item setAction:@selector(choose:)];shortcut(item,action->shortcut());
            if(action->isCheckable())[item setState:action->isChecked()?NSControlStateValueOn:NSControlStateValueOff];
        }
        [native addItem:item];[item release];
    }
    return native;
}
}
bool showNativeContextMenu(QMenu* menu,QWidget* anchor,const QPoint& globalPosition) {
    NSView* view=reinterpret_cast<NSView*>(anchor->window()->winId());if(!view)return false;
    @autoreleasepool {
        // AppKit handles rendering, tracking and accessibility. QAction remains
        // the source of labels, enabled states, submenus and command callbacks.
        NVDContextMenuTarget* target=[[NVDContextMenuTarget alloc]init];std::vector<QPointer<QAction>> actions;
        NSMenu* native=buildMenu(menu,target,actions);[native setMinimumWidth:200];
        const auto local=anchor->window()->mapFromGlobal(globalPosition);
        const auto y=[view isFlipped]?local.y():NSHeight([view bounds])-local.y();
        [native popUpMenuPositioningItem:nil atLocation:NSMakePoint(local.x(),y) inView:view];
        const auto selected=target->selected;
        [native release];[target release];
        // Dispatch after native tracking ends, so commands may open Qt dialogs.
        if(selected>=0 && static_cast<size_t>(selected)<actions.size() && actions[selected] && actions[selected]->isEnabled())actions[selected]->trigger();
    }
    return true;
}
}
