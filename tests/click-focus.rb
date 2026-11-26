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
  extern 'int XSelectInput(void *, unsigned long, long)'
  extern 'int XMapWindow(void *, unsigned long)'
  extern 'int XSync(void *, int)'
  extern 'int XPending(void *)'
  extern 'int XNextEvent(void *, void *)'
  extern 'int XGetInputFocus(void *, void *, void *)'
  extern 'int XGetWindowProperty(void *, unsigned long, unsigned long, long, long, int, unsigned long, void *, void *, void *, void *, void *)'
  extern 'int XFree(void *)'
  extern 'int XSetInputFocus(void *, unsigned long, int, unsigned long)'
  extern 'int XGetGeometry(void *, unsigned long, void *, void *, void *, void *, void *, void *, void *)'
  extern 'unsigned long XInternAtom(void *, char *, int)'
  extern 'int XChangeProperty(void *, unsigned long, unsigned long, unsigned long, int, int, void *, int)'
  extern 'unsigned int XKeysymToKeycode(void *, unsigned long)'
  extern 'int XTestFakeKeyEvent(void *, unsigned int, int, unsigned long)'
  extern 'int XTestFakeButtonEvent(void *, unsigned int, int, unsigned long)'
  extern 'int XTestFakeMotionEvent(void *, int, int, int, unsigned long)'
end

def wait_for
  Timeout.timeout(10) do
    loop do
      return if yield
      sleep 0.01
    end
  end
end

def focus(display)
  window = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
  revert = Fiddle::Pointer.malloc(Fiddle::SIZEOF_INT)
  X.XGetInputFocus(display, window, revert)
  window[0, Fiddle::SIZEOF_LONG].unpack1('L!')
end

def active_window(display, root)
  atom = X.XInternAtom(display, '_NET_ACTIVE_WINDOW', 0)
  type = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
  format = Fiddle::Pointer.malloc(Fiddle::SIZEOF_INT)
  count = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
  remaining = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
  data = Fiddle::Pointer.malloc(Fiddle::SIZEOF_VOIDP)
  status = X.XGetWindowProperty(display, root, atom, 0, 1, 0, 33, type, format, count, remaining, data)
  pointer = data[0, Fiddle::SIZEOF_VOIDP].unpack1('J')
  raise 'invalid _NET_ACTIVE_WINDOW' unless status == 0 && type[0, Fiddle::SIZEOF_LONG].unpack1('L!') == 33 &&
    format[0, Fiddle::SIZEOF_INT].unpack1('i!') == 32 && count[0, Fiddle::SIZEOF_LONG].unpack1('L!') == 1 && pointer != 0
  value = Fiddle::Pointer.new(pointer)[0, Fiddle::SIZEOF_LONG].unpack1('L!')
  X.XFree(pointer)
  value
end

def geometry(display, window)
  root = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
  fields = Array.new(6) { Fiddle::Pointer.malloc(Fiddle::SIZEOF_INT) }
  raise 'XGetGeometry failed' if X.XGetGeometry(display, window, root, *fields) == 0
  fields.map { |field| field[0, Fiddle::SIZEOF_INT].unpack1('i!') }
end

def events(display)
  X.XSync(display, 0)
  result = []
  event = Fiddle::Pointer.malloc(192)
  while X.XPending(display) > 0
    X.XNextEvent(display, event)
    type = event[0, 4].unpack1('i!')
    result << [type, event[32, Fiddle::SIZEOF_LONG].unpack1('L!')] if type == 4 || type == 5
  end
  result
end

