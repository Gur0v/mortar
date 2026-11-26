require 'fiddle/import'
require 'tmpdir'
require 'timeout'

module X
  extend Fiddle::Importer
  dlload 'libX11.so.6', 'libXtst.so.6'
  extern 'void *XOpenDisplay(char *)'
  extern 'int XCloseDisplay(void *)'
  extern 'unsigned long XDefaultRootWindow(void *)'
  extern 'unsigned long XCreateSimpleWindow(void *, unsigned long, int, int, unsigned int, unsigned int, unsigned int, unsigned long, unsigned long)'
  extern 'int XMapWindow(void *, unsigned long)'
  extern 'int XDestroyWindow(void *, unsigned long)'
  extern 'unsigned long XInternAtom(void *, char *, int)'
  extern 'int XChangeProperty(void *, unsigned long, unsigned long, unsigned long, int, int, void *, int)'
  extern 'int XGetGeometry(void *, unsigned long, void *, void *, void *, void *, void *, void *, void *)'
  extern 'int XGetWindowAttributes(void *, unsigned long, void *)'
  WindowAttributes = struct ['int x', 'int y', 'int width', 'int height', 'int border_width', 'int depth', 'void *visual', 'unsigned long root', 'int class', 'int bit_gravity', 'int win_gravity', 'int backing_store', 'unsigned long backing_planes', 'unsigned long backing_pixel', 'int save_under', 'unsigned long colormap', 'int map_installed', 'int map_state', 'long all_event_masks', 'long your_event_mask', 'long do_not_propagate_mask', 'int override_redirect', 'void *screen']
  extern 'int XQueryTree(void *, unsigned long, void *, void *, void *, void *)'
  extern 'int XGetWindowProperty(void *, unsigned long, unsigned long, long, long, int, unsigned long, void *, void *, void *, void *, void *)'
  extern 'int XFree(void *)'
  extern 'int XSync(void *, int)'
  extern 'unsigned int XKeysymToKeycode(void *, unsigned long)'
  extern 'int XTestFakeKeyEvent(void *, unsigned int, int, unsigned long)'
  extern 'int XTestFakeButtonEvent(void *, unsigned int, int, unsigned long)'
  extern 'int XTestFakeMotionEvent(void *, int, int, int, unsigned long)'
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

def wait_for
  Timeout.timeout(10) do
    loop do
      result = yield
      return result if result
      sleep 0.0001
    end
  end
end

abort "Usage: ruby #{__FILE__} /path/to/mortar" unless ARGV.length == 1
wm_path = File.expand_path(ARGV.fetch(0))
bar_path = File.join(File.dirname(wm_path), 'mortarbar')
abort "Missing executable #{bar_path}" unless File.executable?(bar_path)
%w[hide dock].each do |mode|
Dir.mktmpdir('mortar-drag-') do |dir|
  xvfb = wm = display = nil
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
    ENV['PATH'] = [File.dirname(bar_path), ENV.fetch('PATH')].join(File::PATH_SEPARATOR)
    Dir.mkdir(File.join(dir, 'mortar'))
    File.write(File.join(dir, 'mortar', 'config.scm'), "(bar-mode '#{mode})\n(floating-modifier 8)\n")
    log = File.join(dir, 'wm.log')
    wm = Process.spawn(wm_path, in: File::NULL, out: File::NULL, err: log)
    wait_for { File.socket?(ENV['MORTARSOCK']) }
    display = X.XOpenDisplay(nil)
    raise 'XOpenDisplay failed' if display.null?
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
    puts "bar=#{mode} mapped=#{mapped_dock?(display, root)}"
    alt = X.XKeysymToKeycode(display, 0xffe9)
    raise 'Alt key missing' if alt == 0
    puts '100 round trips per scenario; median and p95 include XTEST, WM processing and geometry polling (ms)'
    %w[floating-move floating-resize tiled-resize].each do |scenario|
      windows = Array.new(scenario == 'tiled-resize' ? 2 : 1) do
        window = X.XCreateSimpleWindow(display, root, 100, 100, 300, 250, 0, 0, 0)
        if scenario.start_with?('floating')
          type = X.XInternAtom(display, '_NET_WM_WINDOW_TYPE', 0)
          dialog = X.XInternAtom(display, '_NET_WM_WINDOW_TYPE_DIALOG', 0)
          atom = X.XInternAtom(display, 'ATOM', 0)
          X.XChangeProperty(display, window, type, atom, 32, 0, [dialog].pack('L!'), 1)
        end
        X.XMapWindow(display, window)
        X.XSync(display, 0)
        window
      end
      target = windows.first
      wait_for do
        g = geometry(display, target)
        attributes = X::WindowAttributes.malloc
        X.XGetWindowAttributes(display, target, attributes)
        attributes.map_state == 2 && (scenario.start_with?('floating') ? (g[0] != 100 || g[1] != 100) : (g[2] > 400 && g[2] < 600))
      end
      g = geometry(display, target)
      x = scenario == 'tiled-resize' ? g[0] + g[2] - 30 : g[0] + g[2] * 3 / 4
      y = g[1] + g[3] * 3 / 4
      X.XTestFakeMotionEvent(display, -1, x, y, 0)
      X.XTestFakeKeyEvent(display, alt, 1, 0)
      button = scenario == 'floating-move' ? 1 : 3
      X.XTestFakeButtonEvent(display, button, 1, 0)
      X.XSync(display, 0)
      sleep 0.02
      completions = []
      samples = 100.times.map do |i|
        before = geometry(display, target)
        start = Process.clock_gettime(Process::CLOCK_MONOTONIC)
        X.XTestFakeMotionEvent(display, -1, x + (i.even? ? 10 : 0), y, 0)
        X.XSync(display, 0)
        begin
          wait_for do
            current = geometry(display, target)
            scenario == 'floating-move' ? current[0] != before[0] : current[2] != before[2]
          end
        rescue Timeout::Error
          raise "#{scenario} sample #{i} timed out: before=#{before.inspect} after=#{geometry(display, target).inspect} pointer=#{x},#{y}"
        end
        completions << Process.clock_gettime(Process::CLOCK_MONOTONIC)
        (completions[-1] - start) * 1000
      end
      first = samples.first
      gaps = completions.each_cons(2).map { |a, b| (b - a) * 1000 }.sort
      X.XTestFakeButtonEvent(display, button, 0, 0)
      X.XTestFakeKeyEvent(display, alt, 0, 0)
      windows.each { |window| X.XDestroyWindow(display, window) }
      X.XSync(display, 0)
      sleep 0.1
      samples.sort!
      printf "%s isolated first=%.3f median=%.3f p95=%.3f p99=%.3f max=%.3f gaps_p95=%.3f gaps_p99=%.3f ms\n", scenario, first, (samples[49] + samples[50]) / 2, samples[94], samples[98], samples[-1], gaps[94], gaps[98]
    end
  ensure
    X.XCloseDisplay(display) if display && !display.null?
    [wm, xvfb].compact.each do |pid|
      begin
        Process.kill('TERM', pid)
        Timeout.timeout(5) { Process.wait(pid) }
      rescue Errno::ESRCH, Errno::ECHILD
      end
    end
  end
end
end
