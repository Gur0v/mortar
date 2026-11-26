# X11 support

These lists describe implemented features, not full protocol compliance. Tests cover selected behavior, not every client, server, or protocol interaction.

## ICCCM

**Supported**

- Manage mapped windows and eligible existing root children; ignore override-redirect windows. Set `WM_STATE` for normal, iconic, and withdrawn windows, including initial iconic state, iconify requests, and remapping.
- Read the `WM_HINTS` input flag and send `WM_TAKE_FOCUS` when advertised. Focus follows pointer entry and is restored when changing workspaces.
- Send `WM_DELETE_WINDOW` when advertised. Clients without it are not forcibly killed.
- Claim root substructure redirection and the screen's `WM_Sn` selection, announce ownership with `MANAGER`, and exit on selection loss. An existing owner is not replaced.
- Manage `WM_TRANSIENT_FOR` windows as floating.

**Partial**

- Apply valid minimum, maximum, base, increment, and aspect size hints within tiles when possible and to floating resize actions. Conflicting or impossible hints may not all be satisfied.
- Send synthetic `ConfigureNotify` events for managed configure requests and forward unmanaged requests. Lifecycle and focus handling do not cover every ICCCM interaction.

**Unsupported**

- Selection conversion targets, `WM_CLIENT_LEADER`, and `WM_COLORMAP_WINDOWS`. Other hints and protocols are not generally implemented.

## EWMH

**Supported**

- Publish `_NET_SUPPORTED`, `_NET_SUPPORTING_WM_CHECK`, and a supporting window named `mortar`. Advertised atoms do not imply complete support for their associated behavior.
- Publish desktop names, current desktop, client lists, stacking order, active window, and each managed window's `_NET_WM_DESKTOP`. Handle desktop switching and window movement requests; restore valid stored desktops on startup.
- Handle activation requests. Application requests for hidden workspaces mark the window urgent instead of switching; pager requests may switch workspaces.
- Handle `_NET_CLOSE_WINDOW` through the ICCCM close action using the request timestamp.
- Handle initial fullscreen state and add, remove, and toggle requests. Fullscreen covers output bounds, including dock space; each workspace permits one fullscreen window.
- Recognize `WM_HINTS` urgency and initial or requested `_NET_WM_STATE_DEMANDS_ATTENTION`. Focus clears urgency; mortarbar highlights urgent workspaces in red.
- Manage dialog windows as floating and recognized docks outside the tiling layout. Valid `_NET_WM_STRUT_PARTIAL`, or fallback `_NET_WM_STRUT`, reserves root-edge space on overlapping outputs; changes recalculate layout.

**Partial**

- `_NET_WORKAREA` contains one rectangle per workspace: its owning output's usable area, or the focused output's area for unassigned workspaces. Multi-output interoperability is not established.
- Struts apply only to recognized docks and produce rectangular per-output usable areas. Other state atoms are preserved, but other state and window-type behavior is not generally implemented.

**Unsupported**

- Special `_NET_WM_DESKTOP` values and most other EWMH features.

## RandR

**Supported**

- Negotiate up to RandR 1.5. Use active monitor objects with 1.5 or later, or connected outputs with active CRTCs with 1.2–1.4. Fall back to root geometry if RandR is absent, older, or has no active outputs.
- React to screen, CRTC, output, and resource notifications. Display workspaces per output.
- mortarbar creates a bar for each selected active output, repeats status, and shows that output's visible and occupied workspaces. It follows topology changes and falls back to root geometry without RandR.

**Partial**

- Root-edge bars publish partial struts. `_MORTAR_BAR_OUTPUT` reserves output-local space for other bars only in mortar.
- `_MORTAR_OUTPUT_WORKSPACES` is a private root property, not an EWMH interface. Version 1 uses 32-bit CARDINALs: version and output count, then x, y, width, height, visible workspace, focused flag, and workspace indices for each output, terminated by `0xffffffff`. Coordinates use two's-complement encoding. The list includes the visible and occupied workspaces owned by that output. mortarbar validates this metadata and falls back to the global EWMH desktop list if it is missing, invalid, or does not match the output.
## XKB

**Supported**

- Use the core XKB keyboard and xkbcommon to resolve keysyms across groups and levels, including Scheme keysym/modifier callbacks. Use the group from each key press.
- Alt plus a digit switches workspace. Add Shift to move the focused window, or Control when Shift is needed to produce the digit.
- Detect Caps, Num, and Scroll lock modifiers, grab their combinations, and reload on map or new-keyboard notifications. Failed grab replacement retains previous grabs.

**Partial**

- Missing XKB, an unavailable Alt modifier, or conflicting grabs can prevent startup. Not all keyboard maps and lock configurations have been verified.

**Unsupported**

- Separate per-device bindings; mortar uses the core keyboard.

## Tests

- `tests/client.cpp`, `tests/x11.rb`, `tests/actions.cpp`, and `tests/wm-selection.cpp` exercise lifecycle, focus, geometry, close actions, selection ownership, restart, and shutdown.
- `tests/size-hints.cpp` and `tests/floating-size-hints.cpp` cover selected size calculations. `tests/ewmh.cpp` covers client lists, activation, stacking, and urgency.
- `tests/randr.cpp` and `tests/outputs.cpp` cover discovery and output/workspace state. Bar tests cover selected output and workspace behavior.
- `tests/keyboard.cpp`, `tests/x11.rb`, and `tests/client.cpp` exercise key grabs, remapping, lock handling, French layout, and group changes.

Integration tests use Xvfb and XTEST. They do not establish interoperability with every X server, client, keyboard map, or protocol feature.
