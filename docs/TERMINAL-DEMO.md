# M1 终端画布原型

这是独立的 C 原型：生成 RGBA 测试画面，经 Kitty Graphics Protocol 显示，解析终端键鼠与尺寸事件。默认演示模式不加载 Wine。现已复用该会话程序实现真实 Wine 模式；GDI 验证见 [M2 结果](reports/M2-RESULTS.md)，Unity/Limbus 验证见 [M4 结果](reports/M4-UNITY-RESULTS.md)。
未修改上游 Kitty 源码。

## 构建与运行

需要 C11 编译器、CMake、Ninja、pkg-config 和 libpng；运行测试还需要 Python 3。请在可交互的 Kitty 终端中执行：

```sh
./scripts/run-terminal-demo.sh
```

脚本构建 `build/terminal/winetk-terminal-demo`，日志追加到 `logs/terminal-demo.jsonl`。也可以单独构建：

```sh
cmake -S . -B build/terminal -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/terminal
./build/terminal/winetk-terminal-demo --duration 10 --log logs/demo-check.jsonl
```

程序直接独占控制终端 `/dev/tty`；将 stdout 重定向为文件不会得到画面录制。
启动时实际探测 Kitty 图形、Kitty 键盘协议及像素/字符尺寸，不仅检查 `$TERM`。缺少图形、可靠按键释放或有效尺寸时明确退出。
像素鼠标与同步更新为可选能力；像素鼠标不可用时，以字符格中心映射鼠标位置。

默认逻辑画布 **640×360**，提交上限 **15 fps**。终端较大时默认保持原始像素大小，居中留边；较小时保持比例缩小。`--zoom-in` 允许等比放大至终端可用区域，顶部固定保留一行程序名，宽高比不同则居中留边。鼠标使用同一变换映射，终端变化不改变逻辑桌面尺寸。放大后输出像素与传输/缩放开销会增加。
放大使用双线性插值，原生尺寸直接复制像素，缩小保留原采样行为。`--fit-resolution` 根据 `--width/--height` 的宽高比，在启动时选择终端首行以下可容纳的渲染尺寸，最高 1920×1080，并启用贴边显示。独立 demo 直接以该分辨率绘制；Unity 脚本把选定尺寸传给游戏的 `-screen-width/-screen-height`。一般 Wine 程序仍须自行选择渲染尺寸，启动器仅提供扩大后的虚拟显示，以及可在 argv 中使用的 `@WINETK_WIDTH@` / `@WINETK_HEIGHT@` 完整参数占位符。运行中改变终端尺寸只重新缩放输出，重新启动才改变选定的渲染尺寸。
这个 fps 参数是提交节流上限，不是终端实际显示帧率保证；慢速输出后不会补发积压帧。

| 操作 | 原型行为 |
| --- | --- |
| 方向键 | 移动可拖动方块，含重复按键 |
| 鼠标左键拖动方块 | 验证点击、移动、释放及坐标映射 |
| 滚轮 | 上下移动方块 |
| Space | 暂停/继续动画；暂停时仍响应输入和重绘 |
| 文本、Ctrl/Shift 等组合键 | 显示事件码并写日志，不实现编辑器操作 |
| Esc / Ctrl+C | 正常退出；此处 Ctrl+C 是键盘协议事件 |

失焦时清空本地按键与拖动状态。UTF-8 和粘贴按文本事件记录；完整 IME 不在此演示范围；Wine 模式的 Windows 输入桥接另见 M2 结果。

## 参数与录制

```sh
mkdir -p logs
./scripts/run-terminal-demo.sh --duration 10 --record-dir logs/demo-record-01
```

`--record-dir` 必须是**尚不存在的目录**，且父目录已经存在；程序创建权限为 0700 的目录，重复使用同一路径会失败。
默认只记录首次提交的 3 帧：`frame-000001.png` 等是缩放后的输出像素，`graphics.bin` 是这些帧实际写往 TTY 的图形传输、定位、首行程序名和可选同步更新字节。
它不记录启动探测、备用屏/键盘/鼠标模式设置、输入、退出清理，也不是带时间轴的完整终端录像。失败中断时可能留下不完整记录。

| 参数 | 范围/默认值 |
| --- | --- |
| `--width N --height N` | 320–1920 × 240–1080；默认 640×360 |
| `--zoom-in` | 无参数；允许等比放大，默认关闭，Wine/Unity/红魔乡启动脚本也支持 |
| `--fit-resolution` | 无参数；启动时按终端可用区域选择渲染分辨率，最高 1920×1080，隐含 `--zoom-in` |
| `--fps N` | 1–240；demo 默认 15，Wine 默认 60 |
| `--duration SECONDS` | 0–86400，可有小数；默认 0，等待交互退出 |
| `--log PATH` | 追加 JSONL；直接运行二进制时默认 `winetk-demo.jsonl` |
| `--record-dir PATH` | 新建录制目录，默认关闭 |
| `--record-frames N` | 1–1000；默认 3，不限制程序总运行帧数 |
| `--probe-timeout MS` | 100–10000；默认 1500 |

`--log` 必须指向可追加的普通文件；不能使用终端、管道或符号链接。父目录必须存在。脚本参数中的 `--log` 可覆盖其默认日志路径。
每次运行追加新记录，`time_ms` 从该次进程启动重新计时，阅读多次运行的日志时应按 `summary` 分段。

记录的 JSONL 事件：

