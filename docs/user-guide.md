# Mortar User's Guide

mortar is an X11 window manager configured in Guile Scheme. It arranges windows in a tree of split containers and workspaces.

## Contents

- [Getting started](#getting-started)
- [Default keys](#default-keys)
- [Using mortar](#using-mortar)
- [The tree](#the-tree)
- [Configuration](#configuration)
- [Bar](#bar)
- [Multiple monitors](#multiple-monitors)
- [IPC and exit](#ipc-and-exit)
- [Troubleshooting](#troubleshooting)

## Getting started

Start `mortar` in an X11 session. The default modifier is Alt. Alt+Enter runs `x-terminal-emulator`, and Alt+D runs `dmenu_run`; those executables must be available on your path. mortar starts `mortarbar` separately by default.

Open windows and use Alt+B or Alt+V to choose a horizontal or vertical split. Use the focus and move shortcuts below to arrange them.

## Default keys

These bindings use Alt as mapped by XKB. Keys also require any modifiers needed to produce their keysyms on your keyboard layout.

| Shortcut | Action |
| --- | --- |
| Alt+Enter | Launch `x-terminal-emulator` |
| Alt+D | Launch `dmenu_run` |
| Alt+B / Alt+V | Set horizontal / vertical split |
| Alt+E | Toggle split direction |
| Alt+H / J / K / L | Focus left / down / up / right |
| Alt+arrow | Focus in that direction |
| Alt+Shift+H / J / K / L | Move left / down / up / right |
| Alt+Shift+arrow | Move in that direction |
| Alt+Control+arrow | Grow the focused window by 10 pixels at that edge |
| Alt+A | Select the parent tiling container |
| Alt+Shift+Q | Close the focused window |
| Alt+Shift+Space | Toggle floating |
| Alt+Space | Switch focus between floating and tiled windows |
| Alt+F | Toggle fullscreen |
| Alt+Shift+minus | Move the focused window to the scratchpad |
| Alt+minus | Show, focus, or hide a scratchpad window |
| Alt+digit | Switch workspace |
| Alt+Shift+digit | Move the focused window to a workspace |
| Alt+Shift+C | Reload configuration |

Digits 1–9 select workspaces 1–9; 0 selects workspace 10. If a digit requires Shift, use Control instead of Shift for the move shortcut. Digit levels requiring both Shift and Control are not bound. If a shortcut matches multiple digit levels, switching takes priority over moving.

## Using mortar

Use workspace shortcuts to switch workspaces or move the focused window. Workspace names start as `1` through `10`. The scratchpad holds windows outside normal workspaces until shown.

Hold Alt and drag a floating window with the left mouse button to move it. Alt+right-drag resizes a floating or tiled window from the clicked corner. Tiled resizing requires a neighboring split boundary. The mouse modifier is configurable.

## The tree

A split container groups windows horizontally or vertically. Alt+A selects its parent; repeated presses select ancestors. Split, move, resize, and insertion can act on the selected container. Focusing a window or switching workspaces clears the selection. Tabbed and stacked layouts are not available.

## Configuration

mortar loads `$XDG_CONFIG_HOME/mortar/config.scm` when `XDG_CONFIG_HOME` is nonempty and absolute. Otherwise it uses `$HOME/.config/mortar/config.scm` when `HOME` is nonempty and absolute. A missing file leaves defaults in place. If neither location is available, mortar reports an error and uses defaults.

Configuration is executable Scheme with your user privileges, not a sandbox. On startup, a failed load leaves built-in defaults active. On reload, settings and bindings change only after loading and new key grabs succeed; Scheme side effects cannot be rolled back.

For example:

```scheme
(gaps 5 10)
(floating-modifier 64)
(bind-key '(alt F6) (lambda () (split 'vertical)))
(bind-key '(alt shift Return) (lambda () (spawn "xterm")))
(window-rule "Firefox" 1 #f)
```

`(border width focused unfocused)` sets border width in pixels (0–65535) and focused and unfocused 24-bit RGB colors. For example, `(border 2 #x285577 #x333333)` adds a two-pixel border.

`gaps` sets inner and outer gaps in pixels, each from 0 to 65535; both default to zero. Outer gaps inset the usable workspace. Inner gaps inset tiles by the full amount at workspace edges and half at shared edges, rounded down. Floating and fullscreen windows are unaffected. Oversized gaps can leave tiles at the minimum X11 window size. `floating-modifier` sets the X modifier bitmask for mouse move and resize: 64 is Mod4, and 0 disables both. It does not change keyboard shortcuts.

`bind-key` takes a list with one case-sensitive XKB keysym and any of `alt`, `shift`, `control`, `mod1` through `mod5`, followed by a zero-argument procedure. Keysyms resolve across layouts and levels, including required modifiers; bindings follow keyboard map changes and ignore lock modifiers. Bindings are registered while loading; later registrations replace earlier ones, and arbitrary bindings take precedence over built-in shortcuts. The numeric form `(bind-key keysym modifiers procedure)` accepts an integer keysym and X modifier mask. `(bind-workspace digit procedure)` and `(bind-move-workspace digit procedure)` replace the callbacks for built-in workspace shortcuts.

The example window rule assigns matching new windows to desktop index 1, the second workspace. `(window-rule class workspace floating [instance [title]])` matches specified `WM_CLASS` components and title exactly. Criteria are nonempty strings or `#f`, with at least one specified; omitted criteria are `#f`. The title uses valid `_NET_WM_NAME`, otherwise `WM_NAME`. Workspace is an index from 0 to 9 or `#f`; floating is `'floating`, `'tiled`, or `#f`. Later matching rules override earlier assignments. Rules apply when windows are managed, not retroactively on reload. `window-rule-regex` takes the same arguments but searches with PCRE2 regular expressions; use `^` and `$` to anchor matches. Invalid patterns fail configuration loading; on reload, the previous configuration remains active. Matching has resource limits. A match error, including a resource limit or invalid UTF-8, is reported on standard error and the rule is treated as not matching; an ordinary non-match is not reported.

Actions below run in callbacks or IPC, not during configuration loading. `spawn` passes separate string arguments directly to an executable; it does not invoke a shell.

```scheme
(workspace 1)                    ; digit 0 selects workspace 10
(workspace "work")               ; switch by name, creating it if needed
(rename-workspace 1 "work")
(workspace-next)                 ; or workspace-previous, workspace-back-and-forth
(move-to-workspace 2)            ; optional X11 window ID as second argument
(split 'horizontal)              ; or 'vertical
(toggle-split)
(focus 'left)                    ; also 'right, 'up, 'down
(move 'right)
(resize 'right 10)               ; negative pixels shrink
(focus-parent)                   ; also focus-child, focus-sibling-next,
                                 ; focus-sibling-previous, focus-mode-toggle
(floating-toggle)                ; optional X11 window ID
(fullscreen-toggle)              ; optional X11 window ID
(close)                          ; optional X11 window ID
(scratchpad-move)                ; optional X11 window ID
(scratchpad-show)
(spawn "xterm" "-e" "top")
```

Workspace names must be unique, nonempty, no more than 4096 UTF-8 bytes, and contain no NUL. mortar saves names on the X root window, so they survive reload and restart while that X server retains its state. Up to 16384 workspaces can exist. Next and previous cycle in numeric order through occupied or visible workspaces, wrapping at either end. Back-and-forth returns to the previously focused workspace, even if empty, and does nothing before the first workspace change. These actions have no default shortcuts. Explicit window IDs must be nonzero 32-bit IDs of managed windows; hidden scratchpad windows are not valid targets. Floating resize uses valid `WM_NORMAL_HINTS` size constraints when possible; conflicting constraints may be ignored. An action that fails in a callback is reported and does not trigger the shortcut's default action; earlier side effects remain.

## Bar

mortarbar appears at the bottom by default and shows workspace buttons. Click a workspace button or scroll outside status blocks to change workspace. Configure it while loading, for example:

```scheme
(bar-position 'top)              ; 'bottom is the default
(bar-mode 'hide)                 ; 'dock is the default
(bar-font "monospace 10")
(bar-colors #x000000 #xffffff #x285577 #x333333)
(bar-workspace-buttons #t)
(bar-wrap-scroll #f)
(bar-height 40)
(bar-status-padding 4)
(bar-status-edge-padding 12)
(bar-status "status-program" "argument")
```

`bar-workspace-colors` optionally sets background and text pairs for focused, visible on another output, inactive, and urgent workspaces, in that order. It also sets each button's border to its background color. For example, `(bar-workspace-colors #x000000 #xffffff #x000000 #xffffff #x000000 #x888888 #x000000 #xff0000)` uses black backgrounds and white, gray, or red text. Without it, the existing workspace colors and borders remain unchanged.

In hide mode, hold Mod4 to reveal the bar without reserving space. An urgent workspace also reveals it. Colors are 24-bit RGB integers: background, foreground, focused workspace, and unfocused workspace. Hiding workspace buttons also disables workspace scrolling; wrap-scroll controls wrapping between displayed workspaces. Status block scrolling is unaffected. Height is 1–65535 pixels; status padding is 0–32767 and edge padding is 0–65535. Without a height override, font determines the height. Status padding defaults to 1 and is clamped to half the bar height; edge padding defaults to 3. No status producer runs by default. `bar-status` starts its program directly and reads stdout as an i3bar status stream; click events go to stdin when enabled by the producer. Events include scroll buttons 4–7, geometry, modifiers, and X11 scale 1, including for unnamed blocks. Bar setting changes restart mortarbar on successful reload. If the restart fails, the committed configuration remains active.

## Multiple monitors

mortar tracks outputs and their visible workspaces. `(focus-output-next)` and `(focus-output-previous)` focus the visible workspace on the next or previous output. `(move-to-output-next)` and `(move-to-output-previous)` move the focused window there without changing output focus. These actions cycle through output order and do nothing with fewer than two outputs. Bind them explicitly if desired:

```scheme
(bind-key '(alt F7) (lambda () (focus-output-next)))
(bar-output "HDMI-1")
```

`bar-output` restricts bars to named RandR outputs; repeat it for multiple outputs, or use `"*"` for all. Without registrations, all outputs are selected. A missing output produces no bar until it appears.

## IPC and exit

mortar exposes a local Unix socket for trusted clients to evaluate Scheme and subscribe to events. IPC evaluation runs synchronously on the window manager thread, so a long-running expression freezes window management. See the [IPC reference](ipc.md) for socket discovery, framing, and requests.

Call `(restart)` from a configured callback or IPC evaluation to restart mortar while preserving window state. Existing IPC connections close and must reconnect and resubscribe. If restart preparation or execution fails, the current session continues. A failure after the new process starts may leave the session without a window manager.

There is no default exit shortcut. Call `(exit)` from a configured callback or IPC evaluation to request graceful shutdown; managed clients are preserved. For example, add `(bind-key '(alt shift e) (lambda () (exit)))` to configuration and reload. This exits without a confirmation prompt.

## Troubleshooting

- If a program does not launch, check that its executable is installed and on mortar's path. `spawn` does not interpret shell syntax.
- If configuration fails, check mortar's error output. Startup falls back to defaults; reload retains the prior settings and bindings. Side effects already performed are not undone.
- If a shortcut behaves differently, check the XKB layout and required modifiers. A custom binding can override a default, including reload.
- If the bar is missing, check its mode and output selection, and mortar's error output for a mortarbar startup or restart failure.
- If IPC is unavailable, check mortar's error output and socket directory permissions. IPC setup failure does not prevent mortar from starting.
