require 'socket'
require 'tmpdir'
require 'timeout'
require 'fileutils'
require 'fiddle/import'

module X11
  extend Fiddle::Importer
  dlload File.join(IO.popen(['pkg-config', '--variable=libdir', 'x11'], &:read).strip, 'libX11.so.6')
  extern 'void *XOpenDisplay(char*)'
  extern 'unsigned long XDefaultRootWindow(void*)'
  extern 'unsigned long XCreateSimpleWindow(void*, unsigned long, int, int, unsigned int, unsigned int, unsigned int, unsigned long, unsigned long)'
  extern 'int XMapWindow(void*, unsigned long)'
  extern 'int XDestroyWindow(void*, unsigned long)'
  extern 'int XChangeProperty(void*, unsigned long, unsigned long, unsigned long, int, int, void*, int)'
  extern 'int XGetGeometry(void*, unsigned long, void*, void*, void*, void*, void*, void*, void*)'
  extern 'int XGetInputFocus(void*, void*, void*)'
  extern 'unsigned long XInternAtom(void*, char*, int)'
  extern 'int XGetWindowProperty(void*, unsigned long, unsigned long, long, long, int, unsigned long, void*, void*, void*, void*, void*)'
  extern 'int XFree(void*)'
  extern 'int XWarpPointer(void*, unsigned long, unsigned long, int, int, unsigned int, unsigned int, int, int)'
  extern 'int XSync(void*, int)'
  extern 'int XCloseDisplay(void*)'
end

