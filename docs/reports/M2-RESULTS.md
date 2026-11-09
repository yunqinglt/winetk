# M2：真实 Wine/GDI 终端转发

已在本机真实 Kitty 中运行 Wine 11.19 的 x86-64 和 i386 Windows GDI 程序。
终端左上角 `(0,0)` 显示启动的可执行文件 basename，保留首行；画布、缩放和鼠标映射避开该行。
程序名按终端字符宽度截断；UTF-8 locale 下保留中文名称，控制字节和无效编码替换为 `?`，防止文件名成为终端指令。

## 实现

- 完整构建固定版本 Wine，新增 `wineterm.drv` 的 32/64 位 PE、Unix 与 WoW64 入口。
- `Graphics=term` 驱动选择，800×600 虚拟显示器；GDI surface 将上下方向正确的 BGRX 转为不透明 RGBA，通过私有 Unix stream socket 发送。
- 同一会话接收多个 Wine 进程；校验版本、消息长度、尺寸、UID/PID，回收断线进程窗口，最小全画布合成。上限 8 个连接、64 个窗口、64 MiB surface 存储，输入队列有界。
- 终端键盘/Unicode、SGR 鼠标经 IPC 和 Wine 硬件输入接口进入 Windows 消息队列。物理键产生的关联文本去重；粘贴单独按 Unicode 注入；失焦释放已追踪的按键与鼠标按钮。
- 随帧报告实际 Wine 前台窗口，修复首次点击前键盘落到 explorer 的问题。
- 记录 JSONL 指标、可选 PNG 和 Kitty 原始图形流。Wine stdout/stderr 写入独立文件。
- `run-wine.sh` 固定使用项目 prefix，并持有会话锁；退出时只清理该 prefix。显示环境变量从 Wine 子进程中移除。
- Wine 改动导出为 `patches/wine/0001-wineterm-driver.patch`，可在锁定上游提交干净应用；构建/应用/导出脚本均已加入仓库。

Kitty 不需要修改。它的现有图形协议和 screenshot 接口足够观察本阶段的输出。

## 已有实测证据

| 项目 | 结果 / 本机文件 |
| --- | --- |
| x64 GDI 动画和窗口装饰 | `logs/m2-live-kitty.png`；90 秒正常退出，714 次终端提交，Windows paint=1637、key down=16、character=12、left click=3 |
| i386 GDI 与 WoW64 | `logs/m2-final-input.png`；40 秒正常退出，Windows paint=615、key down=13、character=11、left/right click 各 1、wheel delta=120 |
| 首次点击前键盘 | i386 修复后、点击前 key down=10、character=8，最后码点 `U+6587`；见 `logs/m2-final-wine.log` |
| 中文输入 | `U+4E2D`、`U+6587` 到达 Windows `WM_CHAR`；当前测试 prefix 缺少对应 CJK 字体，截图显示方框，不代表码点丢失 |
| 无 X11/Wayland 显示连接 | Wine 子进程 `strace -f -e trace=connect`：`logs/m2-i386-connect.log`，有本会话 display.sock 连接，无 X11/Wayland 连接；Wine 构建同时禁用两种显示驱动 |
| 首行、resize 和恢复 | PTY 9 个场景通过；包含受限高度、首行 hit-test、缩放后鼠标映射、终端恢复、退出与中断 |
| 自动测试 | CTest 的 input、kitty、wine、demo_pty；普通构建与 Clang ASan/UBSan 通过 |
| 启动脚本 | `run-wine.sh` 实际运行 x64 12 秒正常退出，133 次终端提交；见 `logs/m2-script-status.txt`、`logs/wine-session.jsonl` |

首次 x64 实测以 10 fps 为提交上限，800×600 每帧约 2.57 MB，714 帧/90.6 秒约 20.2 MB/s；最大单次终端提交耗时 33ms。
这是整帧直传的基线，不是游戏性能预测。测试应用自带 50ms 定时器，Wine surface flush 和终端节流各有自己的频率。

## 可重复的真实终端验收

最终双架构自动实测位于 `logs/m2-final-live/`：每个架构都有 `kitty.png`、Windows 状态、会话 JSONL、Wine connect 跟踪与 `result.json`。两个架构均验证了首次点击前的键盘、中文粘贴、左右键、滚轮、首行名称和退出状态 0；截图已检查。

```sh
python3 tests/test_wine_live.py
# 或固定一个尚不存在的结果目录
python3 tests/test_wine_live.py --output-dir logs/my-live-check
```

需要已经构建并初始化的项目 prefix、本机图形会话、支持 screenshot 命令的 Kitty 和 `strace`。
该测试持有与启动脚本相同的专用 prefix 锁，会打开两个短时 Kitty 窗口；它不注册进无界面的 CTest。
普通构建与 sanitizer 的 CTest 命令见 README；另已用中文可执行文件名验证首行 UTF-8 输出。

## 操作

```sh
./scripts/build-wine.sh
./scripts/run-wine.sh
./scripts/run-wine.sh -- "$PWD/out/smoke/gdi-window-i386.exe" --duration 10
./scripts/run-wine.sh -- /absolute/path/program.exe arg1
```

Windows 应用正常使用键盘/鼠标。`Ctrl+Shift+F12` 是会话本地退出键；Esc 和 Ctrl+C 在 Wine 模式传给应用。
测试程序按 Esc 关闭。项目 prefix 的日志和字体设置与系统已有 Wine/Steam prefix 独立。

底层可执行程序仍名为 `winetk-terminal-demo`，通过 `--wine` 进入真实 Wine 模式，便于共享已验证的 TTY/协议实现。
Wine 模式逻辑尺寸固定 800×600，与驱动虚拟显示器一致；终端改变尺寸时只缩放画布。
传入 `--record-dir NEWDIR --record-frames N` 可录制；目录必须尚不存在。运行方法见 `--help` 与 [终端原型说明](../TERMINAL-DEMO.md)。

## 边界和下一步

M2 是可运行的最小驱动。当前合成顺序使用几何更新、点击与前台标记；它不等价于 Wine 的完整窗口树/Z 序。菜单、跨进程弹窗、鼠标 capture、窗口形状、layered alpha、输入法和任意键盘布局仍需 M3 专门验收。
输入使用基本 Windows VK 映射，覆盖字母数字、常用标点、导航/功能键与修饰键；尚未实现水平滚轮和所有功能键。
驱动的 shape/clip 回调当前为空，输出不透明软件表面；断线后停止发送且不自动重连。
flush 是带 250ms 绝对超时的同步 socket 写入，后续需有界队列、脏区和压缩来降低延迟/带宽。

该构建禁用了 OpenGL/Vulkan，不能运行 D3D/Unity 渲染链路；Limbus Company 尚未启动或宣称兼容。
接下来先完成 M3 的真实应用窗口语义，再用独立 GPU 样例实现离屏呈现/读回，逐步推进到 Unity IL2CPP 与目标游戏。
