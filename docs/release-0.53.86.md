# Navidog 0.53.86

- 修复左侧导航树和对象列表右键一次触发两次菜单的问题，避免第二个菜单拦住展开和其他操作。
- 逆向导入对话框关闭后再打开 ER 标签。在导入完成后验证了导航树展开、折叠和右键菜单。

## English

- Fixed duplicate context menus in the connection tree and object lists, which could interrupt subsequent navigation.
- Open the ER tab after the reverse-engineering dialog has closed. Regression coverage exercises expansion, collapse, and the context menu after import.
