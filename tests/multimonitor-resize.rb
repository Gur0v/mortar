require 'fiddle/import'
require 'tmpdir'
require 'timeout'
require 'socket'

module X
  extend Fiddle::Importer
  dlload File.join(IO.popen(['pkg-config', '--variable=libdir', 'x11'], &:read).strip, 'libX11.so.6'),
         File.join(IO.popen(['pkg-config', '--variable=libdir', 'xtst'], &:read).strip, 'libXtst.so.6')
  extern 'void *XOpenDisplay(char *)'
  extern 'int XCloseDisplay(void *)'
  extern 'unsigned long XDefaultRootWindow(void *)'
  extern 'unsigned long XCreateSimpleWindow(void *, unsigned long, int, int, unsigned int, unsigned int, unsigned int, unsigned long, unsigned long)'
  extern 'int XMapWindow(void *, unsigned long)'
  extern 'int XSync(void *, int)'
  extern 'int XGetGeometry(void *, unsigned long, void *, void *, void *, void *, void *, void *, void *)'
  extern 'unsigned long XInternAtom(void *, char *, int)'
  extern 'int XChangeProperty(void *, unsigned long, unsigned long, unsigned long, int, int, void *, int)'
  extern 'int XGetWindowProperty(void *, unsigned long, unsigned long, long, long, int, unsigned long, void *, void *, void *, void *, void *)'
  extern 'int XFree(void *)'
  extern 'unsigned int XKeysymToKeycode(void *, unsigned long)'
  extern 'int XTestFakeKeyEvent(void *, unsigned int, int, unsigned long)'
  extern 'int XTestFakeButtonEvent(void *, unsigned int, int, unsigned long)'
  extern 'int XTestFakeMotionEvent(void *, int, int, int, unsigned long)'
end

def wait_for
  Timeout.timeout(10) do
    loop do
      result = yield
      return result if result
      sleep 0.01
    end
  end
end

def geometry(display, window)
  root = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
  fields = Array.new(6) { Fiddle::Pointer.malloc(Fiddle::SIZEOF_INT) }
  raise 'XGetGeometry failed' if X.XGetGeometry(display, window, root, *fields) == 0
  fields.map { |field| field[0, Fiddle::SIZEOF_INT].unpack1('i!') }
end

