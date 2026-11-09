# M3：无显示服务器的 OpenGL / Direct3D 8 呈现

本轮为 `wineterm.drv` 接入 Wine 11.19 的 EGL / WGL 接口。WineD3D 可以把 Direct3D 8 转成 OpenGL，在 EGL pbuffer 中绘制，再由驱动读取 RGBA 帧送入终端会话。Wine 构建启用 OpenGL 与 Vulkan 内存互操作，继续禁用 X11、Wayland；尚未实现 Vulkan 窗口呈现。

`opengl.c` 复用 Wine `win32u` 的 EGL 上下文、像素格式和离屏 pbuffer 实现，替换窗口 drawable 的存储和呈现。EGL 平台固定为 `EGL_PLATFORM_SURFACELESS_MESA`，原生显示参数为 0。实测驱动为 NVIDIA Tesla V100，OpenGL 4.6.0 / NVIDIA 580.178.04；这些结果使用硬件 OpenGL。

## 已实现的呈现行为

- 帧尺寸取自 Wine 窗口客户区，发送位置和客户区原点一致；不在 RGBA 中包含 Wine 标题栏和窗口边框。
- `SwapBuffers` 或需要呈现的 flush 读取当前窗口 pbuffer，翻转 OpenGL 的上下方向，设置不透明 alpha，发送协议 v2 的 RGBA 帧。
- 客户区改变大小时创建新 EGL surface 和 CPU 缓冲，在需要时重新绑定当前 draw/read surface，成功后再释放旧资源。
- 读取前保存实际 read framebuffer、read buffer、pixel pack buffer，以及 pack alignment / row length / skip pixels / skip rows / swap bytes / LSB first；读取后还原。
- 每个 drawable 的互斥锁保护缓冲、尺寸变更和读取；窗口客户区的 GPU 活跃引用计数由 `term.c` 管理，避免稍后的 GDI 重绘覆盖 GPU 内容。

资源由两层分别释放：本驱动销毁 CPU 帧、翻转用行缓冲和互斥锁；Wine `win32u` 释放 EGL surface 与 client surface 引用。帧的大小受共享协议 1920×1080 上限约束。发送仍使用现有 IPC 的整体 250 ms 超时；终端不消费数据时，GUI 线程不会无限等待。

## 原生颜色缓冲的处理

不能假定 EGL pbuffer 使用 `GL_FRONT`。本机 NVIDIA 的实际默认颜色缓冲为 `GL_BACK`；此前硬编码 `GL_FRONT` 虽然能看到画面，却会留下 `GL_INVALID_OPERATION`，扩大测试后已修复。

驱动首次激活窗口 drawable 时，临时创建一个全新的 EGL context，读取其默认 `GL_DRAW_BUFFER`，随后恢复调用者原来的 context 与 draw/read surface，并销毁临时 context。探测结果按进程的 EGL 平台缓存。这样不会把应用已经设置的 `GL_NONE` 等状态误认为原生缓冲，也不会消耗应用的 GL 错误状态。不能用 WGL 暴露的 `GL_DOUBLEBUFFER` 属性替代这个探测。

当前 WGL 的 front/back 名称映射到同一个原生 pbuffer 颜色缓冲；不调用原生窗口系统 swap。这足以捕获本轮的 WineD3D 呈现，但尚未实现两个独立颜色缓冲的保留和交换语义。需要保留前缓冲、立体绘制或同时由多个线程绘制同一窗口的复杂 WGL 程序，仍需后续验证和扩展。D3D11、DXGI、DXVK/Vulkan 和 Limbus Company 未在本轮 GPU 冒烟测试中验证。

## 可重复的实测

先构建项目 Wine 与 smoke PEs，再运行可选集成测试；它们不会加入普通 CTest：

```sh
./scripts/build-smoke.sh
python tests/test_wgl_live.py --output logs/my-wgl-live
python tests/test_wgl_live.py --api d3d8 --output logs/my-d3d8-live
```

测试使用独立的 `prefixes/gpu-test` 及其排他锁、临时私有 Unix socket，清除 `DISPLAY` 和 `WAYLAND_DISPLAY`。接收端直接检查驱动发送的像素，不依赖 Kitty 截图。`--output` 必须是尚未存在的目录。测试可通过 `WINETK_TEST_WINEDEBUG` 增加 Wine 日志。

`WINETK_GL_METRICS=1` 会按窗口每秒向 stderr 输出 GPU 呈现帧率，以及 GL 状态准备、`glReadPixels`、翻转/alpha 处理、IPC 发送的平均耗时；默认关闭。它只统计驱动呈现阶段，不能把应用在两次 Present 之间的时间归到 GPU 读取。

