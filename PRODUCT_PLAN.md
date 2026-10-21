# 闪寻 FlashFind 产品计划

定位：Everything 1.4 的开源式替代，原生 Win32，零第三方依赖，界面现代化（Fluent），支持 Win10 / Win11。

## 已定技术决策

| 项 | 决定 |
|---|---|
| 语言 / 构建 | C++20，CMake + MSVC（Ninja），静态 CRT |
| 界面 | 原生 Win32 + comctl32 v6；自绘用 Direct2D / DirectWrite；保留系统标题栏（DWM 深色） |
| 系统 | Win10 + Win11；Mica / 圆角仅 Win11，Win10 实色回退 |
| 索引 | 后台 Windows 服务读取 NTFS MFT + USN Journal，界面通过命名管道通信；无服务时回退文件夹遍历 |
| 热键 | Ctrl+Alt+Space 切换 + 双击 Ctrl（WH_KEYBOARD_LL），v1 均提供 |
| 推迟 | HTTP / ETP 服务器、ReFS、文件内容搜索、缩略图 |

## 里程碑

### M1 可用原型 ✅（0.1.0）
- 工程骨架、清单（PerMonitorV2、comctl32 v6）、ini 设置
- 主窗口：搜索框、虚拟列表、状态栏；浅/深色
- 进程内遍历索引 + 子串搜索（AND、短语、路径匹配）、匹配高亮
- 托盘、单实例、全局热键 + 双击 Ctrl、多显示器显示
- 打开 / 打开路径 / 复制路径 / 属性 / 右键菜单

### M2 界面完善（0.2）
- Direct2D/DirectWrite 绘制结果行（圆角选中、强调条、抗锯齿高亮）、搜索框匹配开关 chip
- 列点击排序（名称/路径/大小/扩展名/修改时间，升降序），列显隐（表头右键）
- 过滤器下拉（全部/音频/压缩/文档/可执行/文件夹/图片/视频）
- 搜索语法：`|` OR、`!` NOT、`<>` 分组、通配符 `* ?`、`ext:` `file:` `folder:` `path:` `size:` `dm:`、`regex:`；匹配开关（大小写、整词、路径、正则）
- 系统右键菜单合并（IContextMenu）、拖放、剪切/复制/删除/重命名
- 异步图标加载（exe/lnk 真实图标）
- 选项对话框（常规、界面、搜索、结果、视图、字体颜色、键盘含热键捕获与冲突检测、历史、索引、排除）
- 深色菜单栏、高对比度适配

### M3 服务与 MFT 索引（0.3）
- `FlashFind.exe -svc` 服务模式；`-install-service / -uninstall-service / -start-service / -stop-service`
- MFT 枚举（FSCTL_ENUM_USN_DATA）+ USN 增量监听；大小/时间补全
- 数据库持久化（压缩、USN 检查点、启动秒开），`-reindex`
- 命名管道协议（查询 / 订阅变更），安全描述符；界面自动选择服务或本地遍历
- 文件夹索引：监视（ReadDirectoryChangesW）、定时重扫；可移动盘自动加入/移除

### M4 效率功能（0.4）
- 搜索历史 / 运行历史（运行次数排序）、书签、过滤器管理（导入导出）
- 多窗口、预览窗格（IPreviewHandler）
- 文件列表 EFU（打开/创建/编辑器）、导出 CSV/TXT
- 命令行（`-s -p -filter -bookmark -select -sort ...`）、`es:` URL 协议、文件夹右键“用闪寻搜索”
- 多文件重命名、复制/移动到文件夹

### M5 发布（1.0）
- 安装包（便携版 + 安装程序，开机启动、快捷方式、服务安装）
- 多语言（中 / 英）、代码签名、自动更新检查
- 性能基准：500 万文件冷启动 < 2 s（数据库），搜索 < 50 ms

### 之后
- 文件内容搜索（IFilter / UTF-8）、缩略图视图、重复文件、媒体属性
- ReFS、HTTP / ETP 服务器
