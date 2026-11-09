# Tests

## Automated checks (CTest / CI)

These tests use only a C compiler, libpng/zlib, Python's standard library and Linux
PTY/socket support. They do not load Wine or open a graphical window.

```sh
cmake -S . -B build/check -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/check
ctest --test-dir build/check --output-on-failure
```

| Target | Scope |
| --- | --- |
| `input` | Keyboard/mouse parsing, fragmented replies and viewport coordinates |
| `kitty` | Graphics encoding, bounded writes and transfer cleanup |
| `wine` | Local protocol, frame validation, composition and input routing |
| `scale` | Known bilinear pixels, edge clamping, one-pixel axes and native copies |
| `demo_pty` | Capability negotiation, resize, title reservation, recordings, signals and terminal restoration |
| `unity_prepare` | Isolated cache copies, interrupted import and preservation of existing resources |
| `tui` | Steam discovery/PE filtering, local configuration, argv construction, curses forms and PTY restoration |

ASan/UBSan: repeat the configure step with a fresh build directory and
`-DWINETK_SANITIZERS=ON`. Debug builds keep C `assert()` checks active.

## Opt-in integration tests

`tests/smoke/` contains Windows test **sources**, not prebuilt executables. Build
them using `scripts/build-smoke.sh`; see [smoke instructions](smoke/README.md).

The `test_*_live.py` scripts require a built project Wine, a graphical host session
running Kitty (including its screenshot remote command), `strace` and Pillow.
They open private test windows and write to a new directory under ignored `logs/`.
Steam/game tests additionally require locally installed, owned games and a signed-in
Linux Steam client. They are not run by CTest or GitHub Actions.

```sh
python3 tests/test_wine_live.py --arch x86_64
python3 tests/test_wgl_live.py --api d3d11 --output logs/my-d3d11
python3 tests/test_dinput_live.py --arch i386
python3 tests/test_unity_live.py --game limbus --fit-resolution --output logs/my-unity
```

Inspect `--help` before running a live test. Keep original Steam/Proton prefixes
separate; use the project scripts and prefix locks. Exit a project game with
Ctrl+Shift+F12. Source pixels and terminal submission FPS do not establish
display latency, stable gameplay FPS or complete game compatibility.