Dir.mktmpdir('mortar-restart-') do |dir|
  server = wm = connection = nil
  begin
    reader, writer = IO.pipe
    server = Process.spawn('Xvfb', '-displayfd', '3', '-screen', '0', '800x600x24', 3 => writer, out: File::NULL, err: File::NULL)
    writer.close
    display = Timeout.timeout(10) { reader.gets }
    raise 'Xvfb failed' unless display
    reader.close
    Dir.mkdir(File.join(dir, 'mortar'))
    marker = File.join(dir, 'startup')
    File.write(File.join(dir, 'mortar', 'config.scm'), "(let ((p (open-file #{marker.dump} \"a\"))) (display \"start\\n\" p) (close-port p))\n(bar-output \"nonexistent\")\n")
    binary = File.join(dir, 'mortar-bin')
    FileUtils.cp(ARGV.fetch(0), binary)
    socket_path = File.join(dir, 'ipc.sock')
    log = File.join(dir, 'wm.log')
    env = { 'DISPLAY' => ":#{Integer(display)}", 'XDG_CONFIG_HOME' => dir, 'HOME' => dir, 'MORTARSOCK' => socket_path }
    wm = Process.spawn(env, binary, out: File::NULL, err: log)
    wait = lambda do |message, &condition|
      Timeout.timeout(10) { sleep 0.02 until condition.call }
    rescue Timeout::Error
      raise "#{message}\n#{File.read(log)}"
    end
    wait.call('IPC did not start') { File.socket?(socket_path) }
    request = lambda do |expression|
      UNIXSocket.open(socket_path) do |socket|
        text = "eval #{expression}"
        socket.write([text.bytesize].pack('N') + text)
        Timeout.timeout(5) do
          header = socket.read(4)
          header && socket.read(header.unpack1('N'))
        end
      end
    end
    wait.call('WM not ready') { request.call('(+ 1 2)') == 'ok 3' }
    connection = X11.XOpenDisplay(env.fetch('DISPLAY'))
    raise 'XOpenDisplay failed' if connection.null?
    root = X11.XDefaultRootWindow(connection)
    make_window = lambda do
      window = X11.XCreateSimpleWindow(connection, root, 10, 10, 100, 100, 0, 0, 0)
      X11.XMapWindow(connection, window)
      X11.XSync(connection, 0)
      sleep 0.1
      window
    end
    windows = [make_window.call, make_window.call]
    raise 'split failed' unless request.call("(split 'vertical)").start_with?('ok ')
    windows.concat(4.times.map { make_window.call })
    geometry = lambda do |window|
      values = Array.new(7) { Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG) }
      raise 'XGetGeometry failed' if X11.XGetGeometry(connection, window, *values) == 0
      values[1, 4].map { |value| value[0, 4].unpack1('i') }
    end
    property = lambda do |window, name|
      atom = X11.XInternAtom(connection, name, 0)
      type, format, count, remaining, data = Array.new(5) { Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG) }
      raise "#{name} query failed" unless X11.XGetWindowProperty(connection, window, atom, 0, 128, 0, 0, type, format, count, remaining, data) == 0
      pointer = Fiddle::Pointer.new(data[0, Fiddle::SIZEOF_VOIDP].unpack1('J'))
      size = count[0, Fiddle::SIZEOF_LONG].unpack1('L!')
      values = size.zero? ? [] : pointer[0, size * Fiddle::SIZEOF_LONG].unpack('L!*')
      X11.XFree(pointer) unless pointer.null?
      values
    end
    focus = lambda do
      window = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
      revert = Fiddle::Pointer.malloc(Fiddle::SIZEOF_INT)
      X11.XGetInputFocus(connection, window, revert)
      window[0, Fiddle::SIZEOF_LONG].unpack1('L!')
    end
    ["(floating-toggle #{windows[2]})", "(scratchpad-move #{windows[3]})", "(move-to-workspace 2 #{windows[4]})", "(move-to-workspace 2 #{windows[5]})", '(workspace 2)', "(fullscreen-toggle #{windows[4]})", '(workspace 1)'].each do |action|
      result = request.call(action)
      raise "#{action}: #{result}" unless result&.start_with?('ok ')
    end
    X11.XWarpPointer(connection, 0, windows[0], 0, 0, 0, 0, 5, 5)
    X11.XSync(connection, 0)
    wait.call('Window did not receive focus') { windows.include?(focus.call) }
    before_geometry = windows.map { |window| geometry.call(window) }
    before_focus = focus.call
    fullscreen = X11.XInternAtom(connection, '_NET_WM_STATE_FULLSCREEN', 0)
    raise 'Fullscreen was not set before restart' unless property.call(windows[4], '_NET_WM_STATE').include?(fullscreen)
    raise 'Second workspace was not assigned' unless property.call(windows[4], '_NET_WM_DESKTOP') == [1]
    raise 'Unexpected current workspace' unless property.call(root, '_NET_CURRENT_DESKTOP') == [0]
    raise 'No managed window focused before restart' unless windows.include?(before_focus)
    request.call('(restart)') rescue nil
    wait.call('Restart did not execute startup config') { File.exist?(marker) && File.readlines(marker).size >= 2 }
    wait.call('IPC did not recover after restart') do
      begin
        request.call('(+ 1 2)') == 'ok 3'
      rescue SystemCallError, IOError, Timeout::Error
        false
      end
    end
    raise 'Restart changed process ID' unless Process.waitpid(wm, Process::WNOHANG).nil?
    wait.call('Window geometry or focus changed after restart') do
      windows.map { |window| geometry.call(window) } == before_geometry && focus.call == before_focus
    end
    raise 'Fullscreen state lost' unless property.call(windows[4], '_NET_WM_STATE').include?(fullscreen)
    raise 'Workspace ownership lost' unless property.call(windows[4], '_NET_WM_DESKTOP') == [1]
    raise 'Current workspace lost' unless property.call(root, '_NET_CURRENT_DESKTOP') == [0]
    raise 'scratchpad-show failed' unless request.call('(scratchpad-show)').start_with?('ok ')
    wait.call('Scratchpad window was not shown') { focus.call == windows[3] }
    raise 'workspace switch failed' unless request.call('(workspace 2)').start_with?('ok ')
    wait.call('Workspace switch did not restore fullscreen focus') { focus.call == windows[4] }
    full_geometry = geometry.call(windows[4])
    raise 'fullscreen-toggle failed' unless request.call("(fullscreen-toggle #{windows[4]})").start_with?('ok ')
    wait.call('Fullscreen did not exit') { !property.call(windows[4], '_NET_WM_STATE').include?(fullscreen) && geometry.call(windows[4]) != full_geometry }
    raise 'workspace switch back failed' unless request.call('(workspace 1)').start_with?('ok ')
    raise 'scratchpad-show failed' unless request.call('(scratchpad-show)').start_with?('ok ')
    wait.call('Scratchpad did not regain focus') { focus.call == windows[3] }
    scratch_geometry = geometry.call(windows[3])
    raise 'scratchpad fullscreen failed' unless request.call("(fullscreen-toggle #{windows[3]})").start_with?('ok ')
    wait.call('Scratchpad did not enter fullscreen') { property.call(windows[3], '_NET_WM_STATE').include?(fullscreen) }
    iconic = 2.times.map do
      window = X11.XCreateSimpleWindow(connection, root, 10, 10, 100, 100, 0, 0, 0)
      hints = [2, 0, 3].pack('L!*')
      X11.XChangeProperty(connection, window, 35, 35, 32, 0, hints, 3)
      X11.XMapWindow(connection, window)
      X11.XSync(connection, 0)
      window
    end
    iconic.each { |window| wait.call('Iconic state missing') { property.call(window, 'WM_STATE').first == 3 } }
    request.call('(restart)') rescue nil
    wait.call('Second restart did not execute startup config') { File.readlines(marker).size == 3 }
    wait.call('IPC did not recover after second restart') do
      begin
        request.call('(+ 1 2)') == 'ok 3'
      rescue SystemCallError, IOError, Timeout::Error
        false
      end
    end
    iconic.each { |window| raise 'Iconic state lost' unless property.call(window, 'WM_STATE').first == 3 }
    X11.XDestroyWindow(connection, iconic[0])
    X11.XMapWindow(connection, iconic[1])
    X11.XSync(connection, 0)
    wait.call('Iconic window did not remap after restart') { property.call(iconic[1], 'WM_STATE').first == 1 }
    raise 'Scratchpad fullscreen state lost' unless property.call(windows[3], '_NET_WM_STATE').include?(fullscreen)
    X11.XWarpPointer(connection, 0, windows[3], 0, 0, 0, 0, 400, 300)
    X11.XSync(connection, 0)
    raise 'Scratchpad fullscreen exit failed' unless request.call("(fullscreen-toggle #{windows[3]})").start_with?('ok ')
    wait.call('Scratchpad geometry not restored') { geometry.call(windows[3]) == scratch_geometry }
    wait.call('Scratchpad focus not restored') { focus.call == windows[3] }
    raise 'Scratchpad hide failed' unless request.call('(scratchpad-show)').start_with?('ok ')
    wait.call('Scratchpad did not hide') { focus.call != windows[3] }
    X11.XWarpPointer(connection, 0, root, 0, 0, 0, 0, 799, 599)
    X11.XSync(connection, 0)
    raise 'Scratchpad show failed' unless request.call('(scratchpad-show)').start_with?('ok ')
    wait.call('Scratchpad focus and geometry not restored after show') { focus.call == windows[3] && geometry.call(windows[3]) == scratch_geometry }
    File.unlink(binary)
    request.call('(restart)') rescue nil
    wait.call('Failed exec was not reported') { File.read(log).include?('exec restart') }
    wait.call('WM did not recover from failed exec') do
      begin
        request.call('(+ 1 2)') == 'ok 3'
      rescue SystemCallError, IOError, Timeout::Error
        false
      end
    end
    raise 'Failed exec restarted process' unless File.readlines(marker).size == 3
    request.call('(exit)') rescue nil
    _, status = Timeout.timeout(10) { Process.wait2(wm) }
    wm = nil
    raise 'WM failed to exit' unless status.success?
    if File.read(log).include?('cannot verify inherited IPC socket; leaving path on shutdown')
      raise 'Unverified socket path was removed' unless File.socket?(socket_path)
      File.unlink(socket_path)
    else
      raise 'Socket path remained after exit' if File.exist?(socket_path)
    end

    FileUtils.cp(ARGV.fetch(0), binary)
    wm = Process.spawn(env, binary, out: File::NULL, err: log)
    wait.call('IPC did not start again') { File.socket?(socket_path) }
    wait.call('WM not ready again') { request.call('(+ 1 2)') == 'ok 3' }
    request.call('(restart)') rescue nil
    wait.call('Third restart did not execute startup config') { File.readlines(marker).size == 5 }
    wait.call('IPC did not recover after third restart') do
      begin
        request.call('(+ 1 2)') == 'ok 3'
      rescue SystemCallError, IOError, Timeout::Error
        false
      end
    end
    socket = UNIXSocket.new(socket_path)
    File.unlink(socket_path)
    File.write(socket_path, 'replacement')
    text = 'eval (exit)'
    socket.write([text.bytesize].pack('N') + text)
    _, status = Timeout.timeout(10) { Process.wait2(wm) }
    socket.close
    wm = nil
    raise 'WM failed to exit after replacement' unless status.success?
    raise 'Replacement path was not preserved' unless File.file?(socket_path) && File.read(socket_path) == 'replacement'
  ensure
    X11.XCloseDisplay(connection) if connection && !connection.null?
    [wm, server].compact.each do |pid|
      Process.kill('TERM', pid) rescue nil
      Process.wait(pid) rescue nil
    end
  end
end
