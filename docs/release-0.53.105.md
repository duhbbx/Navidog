# Navidog 0.53.105

- MySQL 格式化保留 `DEFINER` 账号中 `@` 两侧的紧凑写法。
- 在左侧导航筛选结果中选中对象后，点击搜索框旁的定位图标，即可恢复完整导航树并显示该节点。
- 查询和对象定义标签页移除应用设置按钮，统一从应用菜单打开首选项。
- 修复 Debian 和 Ubuntu 日志窗口的等宽字体选择，以及工作区保存与离线元数据读取并发时可能发生的 SQLite 写入冲突。

## English

- MySQL formatting keeps the user and host adjacent to `@` in `DEFINER` accounts.
- The target button beside the navigation filter clears the filter and reveals the selected item in the full tree.
- The application menu provides the preferences entry for query and object definition editors.
- Timing logs use the platform's resolved monospace font. Reading cached metadata releases the SQLite read snapshot before updating its last access time.
