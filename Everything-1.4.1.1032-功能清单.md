# Everything 1.4.1.1032 功能清单

调研对象：`D:\Program Files\Everything\Everything.exe`  
产品：voidtools Everything  
版本：1.4.1.1032（FileVersion / ProductVersion）  
日期：2026-10-21

本文根据已安装程序的静态证据整理用户能碰到的功能。字符串和命令行帮助说明程序里有这些入口；没有逐项启动验证，因此不能当作“当前这份安装已经打开”的证明。

## 1. 程序身份

| 项 | 值 |
|---|---|
| 路径 | `D:\Program Files\Everything\Everything.exe` |
| 大小 | 2,272,424 字节 |
| SHA-256 | `f191f756996a14a11e5445fa7103d302efd510cf2fbf920e6c0c8ed51d512e36` |
| 格式 | PE32+，x86_64，原生程序，不是 .NET |
| 镜像基址 | `0x140000000` |
| 公司 / 产品 | voidtools / Everything |
| 版权 | Copyright 2026 voidtools |
| 清单描述 | Everything Search Engine |
| 执行级别 | `asInvoker` |
| DPI | 清单声明 `dpiAware` |
| 界面库 | Microsoft.Windows.Common-Controls 6.0.0.0 |

Ghidra 12.1.4 概览：约 3070 个函数，4841 条已定义字符串。程序引用 `Everything.pdb`（GUID `d13f8d42-d89b-434f-8796-bf246e9da13a`），本次未加载，函数没有原始符号名。

同一安装目录还有：

| 文件 | 作用 |
|---|---|
| `Everything.ini` | 仅 215 字节。注明设置实际在 `%APPDATA%\Everything\Everything.ini`。当前值为 `app_data=1`、`run_as_admin=0`、`allow_http_server=1`、`allow_etp_server=1` |
| `Everything.lng` | 约 1 MB，头部后是 bzip2。解开后约 4.8 MB，含 48 种界面语言。英文原文在 exe 里 |
| `Changes.txt` | 更新日志。1.4.1.1032 的条目是修复以点号结尾的文件启动时的安全问题 |
| `License.txt` | 许可证 |
| `Uninstall.exe` | 卸载程序 |

## 2. 证据与界限

分析用 REA 6.7.0 命令行和 Ghidra 12.1.4。本会话没有连上 REA 的 MCP 工具。

| 操作 | 证据 ID |
|---|---|
| `inspect_pe_resources` | `ev_9a0d25b8cf495337d73d77f7b5c586939d6a7fbf228a307378598a4c0906b439` |
| Ghidra `binary_overview` | `ev_b67cae01ef31e8a858be49a6c9781511cb1e9a50d44b84cb5eb45a5970f82a4f` |
| Ghidra `search_strings`（正则 `.`，4838 条） | `ev_e9ff3426110e9187e9358ff9659a7fc68503e47efbb6395c13882fb23823821a` |

资源目录共 23 项，覆盖完整：位图 1、图标 11、图标组 1、RCDATA 8、版本信息 1、清单 1。没有菜单、对话框或字符串表，界面由程序自己绘制。REA 只解释图标组；版本信息和清单是按它标出的文件范围解开的。

RCDATA 103（1597 字节）是 HTTP 结果页的 CSS，类名包括 `indexof`、`numresults`、`searchbox`、`nameheader`、`pathheader`、`sizeheader`、`modifiedheader`、`prevnext`、`updir`。RCDATA 104–109 是 GIF，110 是图标数据。

界限：

- 没有启动程序，下文是界面文字和内嵌帮助，不是逐项操作结果。
- Ghidra 只列出它识别出的字符串。未识别字节里仍可能有文字。正则 `.` 不包含空字符串，所以 4841 与 4838 的差是空项和未命中项，不是另一套功能。
- 已定义字符串里混有 Windows API 名。本文只收用户能碰到的功能。
- 选项页名称能直接对上的有 General、Search、Results、View、Context Menu、Fonts and Colors、Keyboard、History、Indexes、NTFS、Folders、File Lists、Exclude。HTTP、ETP/FTP、ReFS、Home 出现在菜单和设置名里，不一定各占一个同名页。

