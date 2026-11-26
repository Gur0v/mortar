# IPC

mortar creates `$XDG_RUNTIME_DIR/mortar-ipc.<uid>.<pid>.sock`, using its effective UID and process ID. A nonempty `MORTARSOCK` at startup overrides this path. The selected path must be absolute, fit in a Unix socket address, and have a parent directory owned by the effective user and inaccessible to group and other users. mortar never removes an existing path to create the socket. An empty `MORTARSOCK` uses the default path.

On success, mortar exports the selected path as `MORTARSOCK` to child processes and writes it to standard error as `mortar: IPC socket PATH`. If setup fails, mortar clears `MORTARSOCK` and continues without IPC. Socket connections must have the same effective UID as mortar. IPC clients are trusted to execute Scheme with the user's privileges.

Messages are a four-byte unsigned big-endian length followed by that many bytes. Frames are limited to 65536 bytes. mortar accepts at most 32 clients and buffers at most 262144 output bytes per client. Oversized frames and clients exceeding the output limit are disconnected. Requests and responses are processed in order, except events can precede the response to the request that causes them.

- `eval EXPRESSION` evaluates Scheme synchronously on the window manager event-loop thread. The response is `ok RESULT` or `error MESSAGE`. A long-running expression freezes window management.
- `subscribe workspace` subscribes the connection to workspace changes and replies `ok subscribed`. Events have the form `event workspace N`, where N is the zero-based workspace index.
- `subscribe output` subscribes to output topology and visible workspace changes and replies `ok subscribed`. Events are `event output unspecified`. No event is sent for unchanged output state or focus changes alone. Existing outputs do not generate events on subscription.
- `subscribe window` subscribes to managed window changes and replies `ok subscribed`. Events are `event window CHANGE ID`, where `CHANGE` is `new`, `close`, `focus`, or `move`, and `ID` is the decimal X11 window ID. `move` reports workspace and directional layout moves, not geometry changes. Focus events report changes to the active managed window; no event is emitted when focus becomes empty. Existing windows do not generate `new` events on subscription.
- Unknown requests receive `error unknown request`.

A connection may subscribe to multiple event types. Repeating a subscription is harmless; closing the connection ends all its subscriptions. Events and replies use the same framing, so subscribed clients must distinguish them by their payloads. The socket is removed on normal shutdown only if it has not been replaced. Its parent directory is not removed.

## Client example

This Ruby client uses the `MORTARSOCK` exported to mortar's child processes. Run it inside the session; a shell started elsewhere must set `MORTARSOCK` to the logged socket path.

```ruby
require 'socket'

socket = UNIXSocket.new(ENV.fetch('MORTARSOCK'))
request = 'eval (+ 1 2)'
socket.write([request.bytesize].pack('N') + request)
header = socket.read(4) or raise 'connection closed'
raise 'incomplete header' unless header.bytesize == 4
size = header.unpack1('N')
raise 'oversized response' if size > 65_536
response = socket.read(size) or raise 'connection closed'
raise 'incomplete response' unless response.bytesize == size
puts response # ok 3
socket.close
```

This example sends one request without subscribing. A subscribed client must also handle event frames while waiting for a reply.
