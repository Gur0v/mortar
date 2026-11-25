# Configuration

Mortar loads Guile Scheme configuration at startup from `$XDG_CONFIG_HOME/mortar/config.scm` when `XDG_CONFIG_HOME` is nonempty and absolute. Otherwise, it uses `$HOME/.config/mortar/config.scm` when `HOME` is nonempty and absolute. If neither location is available, Mortar reports an error and uses defaults. A missing configuration file also leaves the defaults in place.

Configuration is trusted executable Scheme, running with your user privileges. It is not sandboxed; even a failed load can have side effects.

Press Alt+Shift+C to reload the same configuration path. Mortar replaces settings and bindings only after the file loads and all new key grabs succeed. If either fails, it reports the error and keeps the current configuration and bindings. Scheme side effects, including external effects, are not rolled back. An arbitrary binding can override the reload shortcut.

## Workspace bindings

The built-in shortcuts use Alt plus the modifiers required to produce a digit to switch workspaces. Add Shift to move the focused window, or add Control instead when the digit requires Shift. Digit levels requiring both Shift and Control are not bound. If a shortcut matches multiple digit levels, switch actions take priority over move actions. Digits are integers from 0 through 9: 1 selects workspace 1, through 9 for workspace 9, and 0 selects workspace 10.

Configuration can replace the callbacks for these built-in shortcuts. A callback takes no arguments. For example:

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

## Arbitrary bindings and actions

`(bind-key keysym modifiers procedure)` registers a zero-argument callback for an X keysym and X modifier bitmask, both integers. For example, `(bind-key 65475 8 (lambda () (split 'vertical)))` binds Alt+F6. Repeated registrations replace the previous callback; a matching arbitrary binding takes precedence over a built-in shortcut, including workspace shortcuts.

Callbacks may call `(split 'horizontal)` or `(split 'vertical)`, `(focus 'left)`, `(focus 'right)`, `(focus 'up)`, `(focus 'down)`, and `(close)` for the focused window. `(move direction)` moves a tiled window, and `(resize direction pixels)` resizes a tiled or floating window at the specified edge. Positive pixels grow it; negative pixels shrink it. Floating resize applies valid `WM_NORMAL_HINTS` size constraints when possible; conflicting constraints may be ignored. Alt+Control+arrow grows the focused window by 10 pixels at that edge; tiled resizing requires a neighbor at that edge. `(spawn executable argument ...)` launches an executable with separate string arguments, without a shell. Invoke a shell explicitly if needed. These actions, like workspace actions, are only available in callbacks, not while loading configuration.

`(floating-toggle)` and `(fullscreen-toggle)` toggle the focused window. `(scratchpad-move)` moves the focused window to the scratchpad; `(scratchpad-show)` shows, focuses, or hides a scratchpad window. These actions also work in IPC evaluations. They are unavailable while loading configuration.

## Window rules

`(window-rule "Class" workspace floating)` matches the class component of `WM_CLASS` exactly. `workspace` is a desktop index from 0 to 9 or `#f` to leave it unchanged. `floating` is `'floating`, `'tiled`, or `#f` to leave the classification unchanged. Later matching rules override fields set by earlier rules. Rules apply when a window is managed and are replaced on successful configuration reload; existing windows are not reassigned automatically.

## Bar appearance

`(bar-position 'top)` and `(bar-position 'bottom)` select the edge of each output; the default is top. `(bar-font "monospace 10")` selects a Pango font description. The string must be nonempty and at most 4096 UTF-8 bytes, without NUL.

`(bar-output "HDMI-1")` restricts the bar to named RandR outputs. Repeat it to select multiple outputs; `"*"` selects all outputs. Without registrations, all outputs are selected. Names must be nonempty, at most 4096 UTF-8 bytes, and contain no NUL; at most 64 registrations are allowed. A missing output produces no bar until it appears.

`(bar-colors background foreground focused unfocused)` sets 24-bit RGB integer colors. Defaults are `#x222222`, `#xffffff`, `#x285577`, and `#x333333`. Focused and unfocused colors fill workspace buttons; foreground colors their text and default status text. Status blocks may override their own colors.

These settings are accepted only while loading. A successful reload restarts mortarbar if any changed, with the same failure behavior as a status-command change.

## Bar status

`(bar-status "program" "argument" ...)` selects a status producer. Mortarbar starts it directly, without a shell, and reads its stdout as an i3bar status stream. Click events go to its stdin. No producer runs by default. Arguments are separate strings; the program must be nonempty. The last registration wins. After a successful reload, a changed status command restarts mortarbar. Mortar stops the old bar before starting its replacement, escalating to SIGKILL if needed. Restart failures are reported; the committed configuration remains active.
