# mortar

mortar is a tiling window manager for X11, written in C++ and configured with Guile Scheme.

Arrange windows in split containers, organize them across named workspaces and monitors, and customize your session with executable Scheme. mortar includes floating windows, gaps, a scratchpad, and a separate `mortarbar` process.

## Build

You need a C++23 compiler, Meson, Ninja, pkg-config, Guile 3.0, XCB (including RandR, Render, and XKB), xkbcommon-x11, PCRE2, libjq, PangoCairo, and Fontconfig. To run integration tests, also install Ruby, Xvfb, XCB Composite, XTEST, and XKB utilities and data.

```sh
meson setup build --buildtype=release
meson compile -C build
meson test -C build --print-errorlogs
```

See [Development](docs/development.md) for details.

## Configuration

mortar requires `$XDG_CONFIG_HOME/mortar/config.scm`, or `~/.config/mortar/config.scm` when `XDG_CONFIG_HOME` is unset or not absolute. It has no built-in keyboard shortcuts. Copy the installed example to your configuration directory before starting mortar; see the [User’s Guide](docs/user-guide.md) for commands and migration instructions. For example, add:

```scheme
(bind-key '(alt Return) (lambda () (spawn "alacritty")))
(gaps 10 5)
```

Configuration is executable Scheme and runs with your user privileges. Startup fails if configuration cannot be loaded; a failed reload retains the previous settings and bindings. See the [User’s Guide](docs/user-guide.md) for example bindings and available settings.

## Documentation

- [User’s Guide](docs/user-guide.md)
- [IPC reference](docs/ipc.md)
- [X11 support matrix](docs/x11-support.md)
- [Development](docs/development.md)

## License

[0BSD](LICENSE)
