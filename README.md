# 闪寻 FlashFind

Windows 上的极速文件名搜索工具，交互参考 voidtools Everything 1.4。原生 Win32 + C++20，使用 Windows SDK 与静态链接的 PCRE2，单个 `FlashFind.exe`，无需另装运行库。

> 当前版本：**0.3.4（名称排序提速、增量更新修复）**。更新与性能实测见 [发布说明](docs/RELEASE_0.3.4.md)。界面设计见 [docs/UI_DESIGN.md](docs/UI_DESIGN.md)，路线图见 [PRODUCT_PLAN.md](PRODUCT_PLAN.md)。

## 主要功能

- 主窗口：搜索框 + 虚拟列表（`LVS_OWNERDATA`，百万行）+ 自绘状态栏；列：名称 / 路径 / 大小 / 修改时间。
- 输入即搜：后台线程搜索，新输入自动取消旧搜索；空格分隔多个词（AND），`"..."` 短语，含 `\` 的词匹配完整路径；不区分大小写；匹配词高亮。
- 磁盘缓存：启动先加载上次索引；NTFS 通过服务或管理员权限读取 MFT，并按卷追踪 USN 增量。缺少权限时保留旧数据并提示安装服务。
- 增量更新：按批处理创建、删除、改名与大小/修改时间变化；支持按卷设置监视、USN Journal、仅包含目录及全局排除目录。
- 名称结果复用索引中的顺序，切换降序只需反转；也支持按路径、扩展名、大小、修改时间排序。
- 结果操作：Enter 打开、Ctrl+Enter 打开所在位置并选中、双击打开、右键菜单（打开 / 打开路径 / 复制完整路径 / 复制名称 / 属性）、Ctrl+Shift+C 复制路径、键入名称跳转。
- 全局热键 **Ctrl+Alt+Space** 切换显示/隐藏（被占用时托盘气泡 + 状态栏红字提示）；**双击 Ctrl** 切换（`WH_KEYBOARD_LL`，可在“工具”菜单关闭）。
- 显示时移动到鼠标所在显示器、聚焦并全选搜索框；Esc 先清空、再隐藏到托盘；关闭按钮隐藏到托盘；托盘左键切换、右键菜单。
- 单实例：再次启动只会唤出已有窗口。`-startup` 参数启动时隐藏。
- 主题：跟随系统 / 浅色 / 深色；标题栏 DWM 深色；Win11 圆角 + Mica，Win10 回退实色。
- PerMonitorV2 DPI、comctl32 v6、长路径（清单内声明）。
- 设置保存在 `%APPDATA%\FlashFind\FlashFind.ini`（窗口位置、列宽、主题、热键、索引根目录/排除目录）。

## 构建

需要 Visual Studio 2022（或 Build Tools）的“使用 C++ 的桌面开发”、CMake ≥ 3.21、Ninja（VS 自带）。

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build.ps1            # Release
# 或手动：在 “x64 Native Tools Command Prompt” 中
cmake -S . -B build\Release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build\Release
```

产物：`build\Release\FlashFind.exe`（静态 CRT，可直接拷走运行）。

## 运行与快捷键

| 键 | 作用 |
|---|---|
| Ctrl+Alt+Space / 双击 Ctrl | 显示 / 隐藏（全局） |
| ↓ / PgDn（搜索框中） | 进入结果列表 |
| Enter / Ctrl+Enter | 打开 / 打开所在位置 |
| Alt+Enter | 属性 |
| Ctrl+Shift+C | 复制完整路径 |
| Ctrl+F / F3 / Alt+D | 聚焦搜索框 |
| Esc | 清空搜索；为空时隐藏到托盘 |
| F5 / Ctrl+F5 | 刷新当前结果与选中项属性 / 重建索引 |
| Ctrl+W / Ctrl+Q | 隐藏 / 退出 |

## ini 示例

```ini
[index]
roots=C:;D:
exclude_folders=C:\Windows\WinSxS;C:\$Recycle.Bin
[hotkey]
toggle_window_key=Ctrl+Alt+Space
toggle_window_double_ctrl=1
[ui]
theme=0   ; 0 跟随系统 1 浅色 2 深色
```

## 目录结构

```
src/
  main.cpp App.*          入口、单实例、消息循环
  MainWindow.*            主窗口、布局、菜单、快捷键、托盘/热键分发、结果操作
  ResultList.*            虚拟 ListView、自绘（隔行/选中强调条/匹配高亮）
  Index.* Db.* Usn.*       快照、缓存、USN 增量更新与目录过滤
  Search.*                后台搜索线程
  Settings.*              ini 读写
  Theme.*                 浅/深色调色板、DWM（深色标题栏、Mica、圆角）
  Tray.*  Hotkey.*        托盘图标；全局热键 + 双击 Ctrl
  NtfsSource.* Service.*   命名管道客户端、特权读取与服务管理
  MftIndexer.*            NTFS MFT + USN 索引器
tests/                    优化回归测试、排序性能基准
resources/                app.manifest、图标、版本信息
docs/                     UI 设计、原型
```

## 已知限制

- NTFS 首次建立与实时更新需要服务或管理员权限；未安装服务的普通权限进程只能使用已有缓存。
- 同一 FRN 当前保留一个名称，尚未索引全部硬链接路径。
- 暂不支持 ReFS 原生索引、文件内容搜索、HTTP / ETP 服务器。

## 通配符

与 Everything 相同：`*` 匹配任意多个字符，`?` 匹配一个字符。含通配符的词匹配**整个文件名**（如 `1*.exe`、`*.exe`、`a?c`）；词中含 `\` 时匹配完整路径（如 `*\windows\*.dll`）。不含通配符的词仍按子串匹配，可与其他词（AND）和 `"..."` 组合。

## FlashFind 索引服务

读取 NTFS 主文件表和带文件名的 USN 记录需要管理员权限。闪寻可安装一个 LocalSystem 服务（`FlashFind.exe -svc`），只负责特权读取，索引、排序与数据库仍在普通权限的界面进程中：

- 程序内：**工具 → 安装 / 卸载 FlashFind 服务…**，或状态栏盾牌按钮（触发 UAC，完成后无需重启）。
- 命令行（管理员）：`FlashFind.exe -install-service` / `FlashFind.exe -uninstall-service`。
- 服务日志：`%ProgramData%\FlashFind\FlashFind-service.log`。

## 自检

```powershell
ctest --test-dir build\Release --output-on-failure   # -initest、-incrtest（无数据库时使用合成的 226 万对象）
FlashFind.exe -optimizationtest                      # 元数据、目录过滤、通配符与排序缓存回归
FlashFind.exe -optimizationtest -bench               # 226 万对象；优化前后同数据、同结果的排序基准
FlashFind.exe -pipetest G:                            # 启动 -svc-console 测试管道协议（需要 NTFS 卷）
```

## 发布

推送 `v*` 标签后，GitHub Actions（`.github/workflows/release.yml`）会构建、运行自检、用 `scripts/package.ps1` 打包（Inno Setup 安装包 + 便携版 zip + SHA256SUMS），并以 `docs/RELEASE_<版本>.md` 为说明发布 Release。本地打包：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build.ps1
powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -BuildDirectory build\Release
```

## 许可

MIT，见 [LICENSE](LICENSE)。