Dir.mktmpdir('mortar-multimonitor-') do |dir|
  xvfb = wm = display = nil
  begin
    reader, writer = IO.pipe
    xvfb = Process.spawn('Xvfb', '-noreset', '-displayfd', '3', '-screen', '0', '1600x600x24', 3 => writer, out: File::NULL, err: File::NULL)
    writer.close
    number = Timeout.timeout(10) { reader.gets }
    reader.close
    raise 'Xvfb failed' unless number
    ENV['DISPLAY'] = ":#{Integer(number)}"
    ENV['XDG_CONFIG_HOME'] = ENV['HOME'] = ENV['XDG_RUNTIME_DIR'] = dir
    ENV['MORTARSOCK'] = File.join(dir, 'socket')
    raise 'left monitor setup failed' unless system('xrandr', '--setmonitor', 'LEFT', '800/200x600/150+0+0', 'screen', out: File::NULL)
    raise 'right monitor setup failed' unless system('xrandr', '--setmonitor', 'RIGHT', '800/200x600/150+800+0', 'none', out: File::NULL)
    monitors = IO.popen(['xrandr', '--listactivemonitors'], &:read)
    raise "expected two RandR monitors: #{monitors}" unless monitors.include?('Monitors: 2') && monitors.include?('LEFT') && monitors.include?('RIGHT')
    Dir.mkdir(File.join(dir, 'mortar'))
    File.write(File.join(dir, 'mortar', 'config.scm'), "(bar-mode 'hide)\n(floating-modifier 8)\n(gaps 10 5)\n")
    display = X.XOpenDisplay(nil)
    raise 'XOpenDisplay failed' if display.null?
    root = X.XDefaultRootWindow(display)
    wm = Process.spawn(File.expand_path(ARGV.fetch(0)), in: File::NULL, out: File::NULL, err: File.join(dir, 'wm.log'))
    wait_for { File.socket?(ENV['MORTARSOCK']) }
    dock = X.XCreateSimpleWindow(display, root, 0, 0, 800, 30, 0, 0, 0)
    type = X.XInternAtom(display, '_NET_WM_WINDOW_TYPE', 0)
    dock_type = X.XInternAtom(display, '_NET_WM_WINDOW_TYPE_DOCK', 0)
    strut = X.XInternAtom(display, '_NET_WM_STRUT_PARTIAL', 0)
    X.XChangeProperty(display, dock, type, 4, 32, 0, [dock_type].pack('L!'), 1)
    X.XChangeProperty(display, dock, strut, 6, 32, 0, [0, 0, 30, 0, 0, 0, 0, 0, 0, 799, 0, 0].pack('L!*'), 12)
    X.XMapWindow(display, dock)
    X.XSync(display, 0)
    windows = 2.times.map do
      window = X.XCreateSimpleWindow(display, root, 0, 0, 100, 100, 0, 0, 0)
      X.XMapWindow(display, window)
      X.XSync(display, 0)
      wait_for { geometry(display, window)[2] > 100 }
      window
    end
    left = windows.map { |w| geometry(display, w) }
    raise "left layout missing dock or gaps: #{left.inspect}" unless left.all? { |g| g[0] >= 5 && g[0] + g[2] < 800 && g[1] >= 35 && g[3] < 565 }
    socket = UNIXSocket.new(ENV['MORTARSOCK'])
    request = 'eval (workspace 2)'
    socket.write([request.bytesize].pack('N') + request)
    length = socket.read(4).unpack1('N')
    response = socket.read(length)
    socket.close
    raise "workspace switch failed: #{response}" if response.include?('error')
    right = X.XCreateSimpleWindow(display, root, 0, 0, 100, 100, 0, 0, 0)
    X.XMapWindow(display, right)
    X.XSync(display, 0)
    right_before = wait_for { g = geometry(display, right); g if g[0] >= 800 && g[2] > 100 }
    workarea_atom = X.XInternAtom(display, '_NET_WORKAREA', 0)
    workarea = lambda do
      type, format, items, remaining, data = [Fiddle::SIZEOF_LONG, Fiddle::SIZEOF_INT, Fiddle::SIZEOF_LONG, Fiddle::SIZEOF_LONG, Fiddle::SIZEOF_VOIDP].map { |size| Fiddle::Pointer.malloc(size) }
      raise 'XGetWindowProperty failed' unless X.XGetWindowProperty(display, root, workarea_atom, 0, 8, 0, 6, type, format, items, remaining, data) == 0
      count = items[0, Fiddle::SIZEOF_LONG].unpack1('L!')
      pointer = data[0, Fiddle::SIZEOF_VOIDP].unpack1('J')
      values = pointer.zero? ? [] : Fiddle::Pointer.new(pointer)[0, count * Fiddle::SIZEOF_LONG].unpack('L!*')
      X.XFree(pointer) unless pointer.zero?
      values
    end
    wait_for { workarea.call[0, 8] == [0, 30, 800, 570, 800, 0, 800, 600] }
    [60, 30].each do |height|
      previous = windows.map { |w| geometry(display, w) }
      X.XChangeProperty(display, dock, strut, 6, 32, 0, [0, 0, height, 0, 0, 0, 0, 0, 0, 799, 0, 0].pack('L!*'), 12)
      X.XSync(display, 0)
      wait_for { workarea.call[0, 8] == [0, height, 800, 600 - height, 800, 0, 800, 600] }
      wait_for do
        current = windows.map { |w| geometry(display, w) }
        current != previous && current.all? { |g| g[1] >= height && g[1] <= height + 20 }
      end
      raise 'strut change moved dock' unless geometry(display, dock)[0, 4] == [0, 0, 800, 30]
      raise 'strut change affected right output' unless geometry(display, right) == right_before
    end
    before = windows.map { |w| geometry(display, w) }
    target = before.min_by(&:first)
    x = target[0] + target[2] - 20
    y = target[1] + target[3] * 3 / 4
    alt = X.XKeysymToKeycode(display, 0xffe9)
    raise 'Alt key missing' if alt == 0
    X.XTestFakeMotionEvent(display, -1, x, y, 0)
    X.XSync(display, 0)
    sleep 0.1
    X.XTestFakeKeyEvent(display, alt, 1, 0)
    X.XTestFakeButtonEvent(display, 3, 1, 0)
    X.XSync(display, 0)
    sleep 0.1
    X.XTestFakeMotionEvent(display, -1, x + 50, y + 35, 0)
    X.XSync(display, 0)
    wait_for { windows.map { |w| geometry(display, w) } != before }
    X.XTestFakeButtonEvent(display, 3, 0, 0)
    X.XTestFakeKeyEvent(display, alt, 0, 0)
    X.XSync(display, 0)
    sleep 0.1
    after = windows.map { |w| geometry(display, w) }
    raise "resize left output or dock area: #{after.inspect}" unless after.all? { |g| g[0] >= 5 && g[0] + g[2] < 800 && g[1] >= 35 && g[3] < 565 }
    raise "second output changed: #{right_before.inspect} -> #{geometry(display, right).inspect}" unless geometry(display, right) == right_before
  ensure
    X.XCloseDisplay(display) if display && !display.null?
    [wm, xvfb].compact.each do |pid|
      begin
        Process.kill('TERM', pid)
        Process.wait(pid)
      rescue Errno::ESRCH, Errno::ECHILD
      end
    end
  end
end
