# 环境与开发说明

本文记录构建环境与开发方式。早期准备结果见 M0/M1 文档；当前 Wine 与游戏验收见 [M3 结果](M3-RESULTS.md)。

## 本轮环境

准备日期：2026-11-09。主机为 Arch Linux x86_64，包数据库已启用 multilib。
首版构建采用现代 WoW64：64 位 Unix 主体配合 i386/x86_64 PE 模块，不要求为此安装整套 lib32 图形依赖。
已有的系统 Wine 11.19、Kitty 0.49.2 保留，用于版本对照与后续真实终端验收。

已通过 `pkexec pacman -S --needed --noconfirm` 安装/确认：

```text
base-devel git curl xz autoconf clang lld mingw-w64-gcc
cmake ninja pkgconf flex bison python
freetype2 fontconfig gnutls libpng libjpeg-turbo zlib libunwind
alsa-lib libpulse strace
```

执行过程可能升级这些包及其必要依赖；没有运行全系统升级，也没有主动刷新包数据库。
当前机器已存在的可选库可能影响 configure 自动检测，复现时以 configure 摘要和 `config.log` 为准。
M3 已使用本机现有的 `libglvnd` 提供 EGL/OpenGL 头文件与加载器，依赖脚本已加入该包。
运行时还需支持 surfaceless EGL/pbuffer 的 GL 驱动；本机 NVIDIA Tesla V100 已实测，不要求 Wine 建立宿主窗口连接。
构建同时启用 Vulkan，依赖脚本包含 `vulkan-headers` 与 `vulkan-icd-loader`。它为 Wine WoW64 提供 OpenGL 缓冲的 placed-memory 映射，显著改善本机 32 位游戏性能；尚未提供 Vulkan WSI 转发。实际收益依赖 GPU 的外部内存和 `VK_EXT_map_memory_placed` 支持。
真实终端的像素验收脚本另使用 `python-pillow`；运行终端转发器本身不需要 Pillow。

## 源码与版本管理

- 上游：https://gitlab.winehq.org/wine/wine；当前 checkout 来自 https://github.com/wine-mirror/wine.git。
- 固定标签 `wine-11.19`，提交 `455e3509b98a6919fd4ad1def4803e08c41c03b2`；本地开发分支 `winetk/11.19`。
- `sources/wine/` 为浅克隆，已有全部该版本源码；研究更早历史时再按需加深。
- `downloads/wine-11.19.tar.xz` 是额外保留的官方归档缓存，SHA-256 记录于 `config/wine-source.env`。
- 同目录保留官方 `.sign` 文件；本轮归档哈希用于完整性与后续复现，不能据此宣称已经完成 GPG 发布者身份验证。
- 顶层仓库管理项目代码、脚本与文档；Wine 修改在独立 checkout 开发，后续导出至 `patches/wine/`，不要将整个 Wine 树重复加入顶层仓库。
- `fetch-wine.sh` 发现已有非基线 HEAD 会退出并保留工作，不 reset/覆盖源码。后续驱动提交后无需再次运行准备脚本。

可重新核验：

```sh
git -C sources/wine rev-parse HEAD
git -C sources/wine status --short --branch
. config/wine-source.env
printf '%s  downloads/wine-%s.tar.xz\n' "$WINE_ARCHIVE_SHA256" "$WINE_VERSION" | sha256sum -c -
```

## 构建准备

```sh
./scripts/check-environment.sh
./scripts/configure-wine-gdi.sh
```

当前 GDI 与离屏 OpenGL 构建使用以下关键配置（目录名 `wine-gdi` 沿用早期命名）：

```text
--enable-archs=i386,x86_64
--without-x --without-wayland
--with-opengl --with-vulkan --without-sdl --without-va
--disable-tests
--prefix=<项目>/out/wine-gdi
```

`--disable-tests` 仅缩短本轮上游基线构建；新增驱动后按需启用相关测试，不表示无需验收。
已完成双架构完整 Wine 编译、真实 GDI/WGL 和 D3D8 游戏呈现。
配置日志在 `logs/configure-wine-gdi.log` 和 `build/wine-gdi/config.log`；应用补丁并完整构建可执行：

```sh
./scripts/build-wine.sh
```

不要使用 root 编译，也不要将项目 Wine 安装到系统目录。
启动器明确使用项目 Wine、项目 wineserver 和独立 prefix，并将子进程 stdout/stderr 重定向到日志。
仅设置 `WINEPREFIX` 而继续调用 PATH 中的系统 Wine，不能验证项目驱动。

## terminal-browser

本机已有 `~/.local/bin/terminal-browser`。它的终端图形浏览器可作为像素传输性能与交互的参考，但不承担 Wine 的显示实现。
M0 时工具会话的 PTY 没有连接到真实 Kitty 控制通道：普通调用没有 `/dev/tty`，分配 PTY 后 `kitten @ ls` 也不能取得真实终端响应。
M1 已通过单独启动的 Kitty 测试实例及该实例的私有 remote-control socket 验证画面、键盘与 resize；未修改用户现有 Kitty 配置。
后续在用户的实际 Kitty pane 中可使用 `terminal-browser open <URL>`，通过 `terminal-browser action -- snapshot` 等命令查阅资料。
本轮已直接阅读本机 Kitty 协议文档与下载的 Wine 源码，无需为准备工作修改浏览器配置。

## 当前状态与后续记录

M0 准备结果见 [准备记录](PREPARATION-RESULTS.md)。M1、M2 文档保留各阶段历史结果；最新客户端画布、离屏游戏与输入结果见 [M3 验收](M3-RESULTS.md)。
新增 Wine 驱动应沿用相关 Wine 文件的 LGPL-2.1-or-later 许可要求；新增独立项目代码的许可证待确定，不擅自替用户选择。
