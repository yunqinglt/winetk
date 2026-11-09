# Wine GDI smoke application

`gdi-window.c` is a real Windows program. It creates a 640×400 client window at
screen position (64,72), paints color bars and text through GDI, and animates an
orange box using a 50 ms Win32 timer. It uses only `user32`, `gdi32`, and the C
runtime. It does not use a terminal protocol directly.

Build both PE architectures:

```sh
./scripts/build-smoke.sh
```

Outputs are `out/smoke/gdi-window-x86_64.exe` and
`out/smoke/gdi-window-i386.exe`. `WINETK_SMOKE_OUTPUT_DIR` can override the output
directory.

Pass `--duration 5` to close after five seconds, or omit the option for an
interactive run. `--width 960 --height 720` changes the requested Win32 client
size, and the status file reports the dimensions actually returned by Windows.
Type text, use left/right arrows, move the mouse, click either
button, or scroll. Escape closes the window. There is no synthetic input inside
the application: its counters reflect messages delivered to its window procedure.

The program prints `WINETK_SMOKE` event records and a final `event=complete`
record to standard output. The final record includes paint, timer, key, character,
mouse motion, click, and wheel counters. Nonzero paint/timer counters verify that
the Windows message loop and GDI paint procedure ran; terminal frame capture is
still needed to verify the visible forwarding result.

Optional `--status Z:\absolute\unix\path\status.txt` (alias `--log`) writes
the latest counters as a plain `key=value` status file. Use a Windows path accepted
by the Wine prefix; the `Z:` mapping normally exposes Unix absolute paths. The
file updates on input, about once per second, and at shutdown. The parent launcher
should capture standard output separately from its terminal graphics stream.

For an opt-in test using real Kitty screenshots and both PE architectures, first
build Wine and initialize the project prefix with `scripts/run-wine.sh`, then run:

```sh
python3 tests/test_wine_live.py
```

This opens two short-lived Kitty windows, takes the same exclusive project prefix
lock as the launcher, and checks keyboard input before the first mouse click,
Chinese bracketed paste, clicks, wheel input, the native executable title row,
application counters and session exit status. It records screenshots and Wine
`connect` traces under a new `logs/wine-live-TIMESTAMP/` directory. Wine must connect
to the project socket and must not connect to X11/Wayland. Kitty itself uses the
local graphical session. The test is intentionally outside CTest; `strace` and
Kitty with the screenshot remote command are required. `--arch i386` or
`--arch x86_64` selects one architecture; `--output-dir NEW_PATH` selects artifacts.
Pillow is required to check exact client pixels. The x86-64 case requests 960×720,
beyond the initial 800×600 virtual desktop; i386 uses 640×400. Both assert that
Wine's reported client dimensions determine the canvas, nonclient decorations
are absent, and the lower-right client pixels survive the larger dimensions.

The build script also produces `wgl-window-{x86_64,i386}.exe` and
`d3d8-window-{x86_64,i386}.exe`. The D3D8 sample has a 640×480 client and draws an
animated triangle with a target of 60 FPS; `--duration` and `--status` record its
actual Present count, elapsed time, key presses, and clicks.

`dinput8-window-{x86_64,i386}.exe` polls a real foreground DirectInput 8 keyboard
every 10 ms. Its status distinguishes enhanced arrows from numpad 8/2/4/6 and
records down transitions, held samples, and final release state. Run the real
terminal test with `python3 tests/test_dinput_live.py --arch i386`: it holds each
Kitty enhanced arrow for about 250 ms, verifies DirectInput state, and asserts
that no corresponding numpad state appeared. This catches errors that ordinary
`WM_KEYDOWN` smoke checks miss.

## D3D11 / Unity

`d3d11-window.c` draws HLSL-generated triangle pixels through a real hardware D3D11 device, presents for eight seconds, and resizes its swapchain/client from 640×480 to 800×600. Both x86-64 and WoW64 builds are tested with:

```sh
python tests/test_wgl_live.py --api d3d11 --output logs/my-d3d11
```

Installed Unity games have a separate opt-in Kitty/Steam/connect-trace acceptance test:

```sh
python tests/test_unity_live.py --game both --output logs/my-unity
```

See [Unity results and limitations](../../docs/reports/M4-UNITY-RESULTS.md).
