# Navidog 0.54.19

- 查询、函数、存储过程和表触发器编辑器新增左侧 SQL 结构导航。点击节点定位并选中原文，右键可仅定位开头、选中或复制片段。
- 导航展示查询、子查询、CTE、连接、DDL、例程头部和参数、过程体、分支、循环、异常处理，以及 Oracle 包规范和包体。尚在编辑的 SQL 或解析器未完全覆盖的语法会显示可识别的结构。
- 可见的导航栏在后台跟随编辑内容和数据库方言更新。大文件模式暂停结构导航。

## English

- The query, function, procedure and table trigger editors now have a SQL outline. Clicking a node selects its source. Its context menu can jump to the start, select the span, or copy it.
- The outline includes queries, subqueries, CTEs, joins, DDL, routine headers and parameters, procedural bodies, branches, loops, exception handlers, and Oracle package specifications and bodies. Incomplete or partially supported syntax can show the recognized structure.
- Open outlines refresh in the background as text or the active dialect changes. Large document mode pauses the outline.
