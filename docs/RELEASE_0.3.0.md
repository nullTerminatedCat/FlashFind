# 闪寻 FlashFind 0.3.0（预览版）

0.3.0 交付匹配开关、扩展搜索语法、扩展名列、剪贴板文件操作、搜索历史、命令行与 `es:` 协议（范围 A–G）。服务与 MFT 索引已在此前版本提前交付。

## A. 匹配开关

- **搜索菜单**：区分大小写、全字匹配、匹配路径（及既有的正则），带快捷键；勾选状态写入 ini（`[search]`）。
- 菜单优先于芯片式控件；状态栏提示当前开关。

## B. 扩展语法

在既有 `|` `!` `<>` 通配符、`ext:` `size:` `dm:` `path:` `file:` `folder:` `regex:` 之上新增：

| 函数 | 别名 | 含义 |
|---|---|---|
| `startwith:` | `startswith:` | 名称（或路径）前缀 |
| `endwith:` | `endswith:` | 后缀 |
| `exact:` | `wholefilename:` / `wholefile:` | 完整名称精确匹配 |
| `parent:` | `infolder:` | 直接父文件夹名 |
| `depth:` | `parents:` | 路径深度（`\` 个数；支持比较与 `..` 区间） |
| `nopath:` | — | 强制只匹配名称，忽略路径 |

## C. 扩展名列

- 结果列表新增「扩展名」列；从文件名解析，**不扩展 Entry / 数据库 schema**。
- 视图菜单与列头右键可显隐；可按扩展名升降序排序（`SortCol::Ext`）。
- 列宽写入 ini（`[columns] extension`）。

## D. 剪贴板与复制/移动

- **剪切 / 复制**（Ctrl+X / Ctrl+C）：`CF_HDROP`；剪切写入 `Preferred DropEffect = MOVE`。
- **粘贴**（Ctrl+V）：粘贴到当前选中项所在目录（或合适目标）。
- **复制到文件夹… / 移动到文件夹…**：`IFileOperation` 选目录后批量复制或移动，可撤销。

## E. 搜索历史

- 保存在 `%APPDATA%\FlashFind\history.txt`（UTF-16 LE BOM），约 64–128 条，去重置顶。
- 搜索菜单：启用搜索历史、清空历史；ini `[history] enabled`。
- 搜索框按 **↑** 浏览历史（↓ 回到草稿）。

## F. 命令行

```
FlashFind.exe [选项]
  -s, -search <文本>       设置搜索
  -p, -path <路径>         搜索路径（等价 path: 前缀）
  -filter <序号|名称>      过滤器
  -sort <name|path|ext|size|date>
  -sort-ascending / -sort-descending
  -select <完整路径>       选中结果
  -reindex                 强制重建索引
  -url <es:…>              从 es: URL 设置搜索
  -startup                 后台启动（隐藏）
  -exit, -quit              退出已运行的实例
  -help                    显示说明
```

启动参数在索引就绪后应用。

## G. es: 协议与文件夹菜单

- 注册 `es:` URL 协议（HKCU），可用 `es:搜索词` 唤起闪寻。
- 文件夹 / 文件夹背景右键：**用闪寻搜索**（`-p` 该路径）。
- **选项 → 常规** 与 **工具** 菜单均可安装 / 卸载。

## 测试

- `ctest`：initest / incrtest / wildtest / sorttest / syntaxtest / regextest / incrtest_synthetic 全部通过。
- syntaxtest 覆盖新增语法；sorttest 覆盖扩展名列排序。

## 已知限制

- 已有实例再启动并带 `-s`/`-p` 时，目前只激活窗口，不把搜索串转发给旧实例。
- 搜索历史目前用 ↑ 浏览，尚无独立下拉编辑框。
- 本轮不做：D2D 绘制、预览窗格、EFU、书签、内容搜索、文件夹大小、创建时间列。
