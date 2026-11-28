#pragma once
#include <QString>
#include <Qt>
class QTableView;
namespace navidog::ui {
class ObjectGrid;
// Read-only query grids share native cell inspection, menus and bounded copy.
void configureResultGrid(ObjectGrid*);
// Throws before expanding very large selections; does not touch the clipboard.
QString resultSelectionText(QTableView*,bool includeHeaders=false,int valueRole=Qt::UserRole);
}
