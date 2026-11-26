require 'fiddle/import'
require 'tmpdir'
require 'timeout'

module X
  extend Fiddle::Importer
  dlload File.join(IO.popen(['pkg-config', '--variable=libdir', 'x11'], &:read).strip, 'libX11.so.6'),
         File.join(IO.popen(['pkg-config', '--variable=libdir', 'xtst'], &:read).strip, 'libXtst.so.6')
  extern 'void *XOpenDisplay(char *)'
  extern 'int XCloseDisplay(void *)'
  extern 'unsigned long XDefaultRootWindow(void *)'
  extern 'unsigned long XCreateSimpleWindow(void *, unsigned long, int, int, unsigned int, unsigned int, unsigned int, unsigned long, unsigned long)'
  extern 'int XMapWindow(void *, unsigned long)'
  extern 'int XDestroyWindow(void *, unsigned long)'
  extern 'int XGetWindowAttributes(void *, unsigned long, void *)'
  WindowAttributes = struct ['int x', 'int y', 'int width', 'int height', 'int border_width', 'int depth', 'void *visual', 'unsigned long root', 'int class', 'int bit_gravity', 'int win_gravity', 'int backing_store', 'unsigned long backing_planes', 'unsigned long backing_pixel', 'int save_under', 'unsigned long colormap', 'int map_installed', 'int map_state', 'long all_event_masks', 'long your_event_mask', 'long do_not_propagate_mask', 'int override_redirect', 'void *screen']
  extern 'int XQueryTree(void *, unsigned long, void *, void *, void *, void *)'
  extern 'int XGetWindowProperty(void *, unsigned long, unsigned long, long, long, int, unsigned long, void *, void *, void *, void *, void *)'
  extern 'int XFree(void *)'
  extern 'unsigned long XInternAtom(void *, char *, int)'
  extern 'int XChangeProperty(void *, unsigned long, unsigned long, unsigned long, int, int, void *, int)'
  extern 'int XGetGeometry(void *, unsigned long, void *, void *, void *, void *, void *, void *, void *)'
  extern 'int XGetInputFocus(void *, void *, void *)'
  extern 'int XSync(void *, int)'
  extern 'unsigned int XKeysymToKeycode(void *, unsigned long)'
  extern 'int XTestFakeKeyEvent(void *, unsigned int, int, unsigned long)'
  extern 'int XTestFakeButtonEvent(void *, unsigned int, int, unsigned long)'
  extern 'int XTestFakeMotionEvent(void *, int, int, int, unsigned long)'
  extern 'int XGrabPointer(void *, unsigned long, int, unsigned int, int, int, unsigned long, unsigned long, unsigned long)'
  extern 'int XUngrabPointer(void *, unsigned long)'
end

def wait_for
  Timeout.timeout(10) do
    loop do
      result = yield
      return result if result
      sleep 0.001
    end
  end
end

def geometry(display, window)
  root = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
  fields = Array.new(6) { Fiddle::Pointer.malloc(Fiddle::SIZEOF_INT) }
  raise 'XGetGeometry failed' if X.XGetGeometry(display, window, root, *fields) == 0
  fields.map { |field| field[0, Fiddle::SIZEOF_INT].unpack1('i!') }
end