## 3. 搜索与结果

- 输入即搜。
- 匹配开关：大小写、变音符号、整词、全路径、正则。
- 搜索词里出现路径分隔符时，再按路径匹配。
- 使用通配符时，可要求匹配整个文件名。
- 快速 ASCII 搜索。
- 字面量运算符 AND、OR、NOT，以及 `!`。
- 圆括号分组。
- 运算符优先级可配置。
- 高级搜索：全部这些词、任意这些词、排除这些词、精确短语、所在位置、修改日期、大小、扩展名、文件中的词或短语。
- 文件内容：优先用关联的 IFilter；没有 IFilter 时按 UTF-8。另有 ANSI、UTF-8、UTF-16、Unicode、宽字符内容标记。
- 空搜索可以隐藏结果。
- 搜索条件变化时可以清除当前选择。
- 回车时可以把焦点放到运行次数最高的结果。

搜索框中出现的 `名称:` 标记：

| 类别 | 标记 |
|---|---|
| 名称与位置 | `file:` `folder:` `path:` `nopath:` `parent:` `parents:` `infolder:` `child:` `shell:` `startwith:` `endwith:` `exact:` |
| 匹配方式 | `regex:` `wildcards:` `wholeword:` `wholewords:` `wholefilename:` `case:` `diacritics:`，以及对应的 `no*` |
| 大小与计数 | `size:` `count:` `depth:` `runcount:` `childcount:` `childfilecount:` `childfoldercount:` |
| 时间 | `datemodified:` `datecreated:` `dateaccessed:` `daterun:` `recentchange:`，以及对应的 `*dupe:` |
| 属性与重复 | `attrib:` `attribute:` `attributes:` `dupe:` `dupeattrib:` `dupesize:` `dupenamepart:` |
| 内容 | `content:` `ansicontent:` `utf8content:` `utf16content:` `utf16lecontent:` `utf16becontent:` `unicodecontent:` `wcharcontent:` |
| 媒体与图像 | `title:` `artist:` `album:` `genre:` `track:` `comment:` `year:` `width:` `height:` `dimension:` `dimensions:` `orientation:` `bitdepth:` |

这些是程序里的标记，不是一份已经验证过的语法说明书。重复项功能要求对应信息已被索引，并且打开了快速排序。

书签可以记住搜索、大小写、变音、整词、路径、正则、排序、视图和索引。过滤器同样可以记住搜索、大小写、变音、整词、路径和正则。两者都能新建、编辑、删除、导入、导出和组织。

搜索历史：开关、保留天数或永久保留、建议、显示全部、排序。运行历史：开关、保留天数或永久保留、运行次数。

排序列：名称、路径、大小、扩展名、属性、创建时间、修改时间、访问时间、最近更改、运行日期、运行次数。大小和日期可以先按降序。快速排序覆盖 ASCII、路径、大小、扩展名、属性、修改/创建/访问时间。

视图：

- 详细信息。
- 小、中、大、超大缩略图，缩略图尺寸可设。
- 列宽自适应，可单独调整路径列和窗口宽高比。
- 整行选择、隔行变色、高亮匹配词、列表工具提示。
- 状态栏显示大小、选中项、结果数。
- 预览窗格，可显示 Windows 预览处理程序。

窗口与焦点：

- 置顶、全屏、最大化、最小化到托盘、贴齐桌面边缘。
- 多实例、多窗口；从托盘打开时可以新建窗口。
- 执行结果后关闭窗口。
- 激活时把焦点放回搜索框；鼠标点击时可选中搜索文字。
- 热键：新建窗口、显示窗口、切换窗口。
- 主页搜索，可分别记住过滤器、索引和各项匹配开关。

