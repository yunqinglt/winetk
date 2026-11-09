# WineTK

把 Windows 应用的画面直接投放到终端，并将终端键鼠事件送回 Wine。Wine 侧不创建 X11/Wayland 窗口，也不通过宿主截图转发；终端模拟器本身可以运行于桌面环境。

目前支持 **GDI / OpenGL / WineD3D D3D8、D3D11 → 离屏帧 → Unix socket → Kitty**。已实测东方红魔乡、Unity Mono 的 64.0 和 Unity IL2CPP 的 Limbus Company 标题/登录界面。Vulkan 当前仅用于 WoW64 内存互操作，尚无 Vulkan swapchain 呈现；完整多窗口语义、Limbus 视频/资源更新/战斗仍待验收。

## 快速开始

```sh
git clone https://github.com/yunqinglt/winetk.git
cd winetk
./winetk                   # 无参数显示帮助
./winetk --steam           # 扫描已安装的 Windows Steam 游戏，打开 TUI
./winetk --settings        # TUI 设置；S 保存，Esc 放弃未保存修改
./winetk --steam --list    # 仅输出 JSON 游戏列表，不需要交互终端
```

启动器使用 Python 3.10+ 标准库 `curses`。浏览列表和设置不需要先构建 Wine；启动 Windows 应用则需要下述项目 Wine 和支持 Kitty 图形、键盘协议的终端。

Steam 列表：方向键选择，Enter 进入详情并启动，`/` 搜索，`R` 重扫，`S` 设置，`Q` 退出。游戏结束后返回列表；游戏内 **Ctrl+Shift+F12** 结束本次终端会话。Linux Steam 须保持登录，游戏须已安装 Windows 版本；商店支持 Windows 不代表本机安装了 Windows 版本。

## 构建

支持目标：Linux x86-64、现代 Wine WoW64。完整依赖及配置见 [开发指南](docs/DEVELOPMENT.md)。Arch Linux 可执行依赖安装脚本（通过 `pkexec pacman` 安装所列包）：

```sh
./scripts/install-deps-arch.sh
./scripts/check-environment.sh
./scripts/fetch-wine.sh       # 拉取 config/wine-source.env 固定的 Wine 11.19
./scripts/build-wine.sh       # 应用补丁，无 X11/Wayland，构建 Wine 与测试 EXE
./winetk --steam
```

Wine 从源码构建于项目内，不替换系统 Wine。运行时需要支持 surfaceless EGL/pbuffer 的 OpenGL 驱动；构建依赖含 Vulkan，以支持 WoW64 缓冲映射。Steam bridge 默认借用已安装的 Proton Experimental 库，适配版本和限制见 [Unity 记录](docs/reports/M4-UNITY-RESULTS.md)。

仅构建终端核心和运行自动测试（不需要 Wine、Steam、GPU 或游戏）：

```sh
cmake -S . -B build/check -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/check
ctest --test-dir build/check --output-on-failure
```

依赖为 C11 编译器、CMake、Ninja、pkg-config、libpng、zlib 和 Python；详见 [测试指南](tests/README.md)。

## 配置与显示

配置默认位于 `${XDG_CONFIG_HOME:-~/.config}/winetk/config.json`；用 `--config PATH` 指定其他文件。支持帧率、缩放方式、Kitty 传输方式、额外 Steam 路径和 Proton 路径。配置在保存时校验并原子写入；无参数帮助和列表扫描不会创建配置文件。[完整配置说明](docs/TUI.md)。

- `native`：保留源像素，终端较小时等比缩小。
- `zoom-in`：允许双线性等比放大。
- `fit-resolution`（启动器默认）：Unity 按启动时的终端可用区域提高实际渲染分辨率，目前最高 1920×1080。运行中改变终端大小仅缩放输出，重启后重新选择渲染尺寸。

所有模式保留顶部程序名一行，鼠标使用相同的坐标变换。`fps` 是终端提交上限，不代表稳定游戏帧率或实际显示帧率。

```sh
./winetk --demo
./winetk --run /absolute/path/program.exe -- argument1
```

原有底层脚本保留，不读取 TUI 配置，可直接传参数：

```sh
./scripts/run-terminal-demo.sh --fit-resolution
./scripts/run-wine.sh -- /absolute/path/program.exe
./scripts/run-unity.sh 64 --fit-resolution
./scripts/run-unity.sh limbus --fit-resolution
./scripts/run-th06.sh --fps 60
```

Steam 准备脚本在 `out/` 创建游戏副本，在 `prefixes/` 创建专用 Wine 前缀。首次使用 reflink，不支持时需要实际游戏大小的磁盘空间；已有副本不自动同步 Steam 更新。Limbus 还会导入原 Proton 前缀的资源缓存独立副本，本机约 17.55 GB。原安装与原 prefix 保留。游戏数据、Steam/Proton DLL、日志和账号信息不进入此仓库。

## 项目布局

| 路径 | 用途 |
| --- | --- |
| `winetk`, `src/tui/` | Python 命令行入口、curses TUI、Steam 发现和本地配置 |
| `src/demo/` | C 终端会话入口、测试场景、像素缩放 |
| `src/session/` | Wine IPC/合成、进程启动、终端生命周期和 Steam 辅助进程 |
| `src/input/`, `src/terminal/backends/kitty/` | 输入解析、坐标映射、Kitty 图形后端 |
| `include/winetk/` | 公共接口与 IPC 协议 |
| `patches/wine/` | 固定 Wine 基线上的驱动与集成补丁；不嵌入 Wine 源码 |
| `config/`, `scripts/` | Wine 版本锁定、构建、准备和运行脚本 |
| `tests/`, `tests/smoke/` | 单元/PTY 测试、可选真实终端测试、Win32 测试源码 |
| `docs/`, `docs/reports/` | 使用/架构文档和历史验证记录 |
| `.github/workflows/` | 无游戏依赖的普通及 ASan/UBSan 自动测试 |

本地工作目录 `sources/`、`downloads/`、`build/`、`out/`、`prefixes/`、`game/`、`logs/` 由 `.gitignore` 排除。报告中的本地证据路径用于复测定位，日志与截图不随源码分发。

## 文档与贡献

- [文档索引](docs/README.md)：使用、架构、路线和历史验证。
- [贡献指南](CONTRIBUTING.md)：本地检查、Wine 补丁导出和 PR 说明。
- [许可证](LICENSE)：独立项目代码采用 GPL v3；Wine 补丁保留其 LGPL 等原有许可，详见 [第三方说明](NOTICE.md)。