def mapped_dock?(display, root)
  root_return = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
  parent = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
  children = Fiddle::Pointer.malloc(Fiddle::SIZEOF_VOIDP)
  count = Fiddle::Pointer.malloc(Fiddle::SIZEOF_INT)
  raise 'XQueryTree failed' if X.XQueryTree(display, root, root_return, parent, children, count) == 0
  pointer = children[0, Fiddle::SIZEOF_VOIDP].unpack1('J')
  begin
    windows = pointer == 0 ? [] : Fiddle::Pointer.new(pointer)[0, count[0, Fiddle::SIZEOF_INT].unpack1('I!') * Fiddle::SIZEOF_LONG].unpack('L!*')
    type = X.XInternAtom(display, '_NET_WM_WINDOW_TYPE', 0)
    dock = X.XInternAtom(display, '_NET_WM_WINDOW_TYPE_DOCK', 0)
    atom = X.XInternAtom(display, 'ATOM', 0)
    windows.any? do |window|
      actual_type = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
      format = Fiddle::Pointer.malloc(Fiddle::SIZEOF_INT)
      items = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
      remaining = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
      property = Fiddle::Pointer.malloc(Fiddle::SIZEOF_VOIDP)
      status = X.XGetWindowProperty(display, window, type, 0, 32, 0, atom, actual_type, format, items, remaining, property)
      address = property[0, Fiddle::SIZEOF_VOIDP].unpack1('J')
      begin
        attributes = X::WindowAttributes.malloc
        status == 0 && address != 0 && actual_type[0, Fiddle::SIZEOF_LONG].unpack1('L!') == atom && format[0, Fiddle::SIZEOF_INT].unpack1('i!') == 32 && Fiddle::Pointer.new(address)[0, items[0, Fiddle::SIZEOF_LONG].unpack1('L!') * Fiddle::SIZEOF_LONG].unpack('L!*').include?(dock) && X.XGetWindowAttributes(display, window, attributes) != 0 && attributes.map_state == 2
      ensure
        X.XFree(address) if address != 0
      end
    end
  ensure
    X.XFree(pointer) if pointer != 0
  end
end