能直接对上的选项页：General、Search、Results、View、Context Menu、Fonts and Colors、Keyboard、History。

## 4. 对结果的操作

- 打开、打开路径；双击路径列可打开路径。
- 在资源管理器中定位。
- 属性、删除、重命名。重命名时可以不选中扩展名。
- 复制、剪切、粘贴、拖放。
- 复制名称、完整路径、带引号的路径、CSV。
- 全选、反选。
- 复制到文件夹、移动到文件夹。
- 多文件重命名，支持正则和大小写。
- 创建快捷方式。
- 右键菜单可使用外壳扩展，也可放自定义动词（设置名 `custom_verb01` 到 `custom_verb12`）。
- 某项可设为不显示，或只在按住 Shift 时显示。
- 文件夹右键可加入 “Search Everything”。
- 打开文件、打开文件夹、浏览所用的命令行可以改。

## 5. 索引

本地数据库可以压缩后保存、强制重建、只读加载，并指定数据库路径。另有多用户数据库文件名。

NTFS：

- 按卷决定是否纳入索引，并监视变化。
- 从 USN Journal 读取最近更改。
- 创建或删除某个卷的 USN Journal。
- 不索引 NTFS 时，程序提示需要管理员权限，或安装 Everything 服务。

ReFS：设置名里有一套与 NTFS 对应的卷、监视和最近更改项（`refs_volume_*`）。

文件夹索引：

- 立即重扫、全部重扫、按间隔重扫。
- 缓冲区满时重扫。
- 监视变化。
- 可限制深度，并用通配决定包含或排除。

文件列表（EFU）：

- 打开、关闭、监视变化、使用相对路径。
- 同一 exe 包含 File List Editor：新建、打开、保存、另存、导入、导出、添加文件或文件夹、关闭。
- 关于框标题为 “About Everything File List Editor”。

可索引的扩展信息：文件大小、文件夹大小、创建时间、修改时间、访问时间、属性、最近更改。图标、缩略图和扩展信息各有加载优先级。

排除列表：文件、文件夹、隐藏文件和文件夹、系统文件和文件夹。

卷管理：新的固定盘和可移动盘可以自动加入；离线或移走的 NTFS/ReFS 卷可以自动移除。可以扫描盘符，并在可移动卷上查找挂载点。搜索和索引时可以展开环境变量。

能直接对上的选项页：Indexes、NTFS、Folders、File Lists、Exclude。

## 6. 远程访问

HTTP 服务：

- 开关、绑定接口、端口、用户名、密码。
- 默认页、站点根目录、每页条数、标题格式。
- 允许下载。
- 日志文件、最大大小和增长量。
- 结果页样式见 RCDATA 103：搜索框、名称、路径、大小、修改时间、上一层目录、分页。

ETP/FTP 服务：

- 开关、绑定接口、端口、用户名、密码、欢迎信息。
- 允许远程客户端下载。
- 日志。
- 客户端连接形式：`用户名:密码@主机:端口`。
- 可以断开，并按规则重写路径。
- 链接形式：`C:`、`\\Server\C$`、`\\Server\C:`、`ftp://host/C:`。

安装目录里的 ini 把 `allow_http_server` 和 `allow_etp_server` 设为 1。这只代表这份安装的开关。

## 7. 安装、实例与权限

- 命名实例。多个 Everything 可以用不同实例名并存。
- Everything 服务通过命名管道工作，可指定管道名和安全描述符。另有客户端服务，可安装、启动、停止、卸载。
- 普通权限运行，或用服务索引 NTFS。也可以改为以管理员运行。
- 开机启动。
- 桌面快捷方式、开始菜单快捷方式、快速启动快捷方式。
- EFU 文件关联。
- URL 协议，帮助文本写作 `es:` 搜索。
- 选择界面语言；选择是否自动索引卷。
- 数据可放在程序目录，或放在应用数据目录。
- 可指定 ini 文件和数据库文件。
- 启动时检查更新，也可以关掉更新通知。设置名里还有 `beta_updates`。
- 关于框：版本、许可证、致谢、网站、捐赠。

