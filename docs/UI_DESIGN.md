# Everything 克隆 — UI 设计说明

> 目标：复刻 voidtools Everything 1.4.1.1032 的界面与交互。技术栈：原生 Win32 + C++20 + CMake，仅依赖 Windows SDK（comctl32 v6、uxtheme、dwmapi、shell32、ole32），不用 Qt / MFC / ATL。
> 配套静态原型：`docs/ui_mockup.html`（截图 `docs/ui_mockup.png`）。

## 1. 主窗口布局

```
┌──────────────────────────────────────────────────────────────┐
│ 标题栏：<搜索词> - Everything                                 │
├──────────────────────────────────────────────────────────────┤
│ 菜单栏：文件(F) 编辑(E) 视图(V) 搜索(S) 书签(B) 工具(T) 帮助(H) │
├──────────────────────────────────────────────┬───────────────┤
│ [搜索框 ..................................]   │ [Everything ▼]│ ← 过滤器下拉（可隐藏）
├──────────────────────────────────────────────┴───────────────┤
│ 名称 ▲          │ 路径              │   大小 │ 修改时间        │ ← 表头，可增删列
│ 📄 report.docx  │ C:\Users\x\Docs   │  24 KB │ 2026/10/20 9:12 │
│ 📁 src          │ F:\work\everything│        │ 2026/10/21 13:00│
│ ...（虚拟列表，百万行）                              │ 预览窗格(可选)│
├──────────────────────────────────────────────────────────────┤
│ 状态栏：1,234,567 个对象 │ 已选 3 个，1.2 MB │ 排序: 名称       │
└──────────────────────────────────────────────────────────────┘
```

- 默认尺寸 800×600（96 DPI 逻辑像素），位置/大小/列宽/排序保存在 ini。
- 搜索框高度 = 字体高度 + 8px；过滤器下拉宽 150px，靠右。
- 结果列表填满剩余区域；预览窗格位于右侧，中间为可拖动分隔条（4px）。
- 标题栏格式：有搜索词时 `<搜索词> - Everything`，否则 `Everything`。

## 2. Win32 控件映射

| 元素 | 实现 | 关键样式/要点 |
|---|---|---|
| 主窗口 | 自注册窗口类 `EverythingMainWnd` | `WS_OVERLAPPEDWINDOW`；`WM_GETMINMAXINFO` 最小 300×200 |
| 菜单栏 | `CreateMenu` 代码构建（不用 .rc，方便多语言） | 加速键表 `CreateAcceleratorTable` |
| 搜索框 | `EDIT` | `ES_AUTOHSCROLL`；子类化处理 ↑↓/PgUp/PgDn/Enter 转发给列表；`EM_SETCUEBANNER`；输入去抖 0~50ms 后搜索 |
| 搜索建议（历史） | 自绘弹出 `WS_POPUP` + ListBox | 输入时显示历史匹配 |
| 过滤器下拉 | `COMBOBOX` `CBS_DROPDOWNLIST` | 项：Everything/音频/压缩/文档/可执行/文件夹/图片/视频 + 自定义 |
| 结果列表 | `SysListView32` `LVS_REPORT \| LVS_OWNERDATA \| LVS_SHOWSELALWAYS` | 扩展：`LVS_EX_FULLROWSELECT \| LVS_EX_DOUBLEBUFFER \| LVS_EX_HEADERDRAGDROP \| LVS_EX_INFOTIP`；`LVN_GETDISPINFO` 取数据；`NM_CUSTOMDRAW` 实现隔行色和匹配高亮；`LVN_ODFINDITEM` 键入跳转 |
| 缩略图视图 | 同一 ListView 切 `LV_VIEW_ICON` + 自绘，或独立自绘窗口 | `IShellItemImageFactory` 后台线程取缩略图 |
| 图标 | 系统图像列表 `SHGetFileInfo(SHGFI_SYSICONINDEX)` | 后台线程按需加载，先显示按扩展名的默认图标 |
| 表头 | ListView 自带 `SysHeader32` | 右键表头弹出列选择菜单；排序箭头 `HDF_SORTUP/DOWN` |
| 状态栏 | `msctls_statusbar32` | 3 个分区，`SB_SETPARTS` |
| 预览窗格 | 容器窗口 + `IPreviewHandler` | 进程外宿主，失败显示“无预览” |
| 托盘 | `Shell_NotifyIcon` (NOTIFYICONDATA v4) | 左键显示/切换，右键菜单 |
| 选项对话框 | 代码生成的对话框模板 / 手工创建子窗口 | 左侧 `SysTreeView32` 页面树 + 右侧页面容器 |
| 全局热键 | `RegisterHotKey` | 冲突时提示 |

