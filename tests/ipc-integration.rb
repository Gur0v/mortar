require 'socket'
require 'tmpdir'
require 'timeout'

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
    send_frame.call('eval (move-to-workspace 3)')
    frames = [read_frame.call, read_frame.call]
    raise "window move missing: #{frames.inspect}" unless frames.include?("event window move #{window}") && frames.any? { |f| f.start_with?('ok ') }
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
    Process.kill('TERM', wm)
    Process.wait(wm)
    wm = nil
    raise 'socket not removed' if File.exist?(path)
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
    UNIXSocket.new(custom).close
  ensure
    Process.kill('TERM', client) rescue nil if client
    Process.wait(client) rescue nil if client
    Process.kill('TERM', wm) rescue nil if wm
    Process.wait(wm) rescue nil if wm
    Process.kill('TERM', server) rescue nil
    Process.wait(server) rescue nil
  end
end