Dir.mktmpdir('mortar-click-focus-') do |dir|
  xvfb = wm = display = nil
  begin
    reader, writer = IO.pipe
    xvfb = Process.spawn('Xvfb', '-displayfd', '3', '-screen', '0', '800x600x24', 3 => writer, out: File::NULL, err: File::NULL)
    writer.close
    number = Timeout.timeout(10) { reader.gets }
    reader.close
    raise 'Xvfb failed' unless number
    ENV['DISPLAY'] = ":#{Integer(number)}"
    ENV['XDG_CONFIG_HOME'] = ENV['HOME'] = dir
    ENV['MORTARSOCK'] = File.join(dir, 'socket')
    Dir.mkdir(File.join(dir, 'mortar'))
    marker = File.join(dir, 'ready')
    File.write(File.join(dir, 'mortar', 'config.scm'), "(bar-mode 'hide)\n(floating-modifier 8)\n" \
      "(call-with-output-file #{marker.dump} (lambda (port) (display \"ready\" port)))\n")
    display = X.XOpenDisplay(nil)
    raise 'XOpenDisplay failed' if display.null?
    root = X.XDefaultRootWindow(display)
    windows = 3.times.map do |i|
      window = X.XCreateSimpleWindow(display, root, 0, 0, 200, 200, 0, 0, 0)
      X.XSelectInput(display, window, (1 << 2) | (1 << 3))
      if i == 2
        hints = X.XInternAtom(display, 'WM_HINTS', 0)
        X.XChangeProperty(display, window, hints, hints, 32, 0, [1, 0].pack('L!*'), 2)
      end
      X.XMapWindow(display, window)
      X.XSync(display, 0)
      window
    end
    wm = Process.spawn(File.expand_path(ARGV.fetch(0)), in: File::NULL, out: File::NULL, err: File.join(dir, 'wm.log'))
    wait_for { File.socket?(ENV['MORTARSOCK']) }
    wait_for { windows.all? { |window| geometry(display, window)[2] > 200 } }
    first, second, nonfocusable = windows
    2.times do |round|
      if round == 1
        File.unlink(marker)
        socket = UNIXSocket.new(ENV['MORTARSOCK'])
        request = 'eval (restart)'
        socket.write([request.bytesize].pack('N') + request)
        socket.close
        wait_for { File.exist?(marker) }
        wait_for { File.socket?(ENV['MORTARSOCK']) }
        wait_for { windows.all? { |window| geometry(display, window)[2] > 200 } }
      end
      [first, nonfocusable].product([1, 2, 3]).each do |target, button|
        g = geometry(display, target)
        X.XTestFakeMotionEvent(display, -1, g[0] + g[2] / 2, g[1] + g[3] / 2, 0)
        X.XSync(display, 0)
        sleep 0.1
        X.XSetInputFocus(display, second, 1, 0)
        X.XSync(display, 0)
        wait_for { focus(display) == second }
        events(display)
        X.XTestFakeButtonEvent(display, button, 1, 0)
        X.XTestFakeButtonEvent(display, button, 0, 0)
        X.XSync(display, 0)
        wait_for { target == nonfocusable || (focus(display) == target && active_window(display, root) == target) }
        sleep 0.2
        received = events(display)
        raise "wrong click delivery for #{target}: #{received.inspect}" unless received == [[4, target], [5, target]]
        raise 'nonfocusable window took focus' if target == nonfocusable && focus(display) != second
      end
    end
    g = geometry(display, first)
    X.XTestFakeMotionEvent(display, -1, g[0] + g[2] - 20, g[1] + g[3] / 2, 0)
    X.XSync(display, 0)
    sleep 0.1
    events(display)
    alt = X.XKeysymToKeycode(display, 0xffe9)
    raise 'Alt key missing' if alt == 0
    X.XTestFakeKeyEvent(display, alt, 1, 0)
    X.XTestFakeButtonEvent(display, 3, 1, 0)
    X.XSync(display, 0)
    sleep 0.1
    X.XTestFakeMotionEvent(display, -1, g[0] + g[2] + 30, g[1] + g[3] / 2, 0)
    X.XSync(display, 0)
    wait_for { geometry(display, first)[2] != g[2] }
    X.XTestFakeButtonEvent(display, 3, 0, 0)
    X.XTestFakeKeyEvent(display, alt, 0, 0)
    X.XSync(display, 0)
    sleep 0.1
    raise 'modifier drag reached application' unless events(display).empty?
    floating = X.XCreateSimpleWindow(display, root, 100, 100, 200, 160, 0, 0, 0)
    X.XSelectInput(display, floating, (1 << 2) | (1 << 3))
    type = X.XInternAtom(display, '_NET_WM_WINDOW_TYPE', 0)
    dialog = X.XInternAtom(display, '_NET_WM_WINDOW_TYPE_DIALOG', 0)
    X.XChangeProperty(display, floating, type, 4, 32, 0, [dialog].pack('L!'), 1)
    X.XMapWindow(display, floating)
    X.XSync(display, 0)
    wait_for { focus(display) == floating }
    g = geometry(display, floating)
    X.XTestFakeMotionEvent(display, -1, g[0] + 40, g[1] + 40, 0)
    X.XSync(display, 0)
    sleep 0.1
    events(display)
    X.XTestFakeKeyEvent(display, alt, 1, 0)
    X.XTestFakeButtonEvent(display, 1, 1, 0)
    X.XSync(display, 0)
    sleep 0.1
    X.XTestFakeMotionEvent(display, -1, g[0] + 90, g[1] + 70, 0)
    X.XSync(display, 0)
    wait_for { geometry(display, floating)[0, 2] != g[0, 2] }
    X.XTestFakeButtonEvent(display, 1, 0, 0)
    X.XTestFakeKeyEvent(display, alt, 0, 0)
    X.XSync(display, 0)
    sleep 0.1
    raise 'floating modifier drag reached application' unless events(display).empty?
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
