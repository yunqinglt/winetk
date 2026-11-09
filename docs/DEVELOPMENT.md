# 开发与构建

项目分为可独立测试的终端/TUI、固定基线上的 Wine 补丁，以及可选的本机集成测试。目录总览见 [README](../README.md)，测试分类见 [tests/README.md](../tests/README.md)。历史主机环境记录在 [2026-11-09 环境报告](reports/ENVIRONMENT-2026-11-09.md)，不是通用安装前提。

## 不构建 Wine 的开发循环

需要 Linux、C11 编译器、CMake 3.20+、Ninja、pkg-config、libpng/zlib 开发库和 Python 3.10+（含 curses）。例如 Debian/Ubuntu：

```sh
sudo apt-get install build-essential cmake ninja-build pkg-config libpng-dev zlib1g-dev python3
cmake -S . -B build/check -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/check
ctest --test-dir build/check --output-on-failure
python3 scripts/check-repository.py
```

Debug 构建保持 C 断言有效。ASan/UBSan 使用独立目录：

```sh
cmake -S . -B build/sanitize -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWINETK_SANITIZERS=ON
cmake --build build/sanitize
ctest --test-dir build/sanitize --output-on-failure
```

`./winetk` 打印帮助，`--settings` 打开设置页，`--steam --list` 只读扫描本地 Steam。以上不依赖项目 Wine。实际终端 demo 可在 Kitty 中执行 `scripts/run-terminal-demo.sh`。

## Wine WoW64 构建

固定版本是 Wine 11.19，提交 `455e3509b98a6919fd4ad1def4803e08c41c03b2`，来源和归档哈希位于 [config/wine-source.env](../config/wine-source.env)。这些是内部驱动 ABI，不能将补丁随意套用到其他系统 Wine。

完整依赖可参考 [Arch 安装脚本](../scripts/install-deps-arch.sh)，其中 `pkexec pacman` 会安装/更新所列包及依赖；其他发行版需使用对应开发包。现代 WoW64 使用 64 位 Unix 库和 i386/x86-64 PE 工具链，无需整套 lib32 图形库。构建使用本机 CPU 与磁盘，勿以 root 编译或安装到系统目录。

```sh
./scripts/check-environment.sh
./scripts/fetch-wine.sh
./scripts/build-wine.sh
```

脚本在 `sources/wine/` 建立独立浅克隆，在 `build/wine-gdi/` 树外构建，配置为：

```text
--enable-archs=i386,x86_64
--without-x --without-wayland
--with-opengl --with-vulkan --without-sdl --without-va
--disable-tests
```

这里 `--disable-tests` 仅关闭 Wine 上游测试，项目 CTest 独立运行。`WINETK_BUILD_JOBS` 设置并行数（默认 8）；源码、构建目录等覆盖变量见各脚本 `--help`。已有源码 HEAD 不符合基线时会退出并保留原工作，不自动 reset。应用补丁可重复执行。

运行需要支持 surfaceless EGL/pbuffer 的 GL 驱动。Vulkan 用于 WoW64 缓冲映射，不是窗口呈现后端。Steam bridge 使用用户安装的 Proton Experimental 库，具体版本由本地 `out/steam-bridge/provenance.json` 记录；替换 Proton 后应重新验收其 ABI。[Steam 与 Unity 准备](reports/M4-UNITY-RESULTS.md)。

## 修改与导出驱动

```sh
# 在 sources/wine/ 修改驱动，然后增量构建。
make -C build/wine-gdi -j8
./scripts/build-smoke.sh
./scripts/export-wine-patch.sh
./scripts/apply-wine-patch.sh
```

导出覆盖 `patches/wine/0001-wineterm-driver.patch`，使用临时 Git index，不改 Wine checkout 的真实 index。Wine 与项目两侧的 `winetk_protocol.h` 必须保持一致；`check-repository.py` 会在本地 checkout 存在时检查。无需把整个 Wine 源码树加入顶层 Git。

## 本机验收与数据边界

真实 Kitty/GPU/Steam 测试是可选的，不在 CI 中运行。需 Kitty（支持截图 remote command）、`strace` 和 Pillow，游戏测试还需本机合法安装与 Linux Steam 登录。见 [测试指南](../tests/README.md) 与 [历史结果索引](reports/README.md)。

所有应用使用项目 Wine loader、wineserver、私有会话 socket 和独立 prefix；启动器清除 Wine 子进程的 `DISPLAY`/`WAYLAND_DISPLAY`。不要以系统 Wine 或原 Steam prefix 代替，或停止用户的其他 Wine 会话。单纯清空环境变量不能证明显示路径，应配合 socket 跟踪与真实像素检查。

`sources/`、`downloads/`、`build/`、`out/`、`prefixes/`、`game/`、`logs/` 均为本地工作数据。仓库只包含源码、Wine 补丁和说明；原报告的日志/截图路径以本地路径文字保留，不是远端可下载的文件。对外报告应清理 Steam 标识、账号信息和私有路径。

独立代码采用仓库的 GPL v3；Wine 部分保留其原许可与新增驱动的 LGPL-2.1-or-later 标记，见 [NOTICE](../NOTICE.md)。
