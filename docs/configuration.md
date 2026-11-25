# Configuration

Mortar loads Guile Scheme configuration at startup from `$XDG_CONFIG_HOME/mortar/config.scm` when `XDG_CONFIG_HOME` is nonempty and absolute. Otherwise, it uses `$HOME/.config/mortar/config.scm` when `HOME` is nonempty and absolute. If neither location is available, Mortar reports an error and uses defaults. A missing configuration file also leaves the defaults in place.

Configuration is trusted executable Scheme, running with your user privileges. It is not sandboxed; even a failed load can have side effects. There is no configuration reload yet; restart Mortar to apply changes.

## Workspace bindings

The built-in shortcuts are Alt+digit to switch workspaces and Alt+Shift+digit to move the focused window. Digits are integers from 0 through 9: 1 selects workspace 1, through 9 for workspace 9, and 0 selects workspace 10. Keyboard layouts may require additional modifiers to produce a digit.

Configuration can replace the callbacks for these built-in shortcuts, but cannot change their keys or modifiers or add arbitrary shortcuts. A callback takes no arguments. For example:

```scheme
(bind-workspace 1 (lambda () (workspace 2)))
(bind-move-workspace 0 (lambda () (move-to-workspace 3)))
```

Here Alt+1 switches to workspace 2, and Alt+Shift+0 moves the focused window to workspace 3. Other shortcuts retain their default actions.

- `(bind-workspace digit procedure)` replaces the switch callback for that digit.
- `(bind-move-workspace digit procedure)` replaces the move callback for that digit.
- `(workspace digit)` switches to the specified workspace.
- `(move-to-workspace digit)` moves the focused window to the specified workspace.

Bindings can only be registered while loading configuration. Workspace actions can only be called from a callback, not during configuration loading. Repeated registrations for the same shortcut replace the previous callback.

If loading fails, Mortar reports the error and starts with built-in defaults. If a callback fails, Mortar reports the error and does not run that shortcut's default action for that invocation. Effects already performed by Scheme are not rolled back.
