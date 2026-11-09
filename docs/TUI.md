# 启动器、Steam 游戏列表与本地配置

从项目根目录执行 `./winetk`。无参数直接打印帮助页并退出，不要求交互终端、不启动 Wine，也不创建配置文件。程序使用 Python 3.10+ 标准库 `curses`，游戏画面继续由现有 C 程序与 Kitty 后端呈现。

```sh
./winetk --steam
./winetk --settings
./winetk --steam --list
./winetk --demo
./winetk --run /absolute/path/game.exe -- argument1
./winetk --config ./my-settings.json --settings
```

`--steam`、`--settings`、`--demo`、`--run` 互斥。`--steam --list` 输出 JSON，适用于没有控制终端的任务。`--demo -- ...` 可追加底层 demo 参数；`--run EXE -- ...` 的追加参数传给 Windows 程序。底层 `run-*.sh` 保留原行为，不读取新配置。

## 游戏列表与发现范围

自动检查当前用户的 `~/.local/share/Steam`、`~/.steam/steam`、`~/.steam/root`、`~/Steam`、`~/.steam`、Flatpak Steam 路径和 `$XDG_DATA_HOME/Steam`，以及 `WINETK_STEAM_DIR` / 配置中的额外路径。解析每个安装的 `steamapps/libraryfolders.vdf`，发现其他磁盘上的库；支持新旧 KeyValues 路径格式，按实际路径去重。

从 `appmanifest_*.acf` 读取 AppID、名称、安装目录和已安装标志。再检查安装内容中的 PE 可执行文件，识别 x86/x64/ARM64，过滤 Proton 兼容工具、Steam Runtime、卸载器、Unity crash handler 和常见依赖安装目录。只列出包含候选 Windows EXE 的安装；本机扫描结果为 **64.0、Library Of Ruina、Limbus Company**，没有列出本机安装的 Linux 游戏和运行时。

这里判断的是本地实际安装内容，不访问 Steam 商店，不把“商店支持 Windows”当成“本机安装了 Windows 版本”。扫描限于目录深度 5、每游戏最多检查 50,000 个文件；特殊目录布局、仅通过脚本启动、隐藏很深的 EXE 或特殊启动器可能无法自动识别。ARM64 可显示，但当前 x86/x64 Wine 构建不会启动 ARM64 游戏。识别为 Windows 游戏也不代表已通过图形或游戏兼容性验收。

| 按键 | 游戏列表 |
| --- | --- |
| ↑/↓、J/K、PageUp/PageDown | 选择与翻页 |
| Enter | 详情与可执行文件选择；再次 Enter 启动 |
| `/` | 按名称或 AppID 搜索；清空后恢复全部 |
| R | 重新扫描 |
| S | 打开设置，返回后重新扫描 |
| Q / Esc | 返回或退出 |

Steam 游戏通过项目内 Steam bridge 启动，须保持 Linux Steam 登录。64.0 和 Limbus 复用已验证的启动预设、原项目副本/prefix 和 Limbus 缓存导入。其他游戏由通用启动器创建 `out/steam-APPID/` 和 `prefixes/steam-APPID/`；Unity 应用自动使用 D3D11 与配置的渲染适配，其他应用仍由自身选择图形 API / 分辨率。首次复制使用 reflink，文件系统不支持时会占用实际游戏大小；已存在的游戏副本不自动更新。

所有启动参数以 argv 传递，不把游戏名称、EXE 或路径交给 shell 求值。启动前 curses 恢复终端；游戏退出后重开 TUI。项目 prefix 锁和原有清理逻辑保持生效，不使用原 Steam Proton prefix 运行项目 Wine。更复杂的 Steam launch config、商店启动选项、反作弊和所有游戏的专属依赖仍需后续兼容。

## 配置文件与设置页

默认文件为 `${XDG_CONFIG_HOME:-~/.config}/winetk/config.json`，也可通过 `--config PATH` 指定。缺失文件使用以下默认值；仅按 S 保存时创建目录和文件：

```json
{
  "version": 1,
  "fps": 60,
  "scaling": "fit-resolution",
  "kitty_transfer": "auto",
  "steam_roots": [],
  "proton_dir": ""
}
```

| 字段 | 有效值 |
| --- | --- |
| `version` | 当前为整数 1 |
| `fps` | 整数 1–240；终端提交上限 |
| `scaling` | `native`：保持源像素；`zoom-in`：双线性等比放大；`fit-resolution`：Unity 按启动时终端尺寸渲染，最高 1920×1080 |
| `kitty_transfer` | `auto`：探测共享内存并回退直传；`direct`：强制 TTY 直传 |
| `steam_roots` | 最多 32 个额外 Steam 安装/库目录，如 `["/mnt/Games/SteamLibrary"]`；始终也检查默认用户路径 |
| `proton_dir` | 空字符串：使用现有环境/自动默认 `Proton - Experimental`；非空：指定桥接库来源目录 |

设置页用 ↑/↓ 或 Tab 选择，Enter 编辑数值或路径，←/→ 切换枚举选项。额外 Steam 路径用 JSON 数组编辑，可正确包含空格和中文；Ctrl+U 清空编辑框，Enter 应用到表单，Esc 取消编辑。**S 保存所有表单值**，Q/Esc 离开设置页并丢弃未保存修改。

保存前校验值与配置版本，先写同目录临时文件、flush/fsync，再原子替换，权限为 0600。校验或写入失败会显示错误，保留现有配置。无效 JSON、未知字段或不支持的版本在启动时报错，不静默覆盖。程序只保存这些偏好，不采集 Steam 密码或令牌。额外库和路径随配置生效；`WINETK_STEAM_DIR` 仍作为额外扫描路径，设置中的非空 Proton 路径覆盖 `WINETK_PROTON_DIR`。

TUI 最小布局为 48 列 × 14 行；过小时显示调整提示，恢复尺寸后继续操作。Python curses 负责退出时恢复终端状态；原有游戏退出快捷键为 Ctrl+Shift+F12。

## 本轮验证

`tests/test_tui.py` 共 10 项测试：KeyValues 解析、多库与符号链接去重、Windows PE 过滤、损坏清单与目录边界、配置默认值/校验/原子失败/权限、预设和通用 argv、无参数帮助、无 TTY 列表，以及真实 PTY 下保存/重开/放弃修改/缩小恢复/退出和终端属性恢复。已纳入 CTest。

文中的本地 `logs/` 证据不随源码上传；完整自动测试说明见 [测试指南](../tests/README.md)。

本机真实 Kitty 验证完成 **设置页保存 → Steam 列表 → 选择 64.0 → 启动游戏 → Ctrl+Shift+F12 → 返回列表 → 退出**。证据： 设置页（本地：`logs/tui-live-final/settings.png`）、游戏列表（本地：`logs/tui-live-final/steam.png`）、游戏画面（本地：`logs/tui-live-final/game.png`）、返回列表（本地：`logs/tui-live-final/returned.png`）、会话日志（本地：`logs/tui-live-final/session.jsonl`）。测试使用临时配置，没有改写用户默认配置。

```sh
python3 tests/test_tui.py -v
ctest --test-dir build/terminal --output-on-failure
```
