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
- `(workspace digit)` switches to the specified workspace. `(workspace "name")` switches by exact name, creating a workspace if the name is new.
- `(rename-workspace digit "name")` renames a numbered slot and immediately updates `_NET_DESKTOP_NAMES` and the bar. Names must be unique, nonempty, at most 4096 UTF-8 bytes, and contain no NUL. Duplicate names raise errors. Names start as `"1"` through `"10"` and last until Mortar exits or restarts; reload does not reset them. Up to 16384 workspaces can exist. These actions are available in callbacks and IPC, not while loading configuration.
- `(workspace-next)` and `(workspace-previous)` switch among occupied or visible workspaces in numeric order, wrapping at either end.
- `(workspace-back-and-forth)` switches to the previously focused workspace, including one that became empty. It does nothing until a workspace change occurs. These actions have no default shortcuts; use `bind-key` to assign them.
- `(move-to-workspace digit [xid])` moves the focused window or the window identified by `xid` to the specified workspace.
- `(focus-output-next)` and `(focus-output-previous)` focus the visible workspace on the next or previous output. `(move-to-output-next)` and `(move-to-output-previous)` move the focused window to that workspace without changing output focus. These actions cycle through the outputs model's order, wrap at either end, and do nothing with fewer than two outputs. They have no default shortcuts.

Bindings can only be registered while loading configuration. Workspace actions can only be called from a callback, not during configuration loading. Repeated registrations for the same shortcut replace the previous callback.

If loading fails, Mortar reports the error and starts with built-in defaults. If a callback fails, Mortar reports the error and does not run that shortcut's default action for that invocation. Effects already performed by Scheme are not rolled back.

## Gaps

`(gaps inner outer)` sets nonnegative pixel gaps from 0 to 65535; both default to zero. Outer gaps inset the usable workspace. Inner gaps inset tiled windows by the full amount at workspace edges and half at shared edges, rounding down. Floating and fullscreen windows are unaffected. Oversized gaps may leave tiles with the minimum X11 window size. Settings take effect on successful reload.

## Mouse controls

Hold Alt and drag with the left mouse button to move a floating window. Hold Alt and drag with the right mouse button to resize a floating or tiled window from the clicked corner. Tiled windows resize live when neighboring split boundaries are available. `(floating-modifier modifiers)` sets the X modifier bitmask for both mouse actions; for example, `(floating-modifier 64)` uses Mod4. Zero disables these mouse actions. The default follows the keyboard's Alt mapping. This setting does not change keyboard shortcuts. Reload preserves the old configuration if replacement grabs fail.

## Arbitrary bindings and actions

`(bind-key '(alt shift j) procedure)` registers a zero-argument callback for Alt+Shift+J. The list contains exactly one XKB keysym name, case-sensitive, and any of `alt`, `shift`, `control`, `mod1`, `mod2`, `mod3`, `mod4`, and `mod5`. For example, `(bind-key '(alt F6) (lambda () (split 'vertical)))` binds Alt+F6. Alt follows the keyboard mapping; Mod1 through Mod5 use their X modifier masks. Keysyms resolve against all keyboard layouts and levels, including the modifiers required to produce the symbol; bindings follow keyboard map changes. Lock modifiers are ignored.

The numeric form `(bind-key keysym modifiers procedure)` remains supported, with an integer X keysym and X modifier bitmask. For example, `(bind-key 65475 8 (lambda () (split 'vertical)))` binds Mod1+F6. Repeated registrations replace the previous callback; a matching arbitrary binding takes precedence over a built-in shortcut, including workspace shortcuts.

