# Navidog 0.53.109

- 将顶部快速入口封装成独立 Qt 组件，在组件内管理按钮、图标、折叠菜单及点击事件。
- 快速入口通过信号接入工作区操作，建模菜单和工具栏继续共用同一组命令。

## English

- The top toolbar owns its buttons, icons, overflow menu, and click bindings.
- Toolbar signals invoke workspace commands. The Modeling menu and toolbar share their actions.
