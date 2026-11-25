# mortar

Mortar is an i3-inspired X11 tiling window manager written in C++ and configured with Guile Scheme.

## Build

Install Meson, Ninja, a C++23 compiler, Guile 3.0, XCB with RandR and XKB, and xkbcommon-x11. Then run:

```sh
meson setup build
meson compile -C build
```

To run the tests, install Ruby, Xvfb, XTEST, `setxkbmap`, and XKB data, then run `meson test -C build --print-errorlogs`.

## Configuration

Mortar loads `$XDG_CONFIG_HOME/mortar/config.scm`, or `~/.config/mortar/config.scm` when `XDG_CONFIG_HOME` is unset. See the [configuration guide](docs/configuration.md) for bindings and Scheme examples.

## Documentation

- [X11 support matrix](docs/x11-support.md)
- [Sanitizer tests](docs/sanitizers.md)

## License

[0BSD](LICENSE)
