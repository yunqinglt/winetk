# 分阶段开发路线

M0、M1、M2 已完成。本轮还实现客户区画布、动态尺寸、性能优化及 surfaceless OpenGL/WineD3D D3D8 路径，实际显示《东方红魔乡》，见 [本轮结果](reports/M3-RESULTS.md) 和 [游戏记录](TH06.md)。这些成果横跨下面的阶段，不代表完整多窗口会话或 Unity 兼容已经完成。每个阶段用可运行成果验收，不以代码目录或接口数量判断完成。
Unity 新进展：D3D11 双架构像素/resize 验证通过，64.0（Mono）已进入教程，Limbus（IL2CPP）已到登录选择。Steam 桥接、启动器子进程画布和一键启动已接入，见 [Unity 结果](reports/M4-UNITY-RESULTS.md)。Limbus 视频/登录/战斗与长时间兼容仍未完成。
最终目标已明确为 [Limbus Company](TARGET-LIMBUS.md)，GDI 首版是通向该目标的中间交付。
本轮已修复 Kitty 功能键按住/释放，用户配置下的真实编码经过 DirectInput 验证。东方红魔乡第一关已达约 59 FPS 终端提交；Wine Vulkan 内存互操作用于 WoW64 OpenGL 快速映射，仍未接入 Vulkan 窗口呈现。完整证据见 [M3 记录](reports/M3-RESULTS.md)。

| 阶段 | 工作 | 完成条件 |
| --- | --- | --- |
| M0 环境与基线（已完成） | 固定 Wine 源码，安装依赖，树外配置与代表性编译，记录需求 | 源码提交可复现；依赖和无 X11/Wayland 配置检查通过；路线明确 |
| M1 终端协议原型（已实现并验证） | 独立会话程序、TTY 状态管理、能力探测、Kitty 整帧画布、键鼠解析、帧/协议记录 | 真实 Kitty 测试图及像素对比、键盘和 resize 通过；鼠标协议/坐标与退出恢复自动测试通过；已记录提交带宽和耗时基线 |
| M2 最小 Wine 驱动（已实现并实测） | 新增 `wineterm.drv` 的 PE/Unix/WoW64 模块，桌面枚举、驱动选用、GDI surface、最小 IPC | 在无 DISPLAY/WAYLAND_DISPLAY 下把单个 GDI 测试窗口显示到终端，跟踪确认显示链路无宿主窗口连接 |
| M3 单画布完整 GDI 会话（部分实现） | 已实现客户区专用协议 v2、动态尺寸、基本输入及 DirectInput 增强方向键；继续完成窗口树与 Z 序、遮挡/移动/shape、菜单/弹窗、focus/capture | 同画布操作 Notepad 与另一个进程的测试窗口；对话框、菜单、拖动、滚动和快捷键通过验收 |
| M4 稳定与性能（部分实现） | Wine 默认 60 fps、读回与 broker 处理、Kitty 压缩/传输方案已有实现和测量；继续做脏区、异步读回、有界背压与崩溃重连 | 长时间交互无窗口残影、粘键、终端状态或图像资源泄漏；提交可复现测量数据 |
| M5 离屏图形可行性（WGL/D3D8/D3D11/Unity 已实测） | surfaceless EGL pbuffer、双架构 WGL 颜色/resize、真实 D3D8 游戏已经贯通；D3D11/WineD3D、Unity Mono 与 IL2CPP 标题界面已实测；继续评估共享纹理和 DXVK/Vulkan | 非 GDI 动画经专用驱动进入画布，无 X11/Wayland 回退；D3D11/Unity 的颜色、同步、resize、输入与资源释放可核验 |
| M6 Limbus Company 兼容与交付（已到登录选择） | 确认游戏实际 API，在独立 prefix 验证启动、菜单及交互场景；需要 D3D12 时再适配 vkd3d-proton | 目标游戏完成可交互场景；报告画质、帧率、延迟、带宽、音频/网络等兼容限制；形成可复现安装/启动包 |

## M1 的具体交付

从 M1 开始实现独立的 `winetk-terminal-demo`：

1. 探测 Kitty 图形、像素尺寸与键盘能力；明确报告不支持和超时。
2. 在单画布显示固定图案和移动色块，记录实际提交帧与传输字节。
3. 显示键鼠事件和缩放后的位置；测试拖动、滚轮、修饰键、重复、释放和焦点变化。
4. 对退出、SIGINT/SIGTERM、resize 进行真实终端验证；将日志写文件，确保图形流独占。
5. 用测量结果确定默认逻辑桌面尺寸、帧率与首个编码策略，然后进入 M2。
6. 记录帧源与传输数据以便回放和定位黑屏/撕裂；Kitty 已有原始字节记录与命令回放，只有需要额外终端内部观测点时再修改独立 Kitty 源码树。

M2 已完成完整 Wine 构建、实际驱动加载、explorer 初始化和基本键鼠桥接。M3 继续完成窗口语义；已有最小跨进程合成还不能代表完整 GDI 会话。
M3 必须覆盖多进程，否则“单个窗口能显示”不能算首版交付完成。
本轮提前完成 M5 的 WGL/D3D8 验证：GPU 帧在 drawable flush/swap 时从 EGL pbuffer 读回，复用 CPU frame/IPC 接口；真实游戏不经过普通 GDI flush。独立 WGL 测试已在两种架构得到正确颜色与 640×480→800×600 resize。东方红魔乡验证了旧版固定管线 Direct3D 的实际图像与输入，但不能替代 D3D11、Unity 或 Limbus 的验收。
Vulkan 已用于 WoW64 缓冲映射，尚未实现窗口呈现；headless 扩展不代表 DXVK swapchain 已可用。

## 验证策略

协议解析器、像素转换、IPC 帧和坐标变换适合确定性测试；截断/交错响应、异常尺寸和背压须覆盖。
Wine 集成使用小型 x86/x64 Win32 程序验证绘制、弹窗、窗口状态、focus/capture，搭配 Notepad 真实交互。
终端效果、键盘事件和退出恢复须在实际 Kitty 验证，PTY 字节流回放不能替代全部可视验证。
游戏每增加一条 API 路线即增加对应最小样例，不以单纯的 API 初始化成功作为呈现成功。

统一入口 `./winetk` 已实现无参数帮助、Steam Windows 游戏列表、持久化配置和 TUI 设置，保留项目内构建与专用 prefix。Unity 已支持按启动时终端尺寸提高实际渲染分辨率，最高 1920×1080；普通放大使用双线性插值。仓库提供无游戏依赖的 CTest/CI；系统安装包、SSH、其他后端及运行中动态改变实际分辨率仍待排期。
