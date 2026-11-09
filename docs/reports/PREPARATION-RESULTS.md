# 本轮准备结果

日期：2026-11-09。范围：环境准备与开发方案，不包括终端显示器功能实现。

## 已完成与实测结果

| 检查 | 结果与证据 |
| --- | --- |
| 项目目录 | 顶层 Git 仓库 `main`；预留会话、合成、输入、Kitty 后端、Wine 补丁与 smoke 程序目录 |
| Wine 源码 | `sources/wine/` 独立浅克隆；标签 `wine-11.19`；分支 `winetk/11.19`；HEAD `455e3509b98a6919fd4ad1def4803e08c41c03b2`；源码工作区干净 |
| 归档完整性 | 官方 `wine-11.19.tar.xz` 经 `xz -t` 与 SHA-256 检查通过，哈希与 `config/wine-source.env` 一致；另保留 `.sign`，未完成 GPG 身份验证 |
| 系统依赖 | 已实际执行 `pkexec pacman -S --needed --noconfirm`；包管理器事务成功；版本快照位于 `logs/dependency-versions.txt` |
| 依赖发现 | `scripts/check-environment.sh` 通过，检查双架构 MinGW 工具链与 FreeType/fontconfig/GnuTLS/PNG/JPEG/zlib/unwind/ALSA/Pulse 开发库 |
| 脚本语法 | 对每一个 `scripts/*.sh` 分别运行 `bash -n`，通过 |
| 源码准备幂等性 | 重新运行 `scripts/fetch-wine.sh`，保留已有 checkout、分支和源码，正确报告固定基线已就绪 |
| Wine 配置 | `scripts/configure-wine-gdi.sh` 返回 0；x86_64 Unix 主体，`PE_ARCHS = i386 x86_64` |
| 显示配置 | 已确认 Makefile 的 `DISABLED_SUBDIRS` 包含 `winex11.drv`、`winewayland.drv`；config.h 未启用对应宿主库及 GL/EGL/Vulkan soname |
| Wine 代表性编译 | `tools/winebuild/winebuild`、`server/wineserver` 成功生成；`dlls/win32u/dce.o`、`driver.o`、`window.o` 编译通过；本次日志没有编译 warning/error |
| Windows 链接验证 | 双架构 MinGW 均以 `-Wall -Wextra -Werror` 编译并链接调用 `GetSystemMetrics` 的程序；`file` 分别确认为 i386 PE32、x86-64 PE32+ |

代表性编译命令：

```sh
make -C build/wine-gdi -j8 \
  tools/winebuild/winebuild server/wineserver \
  dlls/win32u/dce.o dlls/win32u/driver.o dlls/win32u/window.o
```

日志为 `logs/build-preflight.log`。工具链检查源码和产物在 `build/toolchain-check/`，均为可丢弃的准备产物，没有运行 Windows 测试程序。

## 环境变化

本轮包事务升级了 clang/clang-libs/lld、cmake、pkgconf、python、freetype2、libjpeg-turbo、libpulse、curl、strace；其余清单中的包已经满足依赖。
主要工具版本：GCC 16.2.1、Clang/LLD 23.1.4、MinGW GCC 16.2.0、CMake 4.4.5、Python 3.14.9。
系统 Wine 为 11.19-1，Kitty 为 0.49.2-1。

Wine configure 还报告缺少 OpenCL、SANE、gphoto2、CAPI，以及 OSS 版本过旧。这些是本轮 GDI/终端路径以外的可选功能，不影响当前配置或代表性编译，未为此额外安装。

## 尚未验证或实现

没有构建完整 Wine，也没有运行项目 Wine；没有实现 `wineterm.drv`、会话进程、Kitty 输出或键鼠桥接。
未对 Wine GUI 做“无 X11/Wayland”运行时验收；当前证据仅限构建配置。未完成真实终端的画面、输入、帧率或恢复测试。
`terminal-browser` 已检查 CLI，但当前 PTY 无真实 Kitty 控制通道，未能使用浏览器查阅页面；本轮采用本机协议文档与 Wine 源码核对设计。

下一步按 [开发路线](../ROADMAP.md) 进入 M1：独立 Kitty 画布与键鼠原型。