## 8. 外观与输入

- 字体和字号，图标标题可加下划线。
- 前景色、背景色、隔行颜色、预览背景色。
- 工具栏使用系统主题。
- 选区可以是半透明遮罩或 XOR。
- 搜索框和列表有从右到左阅读、反转布局相关设置。
- 状态栏、筛选条、预览窗格可分别显示。
- 鼠标滚轮配合 Ctrl 的动作可配置。
- 单击打开的行为、单击托盘图标的行为可配置。
- 详细列表的工具提示可单独开关。

## 9. 命令行

内嵌帮助的用法行是：

```text
Everything.exe [filename] [-options]
```

`filename` 表示打开指定的文件列表。

### 搜索与窗口

| 开关 | 帮助中的含义 |
|---|---|
| `-s`、`-search <text>` | 设置搜索 |
| `-filename <filename>` | 按文件名搜索文件或文件夹 |
| `-name-part <filename>` | 搜索文件名的名称部分 |
| `-p`、`-path <path>` | 搜索路径 |
| `-parent <path>` | 搜索指定文件夹中的文件和文件夹 |
| `-parentpath <path>` | 搜索某路径的父路径 |
| `-filter <name>` | 选择过滤器 |
| `-bookmark <name>` | 打开书签 |
| `-home` | 打开主页搜索 |
| `-select <filename>` | 聚焦并选中指定结果 |
| `-search-file-list <filename>` | 按文本文件中的文件名列表搜索 |
| `-case` / `-nocase` | 打开或关闭大小写匹配 |
| `-diacritics` / `-nodiacritics` | 打开或关闭变音符号匹配 |
| `-wholeword`、`-ww` / `-nowholeword`、`-noww` | 打开或关闭整词匹配 |
| `-regex` / `-noregex` | 打开或关闭正则 |
| `-matchpath` / `-nomatchpath` | 打开或关闭全路径匹配 |
| `-sort <name>` | 设置排序 |
| `-sort-ascending`、`-sort-descending` | 升序或降序 |
| `-details` | 详细信息视图 |
| `-thumbnails`、`-thumbnail-size <size>` | 缩略图视图及像素尺寸 |
| `-focus-results` | 聚焦结果列表 |
| `-focus-top-result`、`-focus-bottom-result` | 聚焦顶部或底部结果 |
| `-focus-last-run-result`、`-focus-most-run-result` | 聚焦上次运行或运行最多的结果 |
| `-newwindow` / `-nonewwindow` | 新建窗口，或使用已有窗口 |
| `-close` | 关闭当前搜索窗口 |
| `-toggle-window` | 隐藏当前前台搜索窗口，或显示搜索窗口 |
| `-ontop` / `-noontop` | 打开或关闭置顶 |
| `-fullscreen` / `-nofullscreen` | 全屏或窗口 |
| `-maximized` / `-nomaximized` | 最大化或取消最大化 |
| `-minimized` / `-nominimized` | 最小化或取消最小化 |
| `-url <[es:]search>` | 从 ES: URL 设置搜索 |

### 数据与索引

| 开关 | 帮助中的含义 |
|---|---|
| `-l`、`-local` | 加载本地数据库 |
| `-db <filename>` | 指定数据库文件 |
| `-nodb` | 不从数据库文件读取，也不写入 |
| `-read-only` | 只读加载数据库 |
| `-reindex` | 强制重建数据库 |
| `-update` | 把数据库保存到磁盘 |
| `-rescan-all` | 重扫全部文件夹索引 |
| `-filelist <filename>` | 打开文件列表 |
| `-edit <filename>` | 用文件列表编辑器打开 |
| `-create-file-list <filename> <path>` | 为某路径创建文件列表 |
| `-create-file-list-exclude-files <list>` | 创建时排除文件通配，分号分隔 |
| `-create-file-list-exclude-folders <list>` | 创建时排除文件夹通配 |
| `-create-file-list-include-only-files <list>` | 创建时只包含这些文件通配 |
| `-create-usn-journal <volume> <max-size-bytes> <allocation-delta-bytes>` | 创建 USN Journal |
| `-delete-usn-journal <volume>` | 删除 USN Journal |
| `-choose-volumes` | 不自动索引卷 |
| `-load-delay <milliseconds>` | 延迟加载数据库 |
| `-config <filename>` | 指定 ini 文件 |