## 3. 菜单

### 文件(F)
- 新建搜索窗口　Ctrl+N
- 打开文件列表...　Ctrl+O
- 关闭文件列表
- 文件列表编辑器...
- 导出...　Ctrl+S（EFU / CSV / TXT）
- 复制完整路径到剪贴板 —— 见编辑
- ——
- 关闭　Ctrl+W
- 退出　Ctrl+Q

### 编辑(E)
- 剪切 Ctrl+X / 复制 Ctrl+C / 粘贴 Ctrl+V
- 复制到文件夹... / 移动到文件夹...
- 全选 Ctrl+A / 反选
- ——
- 高级复制 ▸ 复制名称 / 复制完整路径 Ctrl+Shift+C / 复制带引号路径 / 复制为 CSV
- 高级重命名（多文件重命名）... F2（多选时）

### 视图(V)
- 过滤器 / 预览 Alt+P / 状态栏（勾选）
- ——
- 超大/大/中缩略图 Ctrl+Shift+1/2/3、详细信息 Ctrl+Shift+6
- ——
- 窗口大小 ▸ 小/中/大/自动适应
- 排序方式 ▸ 名称 Ctrl+1 / 路径 Ctrl+2 / 大小 Ctrl+3 / 扩展名 Ctrl+4 / 类型 / 修改时间 Ctrl+5 / 创建时间 Ctrl+6 / 属性 Ctrl+7 / 最近更改 / 访问时间 / 运行次数 / 运行日期；升序/降序
- 转到 ▸ 返回 Alt+← / 前进 Alt+→
- ——
- 刷新 F5
- 置顶 ▸ 从不/始终 Ctrl+T/搜索时
- 全屏 F11

### 搜索(S)
- 区分大小写 Ctrl+I / 区分变音 Ctrl+M / 全字匹配 Ctrl+B / 匹配路径 Ctrl+U / 正则 Ctrl+R
- ——
- 过滤器：Everything / 音频 / 压缩 / 文档 / 可执行 / 文件夹 / 图片 / 视频（单选）
- ——
- 添加到过滤器... / 管理过滤器...
- 高级搜索...

### 书签(B)
- 添加到书签... Ctrl+D
- 管理书签... Ctrl+Shift+B
- ——（书签列表，支持文件夹组织）

### 工具(T)
- 连接 ETP 服务器... / 断开
- 启动/停止 HTTP 服务器、ETP/FTP 服务器（实际由选项控制，菜单为快捷开关）
- 文件列表编辑器...
- 调试 ▸ 控制台 / 日志
- ——
- 选项... Ctrl+P

### 帮助(H)
- Everything 帮助 F1 / 搜索语法 / 正则语法 / 命令行选项
- Everything 网站 / 检查更新 / 捐赠
- 关于 Everything...

## 4. 右键菜单

**结果项（文件）**：打开(粗体) / 打开路径 / 在资源管理器中定位 / ——/ 自定义动词(custom_verb01~12) / ——/ 系统外壳扩展（`IContextMenu` 合并：发送到、7-Zip 等）/ ——/ 剪切 / 复制 / 复制到文件夹... / 移动到文件夹... / ——/ 复制完整路径 / 复制名称 / ——/ 创建快捷方式 / 删除 / 重命名 / ——/ 属性

