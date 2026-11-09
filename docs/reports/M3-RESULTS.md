# 客户区画布、离屏游戏与输入验收

日期：2026-11-09。Wine 11.19 固定基线，Arch x86_64，Kitty 0.49.2，NVIDIA Tesla V100 / 580.178.04。
本轮已运行用户提供的《东方红魔乡》1.02f，图形路径为 D3D8 → WineD3D → surfaceless EGL pbuffer → RGBA → 会话 broker → Kitty。
Wine 不连接 X11/Wayland，不创建供捕获的宿主游戏窗口；Kitty 自身仍是正常的桌面终端。
这次交付提前实现了部分 GPU 与性能路线，不代表完整多窗口 M3 或 Unity/Limbus 已完成。

## 客户区与分辨率

- IPC 升至 v2，窗口结构为 48 字节，分别报告外框与客户区桌面坐标。GDI 裁出客户区，GPU 直接发布客户区像素。
- broker 跟随启动进程中已有像素的最大可见窗口；客户区尺寸决定单画布分辨率，输入加回桌面原点后命中窗口。其余窗口继续合成到该画布。
- 初始虚拟显示模式 800×600 与实际画布独立；提供经典 16/32 位显示模式。修复旧虚拟桌面裁剪和默认最大跟踪尺寸造成的截断。
- x64 测试窗口真实客户区 **960×720**，i386 为 **640×400**，均无应用专用 `WM_GETMINMAXINFO` 修补。像素对比覆盖客户区首行、正文和右下角；无旧 800×600 截断，无 Wine 标题栏/边框。证据：`logs/m3-gdi-final/results.json` 及各架构截图、状态、strace。
- 终端 `(0,0)` 固定保留可执行文件名；红魔乡报告 640×480。小终端保持等比缩放。

这里移除的是 Wine 非客户区，包括原生菜单和非客户区滚动条。应用在客户区自行绘制的 CSD 无法通用识别；跨进程 Z 序、复杂弹窗、透明/shape 与 capture 尚未完整验收。

## 方向键修复

修复涉及两处。Wine 默认键盘映射会把导航虚拟键映射到数字小键盘扫描码；仅调用 `MAPVK_VK_TO_VSC_EX` 仍不足，驱动显式发送 E0 扫描码，例如 Up=`e048`、Left=`e04b`，同时设置 `KEYEVENTF_EXTENDEDKEY`。
另一个问题位于终端端：Kitty 开启增强键盘后仍可发送 `CSI A/B/C/D` 按下与 `CSI 1;1:3A/B/C/D` 松开。旧代码把前一种拼写当成不支持松开的旧终端，立即合成 key-up。Notepad 的消息处理能响应瞬间按下，游戏 DirectInput 轮询却常常读到松开。现在以本会话是否启用键盘事件报告来判断松开能力，保留实际按住状态；带 `:event_type` 的功能键序列也不再标记为旧协议。

最初的 i386 测试直接发送 `CSI 57352u` 等编码，验证 E0 修复但漏测了 Kitty 常用功能键拼写；用户真实键盘反馈发现上述第二处问题，不能用旧测试声称覆盖真实方向键。旧结果保存在 `logs/m3-dinput-live/i386/`，后续功能键回归需分别验证按住与松开、重复与组合键。
修复后已通过真实 Kitty 编码器测试：默认配置覆盖 Kitty 原生编码、功能键 CSI、原始 CSI-u 三种路径；用户实际 `~/.config/kitty/kitty.conf` 覆盖原生编码路径。每方向按住约 250ms，期间发送重复事件，DirectInput 仅产生一次按下、多次按住采样，最后全部释放，数字小键盘事件为零。证据：`logs/m3-dinput-functional-default/`、`logs/m3-dinput-real-user/`；实际字节捕获见 `logs/m3-native-key-capture/{default,user}/bytes.json`。两种配置编码相同。
测试必须保持按键一段时间；把按下与松开瞬间连续发送会被游戏状态轮询漏掉。

## 性能改动与测量

Wine 默认提交上限从 15 提至 60，可配置 1..240；驱动报告的刷新率和忙碌 GDI flush 使用同一设置。复用软件帧缓冲，GPU 活跃窗口避免 GDI 旧帧覆盖。
broker 改为等待 socket 就绪，避免每个 IPC 分片额外睡眠 4ms；两端 socket 缓冲提高到 4MiB。帧限速扣除处理时间并避免追赶旧帧。

Kitty 直传合并每块的头部、Base64 和结束符，大帧采用 zlib level 1，仅在压缩后更小时使用。默认另行探测 `t=s` POSIX 共享内存，成功后直接发布 RGBA，终端流只传对象名称与 placement。队列最多四帧，终端消费后 unlink；满额或系统调用失败就直传，不无限排队。
录制自动直传，确保 `graphics.bin` 为包含像素的独立记录；PNG 记录不依赖共享内存。

早期真实游戏会话约 27 FPS；修正 IPC 等待后达到约 40–53 FPS。后续隔离测试在相同 640×480 角色选择场景、每次 40 秒的末十秒采样得到：

