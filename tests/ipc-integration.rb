require 'socket'
require 'tmpdir'
require 'timeout'
require 'fiddle/import'

module X11
  extend Fiddle::Importer
  dlload File.join(IO.popen(['pkg-config', '--variable=libdir', 'x11'], &:read).strip, 'libX11.so.6')
  extern 'void *XOpenDisplay(char*)'
  extern 'unsigned long XDefaultRootWindow(void*)'
  extern 'unsigned long XInternAtom(void*, char*, int)'
  extern 'int XGetWindowProperty(void*, unsigned long, unsigned long, long, long, int, unsigned long, void*, void*, void*, void*, void*)'
  extern 'int XFree(void*)'
  extern 'int XCloseDisplay(void*)'
end

Dir.mktmpdir('mortar-ipc') do |dir|
  Dir.mkdir(File.join(dir, 'mortar'))
  File.write(File.join(dir, 'mortar', 'config.scm'), '#t')
  reader, writer = IO.pipe
  server = Process.spawn('Xvfb', '-displayfd', '3', '-screen', '0', '800x600x24', 3 => writer, out: File::NULL, err: File::NULL)
  writer.close
  wm = nil
  client = nil
  begin
    display = Timeout.timeout(10) { reader.gets }
    raise 'Xvfb failed' unless display
    log_reader, log_writer = IO.pipe
    wm = Process.spawn({ 'DISPLAY' => ":#{Integer(display)}", 'XDG_RUNTIME_DIR' => dir, 'XDG_CONFIG_HOME' => dir, 'MORTARSOCK' => nil }, ARGV.fetch(0), err: log_writer)
    log_writer.close
    path = Timeout.timeout(10) do
      loop do
        line = log_reader.gets or raise 'Mortar exited before IPC setup'
        break line.split('IPC socket ', 2).last.strip if line.include?('IPC socket ')
      end
    end
    expected = File.join(dir, "mortar-ipc.#{Process.uid}.#{wm}.sock")
    raise "unexpected socket path: #{path}" unless path == expected
    socket = UNIXSocket.new(path)
    send_frame = ->(text) { socket.write([text.bytesize].pack('N') + text) }
    read_frame = -> { Timeout.timeout(5) { size = socket.read(4).unpack1('N'); socket.read(size) } }
    send_frame.call('subscribe workspace')
    raise 'subscription failed' unless read_frame.call == 'ok subscribed'
    send_frame.call('eval (getenv "MORTARSOCK")')
    raise 'socket environment missing' unless read_frame.call == %(ok "#{path}")
    send_frame.call('eval (+ 1 2)')
    raise 'evaluation failed' unless read_frame.call == 'ok 3'
    send_frame.call('eval (error "failure")')
    raise 'evaluation error missing' unless read_frame.call.start_with?('error ')
    send_frame.call('subscribe output')
    raise 'output subscription failed' unless read_frame.call == 'ok subscribed'
    send_frame.call('subscribe output')
    raise 'repeat output subscription failed' unless read_frame.call == 'ok subscribed'
    send_frame.call('eval (workspace-back-and-forth)')
    raise 'initial back-and-forth changed workspace' unless read_frame.call.start_with?('ok ')
    send_frame.call('eval (workspace 2)')
    frames = 3.times.map { read_frame.call }
    raise "workspace or output event missing: #{frames.inspect}" unless frames.include?('event workspace 1') && frames.include?('event output unspecified') && frames.any? { |f| f.start_with?('ok ') }
    send_frame.call('eval (workspace 2)')
    raise 'unexpected event for unchanged workspace' unless read_frame.call.start_with?('ok ')
    ['workspace-back-and-forth', 'workspace-back-and-forth'].zip([0, 1]).each do |action, workspace|
      send_frame.call("eval (#{action})")
      frames = 3.times.map { read_frame.call }
      raise "navigation failed: #{frames.inspect}" unless frames.include?("event workspace #{workspace}") && frames.include?('event output unspecified') && frames.any? { |f| f.start_with?('ok ') }
    end
    send_frame.call('eval (workspace-next)')
    raise 'empty workspace should not be visited' unless read_frame.call.start_with?('ok ')
    connection = X11.XOpenDisplay(":#{Integer(display)}")
    raise 'cannot open display' if connection.null?
    root = X11.XDefaultRootWindow(connection)
    property = lambda do |name, type|
      actual_type = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
      format = Fiddle::Pointer.malloc(Fiddle::SIZEOF_INT)
      count = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
      remaining = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
      data = Fiddle::Pointer.malloc(Fiddle::SIZEOF_VOIDP)
      atom = X11.XInternAtom(connection, name, 0)
      result = X11.XGetWindowProperty(connection, root, atom, 0, 16384, 0, type, actual_type, format, count, remaining, data)
      raise "invalid #{name} property" unless result == 0 && actual_type[0, Fiddle::SIZEOF_LONG].unpack1('L!') == type && remaining[0, Fiddle::SIZEOF_LONG].unpack1('L!') == 0
      values = Fiddle::Pointer.new(data[0, Fiddle::SIZEOF_VOIDP].unpack1('J'))
      size = count[0, Fiddle::SIZEOF_LONG].unpack1('L!')
      value = format[0, Fiddle::SIZEOF_INT].unpack1('I') == 8 ? values[0, size] : values[0, size * Fiddle::SIZEOF_LONG].unpack('L!*')
      X11.XFree(values)
      value
    end
    wait_for_property = lambda do |name, type, expected|
      Timeout.timeout(1) do
        loop do
          break if property.call(name, type) == expected
          sleep 0.01
        end
      end
    end
    utf8 = X11.XInternAtom(connection, 'UTF8_STRING', 0)
    names = (1..10).map(&:to_s)
    send_frame.call('eval (rename-workspace 3 "web-é")')
    raise 'rename failed' unless read_frame.call.start_with?('ok ')
    names[2] = 'web-é'
    wait_for_property.call('_NET_DESKTOP_NAMES', utf8, names.join("\0").b + "\0")
    send_frame.call('eval (workspace "web-é")')
    frames = 3.times.map { read_frame.call }
    raise "named switch failed: #{frames.inspect}" unless frames.include?('event workspace 2') && frames.include?('event output unspecified') && frames.any? { |f| f.start_with?('ok ') }
    wait_for_property.call('_NET_CURRENT_DESKTOP', 6, [2])
    ['(rename-workspace 4 "web-é")'].each do |expression|
      send_frame.call("eval #{expression}")
      raise "invalid workspace action accepted: #{expression}" unless read_frame.call.start_with?('error ')
    end
    raise 'failed action changed EWMH names' unless property.call('_NET_DESKTOP_NAMES', utf8) == names.join("\0").b + "\0"
    raise 'failed action changed current desktop' unless property.call('_NET_CURRENT_DESKTOP', 6) == [2]
    send_frame.call('eval (workspace "new-é")')
    frames = 3.times.map { read_frame.call }
    raise "named creation failed: #{frames.inspect}" unless frames.include?('event workspace 10') && frames.include?('event output unspecified') && frames.any? { |f| f.start_with?('ok ') }
    names << 'new-é'
    wait_for_property.call('_NET_DESKTOP_NAMES', utf8, names.join("\0").b + "\0")
    wait_for_property.call('_NET_NUMBER_OF_DESKTOPS', 6, [11])
    wait_for_property.call('_NET_CURRENT_DESKTOP', 6, [10])
    send_frame.call('eval (workspace 2)')
    frames = 3.times.map { read_frame.call }
    raise "workspace reset failed: #{frames.inspect}" unless frames.include?('event workspace 1') && frames.include?('event output unspecified') && frames.any? { |f| f.start_with?('ok ') }
    send_frame.call('subscribe window')
    raise 'window subscription failed' unless read_frame.call == 'ok subscribed'
    client_reader, client_output = IO.pipe
    client_input, client_writer = IO.pipe
    client = Process.spawn({ 'DISPLAY' => ":#{Integer(display)}" }, ARGV.fetch(1), 'ipc-window', in: client_input, out: client_output)
    client_input.close
    client_output.close
    window = Timeout.timeout(5) { Integer(client_reader.gets) }
    events = []
    Timeout.timeout(5) do
      events << read_frame.call until events.include?("event window new #{window}") && events.include?("event window focus #{window}")
    end
    ['close', 'floating-toggle', 'fullscreen-toggle', 'scratchpad-move'].each do |action|
      send_frame.call("eval (#{action} 4294967295)")
      raise "unmanaged target accepted by #{action}" unless read_frame.call.start_with?('error ')
    end
    ['floating-toggle', 'floating-toggle', 'fullscreen-toggle', 'fullscreen-toggle'].each do |action|
      send_frame.call("eval (#{action} #{window})")
      raise "targeted #{action} failed" unless read_frame.call.start_with?('ok ')
    end
    send_frame.call('eval (move-to-workspace 3 4294967295)')
    raise 'unmanaged move target accepted' unless read_frame.call.start_with?('error ')
    send_frame.call("eval (move-to-workspace 3 #{window})")
    frames = [read_frame.call, read_frame.call]
    raise "window move missing: #{frames.inspect}" unless frames.include?("event window move #{window}") && frames.any? { |f| f.start_with?('ok ') }
    send_frame.call("eval (scratchpad-move #{window})")
    raise 'targeted scratchpad move failed' unless read_frame.call.start_with?('ok ')
    send_frame.call("eval (scratchpad-move #{window})")
    raise 'hidden scratchpad target accepted' unless read_frame.call.start_with?('error ')
    client_writer.puts
    client_writer.close
    Timeout.timeout(5) { events << read_frame.call until events.include?("event window close #{window}") }
    Process.wait(client)
    client = nil
    child_env = File.join(dir, 'child-env')
    send_frame.call(%(eval (spawn "/bin/sh" "-c" "printf %s $MORTARSOCK > $1" "sh" "#{child_env}")))
    raise 'spawn failed' unless read_frame.call.start_with?('ok ')
    Timeout.timeout(5) { Thread.pass until File.exist?(child_env) && File.size?(child_env) }
    raise 'child socket environment missing' unless File.read(child_env) == path
    socket.close
    Process.kill('KILL', wm)
    Process.wait(wm)
    wm = nil
    custom = File.join(dir, 'custom.sock')
    log_reader.close
    log_reader, log_writer = IO.pipe
    wm = Process.spawn({ 'DISPLAY' => ":#{Integer(display)}", 'XDG_RUNTIME_DIR' => dir, 'XDG_CONFIG_HOME' => dir, 'MORTARSOCK' => custom }, ARGV.fetch(0), err: log_writer)
    log_writer.close
    selected = Timeout.timeout(10) do
      loop do
        line = log_reader.gets or raise 'Mortar exited before IPC setup'
        break line.split('IPC socket ', 2).last.strip if line.include?('IPC socket ')
      end
    end
    raise 'override ignored' unless selected == custom
    ready = UNIXSocket.new(custom)
    ready.write([12].pack('N') + 'eval (+ 1 2)')
    begin
      raise 'second WM not ready' unless ready.read(4).unpack1('N') == 4 && ready.read(4) == 'ok 3'
    rescue StandardError => error
      raise "#{error}: #{log_reader.read}"
    end
    ready.close
    raise 'workspace names lost on startup' unless property.call('_NET_DESKTOP_NAMES', utf8) == names.join("\0").b + "\0"
    raise 'workspace count lost on startup' unless property.call('_NET_NUMBER_OF_DESKTOPS', 6) == [11]
    X11.XCloseDisplay(connection)
    UNIXSocket.new(custom).close
    Process.kill('KILL', wm)
    Process.wait(wm)
    wm = nil
    stale_inode = File.lstat(custom).ino
    log_reader.close
    log_reader, log_writer = IO.pipe
    wm = Process.spawn({ 'DISPLAY' => ":#{Integer(display)}", 'XDG_RUNTIME_DIR' => dir, 'XDG_CONFIG_HOME' => dir, 'MORTARSOCK' => custom }, ARGV.fetch(0), err: log_writer)
    log_writer.close
    fallback = Timeout.timeout(10) do
      loop do
        line = log_reader.gets or raise 'Mortar exited before IPC setup'
        break line.split('IPC socket ', 2).last.strip if line.include?('IPC socket ')
      end
    end
    raise 'fallback path incorrect' unless fallback == File.join(dir, "mortar-ipc.#{Process.uid}.#{wm}.sock")
    raise 'stale socket replaced' unless File.lstat(custom).ino == stale_inode
    restarted = UNIXSocket.new(fallback)
    request = 'eval (getenv "MORTARSOCK")'
    restarted.write([request.bytesize].pack('N') + request)
    size = restarted.read(4).unpack1('N')
    raise 'fallback environment missing' unless restarted.read(size) == %(ok "#{fallback}")
    restarted.close
  ensure
    Process.kill('TERM', client) rescue nil if client
    Process.wait(client) rescue nil if client
    Process.kill('TERM', wm) rescue nil if wm
    Process.wait(wm) rescue nil if wm
    Process.kill('TERM', server) rescue nil
    Process.wait(server) rescue nil
  end
end
