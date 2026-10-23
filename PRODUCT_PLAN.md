# 闪寻 FlashFind 产品计划

定位：Everything 1.4 的开源式替代，原生 Win32，静态链接 PCRE2，界面现代化（Fluent），支持 Win10 / Win11。

## 已定技术决策

| 项 | 决定 |
|---|---|
| 语言 / 构建 | C++20，CMake + MSVC（Ninja），静态 CRT |
| 界面 | 原生 Win32 + comctl32 v6；自绘用 Direct2D / DirectWrite；保留系统标题栏（DWM 深色） |
| 系统 | Win10 + Win11；Mica / 圆角仅 Win11，Win10 实色回退 |
| 索引 | 后台 Windows 服务读取 NTFS MFT + USN Journal，界面通过命名管道通信；普通权限无服务时保留已有 NTFS 缓存并提示 |
| 热键 | Ctrl+Alt+Space 切换 + 双击 Ctrl（WH_KEYBOARD_LL），v1 均提供 |
| 推迟 | HTTP / ETP 服务器、ReFS、文件内容搜索、缩略图 |

## 里程碑

### M1 可用原型 ✅（0.1.0）
- 工程骨架、清单（PerMonitorV2、comctl32 v6）、ini 设置
- 主窗口：搜索框、虚拟列表、状态栏；浅/深色
- 进程内遍历索引 + 子串搜索（AND、短语、路径匹配）、匹配高亮
- 托盘、单实例、全局热键 + 双击 Ctrl、多显示器显示
- 打开 / 打开路径 / 复制路径 / 属性 / 右键菜单

### M2 界面完善 ✅（0.2.x）
- 列点击排序（名称/路径/大小/修改时间），过滤器下拉，GDI 结果列表（滚动重影修复）
- 搜索语法：`|` OR、`!` NOT、`<>` 分组、通配符、`ext:` `file:` `folder:` `path:` `size:` `dm:`、`regex:`
- 系统右键、拖放、删除/重命名；选项（常规/键盘/NTFS/排除）；深色菜单
- （未做：D2D 绘制、chip 开关、完整选项页）

### M3 服务与 MFT 索引 ✅（随 0.2.x 提前交付）
- `-svc` / 安装卸载服务；MFT + USN；数据库持久化与 `-reindex`；命名管道

### M3.1 匹配与效率 ✅（0.3.0）
- A 匹配开关：区分大小写 / 全字匹配 / 匹配路径（搜索菜单 + ini）
- B 扩展语法：`startwith:` `endwith:` `exact:` `parent:` `depth:` `nopath:`（及别名）
- C 扩展名列：解析自文件名、显隐、按扩展名排序（无 Entry/DB 扩列）
- D 剪切/复制（CF_HDROP）/粘贴；「复制到…」「移动到…」（IFileOperation）
- E 搜索历史：`%APPDATA%\FlashFind\history.txt`，开关/清空，↑ 浏览
- F 命令行：`-s/-p/-filter/-sort/-select/-reindex/-exit/-help`（及 `-url`）
- G `es:` 协议 + 文件夹右键「用闪寻搜索」（选项「常规」+ 工具菜单安装/卸载）

### M4 效率功能（0.4）
- 运行历史、书签、过滤器管理；多窗口、预览窗格；EFU / 导出
- （命令行/`es:`/复制移动到 — 已并入 0.3.0）

### M5 发布（1.0）
- 安装包（便携版 + 安装程序，开机启动、快捷方式、服务安装）
- 多语言（中 / 英）、代码签名、自动更新检查
- 性能基准：500 万文件冷启动 < 2 s（数据库），搜索 < 50 ms

### 之后
- 文件内容搜索（IFilter / UTF-8）、缩略图视图、重复文件、媒体属性
- ReFS、HTTP / ETP 服务器