- `capabilities`：图形、键盘、像素鼠标、同步更新探测结果与会话 image ID。
- `resize`：终端字符/像素尺寸及画布的输出矩形；`source_width/source_height` 为源画布尺寸，`zoom_in` 为是否允许放大。
- `render_resolution`：自动选择的启动渲染分辨率和虚拟显示尺寸；后者包含隐藏窗口边框的余量，避免客户区被限制。
- `frame`：帧号、输出尺寸、发送字节、`render_ms`（绘制和缩放）、`present_ms`（编码/TTY 写入，启用录制时包括协议文件写入）。PNG 编码时间不包含在这两个字段中。
- `key`：Kitty 键码、动作（1 按下、2 重复、3 释放）、修饰键位掩码及本地按下键数量；Shift=1、Alt=2、Ctrl=4、Super=8。方向键使用 Kitty 功能键码，不是 Unicode 字符。
- `text`、`paste`：Unicode 码点、是否属于粘贴及粘贴边界。键事件与文本事件可以由一次输入同时产生；释放不产生文本。
- `mouse`、`focus`：原始与映射坐标、画布内外、按钮/滚轮及焦点变化。鼠标 action 为 0 按下、1 释放、2 移动、3 滚轮。
- `summary`：退出码、完成帧数、输入事件数、帧传输总字节、进程总耗时、最长 present 时间及错误/丢弃序列计数。输入事件数不是物理操作次数；探测响应不计入其中。

`frame.bytes` / `summary.frame_bytes` 包括帧相关图形及同步控制字节，不包括全部会话控制流。
这些时间和帧数衡量提交链路，**不能证明终端最终显示时刻、实际显示帧率或端到端输入延迟**。PNG 证明帧源，协议记录证明输出；最终画面还需真实 Kitty 截图/观察。

## 测试与诊断

```sh
ctest --test-dir build/terminal --output-on-failure

cmake -S . -B build/terminal-sanitize -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWINETK_SANITIZERS=ON
cmake --build build/terminal-sanitize
ctest --test-dir build/terminal-sanitize --output-on-failure
```

当前 CTest 有 `input`、`kitty`、`wine`、`demo_pty`、`unity_prepare`、`scale`、`tui` 七个测试目标；覆盖输入/坐标、图形编码、有界 IPC、PTY 协商/缩放/分辨率适配/录制/清理，资源缓存的独立复制、已有条目保留和中断恢复、双线性插值，以及 Steam 扫描、配置和 curses 界面。PTY 测试不渲染图形，不能替代真实终端验证。Sanitizer 配置在 GCC/Clang 下启用 AddressSanitizer 与 UndefinedBehaviorSanitizer。

本机真实 Kitty 会话已记录 Ctrl+A、右方向键的按下/释放、Unicode 文本和 Ctrl+C 正常退出（`exit_status=0`）；记录位于 `logs/m1-live.jsonl`。
可视证据为 正常画布（本地：`logs/m1-live-kitty.png`） 与 缩小画布（本地：`logs/m1-live-downscaled.png`）。这些本机验证文件不属于游戏兼容性证明，具体测量另见本轮结果记录。

常见错误：

- 无控制 TTY：在真实 Kitty 的 shell 中运行；单纯管道执行或无 TTY 的任务不适用。
- 图形/键盘探测超时：先在 Kitty 中直接运行，检查 `--probe-timeout` 和终端响应；当前不承诺 tmux/screen/SSH 穿透。
- 尺寸不可用：检查终端能否回复像素/字符尺寸；不以猜测的字符大小继续运行。
- `terminal/recording I/O failed`：检查 TTY、磁盘空间及录制目录权限。输出有截止时间，背压超时会结束会话并尝试清理。
- 记录目录已存在：换一个新目录；不会覆盖之前的记录。

Kitty 自带 `--dump-bytes=PATH` 可额外捕获子进程完整原始输出，`--dump-commands` / `--replay-commands` 可辅助命令回放，`--debug-input` 可查看终端收到的输入。它们与本原型的像素/图形帧记录口径不同；截图用于核对最终画面。

正常退出及可处理信号会恢复 termios、备用屏、键盘栈和已保存的鼠标等模式，并只删除本会话图像。SIGKILL 等无法执行清理。程序已结束但终端状态异常时，可在该 TTY 执行：

```sh
printf '\030\033\\\033[?2026l\033[=0u\033[?1000l\033[?1002l\033[?1003l\033[?1004l\033[?1006l\033[?1016l\033[?2004l\033[?1049l\033[=0u\033[?25h' > /dev/tty
stty sane < /dev/tty
reset
```

`CSI =0u` 清除当前屏幕的 Kitty 键盘增强标志；这里在退出备用屏前后分别清除。正常程序清理使用 `CSI <u` 弹出自己压入的状态，避免破坏调用方；上述命令是异常后的人工恢复。若该终端已不能正常输入，从另一个终端把相同序列写入失常终端对应的 `/dev/pts/N`，并对该 TTY 执行 `stty sane`。

## 后续接入点

`include/winetk/backend.h` 规定 top-down、straight-alpha RGBA8 帧、stride/可读大小与独立 viewport；调用方在 `present()` 返回前持有像素。后端当前提供 `probe/present/close` 和可替换写入回调，不管理 TTY 生命周期，也不自行缩放。
`include/winetk/input.h` 提供无 I/O 的流式解析器和规范化事件，文本与物理键分开；`src/session/terminal.c` 负责原始模式、信号及恢复。
下一步将 Wine GDI surface 替换测试场景生产者，再把离屏 GPU 读回接到同一帧契约；Wine 驱动、IPC、多窗口合成与 Windows 输入注入尚待实现。