Callbacks may call `(split 'horizontal)` or `(split 'vertical)`, `(toggle-split)` to toggle the focused tiled container's split direction (Alt+E by default), `(focus 'left)`, `(focus 'right)`, `(focus 'up)`, `(focus 'down)`, and `(close)` for the focused window. `(move direction)` moves a tiled window, and `(resize direction pixels)` resizes a tiled or floating window at the specified edge. Positive pixels grow it; negative pixels shrink it. Floating resize applies valid `WM_NORMAL_HINTS` size constraints when possible; conflicting constraints may be ignored. Alt+Control+arrow grows the focused window by 10 pixels at that edge; tiled resizing requires a neighbor at that edge. `(spawn executable argument ...)` launches an executable with separate string arguments, without a shell. Invoke a shell explicitly if needed. These actions, like workspace actions, are only available in callbacks, not while loading configuration.

`(exit)` requests graceful window manager shutdown from a callback or IPC evaluation. Managed clients are preserved. It is unavailable while loading configuration. No default exit shortcut is provided: i3's default uses `i3-nagbar` and a shell command for confirmation, which Mortar does not provide. An IPC client requesting exit may disconnect before receiving its response.

`(focus-mode-toggle)` switches focus between the most recently focused tiling and floating windows on the current workspace. It does nothing when the other mode has no window. Alt+Space runs this action by default. `(focus-sibling-next)` and `(focus-sibling-previous)` focus the next or previous sibling without wrapping. They have no default shortcuts.

`(floating-toggle)` and `(fullscreen-toggle)` toggle the focused window. `(close xid)`, `(floating-toggle xid)`, and `(fullscreen-toggle xid)` instead target a managed window by its nonzero 32-bit X11 window ID. An invalid or unmanaged ID raises an error. Hidden scratchpad windows are not valid targets. Omitting the ID keeps the focused-window behavior. Fullscreen may change focus, as with the focused-window action. `(scratchpad-move [xid])` moves the focused window or the window identified by `xid` to the scratchpad; `(scratchpad-show)` shows, focuses, or hides a scratchpad window. These actions also work in IPC evaluations. They are unavailable while loading configuration.

## Window rules

`(window-rule class workspace floating [instance [title]])` matches the specified class and instance components of `WM_CLASS` and window title exactly. Each criterion is a nonempty string or `#f`; omitted criteria default to `#f`. At least one criterion is required, and all specified criteria must match. The title uses `_NET_WM_NAME` when valid, otherwise `WM_NAME`. Existing `(window-rule "Class" workspace floating)` rules remain valid. `workspace` is a desktop index from 0 to 9 or `#f` to leave it unchanged. `floating` is `'floating`, `'tiled`, or `#f` to leave the classification unchanged. Later matching rules override fields set by earlier rules. Rules apply when a window is managed and are replaced on successful configuration reload; existing windows are not reassigned automatically.

`(window-rule-regex class workspace floating [instance [title]])` accepts the same arguments and assignments, but treats each specified criterion as a C++ ECMAScript regular expression. It searches for a match anywhere in each value; use `^` and `$` to anchor it. Invalid patterns fail configuration loading, leaving the previous configuration active on reload. Literal `window-rule` matching remains exact.

## Bar appearance

`(bar-mode 'dock)` shows the bar and reserves space; it is the default. `(bar-mode 'hide)` hides the bar and releases its space. Hold Mod4 to reveal the bar without reserving space. Change the configuration and reload to switch modes. Sway's urgency and binding-mode reveal behavior are not supported.

`(bar-position 'top)` and `(bar-position 'bottom)` select the edge of each output; the default is bottom. `(bar-font "monospace 10")` selects a Pango font description. The string must be nonempty and at most 4096 UTF-8 bytes, without NUL.

`(bar-output "HDMI-1")` restricts the bar to named RandR outputs. Repeat it to select multiple outputs; `"*"` selects all outputs. Without registrations, all outputs are selected. Names must be nonempty, at most 4096 UTF-8 bytes, and contain no NUL; at most 64 registrations are allowed. A missing output produces no bar until it appears.

`(bar-colors background foreground focused unfocused)` sets 24-bit RGB integer colors. Defaults are `#x000000`, `#xffffff`, `#x285577`, and `#x333333`. Focused and unfocused colors fill workspace buttons; foreground colors their text and default status text. Status blocks may override their own colors.

`(bar-workspace-buttons #f)` hides workspace buttons and disables workspace scrolling. The default is `#t`. `(bar-wrap-scroll #t)` wraps scrolling between the first and last displayed workspaces; the default is `#f`. Both accept only booleans. Status block scrolling remains unaffected.

`(bar-height 40)` overrides bar height in pixels, from 1 to 65535. By default, height is calculated from the configured font. `(bar-status-padding 4)` sets vertical padding around status block backgrounds and borders, from 0 to 32767; the default is 1. Padding is clamped to half the bar height. `(bar-status-edge-padding 12)` sets the space between status blocks and the right edge, from 0 to 65535; the default is 3. Text remains vertically centered.

These settings are accepted only while loading. A successful reload restarts mortarbar if any changed, with the same failure behavior as a status-command change.

## Bar status

`(bar-status "program" "argument" ...)` selects a status producer. Mortarbar starts it directly, without a shell, and reads its stdout as an i3bar status stream. Click events go to its stdin. No producer runs by default. Arguments are separate strings; the program must be nonempty. The last registration wins. After a successful reload, a changed status command restarts mortarbar. Mortar stops the old bar before starting its replacement, escalating to SIGKILL if needed. Restart failures are reported; the committed configuration remains active.

When the producer enables `click_events`, button presses over status blocks are sent as JSON, including scroll buttons 4–7, coordinates, block dimensions, modifiers, and `scale` (always 1 on X11). Unlike Swaybar, Mortarbar also sends events for blocks without a name and does not send Sway's Wayland-specific `event` field. Scroll outside status blocks changes workspace without wrapping.
