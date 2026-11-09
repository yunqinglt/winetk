# Contributing

Start with [the development guide](docs/DEVELOPMENT.md) and
[architecture](docs/ARCHITECTURE.md). The project currently targets Linux x86-64,
Python 3.10+, a pinned Wine WoW64 build and Kitty's graphics/keyboard protocols.

For terminal, IPC or TUI changes, the default checks need no Wine build, Steam
account, game installation or graphical session:

```sh
cmake -S . -B build/check -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/check
ctest --test-dir build/check --output-on-failure
python3 scripts/check-repository.py
```

For memory-safety checks, use a separate build directory and pass
`-DWINETK_SANITIZERS=ON`. Use Debug builds for tests so C assertions are active.
Tests under `tests/test_*_live.py` are opt-in and launch real Kitty/Wine sessions;
see [the test guide](tests/README.md) before running them.

Wine development happens in the ignored `sources/wine/` checkout. Keep the
baseline in `config/wine-source.env`, modify the driver there, build/test it, then
run `scripts/export-wine-patch.sh`. Include the exported patch in your change;
do not add the Wine checkout, binaries, prefixes or game resources. Shared IPC
headers in `include/winetk/wine_protocol.h` and Wine's
`include/wine/winetk_protocol.h` must stay identical.

Describe the trigger, changed behavior, checks performed and remaining limits in
your pull request. Distinguish unit/PTY verification, captured source pixels,
terminal submissions and actual rendered game behavior. Do not claim full game
compatibility from a title-screen or initialization test.

Before attaching logs or screenshots to an issue, remove Steam identifiers,
account data and unrelated user paths. Game data and unreviewed local test logs
do not belong in Git. Follow [NOTICE.md](NOTICE.md) for project/Wine licensing.
