# Development

mortar builds on Linux with a C++23 compiler, Meson, Ninja and pkg-config. Build dependencies are XCB, XCB RandR, XCB XKB, xkbcommon-x11, Guile 3.0, PCRE2 (8-bit), jq, PangoCairo and Fontconfig development libraries. Meson looks for `libjq` through pkg-config, then falls back to the `jq` library.

Tests are enabled by default. They also require XCB XTest development files, Ruby, Xvfb and XKB data and utilities. On Debian trixie, CI installs these packages:

```sh
apt-get install g++ clang meson ninja-build pkg-config guile-3.0-dev libxcb1-dev libxcb-randr0-dev libxcb-xkb-dev libxkbcommon-x11-dev libxcb-xtest0-dev libxtst-dev libjq-dev libpango1.0-dev libpcre2-dev ruby xvfb x11-xkb-utils xkb-data
```

## Build and test

```sh
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
```

X11 integration tests start their own Xvfb servers; do not run them against an active desktop. Use `-Dtests=false` at setup to omit test targets and their dependencies.

CI builds with GCC and Clang in debug and release modes, with warnings treated as errors and assertions disabled in release. It runs only `config size-hints layout workspaces keyboard actions status mortarbar`, not the full test suite. Reproduce one CI build with:

```sh
CXX=clang++ meson setup build-ci --buildtype=release -Db_ndebug=if-release -Dwerror=true
meson compile -C build-ci
meson test -C build-ci config size-hints layout workspaces keyboard actions status mortarbar --print-errorlogs
```

## Sanitizers

CI uses GCC with AddressSanitizer and UndefinedBehaviorSanitizer and runs the same test selection:

```sh
meson setup build-sanitize -Dwerror=true -Db_sanitize=address,undefined
meson compile -C build-sanitize
ASAN_OPTIONS=fast_unwind_on_malloc=0 LSAN_OPTIONS=suppressions="$PWD/tests/lsan.supp" UBSAN_OPTIONS=halt_on_error=1 meson test -C build-sanitize config size-hints layout workspaces keyboard actions status mortarbar --print-errorlogs
```

Set `CXX=g++` for setup if another compiler is the default. `fast_unwind_on_malloc=0` retains library frames in allocation stacks. The LeakSanitizer suppression matches any allocation stack containing `libguile-3.0.so`, including Guile process-global and iconv allocations. It also hides other leaks with a Guile frame, including leaks in mortar callbacks invoked by Guile. Leaks without a Guile frame and other ASan and UBSan errors remain reported. Revisit the suppression when Guile or its lifecycle changes.
