# 最终目标：Limbus Company

用户指定的安装目录：`~/.local/share/Steam/steamapps/common/Limbus Company`。
目标是让该游戏的画面经 Wine 专用驱动进入一个可交互的终端画布；Wine 显示与 GPU 呈现链路不连接 X11/Wayland。
终端本身使用桌面显示系统不影响这一要求。GDI 首版是中间里程碑，不能作为游戏支持已完成的证明。

## 最新运行结果

已用原版 x64 IL2CPP 游戏及项目独立前缀，实际贯通 D3D11 → WineD3D → surfaceless EGL → Kitty，并通过 Steam 桥接进入登录选择界面。视频共享纹理/Media Foundation、登录、资源更新与战斗尚未验收。启动方式、截图与结果见 [Unity 里程碑](reports/M4-UNITY-RESULTS.md)。

以下静态调查是开发前的记录，不代表当前构建仍禁用 GPU。

## 开发前只读核验（2026-11-09）

本轮仅检查文件类型、PE 导入、版本字符串和公开运行配置，未启动游戏，未修改游戏、Steam 或已有 prefix，也未复制游戏资源进入项目。

| 观察 | 证据与结论边界 |
| --- | --- |
| 64 位 Windows 程序 | `file` / `objdump -p`：`LimbusCompany.exe`、`UnityPlayer.dll`、`GameAssembly.dll` 均为 x86-64 PE32+；EXE 导入 `UnityPlayer.dll` |
| Unity 6000.3.12f1 | `globalgamemanagers` 首部字符串为 `6000.3.12f1`；`UnityPlayer.dll` 同时含 `6000.3.12f1_fca03ac9b0d5` |
| IL2CPP 文件布局 | 存在 `GameAssembly.dll` 和 `LimbusCompany_Data/il2cpp_data/Metadata/global-metadata.dat`；不需要反编译游戏业务逻辑来实现显示链路 |
| D3D11 是首个验证候选 | `UnityPlayer.dll` 静态导入 `d3d11.dll`、`dxgi.dll`，同时导入 `OPENGL32.dll`；这不能证明实际会选哪个 API |
| 还有 D3D12/Vulkan 候选 | Player 含 `force-d3d11`、`force-d3d12`、`force-vulkan`、`force-glcore` 字符串；目录有 `D3D12/D3D12Core.dll`。这些不是游戏 Player Settings 启用相应后端或成功运行的证明 |
| 启动配置未固定 API | `boot.config` 包含 `gfx-enable-gfx-jobs=1`、`gfx-threading-mode=6`、`hdr-display-enabled=0`，未见显式图形 API 选择；不能从此推断默认 API |
| 需要常规游戏运行环境 | 原生插件目录含 `steam_api64.dll`、FMOD、Firebase 等文件；登录、网络、音频、Steam 集成仍需实际运行验证 |

安装目录中的 `vkd3d-proton.cache` 不能单独证明当前版本运行时使用 D3D12；缓存的来源和对应运行条件尚未核验。
后续以 Player 日志、Wine 图形日志和实际呈现结果确认 API。强制 API 参数仅作为实验变量，须确认该构建接受，不能假定其可覆盖游戏构建时的能力。

## 本机离屏基础与建议路线

清除 `DISPLAY`、`WAYLAND_DISPLAY` 后的诊断结果：

- `vulkaninfo --summary` 成功，loader 枚举 `VK_EXT_headless_surface`；设备为 Intel UHD Graphics 770（Mesa 26.2.4）与 Tesla V100（NVIDIA 580.178.04）。尚未逐个 GPU 验证 headless surface、队列呈现、swapchain 格式和图像读回。
- `eglinfo -B -p surfaceless` 成功，在 Tesla V100 上报告 EGL 1.5 / OpenGL 4.6。这证明宿主离屏上下文可用，不证明 Wine/Unity 呈现已经可用，也不代替 socket 调用跟踪。
- 固定的 Wine 11.19 在 `dlls/win32u/opengl.c` 已有 surfaceless EGL / framebuffer surface 机制，在 `dlls/win32u/vulkan.c` 已有使用 `vkCreateHeadlessSurfaceEXT` 的空驱动 fallback；默认 client surface 的 present 不负责把游戏图像交给终端。

