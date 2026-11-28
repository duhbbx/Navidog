#pragma once
#include "components.hpp"
#include "export_paths.hpp"
#include "navidog/table_actions.hpp"
class TableReportDialog final : public navidog::ui::Dialog {
public:
    TableReportDialog(const navidog::TableTask&,const navidog::TableTaskResult&,QWidget* parent=nullptr,
                      navidog::ui::ExportContext context={});
};