- 按住 Shift 右键：显示仅 Shift 可见项及系统扩展菜单（`CMF_EXTENDEDVERBS`）。
- 多选：打开(多个) / 复制 / 删除 / 高级重命名...
- 结果空白处：粘贴 / 全选 / 视图 ▸ / 排序方式 ▸。
- **表头**：可选列（名称/路径/大小/扩展名/类型/修改时间/创建时间/访问时间/属性/最近更改/运行次数/运行日期/文件列表文件名）+ “自适应列宽”。
- **托盘**：显示搜索窗口 / 新建窗口 / 选项 / 退出。
- **搜索框**：标准编辑菜单 + “搜索历史 ▸”。

## 5. 选项对话框

尺寸约 640×480，左树右页，底部 [确定] [取消] [应用] [恢复默认]。

| 页 | 主要控件 |
|---|---|
| 常规 General | 开机启动、以管理员运行 / Everything 服务（安装/卸载按钮）、URL 协议 es:、EFU 关联、文件夹右键 “Search Everything”、桌面/开始菜单/快速启动快捷方式、数据存放（AppData/程序目录）、启动检查更新、语言 |
|　└ 界面 UI | 托盘图标、最小化到托盘、关闭到托盘、单击托盘行为、新窗口、多实例、单击打开 |
|　└ 主页 Home | 主页搜索/过滤器/索引/匹配开关：使用上次值/默认/自定义 |
|　└ 搜索 Search | 输入即搜、匹配全路径当含 `\`、通配匹配全名、快速 ASCII、运算符优先级、空搜索隐藏结果、搜索变更清除选择、回车聚焦运行最多项、展开环境变量 |
|　└ 结果 Results | 自动滚动/选择、双击路径打开路径、重命名不选扩展名、执行后关闭窗口、打开/打开文件夹/浏览的命令行 |
|　└ 视图 View | 整行选择、隔行变色、高亮匹配、工具提示、状态栏显示大小/选中、缩略图尺寸、自适应列宽、预览处理程序 |
|　└ 右键菜单 Context Menu | 列表：每项 显示/隐藏/仅 Shift 显示；自定义动词编辑（名称+命令） |
|　└ 字体和颜色 | 字体/字号按钮、元素列表（正常/高亮/选中/隔行/预览背景）× 前景/背景色、下划线图标标题、选区 半透明/XOR |
|　└ 键盘 Keyboard | 全局热键：新建/显示/切换窗口；命令列表 + 快捷键编辑框（`HOTKEY_CLASS`）；Ctrl+滚轮动作 |
|　└ 历史 History | 搜索历史开/保留天数/永久/建议；运行历史开/天数/清除按钮 |
| 索引 Indexes | 数据库位置、压缩、多用户文件名；索引文件大小/文件夹大小/日期/属性/最近更改；快速排序勾选；[强制重建] |
|　└ NTFS | 卷列表（ListView 勾选）；每卷：纳入索引、监视 USN、启用最近更改、包含仅挂载点；[创建/删除 USN Journal]；自动加入新固定/可移动卷、自动移除离线卷 |
|　└ ReFS | 同 NTFS |
|　└ 文件夹 Folders | 文件夹列表 + [添加][删除][立即重扫][全部重扫]；监视变化、缓冲溢出重扫、定时重扫（每天 hh:mm / 每 N 小时）、深度、包含/排除通配 |
|　└ 文件列表 File Lists | EFU 列表 + 添加/删除/监视变化/相对路径 |
|　└ 排除 Exclude | 排除隐藏、排除系统；排除文件夹列表；包含仅文件 / 排除文件 通配 |
| ETP/FTP 服务器 | 启用、绑定接口、端口(21)、用户名、密码、欢迎信息、允许下载、日志文件/大小 |
| HTTP 服务器 | 启用、绑定接口、端口(80)、用户名、密码、默认页、根目录、每页条数、标题格式、允许下载、日志 |

页面实现：每页一个 `PageProc`，`Load(Settings&)` / `Save(Settings&)`，切换只隐藏/显示子窗口；“应用”统一写 ini。

## 6. 高级搜索对话框

| 区 | 控件 |
|---|---|
| 包含全部这些词 / 任意这些词 / 排除这些词 / 精确短语 | 4 个 EDIT |
| 文件中的词或短语 | EDIT + 编码下拉（自动/ANSI/UTF-8/UTF-16LE/UTF-16BE）|
| 所在位置 | EDIT + [浏览...]（`IFileOpenDialog` FOS_PICKFOLDERS）+ 含子文件夹 |
| 匹配 | 大小写/全字/变音/路径/正则 复选 |
| 过滤器 | 下拉：文件和文件夹/仅文件/仅文件夹/各过滤器 |
| 修改日期 | 下拉（任意/今天/本周/本月/今年/介于）+ 2 个 `SysDateTimePick32` |
| 大小 | 下拉（任意/空/<10KB/.../介于）+ 2 个 EDIT + 单位下拉 |
| 扩展名 | EDIT（`;` 分隔） |
| 底部 | 生成的搜索语法预览（只读 EDIT）+ [确定] [取消] |

确定后把生成的语法写入主搜索框，如 `ext:docx;xlsx dm:thismonth size:>1mb "年度报告" infolder:"F:\work"`。

另有：添加书签/过滤器对话框、书签/过滤器管理器、多文件重命名对话框（旧格式/新格式 + 正则 + 预览列表）、复制/移动到文件夹、连接 ETP、关于、文件列表编辑器窗口（与主窗口同布局，带工具菜单）。

## 7. 键盘快捷键（默认）

| 范围 | 键 | 动作 |
|---|---|---|
| 全局（可选） | 未设 / 用户配置 | 显示/新建窗口 |
| 窗口 | Ctrl+F / F3 / Alt+D | 聚焦并全选搜索框 |
| 全局 | Ctrl+Alt+Space / 双击 Ctrl | 显示/隐藏窗口切换（见 7.1） |
| 窗口 | Esc | 清空搜索；为空时隐藏到托盘 |
| 搜索框 | ↓ / PgDn / Enter | 移到列表；Enter 打开首个/运行最多项 |
| 列表 | Enter / Ctrl+Enter | 打开 / 打开路径 |
| 列表 | Alt+Enter | 属性 |
| 列表 | F2 / Del / Shift+Del | 重命名 / 回收站 / 永久删除 |
| 列表 | Ctrl+C / Ctrl+Shift+C | 复制文件 / 复制完整路径 |
| 列表 | Ctrl+A | 全选 |
| 列表 | 字母键 | 按名称跳转 |
| 任意 | Ctrl+1..7 | 排序 |
| 任意 | Ctrl+I/M/B/U/R | 匹配开关 |
| 任意 | Ctrl+N / Ctrl+W / Ctrl+Q | 新窗口 / 关闭 / 退出 |
| 任意 | Ctrl+P | 选项 |
| 任意 | Ctrl+D | 添加书签 |
| 任意 | Alt+P / F11 / F5 | 预览 / 全屏 / 刷新 |
| 任意 | Ctrl+滚轮 / Ctrl+± | 缩放字体或缩略图尺寸 |

## 7.1 全局热键：显示/隐藏切换（用户新增需求）

- **默认热键**：`Ctrl+Alt+Space`（避开系统占用的 `Alt+Space`、`Win+*`）。可选模式：**双击 Ctrl**（两次按下间隔 ≤ 300ms，期间无其他键）。两者可同时启用。
- **切换逻辑**：
  - 窗口隐藏 / 最小化 / 非前台 → 显示、恢复、`SetForegroundWindow`（用 `AllowSetForegroundWindow` / 热键消息自带前台权限），焦点到搜索框并 `EM_SETSEL(0,-1)` 全选。
  - 窗口已是前台 → `ShowWindow(SW_HIDE)` 隐藏到托盘（托盘图标常驻）。
  - 多窗口时作用于最近一次活动的搜索窗口。
- **Esc**：搜索框非空时第一下清空（可在选项中关闭）；为空或第二下 → 隐藏到托盘（不退出）。
- **多显示器**：显示时取 `GetCursorPos` → `MonitorFromPoint(MONITOR_DEFAULTTONEAREST)`；若窗口不在该显示器，则按该显示器工作区（`GetMonitorInfo.rcWork`）居中（或保持相对位置），并按目标显示器 DPI 缩放尺寸。
- **实现**：
  - 组合键：`RegisterHotKey(hwndMain, ID_TOGGLE, MOD_CONTROL|MOD_ALT|MOD_NOREPEAT, VK_SPACE)`，处理 `WM_HOTKEY`。
  - 双击 Ctrl：`RegisterHotKey` 无法注册单修饰键，使用 `SetWindowsHookEx(WH_KEYBOARD_LL)` 低级键盘钩子（钩子回调需快速返回，仅做计时并 `PostMessage`）。
- **配置界面**：选项 › 键盘 › “全局热键”分组：
  - 显示/隐藏窗口：热键捕获控件（`HOTKEY_CLASS` `msctls_hotkey32`，或自绘捕获框：获得焦点后按下组合即录入，Backspace 清除）+ [清除] 按钮；
  - 复选“双击 Ctrl 显示/隐藏”；新建窗口、显示窗口热键同样各一个捕获框。
- **冲突检测**：应用时先 `UnregisterHotKey` 旧值再 `RegisterHotKey` 新值；失败（`ERROR_HOTKEY_ALREADY_REGISTERED`）→ 捕获框旁显示红色提示“该热键已被其他程序占用”，保留旧热键并阻止“确定”关闭；启动时注册失败则托盘气泡提示并在状态栏标记。
- 设置键名：`toggle_window_key`、`toggle_window_double_ctrl`、`esc_hides_window`。

## 8. DPI 与深色模式

**DPI**
- 清单声明 `PerMonitorV2`（`<dpiAwareness>PerMonitorV2</dpiAwareness>`），兼容回退 `dpiAware=true`。
- 所有尺寸以 96 DPI 逻辑值存储，`MulDiv(v, dpi, 96)` 换算；`GetDpiForWindow`。
- 处理 `WM_DPICHANGED`：按建议矩形 `SetWindowPos`，重新创建字体（`SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS)`）、系统图像列表换尺寸（16/20/24/32 按 DPI 选 SHIL_SMALL/SHIL_LARGE）、列宽按比例缩放。
- 对话框使用 `DIALOG` 单位或在 `WM_INITDIALOG` 中按 DPI 布局。

**深色模式**（Everything 1.4 原版无深色模式，此为增强，可在“字体和颜色”里选 跟随系统/浅色/深色）
- 读取 `HKCU\...\Themes\Personalize\AppsUseLightTheme`，监听 `WM_SETTINGCHANGE("ImmersiveColorSet")`。
- 标题栏：`DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE=20)`。
- 列表/表头/滚动条：`SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr)` / `L"DarkMode_ItemsView"`；颜色通过 `ListView_SetBkColor/TextBkColor/TextColor` 与 custom draw 控制。
- 菜单栏：Win32 菜单栏无官方深色 API，用 `WM_UAHDRAWMENU` 自绘（未文档化）或 uxtheme 序号 135 `SetPreferredAppMode`（未文档化）— **待确认是否接受**。
- 编辑框/下拉/状态栏：`WM_CTLCOLOREDIT` 等返回深色画刷；状态栏自绘。
- 浅色配色：背景 #FFFFFF，隔行 #F7F7F7，选中 #CCE8FF（焦点）/#D9D9D9（非焦点），高亮匹配词加粗。深色：背景 #202020，隔行 #262626，文字 #E6E6E6，选中 #2D4F6E。

## 9. 性能相关的 UI 约定
- 列表永远 `LVS_OWNERDATA`，`ListView_SetItemCountEx(n, LVSICF_NOINVALIDATEALL|LVSICF_NOSCROLL)`。
- 搜索在工作线程，完成后 `PostMessage(WM_APP_RESULTS)`；新输入取消旧搜索（代号/原子计数）。
- 图标、大小、日期等扩展信息按可见范围懒加载，`LVN_ODCACHEHINT` 预取。
- 状态栏在索引期间显示“正在扫描 C: 35%...”。

## 10. 源码模块建议（与 UI 相关）
```
src/main.cpp           入口、单实例、命令行
src/MainWindow.*       主窗口、布局、菜单、加速键
src/SearchEdit.*       搜索框子类化 + 历史建议
src/ResultList.*       虚拟 ListView、custom draw、列管理
src/ContextMenu.*      IContextMenu 合并、自定义动词
src/PreviewPane.*      IPreviewHandler 宿主
src/Tray.*             托盘
src/OptionsDialog.*    选项对话框 + pages/*.cpp
src/AdvancedSearch.*   高级搜索
src/Dpi.* Theme.*      DPI/深色
src/Settings.*         ini 读写（兼容 Everything.ini 键名）
resources/app.manifest PerMonitorV2 + comctl32 v6
```

## 11. 视觉设计 v2（Windows 11 Fluent 风格）

原型：`docs/ui_mockup_v2.html`（右上按钮切换浅/深色；URL 加 `#dark` 直接深色），截图 `ui_mockup_v2_light.png` / `ui_mockup_v2_dark.png`。

### 11.1 调色板

| Token | 浅色 | 深色 | 用途 |
|---|---|---|---|
| 窗口底（Mica 回退色） | #F3F3F3 | #202020 | 客户区背景 |
| 卡片 / 列表面 | #FBFBFB | #2B2B2B | 结果列表、设置卡片 |
| 控件层 | #FFFFFF | #272727 | 编辑框、下拉、菜单 |
| 描边 | #E5E5E5 / #D1D1D1 | #3A3A3A / #454545 | 分隔线、控件边框 |
| 主文字 / 次文字 / 弱文字 | #1B1B1B / #5D5D5D / #8A8A8A | #F0F0F0 / #C5C5C5 / #8F8F8F | |
| 强调色 | 系统强调色，回退 #0067C0 | 系统强调色浅变体，回退 #4CC2FF | 搜索框下划线、选中条、开关、主按钮 |
| 悬停 | 黑 3.7% | 白 5.5% | 行/菜单项 hover |
| 选中 | 强调色 10% + 3px 左侧强调条 | 强调色 16% + 强调条 | 结果行 |
| 隔行 | 黑 1.8% | 白 2.2% | |
| 匹配高亮 | 底 #FFF1B8，字加粗 | 底 #5C4A00，字 #FFE58A | 搜索词 |
| 错误 | #C42B1C | #FF99A4 | 热键冲突 |

强调色：`DwmGetColorizationColor` 或 `UISettings`（WinRT，可选）读取；监听 `WM_DWMCOLORIZATIONCOLORCHANGED`。

### 11.2 字体

- 界面：`Segoe UI Variable Text`（Win11），回退 `Segoe UI`；中文回退 `Microsoft YaHei UI`（通过字体链接自动）。正文 9pt（96DPI 下 12~13px）。
- 选项页标题 `Segoe UI Variable Display` 20pt Semibold；分组标题 Semibold。
- 用 `CreateFontIndirect` 按 DPI 创建，`lfQuality = CLEARTYPE_QUALITY`。

### 11.3 尺寸（96 DPI 逻辑像素）

| 元素 | 值 |
|---|---|
| 标题栏 | 40（自定义标题栏时），标题按钮 46×40 |
| 搜索框 / 下拉 | 高 34，圆角 6，聚焦时底部 2px 强调色 |
| 结果行高 | 30（紧凑模式 24），行内圆角 4，左右内缩 4 |
| 图标 | 16（列表）/ 18（导航） |
| 状态栏 | 高 32 |
| 菜单 / 右键菜单 | 项高 30，圆角 8 外框 / 4 项，外边距 4 |
| 选项对话框 | 760×740，左导航宽 210，导航项高 32，设置卡片内边距 12×14、圆角 6、卡片间距 4 |
| 按钮 | 高 32，圆角 5；开关 40×20 |

### 11.4 Win32 实现说明

- **Mica**：Win11 22H2+ `DwmSetWindowAttribute(DWMWA_SYSTEMBACKDROP_TYPE=38, DWMSBT_MAINWINDOW)`，并 `DwmExtendFrameIntoClientArea(-1)`，客户区背景用透明画刷；Win10 回退为纯色 #F3F3F3/#202020。
- **圆角**：顶层窗口 `DWMWA_WINDOW_CORNER_PREFERENCE=33, DWMWCP_ROUND`；弹出菜单 `DWMWCP_ROUNDSMALL`。控件内部圆角用 GDI+ / Direct2D 自绘（推荐 Direct2D + DirectWrite，系统自带，无额外依赖）。
- **标题栏**：保留系统标题栏（最简单，深色用 `DWMWA_USE_IMMERSIVE_DARK_MODE`、`DWMWA_CAPTION_COLOR=35`）；如要原型中的一体化外观，处理 `WM_NCCALCSIZE` + `WM_NCHITTEST` 自绘，Win11 贴靠布局需对最大化按钮返回 `HTMAXBUTTON`。
- **结果列表**：仍为 `LVS_OWNERDATA` ListView，`NM_CUSTOMDRAW` 在 `CDDS_ITEMPREPAINT` 自绘整行（圆角选中背景、左侧强调条、隔行），`CDDS_SUBITEMPREPAINT` 中用 DirectWrite 分段绘制匹配高亮；表头 `SetWindowTheme(L"ItemsView")`。
- **搜索框**：`EDIT` 无边框嵌入自绘容器（容器画圆角和强调下划线），匹配开关 chip 为自绘按钮（Aa / 整词 / 路径 / .*）。
- **图标**：Fluent 线性图标 → 打包 `Segoe Fluent Icons`（Win11 自带字体，`DrawText` 画字形）回退 `Segoe MDL2 Assets`；文件图标仍用系统图像列表。
- **右键菜单**：常用项自绘弹出窗口（Fluent 外观），“更多选项”进入系统 `IContextMenu`（与 Win11 资源管理器一致）；或用 owner-draw `HMENU`（`MF_OWNERDRAW`，`WM_MEASUREITEM/WM_DRAWITEM`）保留原生键盘行为——推荐后者。
- **选项对话框**：左导航为自绘 ListBox（`LBS_OWNERDRAWFIXED`）；右侧可滚动面板中放“设置卡片”自绘子窗口，开关为自绘按钮（`BS_OWNERDRAW`），热键捕获框为自绘控件（处理 `WM_KEYDOWN/WM_SYSKEYDOWN`，显示 kbd 样式键帽）。
- **动画**：仅悬停淡入（可选，`SetTimer` 120ms），尊重 `SPI_GETCLIENTAREAANIMATION`。
- **高对比度**：`SPI_GETHIGHCONTRAST` 为真时禁用全部自定义配色，回退系统颜色 `GetSysColor`。
