# Navidog 0.54.12

- 补丁版本满 100 后向次版本进位。
- 查询标签负责准备执行计划所需的 SQL。工具栏与查询菜单共用同一流程，优先解释选中的 SQL，否则解释光标所在语句。
- 表数据标签负责加载、提交、停止和导出，并管理筛选、分页和编辑状态。

## English

- Patch versions roll over to the next minor version at 100.
- Query tabs prepare SQL for execution plans. The toolbar and Query menu both explain the selection when present, or the statement at the cursor.
- Table data tabs own loading, saving, cancellation, and export, along with filters, pagination, and editing state.
