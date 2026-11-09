# Unity：Mono 与 IL2CPP 的终端离屏呈现

2026-11-09，本机已验证两个真实 Unity 游戏经 **D3D11 → WineD3D → surfaceless EGL pbuffer → RGBA IPC → Kitty** 显示，并完成基础交互。64.0 已进入教程；Limbus Company 已到登录选择界面，尚未验收登录、资源更新和战斗。该路径复用原有离屏 WGL 驱动，不是 DXVK/Vulkan swapchain 呈现。

## 运行

先保持 Linux Steam 已登录，在 Kitty 中执行：

```sh
./scripts/build-wine.sh
./scripts/run-unity.sh 64
./scripts/run-unity.sh limbus
```

`Ctrl+Shift+F12` 结束会话。脚本支持无值的 `--zoom-in`、`--fit-resolution`，以及 `--fps`、`--duration`、`--record-dir`、`--record-frames` 和 `--kitty-transfer`；`--` 后追加 Unity 参数。例如：

```sh
./scripts/run-unity.sh limbus --duration 60
./scripts/run-unity.sh limbus --zoom-in
./scripts/run-unity.sh limbus --fit-resolution
./scripts/run-unity.sh 64 -- -screen-width 640 -screen-height 480
```

默认使用 `~/.local/share/Steam`，桥接库来源为已安装的 `Proton - Experimental`。可用 `WINETK_STEAM_DIR`、`WINETK_PROTON_DIR` 改路径。本轮实测的桥接库版本为 `experimental-11.0-20261027b-x86_64`；跨版本 PE/Unix ABI 不能保证，替换 Proton 后需要重跑集成测试。

第一次运行使用 `cp -a --reflink=auto` 创建 `out/unity-64/` 或 `out/unity-limbus/`；不支持 reflink 的文件系统会实际复制约 455 MB / 12 GB。已有副本保留，不自动同步 Steam 更新。两者分别使用 `prefixes/unity-64/`、`prefixes/unity-limbus/`，准备和运行均持有该前缀的会话锁。只向项目副本、前缀和日志写文件，不覆盖 Steam 安装目录或其 compatdata。

Player 日志是 `logs/unity-64-Player.log` / `logs/unity-limbus-Player.log`；终端与 Wine 日志遵循 `logs/wine-unity-*-session.jsonl` / `logs/wine-unity-*-application.log`。游戏及 Steam 二进制只保存在被 Git 忽略的本地目录，不作为仓库交付。

## 分辨率、终端放大与原资源缓存

当前脚本把 Limbus 客户区设为 **800×450**，GPU 帧和 `wine_canvas` 日志均确认该尺寸。原 Steam 前缀的 Screenmanager 保存值为 **2556×1410**，所以项目的渲染分辨率确实较低。Player 日志的 **1920×1080** 则是视频轨道素材尺寸，不代表交换链尺寸。默认较大终端仍显示 800×450 像素，较小终端才缩小。

`--zoom-in` 在顶部程序名行以下等比适配终端，保留完整画面并居中留边；改变终端大小后重新计算，鼠标逆向映射到原始游戏坐标。它只放大输出，不能增加 800×450 源画面的细节。最初最近邻版本真实 Kitty 实测源 800×450 → 输出 **1314×739**，顶部名称可见，鼠标点击仍打开登录选择；45 秒 2239 次提交，末 10 秒约 49.52 次/秒。见 截图（本地：`logs/m4-limbus-zoom/limbus/after-input.png`） 和 结果（本地：`logs/m4-limbus-zoom/limbus/result.json`）。后续因放大颗粒明显，放大采样已改为双线性插值。

`--fit-resolution` 则按启动时的终端可用区域提高游戏的实际渲染分辨率，保持 64.0 的 4:3 / Limbus 的 16:9，当前上限 1920×1080。终端 demo 完成像素尺寸协商后再替换启动 argv 的宽高占位符；Wine 驱动通过 `WINETK_DESKTOP_SIZE` 设置带 64 像素边框余量的虚拟显示，解决原来 800×600 的窗口限制。在未达到上限的终端中，游戏帧直接以原生像素显示，无需再次放大。运行中调整终端大小仍只缩放输出，重启后重新选择渲染尺寸。普通及 ASan/UBSan CTest 均 6/6 通过，覆盖插值、自动选择分辨率、终端变化、顶部留行与鼠标映射。

2026-11-10 真实 Kitty 自动适配实测（终端 1314×1380，首行 20 像素）：

