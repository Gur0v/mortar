# IPC

Mortar creates `$XDG_RUNTIME_DIR/mortar-ipc.<uid>.<pid>.sock`, using its effective UID and process ID. A nonempty `MORTARSOCK` at startup overrides this path. The selected path must be absolute, fit in a Unix socket address, and have a parent directory owned by the effective user and inaccessible to group and other users. Mortar never removes an existing path to create the socket. An empty `MORTARSOCK` uses the default path.

On success, Mortar exports the selected path as `MORTARSOCK` to child processes and writes it to standard error as `mortar: IPC socket PATH`. If setup fails, Mortar clears `MORTARSOCK` and continues without IPC. Socket connections must have the same effective UID as Mortar. IPC clients are trusted to execute Scheme with the user's privileges.

Messages are a four-byte unsigned big-endian length followed by that many bytes. Frames are limited to 65536 bytes. Mortar accepts at most 32 clients and buffers at most 262144 output bytes per client. Oversized frames and clients exceeding the output limit are disconnected. Requests and responses are processed in order, except events can precede the response to the request that causes them.

- `eval EXPRESSION` evaluates Scheme synchronously on the window manager event-loop thread. The response is `ok RESULT` or `error MESSAGE`. A long-running expression freezes window management.
- `subscribe workspace` subscribes the connection to workspace changes and replies `ok subscribed`. Events have the form `event workspace N`, where N is the zero-based workspace index.
- `subscribe window` subscribes to managed window changes and replies `ok subscribed`. Events are `event window CHANGE ID`, where `CHANGE` is `new`, `close`, `focus`, or `move`, and `ID` is the decimal X11 window ID. `move` reports workspace and directional layout moves, not geometry changes. Focus events report changes to the active managed window; no event is emitted when focus becomes empty. Existing windows do not generate `new` events on subscription.
- Unknown requests receive `error unknown request`.

Closing the connection ends its subscription. The socket is removed on normal shutdown only if it has not been replaced. Its parent directory is not removed.
