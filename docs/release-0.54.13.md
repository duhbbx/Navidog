# Navidog 0.54.13

- 表设计控制器负责数据库读取、保存、停止和异步结果匹配。ER 模型继续复用独立的表设计界面。
- 工作区统一通知各标签数据库忙碌状态，查询标签自行更新运行、连接选择和事务控件。
- 主窗口协调结构变更后的导航树与数据页刷新。数据库工具和表数据页统一通过连接控制器取得已认证的连接配置。

## English

- A table design controller handles database reads, saves, cancellation, and asynchronous result routing. ER modeling reuses the standalone table designer.
- Workspace tabs receive database activity updates and manage their controls. Query tabs handle run, connection selection, and transaction controls.
- The main window coordinates catalog and data tab refresh after a schema change. Database tools and data tabs resolve authenticated profiles through the connection controller.
