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
  begin
    display = Timeout.timeout(10) { reader.gets }
    raise 'Xvfb failed' unless display
    log_reader, log_writer = IO.pipe
    wm = Process.spawn({ 'DISPLAY' => ":#{Integer(display)}", 'XDG_RUNTIME_DIR' => dir, 'XDG_CONFIG_HOME' => dir }, ARGV.fetch(0), err: log_writer)
    log_writer.close
    path = Timeout.timeout(10) do
      loop do
        line = log_reader.gets or raise 'Mortar exited before IPC setup'
        break line.split('IPC socket ', 2).last.strip if line.include?('IPC socket ')
      end
    end
    socket = UNIXSocket.new(path)
    send_frame = ->(text) { socket.write([text.bytesize].pack('N') + text) }
    read_frame = -> { Timeout.timeout(5) { size = socket.read(4).unpack1('N'); socket.read(size) } }
    send_frame.call('subscribe workspace')
    raise 'subscription failed' unless read_frame.call == 'ok subscribed'
    send_frame.call('eval (+ 1 2)')
    raise 'evaluation failed' unless read_frame.call == 'ok 3'
    send_frame.call('eval (error "failure")')
    raise 'evaluation error missing' unless read_frame.call.start_with?('error ')
    send_frame.call('eval (workspace 2)')
    frames = [read_frame.call, read_frame.call]
    raise "workspace event missing: #{frames.inspect}" unless frames.include?('event workspace 1') && frames.any? { |f| f.start_with?('ok ') }
    socket.close
  ensure
    Process.kill('TERM', wm) rescue nil if wm
    Process.wait(wm) rescue nil if wm
    Process.kill('TERM', server) rescue nil
    Process.wait(server) rescue nil
  end
end