| 测试 | 校验内容 | 实测结果 |
| --- | --- | --- |
| WGL x86-64 | 顶部左侧红色、右下蓝色，通道顺序和不透明 alpha；640×480 → 800×600；自定义 read FBO、绑定 pack PBO、非默认 pack 参数；初始化与每次 SwapBuffers 后 GL 无错误 | 800 帧，100.03 fps；两种尺寸分别 399 / 401 帧 |
| WGL i386 / WoW64 | 与 x86-64 相同的像素、状态和运行时尺寸变更检查；日志确认 Vulkan 互操作设备初始化，暴露 OpenGL 4.6 | 798 帧，99.74 fps；两种尺寸分别 397 / 401 帧 |
| Direct3D 8 i386 | `CreateDevice` 成功，清屏背景 RGBA 与移动彩色三角形像素正确，真实 Present 循环，进程正常退出 | 481 次 Present / 8021 ms，即 59.97 fps；IPC 捕获 481 帧，间隔统计 60.35 fps |

WGL 的 8 ms Windows timer 在本机约每 10 ms 触发一次，因此 100 fps 是这个 smoke 的实际触发频率，不是终端端到端帧率或游戏帧率保证。上游 Wine 默认只构建 i386 的 `d3d8.dll`（`configure.ac` 的 `enable_d3d8` 默认值）；所以 Direct3D 8 集成测试只运行 i386。x64 WGL 不受此限制。

本轮证据：

- 启用互操作后的完整 Wine 构建：logs/m3-full-build-interop.log（本地：`logs/m3-full-build-interop.log`）。
- 最终 WGL 像素、尺寸、状态与互操作设备日志：logs/m3-wgl-interop-final（本地：`logs/m3-wgl-interop-final`）。
- 最终 Direct3D 8 正常完成的 stdout、状态 JSON 和帧 PNG：logs/m3-d3d8-interop-final（本地：`logs/m3-d3d8-interop-final`）。

## 东方红魔乡的实际关卡与 WoW64 映射

菜单接近 60 fps 不能代表关卡性能。最初禁用 Vulkan 的构建在菜单正常，进入第一关后却只有约 5.3 fps；直接消费驱动 IPC 也能复现，排除了 Kitty 传输。驱动耗时显示每帧 `glReadPixels` 约 7～11 ms，而大部分时间花在进入 Present 之前。Wine 同时报告 `Doing a copy of a mapped buffer (expect performance issues)`：32 位 WoW64 无法直接使用高地址的 OpenGL 映射缓冲，只能退回拷贝。

构建改为 `--with-opengl --with-vulkan` 后，Wine 上游的 OpenGL/Vulkan 外部内存互操作可以把缓冲映射到低地址，避免该回退。它要求 GPU 支持外部内存 fd、`VK_EXT_map_memory_placed` 及相关 placed mapping 功能；本机 NVIDIA Tesla V100 / 580.178.04 满足条件。这个 Vulkan 设备仅服务缓冲映射，游戏仍由 WineD3D → OpenGL → EGL pbuffer → RGBA → Kitty 呈现，没有创建 X11、Wayland 或 Vulkan 窗口。

使用单独测试副本 `out/game-gpu-cadence`、独立前缀，自动进入第一关并直接消费 IPC，45 秒采样共捕获 2577 帧；完整的关卡秒段稳定为每秒 62～63 帧，末帧画面包含敌机、子弹和游戏自身的 `62.50fps`。总平均 58.16 fps 包含启动时约 3 秒的加载空档。驱动每帧平均读取约 0.53 ms、翻转/alpha 处理约 0.24 ms、IPC 发送约 0.19 ms。证据位于 logs/m3-game-stage-interop（本地：`logs/m3-game-stage-interop`）。

独立的实际 Kitty 测试同样进入第一关，并验证方向键移动：末段 9992 ms 提交 591 帧，即 59.05 fps；共享内存呈现平均 0.712 ms，p95 提交间隔 20 ms。数据与画面位于 logs/m3-th06-stage-interop（本地：`logs/m3-th06-stage-interop`）。这两个结果分别衡量驱动输出与终端提交，不能把直接 IPC 的 62.5 fps 当作 Kitty 端到端帧率。

游戏采样会在指定时间停止自己的 Wine 前缀，未据此宣称正常退出或长时间稳定性通过。早期 18 秒菜单采样在停止时出现过 `pthread_mutex_lock` 断言；正常退出检查由上面的 Direct3D 8 smoke 单独完成。测试副本和前缀不会替代用户游戏会话，原始 `game/` 不由采样修改。