| 游戏 | 实际源 / 输出 | 45 秒运行结果 |
| --- | --- | --- |
| 64.0 | 1314×985 / 1314×985 | 2130 次提交，末 10 秒 55.26 次/秒；进入教程并接受方向键。结果（本地：`logs/m4-unity-fit-final/64/result.json`） |
| Limbus Company | 1314×739 / 1314×739 | 1468 次提交，末 10 秒 18.84 次/秒；已有 Steam 登录状态，点击标题进入 CONNECTING。结果（本地：`logs/m4-limbus-fit-native/limbus/result.json`）、截图（本地：`logs/m4-limbus-fit-native/limbus/before-input.png`） |

两者正常退出，Wine 侧 connect 跟踪均无 X11/Wayland 连接。Limbus 后段包含连接/资源初始化，提交数不能作为稳定游戏帧率基准。早期验收脚本只记录尺寸变化，已修正为也记录初始同尺寸画布；截图差异检查也改为只比较游戏区域，避免黑边稀释已有登录状态下较小的 CONNECTING 提示。源 800×450 最近邻放大与原生 1314×739 的相同按钮裁剪见 像素对比（本地：`logs/m4-resolution-comparison.png`）。

重新下载的原因是 Unity 资源缓存位于原 Proton 前缀，复制游戏安装目录并不包含这些资源。准备脚本现会一次性导入以下两个目录：

- 原 `steamapps/compatdata/1973530/pfx/drive_c/users/steamuser/AppData/LocalLow/Unity/ProjectMoon_LimbusCompany` → 项目前缀对应用户的 `AppData/LocalLow/Unity/ProjectMoon_LimbusCompany`。
- 原 `AppData/LocalLow/ProjectMoon/LimbusCompany/com.unity.addressables` → 项目前缀对应目录。

本机源共 **3141 文件、17,551,604,639 字节**；复制后数量、大小一致，三个最大 bundle 与两个 catalog 文件的 SHA-256 抽检一致。使用 `cp -a --reflink=auto` 创建独立副本，没有复用原前缀或共享可写缓存，也没有导入账号、存档、PlayerPrefs。已有项目文件优先保留；缺失子树先在临时目录完整复制再发布，失败可安全重试。导入记录在 `prefixes/unity-limbus/.winetk-limbus-cache-v1.json`。后续如原缓存增加，运行：

```sh
python3 scripts/prepare-unity.py limbus --refresh-cache
```

该命令补充缺失条目，保留已有内容；首次成功后正常启动不反复复制。游戏仍可能检查远端版本并下载新增或失效的资源。

## 架构与 Steam 基线

已在两个原始游戏仍运行时记录完整 argv、cwd，以及少量图形/Steam 环境变量，见 Steam 进程记录（本地：`logs/m4-unity-baseline/steam-processes.json`）。没有采集完整进程环境或账号令牌。

| 游戏 | 本机架构 | Unity | 原 Steam 启动 |
| --- | --- | --- | --- |
| 64.0，AppID 578850 | i386 PE32，Mono；有 `Managed/Assembly-CSharp.dll` | 5.6.5f1 | Proton 9.0 (Beta)，SteamLinuxRuntime_sniper；`PROTON_USE_WINED3D=1` |
| Limbus Company，AppID 1973530 | x86-64 PE32+，IL2CPP；`GameAssembly.dll` 与 metadata | 6000.3.12f1 | Proton - Experimental，SteamLinuxRuntime_4；原前缀以原生 D3D/DXGI DLL 覆盖 |

项目两款游戏都显式传 `-force-d3d11`，由各自 Player 日志确认实际 API 与线程化渲染。WineD3D 报告的 `NVIDIA GeForce GTX 470` 是其兼容层适配器标识，不能据此推断物理 GPU 已改变。

## 新增实现