### 重命名

| 开关 | 帮助中的含义 |
|---|---|
| `-rename <filename1> <...>` | 打开多文件重命名 |
| `-copyto <filename1> <...>` | 以复制到文件夹的方式打开多文件重命名 |
| `-moveto <filename1> <...>` | 以移动到文件夹的方式打开多文件重命名 |

### 连接与服务

| 开关 | 帮助中的含义 |
|---|---|
| `-connect <user:pass@host:port>` | 连接 ETP 服务器 |
| `-admin` | 以管理员运行 |
| `-instance <name>` | 指定实例名 |
| `-svc` | 作为服务运行 |
| `-install-service`、`-start-service`、`-stop-service`、`-uninstall-service` | 安装并启动、启动、停止、卸载服务 |
| `-install-client-service`、`-start-client-service`、`-stop-client-service`、`-uninstall-client-service` | 客户端服务的对应操作 |
| `-service-pipe-name <name>`、`-svc-pipe-name <name>` | 连接或托管指定管道名 |
| `-install-service-security-descriptor`、`-svc-security-descriptor <sd>` | 指定管道安全描述符 |
| `-admin-server-share-links` | ETP 使用 `\\Server\C$` 链接 |
| `-drive-links` | ETP 使用 `C:` 链接 |
| `-server-share-links` | ETP 使用 `\\Server\C:` 链接 |
| `-ftp-links` | ETP 使用 `ftp://host/C:` 链接 |

### 安装、卸载与维护

| 开关 | 帮助中的含义 |
|---|---|
| `-install <location>` | 安装到新位置 |
| `-uninstall [path]` | 从指定路径卸载 |
| `-uninstall-user` | 卸载用户文件 |
| `-choose-language`、`-language <langID>` | 显示语言选择页，或设置语言 ID |
| `-app-data` / `-noapp-data` | 数据放在应用数据目录，或放在程序目录 |
| `-install-desktop-shortcut`、`-uninstall-desktop-shortcut` | 桌面快捷方式 |
| `-install-start-menu-shortcuts`、`-uninstall-start-menu-shortcuts` | 开始菜单快捷方式 |
| `-install-quick-launch-shortcut`、`-uninstall-quick-launch-shortcut` | 快速启动快捷方式 |
| `-install-efu-association`、`-uninstall-efu-association` | EFU 关联 |
| `-install-folder-context-menu`、`-uninstall-folder-context-menu` | 文件夹右键菜单 |
| `-install-run-on-system-startup`、`-uninstall-run-on-system-startup` | 开机启动 |
| `-install-url-protocol`、`-uninstall-url-protocol` | URL 协议 |
| `-enable-run-as-admin`、`-disable-run-as-admin` | 是否以管理员运行 |
| `-enable-update-notification`、`-disable-update-notification` | 启动时的更新通知 |
| `-startup` | 在后台运行 |
| `-exit`、`-quit` | 退出 |
| `-first-instance` | 仅在这是第一个实例时运行 |
| `-console`、`-debug` | 显示调试控制台 |
| `-debug-log` | 把调试信息写到磁盘 |
| `-verbose` / `-noverbose` | 全部调试信息，或只显示基本信息 |
| `-?`、`-h`、`-help` | 显示这份帮助 |

安装程序还会看到这些内部开关：`-install-config`、`-install-options`、`-save-install-options`、`-install-language`、`-isrunas`。它们出现在字符串里，不在上面那份面向用户的帮助表中。