wm_path = File.expand_path(ARGV.fetch(0))
ENV['PATH'] = [File.dirname(wm_path), ENV.fetch('PATH')].join(File::PATH_SEPARATOR)
%w[hide dock].each do |mode|
Dir.mktmpdir('mortar-drag-burst-') do |dir|
  xvfb = wm = display = probe = nil
  begin
    reader, writer = IO.pipe
    xvfb = Process.spawn('Xvfb', '-displayfd', '3', '-screen', '0', '1024x768x24', 3 => writer, out: File::NULL, err: File::NULL)
    writer.close
    number = Timeout.timeout(10) { reader.gets }
    reader.close
    raise 'Xvfb failed' unless number
    ENV['DISPLAY'] = ":#{Integer(number)}"
    ENV['XDG_CONFIG_HOME'] = ENV['HOME'] = dir
    ENV['MORTARSOCK'] = File.join(dir, 'socket')
    Dir.mkdir(File.join(dir, 'mortar'))
    File.write(File.join(dir, 'mortar', 'config.scm'), "(bar-mode '#{mode})\n(floating-modifier 8)\n")
    wm = Process.spawn(wm_path, in: File::NULL, out: File::NULL, err: File.join(dir, 'wm.log'))
    wait_for { File.socket?(ENV['MORTARSOCK']) }
    display = X.XOpenDisplay(nil)
    probe = X.XOpenDisplay(nil)
    raise 'XOpenDisplay failed' if display.null? || probe.null?
    root = X.XDefaultRootWindow(display)
    if mode == 'dock'
      workarea = X.XInternAtom(display, '_NET_WORKAREA', 0)
      wait_for do
        type = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
        format = Fiddle::Pointer.malloc(Fiddle::SIZEOF_INT)
        items = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
        remaining = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
        property = Fiddle::Pointer.malloc(Fiddle::SIZEOF_VOIDP)
        status = X.XGetWindowProperty(display, root, workarea, 0, 4, 0, 6, type, format, items, remaining, property)
        address = property[0, Fiddle::SIZEOF_VOIDP].unpack1('J')
        begin
          status == 0 && address != 0 && type[0, Fiddle::SIZEOF_LONG].unpack1('L!') == 6 && format[0, Fiddle::SIZEOF_INT].unpack1('i!') == 32 && items[0, Fiddle::SIZEOF_LONG].unpack1('L!') == 4 && Fiddle::Pointer.new(address)[0, 4 * Fiddle::SIZEOF_LONG].unpack('L!*')[3] < 768 && mapped_dock?(display, root)
        ensure
          X.XFree(address) if address != 0
        end
      end
    end
    raise 'Unexpected mapped dock' if mode == 'hide' && mapped_dock?(display, root)
    alt = X.XKeysymToKeycode(display, 0xffe9)
    raise 'Alt key missing' if alt == 0
    %w[floating-move floating-resize tiled-resize].each do |scenario|
      windows = Array.new(scenario == 'tiled-resize' ? 2 : 1) do
        window = X.XCreateSimpleWindow(display, root, 100, 100, 300, 250, 0, 0, 0)
        if scenario.start_with?('floating')
          type = X.XInternAtom(display, '_NET_WM_WINDOW_TYPE', 0)
          dialog = X.XInternAtom(display, '_NET_WM_WINDOW_TYPE_DIALOG', 0)
          X.XChangeProperty(display, window, type, 4, 32, 0, [dialog].pack('L!'), 1)
        end
        X.XMapWindow(display, window)
        X.XSync(display, 0)
        window
      end
      target = windows.first
      focused = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
      revert = Fiddle::Pointer.malloc(Fiddle::SIZEOF_INT)
      wait_for do
        X.XGetInputFocus(display, focused, revert)
        g = geometry(display, target)
        focused[0, Fiddle::SIZEOF_LONG].unpack1('L!') == windows.last &&
          (scenario.start_with?('floating') ? (g[0] != 100 || g[1] != 100) : (g[2] > 400 && g[2] < 600))
      end
      initial = windows.map { |window| geometry(display, window) }
      g = initial.first
      x = scenario == 'tiled-resize' ? g[0] + g[2] - 30 : g[0] + g[2] * 3 / 4
      y = g[1] + g[3] * 3 / 4
      button = scenario == 'floating-move' ? 1 : 3
      X.XTestFakeMotionEvent(display, -1, x, y, 0)
      X.XTestFakeKeyEvent(display, alt, 1, 0)
      X.XTestFakeButtonEvent(display, button, 1, 0)
      X.XSync(display, 0)
      wait_for do
        status = X.XGrabPointer(probe, root, 0, 0, 1, 1, 0, 0, 0)
        if status == 0
          X.XUngrabPointer(probe, 0)
          X.XSync(probe, 0)
        end
        status == 1
      end
      field = scenario == 'floating-move' ? 0 : 2
      [80, -80].each do |offset|
        X.XTestFakeMotionEvent(display, -1, x + offset, y, 0)
        X.XSync(display, 0)
        wait_for { geometry(display, target)[field] == g[field] + offset }
      end
      start = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      2000.times do |i|
        X.XTestFakeMotionEvent(display, -1, x + (i.even? ? 80 : -80), y, 0)
      end
      X.XTestFakeMotionEvent(display, -1, x + 37, y, 0)
      released = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      X.XTestFakeButtonEvent(display, button, 0, 0)
      X.XTestFakeKeyEvent(display, alt, 0, 0)
      X.XSync(display, 0)
      submitted = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      wait_for do
        status = X.XGrabPointer(probe, root, 0, 0, 1, 1, 0, 0, 0)
        if status == 0
          X.XUngrabPointer(probe, 0)
          X.XSync(probe, 0)
        end
        status == 0
      end
      expected = initial.map(&:dup)
      expected[0][field] += 37
      if scenario == 'tiled-resize'
        expected[1][0] += 37
        expected[1][2] -= 37
      end
      wait_for { windows.map { |window| geometry(display, window) } == expected }
      completed = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      printf "bar=%s %s burst motions=2001 reversals=1999 submit_ms=%.3f release_to_final_ms=%.3f geometry=%s\n", mode, scenario, (submitted - start) * 1000, (completed - released) * 1000, expected.inspect
      windows.each { |window| X.XDestroyWindow(display, window) }
      X.XSync(display, 0)
    end
  ensure
    X.XCloseDisplay(probe) if probe && !probe.null?
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
end