建议先做 **D3D11 → WineD3D → surfaceless EGL/FBO → CPU 像素帧** 的最小 Wine GPU 实验，复用已有 Wine 机制，先证明渲染与读回正确。它是兼容性验证路径，性能是否足够运行该游戏仍需测量。
同时将 **D3D11 → DXVK → Vulkan headless / 离屏呈现 → staging 读回** 作为游戏性能主候选；若实际游戏需要 D3D12，再评估 vkd3d-proton，避免同时引入两套 D3D 转译栈。

GPU 实验的顺序：

1. 原生 EGL FBO 动画输出到 M1 的同一 CPU frame 接口，校验颜色、上下方向、帧序号与 resize；随后验证 Vulkan headless 的 surface 支持和 swapchain transfer-src 能力。没有可用的 swapchain 读回能力时，明确实现中间 image/WSI 适配，不能只创建 headless surface 后宣布成功。
2. 用小型 Win32 D3D11 三角形/动画程序验证 HWND、swapchain、present、同步、读回和关闭；处理 MSAA resolve、sRGB、BGRA/RGBA、行跨度。读回要在正确的 present 所有权转换前安排，不在终端阻塞时持有 GPU/Wine 锁。
3. 用自行构建的 Unity IL2CPP 小样验证 D3D11、线程化渲染、输入与 resize，再尝试目标游戏。所有实验使用项目内独立构建与隔离 prefix，不覆盖既有 Steam/Proton 配置。
4. 目标游戏依次验收启动画面、菜单鼠标命中、持续动画与实际交互场景，记录渲染 API、分辨率、帧率、CPU/GPU 读回、编码、传输、丢帧和输入延迟。音频/登录/网络兼容问题与显示链路分别记录。

现有 `scripts/configure-wine-gdi.sh` 已启用 OpenGL/Vulkan 内存互操作，继续禁用 X11/Wayland；D3D11/Unity 通过 EGL 离屏路径呈现，Vulkan swapchain 后端仍待实现。

## 与 M1、M2、M3 的连接

M1 把不可变 CPU 帧、后端、输入事件、终端清理和录制先跑通；GPU readback 将来只需成为新的 frame producer，不能另起一套终端输出通道。
M2 把真实 Wine 软件 surface 接入会话；M3 解决所有顶层窗口、弹窗、焦点、捕获与多进程输入。即使最终主要画面来自 Unity swapchain，菜单/辅助窗口和 Wine 窗口状态仍依赖这些基础。
M5 的小规模 GPU 研究可以与 GDI 路线并行；不把 M3 的 GDI 成功或 M1 的动画演示表述为 Limbus Company 已能运行。

## 录制与 Kitty 修改边界

本机 Kitty 0.49.2 已提供 `--dump-bytes=PATH` 记录子进程原始输出、`--dump-commands` / `--replay-commands` 记录和回放命令，以及 `--debug-input` 查看输入事件。
M1 优先在 producer/后端记录帧序号、尺寸、像素或哈希、时间戳与发送字节；Kitty 原始流记录用于核对传输。已有协议支持直接传输、zlib 与本机共享内存，先测量再决定优化。

原始字节回放和像素源记录不能证明终端最终合成画面或端到端显示延迟。
用户已允许修改 Kitty；只有需要捕获“解码后/上传后/实际显示”的图像或时间点且现有调试能力不够时，再在独立 Kitty 源码树增加可关闭的记录 hook，并记录 Kitty 提交、捕获阶段和计时口径。
本轮无需为转发功能修改 Kitty。

游戏资源、账号信息、存档和缓存不提交或再分发；仓库仅保存工程代码、诊断结论与可自行生成的测试素材。
