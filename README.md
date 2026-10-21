# 闪寻 FlashFind

Windows 上的极速文件名搜索工具，交互参考 voidtools Everything 1.4。原生 Win32 + C++20，只依赖 Windows SDK（不用 Qt / MFC / 第三方库），单个 `FlashFind.exe`。

> 当前版本：**0.2.1（M2：搜索语法、正则、排序、文件操作）**。界面设计见 [docs/UI_DESIGN.md](docs/UI_DESIGN.md)，视觉稿见 `docs/ui_mockup_v2.html`；路线图见 [PRODUCT_PLAN.md](PRODUCT_PLAN.md)。

## 已实现（M1）

- 主窗口：搜索框 + 虚拟列表（`LVS_OWNERDATA`，百万行）+ 自绘状态栏；列：名称 / 路径 / 大小 / 修改时间。
- 输入即搜：后台线程搜索，新输入自动取消旧搜索；空格分隔多个词（AND），`"..."` 短语，含 `\` 的词匹配完整路径；不区分大小写；匹配词高亮。
- 进程内索引：启动时后台遍历所有固定磁盘（不跟随联接点，可配置排除目录），完成后按名称预排序。
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
| F5 | 重建索引 |
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
  Index.*                 进程内文件夹遍历索引（M1）
  Search.*                后台搜索线程
  Settings.*              ini 读写
  Theme.*                 浅/深色调色板、DWM（深色标题栏、Mica、圆角）
  Tray.*  Hotkey.*        托盘图标；全局热键 + 双击 Ctrl
  ServiceClient.*         [桩] 命名管道客户端（M3）
  MftIndexer.*            [桩] NTFS MFT + USN 索引器（M3）
resources/                app.manifest、图标、版本信息
docs/                     UI 设计、原型
```

## 已知限制（M1）

- 索引在每次启动时重新遍历，不持久化，不监视变化（F5 手动重建）；大盘首次需要几十秒到数分钟。
- 普通文件按扩展名取图标（exe/lnk 无专属图标）。
- 只能按名称排序；无过滤器、书签、高级搜索、选项对话框。
- 菜单栏在深色模式下仍为浅色（Win32 限制，计划 M2 处理）。

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