| 传输 | 提交 FPS | 平均 present 耗时 | 每帧 TTY 字节 | 提交间隔 P95 |
| --- | ---: | ---: | ---: | ---: |
| 压缩直传 | 60.27 | 11.32ms | 635,055 | 19ms |
| 本机共享内存 | 60.20 | 0.61ms | 158 | 19ms |

证据：`logs/m3-th06-performance-final/results.json`、两种模式的 `interaction.png` 与 JSONL。两种模式均正常退出。游戏截图自身计数约 62 FPS；此表是 **broker 提交速率**，不是屏幕扫描输出或输入到显示的延迟测量。
共享内存字节数仅计算 TTY 指令，像素仍经共享内存复制和 GPU 上传；640×480 RGBA、60fps 的像素体积约 74MB/s。
独立 GPU → IPC 接收器测试平均约 58.4 FPS，说明终端编码之外也存在游戏定时与 GPU 读回开销。启动加载阶段有数秒停顿，未纳入稳态末十秒统计。

菜单帧率不能代表关卡帧率。进一步实际进入第一关后，早期构建仅约 **6.6 FPS**，截图中游戏自身计数 6.26；`logs/m3-th06-stage-final/auto/` 保留该结果。独立 IPC 重现约 5.3 FPS，驱动统计显示 readback 约 8–11ms，其余状态/翻转/发送低于 1ms，主要间隔发生于 present 之前。关闭 CSMT、清除游戏软件纹理混合选项均未改善；这些实验只修改测试副本，没有更改普通游戏副本。

最终构建启用 `--with-vulkan`，让 Wine 11.19 的 OpenGL WoW64 使用已有的 Vulkan placed-memory 互操作，避免原先昂贵的缓冲映射复制。本机支持 `VK_EXT_map_memory_placed`、`VK_KHR_external_memory_fd` 及所需 feature；没有为此接入 Vulkan 窗口呈现或 X11/Wayland。相同游戏选项 `0x1`、零跳帧的实际第一关，45 秒运行末十秒 **59.05 FPS** 提交、平均 present **0.71ms**、TTY **158 字节/帧**、提交间隔 P95 **20ms**，正常退出。截图可见实际弹幕与角色移动，游戏自身计数 **62.50fps**。证据：`logs/m3-th06-stage-interop/auto/result.json`、`interaction.png`。这比早期菜单结果更能代表本轮完成的简单游戏路径，完整关卡/长时间稳定性仍待验收。

完整 Wine 重建后，又用用户实际 Kitty 配置及 Kitty 自己编码的键盘事件重测第一关：末十秒 **59.32 FPS**，present **0.87ms**，P95 **20ms**，45 秒正常退出。`before-right.png` 与 `after-right.png` 可见角色向右移动约 67 像素；日志保持约 300ms 按住，重复事件后没有提前松开。游戏截图仍为 **62.50fps**。最终证据：`logs/m3-th06-user-final/auto/`。该次每帧平均 TTY 约 701 字节，包含偶发直传回退，不能把共享内存路径视为每帧固定字节数。

## 回归与复现

```sh
cmake --build build/terminal
ctest --test-dir build/terminal --output-on-failure
./scripts/build-smoke.sh
python3 tests/test_dinput_live.py --arch i386
python3 tests/test_dinput_live.py --arch i386 --kitty-config user --key-source kitty
python3 tests/test_wine_live.py
python3 tests/test_wgl_live.py
python3 tests/test_th06_live.py --mode both --duration 40
python3 tests/test_th06_live.py --mode auto --duration 45 --kitty-config user
./scripts/run-th06.sh
```

真实 Kitty 测试需可用宿主桌面；图形子进程由启动器清除 DISPLAY/WAYLAND_DISPLAY。真实像素验证需要 Pillow。测试使用独立前缀锁，游戏性能测试使用 `prefixes/th06-performance`；正常交互使用 `prefixes/th06`。

普通与 Clang ASan/UBSan 的 CTest 各 **4/4 通过**，包含输入解析、Kitty 编码、Wine IPC/几何/输入和九种 PTY 生命周期场景。共享内存专测覆盖像素/步幅裁剪、0600 权限、探测、满队列回退、文件描述符耗尽回退、消费后复用、写失败与退出清理。正常退出与 SIGTERM 清理无残留；SIGKILL/进程崩溃且终端尚未消费时，仍可能留下最多四帧和一个探测对象，需要后续 watchdog 或过期资源回收。

最终完整 Wine 构建通过，`logs/m3-full-build-interop.log`。导出补丁已校验可应用到固定基线，重复应用脚本识别已应用；两份 IPC 头文件一致，Wine diff 无空白错误。后续 WGL 双架构约 100fps、D3D8 i386 约 60fps 的像素/GL 状态/resize/正常退出检查通过，分别见 `logs/m3-wgl-interop-final/` 与 `logs/m3-d3d8-interop-final/`。

离屏 WGL 的状态、resize 与颜色验证详见 [GPU 验证](M3-GPU-RESULTS.md)。当前仍使用同步 GPU 读回；前后缓冲别名、复杂多上下文和不同 GPU 驱动需扩大兼容测试。D3D11、DXVK/Vulkan、Unity IL2CPP、Limbus Company 为后续工作。游戏音频、手柄、完整关卡/长时间稳定性尚未验收。