- 新增双架构 D3D11 smoke：硬件设备、HLSL 顶点/像素着色器、真实 Draw/Present、交换链 ResizeBuffers，检查背景/三角形 RGBA、帧尺寸和正常退出。
- `steam-host.c` 维持本前缀的 Windows Steam 进程 PID 和命名事件，并等待游戏子进程。Steamworks 通过 Proton `lsteamclient.dll/.so` 连接当前 Linux Steam 客户端；没有实现账号或授权状态的替代逻辑。
- Wine loader 增加仅在 `WINETK_STEAM_BRIDGE=1` 时启用的 Steam 接入。参考 [Proton loader](https://github.com/ValveSoftware/wine/blob/7b3ceaf8644bb5d57140903e12897efac69be16a/dlls/ntdll/loader.c) 的原生 Steam 客户端映像保留方式，把内存中原版 `steamclient.dll` / `steamclient64.dll` 的已支持导出跳转到 lsteamclient，并跳过原生 Windows 客户端初始化。跳转仅修改进程私有内存；安装文件保持原样。桥接失败时返回加载错误，不运行部分改写后的原生客户端。
- 只把 lsteamclient 的 PE 与 x86-64 Unix 库放入独立 `WINEDLLPATH`；WoW64 使用其已有的 32 位参数转换表。没有把 Proton 的图形驱动、D3D/DXGI 或 wineserver 混入项目 Wine。实际复制的库路径、版本与 SHA-256 保存在 `out/steam-bridge/provenance.json`。
- GPU 帧新增协议 v2 的 `WT_WINE_GPU` 标记。直接启动进程仍优先；当它只是无窗口启动器时，画布跟随同一私有会话中的 GPU 子进程，几何通知不丢失 producer 类型。首行 `--title` 保持实际游戏名称，不显示辅助进程 `steam.exe`。

初始 Limbus 的退出码 53，以及补齐进程存在性后的 `Application load error 3:0000065432`，均发生在 Unity Player 日志创建前。保留原生 Steam 映像并桥接导出后，原版游戏正常进入 Unity、执行 IL2CPP 和 Steam 回调。64.0 原先的 `SteamAPI_Init() failed` 也已消失。

## 实测证据

所有下列游戏测试都使用真实 Kitty、独立项目前缀，并对 Wine 及其子进程执行 `strace -f -e connect`。轨迹包含 `display.sock`，未发现 `.X11-unix`、XIM 或 `wayland-N` 连接。截图来自 Kitty 最终显示；性能数值是应用日志/终端提交指标，不是显示扫描输出或输入延迟。

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| D3D11 x86-64 | 480 个匹配 GPU 帧，59.92 fps；640×480 与 800×600 各 240 帧；正常退出 | 结果（本地：`logs/m4-d3d11-rgba-final/x86_64/result.json`）、像素（本地：`logs/m4-d3d11-rgba-final/x86_64/frame.png`） |
| D3D11 i386/WoW64 | 480 个匹配 GPU 帧，59.93 fps；两种尺寸各 240 帧；正常退出 | 结果（本地：`logs/m4-d3d11-rgba-final/i386/result.json`）、像素（本地：`logs/m4-d3d11-rgba-final/i386/frame.png`） |
| 64.0 | 640×480；45 秒会话共 2312 次终端提交；末 10 秒 58.06 fps；Enter 进入菜单、教程，增强方向键按住/松开；SteamAPI 无初始化失败 | 结果（本地：`logs/m4-unity-final/64/result.json`）、教程截图（本地：`logs/m4-unity-final/64/after-input.png`）、Player 日志（本地：`logs/m4-unity-final/64/Player.log`） |
| Limbus Company | 800×450；45 秒会话共 2298 次终端提交；末 10 秒 56.61 fps；标题界面鼠标操作后出现登录选择；键盘输入送达 | 结果（本地：`logs/m4-limbus-input-final/limbus/result.json`）、登录选择截图（本地：`logs/m4-limbus-input-final/limbus/after-input.png`）、Player 日志（本地：`logs/m4-limbus-input-final/limbus/Player.log`） |

以上游戏帧率包括 syscall 跟踪开销。退出码 0 来自有限时长终端会话结束及其前缀清理，不表示游戏自行正常关闭或长时间稳定性已通过；正常资源退出由 D3D11 smoke 单独检查。两个游戏的截图变化用于基础交互证据，不替代完整游戏操作验收。

完整 Wine 增量构建通过：构建记录（本地：`logs/m4-wine-build-final.log`）。普通与 ASan/UBSan 的 CTest 均 4/4 通过；新增单元检查覆盖启动器无 surface 时的 GPU 子进程画布、几何更新与输入坐标。补丁重复应用检测通过，两份 IPC 头文件相同，Wine diff 无空白错误。

复测命令：

```sh
./scripts/build-smoke.sh
python tests/test_wgl_live.py --api d3d11 --output logs/my-d3d11-check
python tests/test_unity_live.py --game both --output logs/my-unity-check
ctest --test-dir build/terminal --output-on-failure
```

## 当前边界

Limbus 开场视频报告 `IDXGIResource` 共享句柄未实现（`0x80004001`），其后视频资源也有 Media Foundation `0xc00d36bb` 错误。游戏能继续到标题和登录选择，但视频背景不完整。早期点击过快还触发过 `Not Initialize Addressable`。本轮没有完成 Limbus 账号登录、网络内容更新、战斗、音视频或长时间稳定性验收，不能表述为 Limbus 已完整可玩。

默认虚拟显示为 800×600，所以普通启动的 Limbus 为 800×450；`--fit-resolution` 已扩展到按终端选择、最高 1920×1080 的客户区，并相应扩大虚拟显示。超过该上限、运行中动态改变实际渲染尺寸、Unity 其他图形后端、多交换链、多上下文和共享纹理仍需扩展。Vulkan 在现有构建中的用途依旧是 WoW64 内存互操作，尚无 Vulkan 窗口呈现。
