# Navidog 0.54.18

- 查询页可以打开和编辑最多 128 MiB 的 SQL 文件，最多 200 万行且单行不超过 2 Mi 字符。保留 UTF-8、带 BOM 的 UTF-16、原换行风格及原子保存和外部修改检测。
- 大文件先显示文件开头，全文在后台准备，完成后可以编辑。查询页显示当前加载状态和降级策略。超过编辑上限时可查看开头，界面显示具体原因。编辑器仅为可见行做高亮，超长行的高亮限于行首 8192 个字符。实时检查、补全、自动换行和全部替换暂停。格式化、分析、转换和运行可处理较小的选区。
- 保留撤销、定位、查找和单处替换。工作区保存大文件路径，未保存的大文件草稿单独保存在本地，切换到恢复的标签后再读取。

## English

- Query tabs open and edit SQL files up to 128 MiB, with up to two million lines and two Mi characters per line. The editor preserves UTF-8 and BOM-marked UTF-16 support, line endings, atomic saves, and checks for external changes.
- Large files show their opening text while the full editable document is prepared in the background. The query tab displays the loading status and editing policy. Files beyond the editing limits show the prefix and report the reason. Highlighting is limited to visible lines and the first 8192 characters of long lines. Live checks, completion, word wrap, and replace-all pause. Formatting, analysis, conversion, and execution can use a small selection.
- Undo, navigation, find, and single replacement remain available. The workspace stores file references and keeps unsaved large drafts separately. Restored files load when their tab is shown.
