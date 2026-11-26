require 'rbconfig'
require 'timeout'
require 'fiddle/import'

module X11
  extend Fiddle::Importer
  libdir = IO.popen(['pkg-config', '--variable=libdir', 'x11'], &:read).strip
  xtest = IO.popen(['pkg-config', '--variable=libdir', 'xtst'], &:read).strip
  dlload File.join(libdir, 'libX11.so.6'), File.join(xtest, 'libXtst.so.6')
  extern 'void *XOpenDisplay(char*)'
  extern 'unsigned long XDefaultRootWindow(void*)'
  extern 'int XQueryTree(void*, unsigned long, void*, void*, void*, void*)'
  extern 'int XGetGeometry(void*, unsigned long, void*, void*, void*, void*, void*, void*, void*)'
  extern 'int XFree(void*)'
  extern 'void *XGetImage(void*, unsigned long, int, int, unsigned int, unsigned int, unsigned long, int)'
  extern 'unsigned long XGetPixel(void*, int, int)'
  extern 'int XDestroyImage(void*)'
  extern 'int XCloseDisplay(void*)'
  extern 'unsigned long XInternAtom(void*, char*, int)'
  extern 'int XChangeProperty(void*, unsigned long, unsigned long, unsigned long, int, int, void*, int)'
  extern 'int XSync(void*, int)'
  extern 'unsigned char XKeysymToKeycode(void*, unsigned long)'
  extern 'int XTestFakeKeyEvent(void*, unsigned int, int, unsigned long)'
  extern 'int XGetWindowProperty(void*, unsigned long, unsigned long, long, long, int, unsigned long, void*, void*, void*, void*, void*)'
end

bar = ARGV.fetch(0)
reader, writer = IO.pipe
server = Process.spawn('Xvfb', '-displayfd', '3', '-screen', '0', '800x600x24', 3 => writer, out: File::NULL, err: File::NULL)
writer.close
begin
  display = Timeout.timeout(10) { reader.gets }
  abort 'Xvfb failed to start' unless display
  reader_status, writer_status = IO.pipe
  code = 'STDERR.write(ARGV.fetch(0)); STDOUT.write(%q({"version":1,"click_events":true}
[[{"full_text":"","background":"#ff00ff","border":"#ff00ff","min_width":200},{"full_text":"no separator","separator":false},{"full_text":"a very long status value that must be shortened","short_text":"S","min_width":40,"align":"right","urgent":true,"separator":true,"separator_block_width":0},{"full_text":"<span foreground=\"#ff0000\">é漢</span>","markup":"pango","name":"test","color":"#ffffff","background":"#00ff00","border":"#0000ff","border_top":2}]]
)); STDOUT.flush; STDIN.read'
  pid = Process.spawn({ 'DISPLAY' => ":#{Integer(display)}" }, bar, RbConfig.ruby, '-e', code, 'a b', err: writer_status)
  writer_status.close
  abort 'status arguments changed' unless Timeout.timeout(10) { reader_status.read(3) } == 'a b'
  reader_status.close
  sleep 0.2
  abort 'bar exited unexpectedly' unless Process.waitpid(pid, Process::WNOHANG).nil?
  connection = X11.XOpenDisplay(":#{Integer(display)}")
  abort 'cannot open display' if connection.null?
  root = X11.XDefaultRootWindow(connection)
  root_return = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
  parent = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
  children = Fiddle::Pointer.malloc(Fiddle::SIZEOF_VOIDP)
  count = Fiddle::Pointer.malloc(Fiddle::SIZEOF_INT)
  abort 'cannot query windows' if X11.XQueryTree(connection, root, root_return, parent, children, count) == 0
  multi = ARGV[1] == 'outputs'
  abort 'bar window count incorrect' unless count[0, Fiddle::SIZEOF_INT].unpack1('I') == (multi ? 3 : 1)
  list = Fiddle::Pointer.new(children[0, Fiddle::SIZEOF_VOIDP].unpack1('J'))
  windows = list[0, (multi ? 3 : 1) * Fiddle::SIZEOF_LONG].unpack('L!*')
  window = multi ? windows[1] : windows.last
  X11.XFree(list)
  strut = X11.XInternAtom(connection, '_NET_WM_STRUT_PARTIAL', 0)
  actual_type = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
  actual_format = Fiddle::Pointer.malloc(Fiddle::SIZEOF_INT)
  items = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
  remaining = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
  data = Fiddle::Pointer.malloc(Fiddle::SIZEOF_VOIDP)
  reservations = windows.first(multi ? 2 : 1).map do |bar_window|
  result = X11.XGetWindowProperty(connection, bar_window, strut, 0, 12, 0, 6, actual_type, actual_format, items, remaining, data)
  abort 'bar strut missing' unless result == 0 && items[0, Fiddle::SIZEOF_LONG].unpack1('L!') == 12
  values = Fiddle::Pointer.new(data[0, Fiddle::SIZEOF_VOIDP].unpack1('J'))
  reservation = values[0, 12 * Fiddle::SIZEOF_LONG].unpack('L!*')
  X11.XFree(values)
  reservation
  end
  geometry = Array.new(6) { Fiddle::Pointer.malloc(Fiddle::SIZEOF_INT) }
  abort 'cannot get bar geometry' if X11.XGetGeometry(connection, window, root_return, *geometry) == 0
  bar_height = geometry[3][0, Fiddle::SIZEOF_INT].unpack1('I')
  abort 'default bar height incorrect' unless bar_height > 6
  spans = reservations.map { |reservation| [reservation[3], reservation[10], reservation[11]] }.sort
  abort 'bar strut incorrect' unless spans == (multi ? [[bar_height, 0, 399], [bar_height, 400, 799]] : [[bar_height, 0, 799]])
  if multi
    geometry = Array.new(6) { Fiddle::Pointer.malloc(Fiddle::SIZEOF_INT) }
    abort 'cannot get lower bar geometry' if X11.XGetGeometry(connection, windows[2], root_return, *geometry) == 0
    abort 'lower bar geometry incorrect' unless geometry.first(4).map { |v| v[0, Fiddle::SIZEOF_INT].unpack1('i') } == [0, 500 - bar_height, 800, bar_height]
    local = X11.XInternAtom(connection, '_MORTAR_BAR_OUTPUT', 0)
    result = X11.XGetWindowProperty(connection, windows[2], local, 0, 4, 0, 6, actual_type, actual_format, items, remaining, data)
    abort 'local bar property missing' unless result == 0 && items[0, Fiddle::SIZEOF_LONG].unpack1('L!') == 4
    values = Fiddle::Pointer.new(data[0, Fiddle::SIZEOF_VOIDP].unpack1('J'))
    abort 'local bar output incorrect' unless values[0, 4 * Fiddle::SIZEOF_LONG].unpack('L!*') == [0, 300, 800, 200]
    X11.XFree(values)
    X11.XGetWindowProperty(connection, windows[2], strut, 0, 12, 0, 6, actual_type, actual_format, items, remaining, data)
    abort 'local bar has root strut' unless items[0, Fiddle::SIZEOF_LONG].unpack1('L!') == 0
  end
  names = X11.XInternAtom(connection, '_NET_DESKTOP_NAMES', 0)
  utf8 = X11.XInternAtom(connection, 'UTF8_STRING', 0)
  current = X11.XInternAtom(connection, '_NET_CURRENT_DESKTOP', 0)
  clients = X11.XInternAtom(connection, '_NET_CLIENT_LIST', 0)
  desktop = X11.XInternAtom(connection, '_NET_WM_DESKTOP', 0)
  sleep 0.2
  image = X11.XGetImage(connection, window, 0, 0, 100, bar_height, -1, 2)
  abort 'cannot capture empty workspace bar' if image.null?
  colors = (0...100).map { |x| X11.XGetPixel(image, x, 1) & 0xffffff }
  X11.XDestroyImage(image)
  abort 'empty workspace list drew a button' if colors.include?(0x285577) || colors.include?(0x4c7899)
  labels = "1\0 2\0".b
  X11.XChangeProperty(connection, root, names, utf8, 8, 0, labels, labels.bytesize)
  X11.XChangeProperty(connection, root, current, 6, 32, 0, [0].pack('L!'), 1)
  X11.XChangeProperty(connection, root, clients, 33, 32, 0, [window].pack('L!'), 1)
  X11.XChangeProperty(connection, window, desktop, 6, 32, 0, [1].pack('L!'), 1)
  X11.XSync(connection, 0)
  deadline = Process.clock_gettime(Process::CLOCK_MONOTONIC) + 10
  loop do
    image = X11.XGetImage(connection, window, 0, 0, 100, bar_height, -1, 2)
    abort 'cannot capture bar' if image.null?
    colors = (0...100).map { |x| X11.XGetPixel(image, x, 1) & 0xffffff }
    X11.XDestroyImage(image)
    break if colors.include?(0x285577) && colors.include?(0x222222) && colors.include?(0x333333)
    if Process.clock_gettime(Process::CLOCK_MONOTONIC) >= deadline
      abort "workspace backgrounds missing: active=#{colors.include?(0x285577)}, occupied=#{colors.include?(0x333333)}, observed=#{colors.uniq.map { |color| format('#%06x', color) }.join(', ')}"
    end
    sleep 0.02
  end
  image = X11.XGetImage(connection, window, 0, 0, 100, bar_height, -1, 2)
  abort 'focused border incorrect' unless X11.XGetPixel(image, 0, 0) & 0xffffff == 0x4c7899
  abort 'workspace inset incorrect' unless X11.XGetPixel(image, 1, 1) & 0xffffff == 0x285577
  abort 'bar background incorrect' unless X11.XGetPixel(image, 99, 0) & 0xffffff == 0
  X11.XDestroyImage(image)
  X11.XChangeProperty(connection, window, desktop, 6, 32, 0, [0].pack('L!'), 1)
  X11.XSync(connection, 0)
  sleep 0.2
  image = X11.XGetImage(connection, window, 0, 0, 100, bar_height, -1, 2)
  colors = (0...100).map { |x| X11.XGetPixel(image, x, 1) & 0xffffff }
  abort 'inactive workspace background missing' unless colors.include?(0x222222)
  X11.XDestroyImage(image)
  if multi
    metadata = X11.XInternAtom(connection, '_MORTAR_OUTPUT_WORKSPACES', 0)
    values = [1, 2, 0, 0, 400, 600, 0, 1, 0, 0xffffffff,
              400, 0, 400, 600, 1, 0, 1, 0xffffffff]
    X11.XChangeProperty(connection, root, metadata, 6, 32, 0, values.pack('L!*'), values.size)
    X11.XSync(connection, 0)
    sleep 0.2
    colors_by_bar = windows.map do |bar_window|
      image = X11.XGetImage(connection, bar_window, 0, 0, 100, bar_height, -1, 2)
      colors = (0...100).map { |x| X11.XGetPixel(image, x, 1) & 0xffffff }
      X11.XDestroyImage(image)
      colors
    end
    abort 'focused output metadata ignored' unless colors_by_bar[0].include?(0x285577)
    abort 'visible output metadata ignored' unless colors_by_bar[1].include?(0x5f676a) && !colors_by_bar[1].include?(0x285577)
    values[0] = 2
    X11.XChangeProperty(connection, root, metadata, 6, 32, 0, values.pack('L!*'), values.size)
    X11.XSync(connection, 0)
    sleep 0.2
    image = X11.XGetImage(connection, windows[1], 0, 0, 100, bar_height, -1, 2)
    colors = (0...100).map { |x| X11.XGetPixel(image, x, 1) & 0xffffff }
    X11.XDestroyImage(image)
    abort 'invalid metadata did not fall back' unless colors.include?(0x285577)
  end
  long_name = ('W' * 400 + "\0").b
  X11.XChangeProperty(connection, root, names, utf8, 8, 0, long_name, long_name.bytesize)
  X11.XSync(connection, 0)
  sleep 0.2
  image = X11.XGetImage(connection, window, 0, 0, (multi ? 400 : 800), bar_height, -1, 2)
  abort 'cannot capture bar' if image.null?
  colors = (multi ? (200...400) : (600...800)).flat_map { |x| (0...bar_height).map { |y| X11.XGetPixel(image, x, y) & 0xffffff } }
  abort 'empty status block drawn' if colors.include?(0xff00ff)
  abort 'urgent background missing' unless colors.include?(0x900000)
  abort 'urgent border missing' unless colors.include?(0x2f343a)
  abort 'zero-width separator antialiasing incorrect' unless colors.include?(0x333333) && !colors.include?(0x666666)
  abort 'status background missing' unless colors.include?(0x00ff00)
  abort 'status border missing' unless colors.include?(0x0000ff)
  abort 'status foreground missing' unless colors.any? { |pixel| ((pixel >> 16) & 0xff) > ((pixel >> 8) & 0xff) + 32 }
  X11.XDestroyImage(image)
  X11.XCloseDisplay(connection)
  Process.kill('TERM', pid)
  Process.wait(pid)

  unless multi
    pid = Process.spawn({ 'DISPLAY' => ":#{Integer(display)}" }, bar, '--mortar-settings', 'bottom', 'monospace 30',
                        '2236962', '16777215', '2643319', '3355443', '--')
    sleep 0.3
    connection = X11.XOpenDisplay(":#{Integer(display)}")
    root = X11.XDefaultRootWindow(connection)
    X11.XQueryTree(connection, root, root_return, parent, children, count)
    abort 'large font bar missing' unless count[0, Fiddle::SIZEOF_INT].unpack1('I') == 1
    list = Fiddle::Pointer.new(children[0, Fiddle::SIZEOF_VOIDP].unpack1('J'))
    large = list[0, Fiddle::SIZEOF_LONG].unpack1('L!')
    X11.XFree(list)
    geometry = Array.new(6) { Fiddle::Pointer.malloc(Fiddle::SIZEOF_INT) }
    X11.XGetGeometry(connection, large, root_return, *geometry)
    y, height = [geometry[1], geometry[3]].map { |v| v[0, Fiddle::SIZEOF_INT].unpack1('i') }
    abort 'large font height incorrect' unless height > 24 && y == 600 - height
    X11.XGetWindowProperty(connection, large, strut, 0, 12, 0, 6, actual_type, actual_format, items, remaining, data)
    values = Fiddle::Pointer.new(data[0, Fiddle::SIZEOF_VOIDP].unpack1('J'))
    abort 'large font strut incorrect' unless values[0, 12 * Fiddle::SIZEOF_LONG].unpack('L!*')[3] == height
    X11.XFree(values)
    X11.XCloseDisplay(connection)
    Process.kill('TERM', pid)
    Process.wait(pid)

    pid = Process.spawn({ 'DISPLAY' => ":#{Integer(display)}" }, bar, '--mortar-settings', 'bottom', 'monospace 30',
                        '2236962', '16777215', '2643319', '3355443', '--height', '40', '--status-padding', '4',
                        '--status-edge-padding', '20', '--', RbConfig.ruby, '-e', code, 'a b', err: File::NULL)
    sleep 0.3
    connection = X11.XOpenDisplay(":#{Integer(display)}")
    root = X11.XDefaultRootWindow(connection)
    X11.XQueryTree(connection, root, root_return, parent, children, count)
    abort 'configured bar missing' unless count[0, Fiddle::SIZEOF_INT].unpack1('I') == 1
    list = Fiddle::Pointer.new(children[0, Fiddle::SIZEOF_VOIDP].unpack1('J'))
    configured = list[0, Fiddle::SIZEOF_LONG].unpack1('L!')
    X11.XFree(list)
    geometry = Array.new(6) { Fiddle::Pointer.malloc(Fiddle::SIZEOF_INT) }
    X11.XGetGeometry(connection, configured, root_return, *geometry)
    abort 'configured bar geometry incorrect' unless geometry.first(4).map { |v| v[0, Fiddle::SIZEOF_INT].unpack1('i') } == [0, 560, 800, 40]
    X11.XGetWindowProperty(connection, configured, strut, 0, 12, 0, 6, actual_type, actual_format, items, remaining, data)
    values = Fiddle::Pointer.new(data[0, Fiddle::SIZEOF_VOIDP].unpack1('J'))
    abort 'configured strut incorrect' unless values[0, 12 * Fiddle::SIZEOF_LONG].unpack('L!*')[3] == 40
    X11.XFree(values)
    Timeout.timeout(10) do
      loop do
        image = X11.XGetImage(connection, configured, 0, 0, 800, 40, -1, 2)
        correct = (X11.XGetPixel(image, 779, 0) & 0xffffff) == 0x222222 &&
          (X11.XGetPixel(image, 779, 4) & 0xffffff) == 0x0000ff && (X11.XGetPixel(image, 780, 4) & 0xffffff) == 0x222222
        X11.XDestroyImage(image)
        break if correct
        sleep 0.02
      end
    end
    X11.XCloseDisplay(connection)
    Process.kill('TERM', pid)
    Process.wait(pid)
  end

  if multi
    [['right', 1], ['missing', 0], ['*', 3]].each do |name, expected|
      pid = Process.spawn({ 'DISPLAY' => ":#{Integer(display)}" }, bar, '--mortar-settings', 'top', 'monospace 10',
                          '2236962', '16777215', '2643319', '3355443', '--output', name, '--')
      sleep 0.2
      connection = X11.XOpenDisplay(":#{Integer(display)}")
      root = X11.XDefaultRootWindow(connection)
      X11.XQueryTree(connection, root, root_return, parent, children, count)
      abort "selected output #{name} count incorrect" unless count[0, Fiddle::SIZEOF_INT].unpack1('I') == expected
      list = Fiddle::Pointer.new(children[0, Fiddle::SIZEOF_VOIDP].unpack1('J'))
      if expected == 1
        selected = list[0, Fiddle::SIZEOF_LONG].unpack1('L!')
        geometry = Array.new(6) { Fiddle::Pointer.malloc(Fiddle::SIZEOF_INT) }
        X11.XGetGeometry(connection, selected, root_return, *geometry)
        abort 'wrong selected output' unless geometry[0][0, Fiddle::SIZEOF_INT].unpack1('i') == 400
      end
      X11.XFree(list) unless list.null?
      X11.XCloseDisplay(connection)
      Process.kill('TERM', pid)
      Process.wait(pid)
    end
  end

  unless multi
    pid = Process.spawn({ 'DISPLAY' => ":#{Integer(display)}" }, bar, '--mortar-settings', 'top', 'monospace 10',
                        '2236962', '16777215', '2643319', '3355443', '--hidden', '--')
    connection = X11.XOpenDisplay(":#{Integer(display)}")
    root = X11.XDefaultRootWindow(connection)
    key = X11.XKeysymToKeycode(connection, 0xffeb)
    abort 'Super key unavailable' if key == 0
    get_windows = lambda do
      X11.XSync(connection, 0)
      X11.XQueryTree(connection, root, root_return, parent, children, count)
      n = count[0, Fiddle::SIZEOF_INT].unpack1('I')
      list = Fiddle::Pointer.new(children[0, Fiddle::SIZEOF_VOIDP].unpack1('J'))
      result = n == 0 ? [] : list[0, n * Fiddle::SIZEOF_LONG].unpack('L!*')
      X11.XFree(list) unless list.null?
      result
    end
    sleep 0.2
    abort 'hidden bar visible' unless get_windows.call.empty?
    X11.XTestFakeKeyEvent(connection, key, 1, 0)
    X11.XSync(connection, 0)
    Timeout.timeout(10) { sleep 0.02 until get_windows.call.size == 1 }
    window = get_windows.call.first
    [strut, X11.XInternAtom(connection, '_MORTAR_BAR_OUTPUT', 0)].each do |property|
      X11.XGetWindowProperty(connection, window, property, 0, 12, 0, 6, actual_type, actual_format, items, remaining, data)
      abort 'revealed bar reserved space' unless items[0, Fiddle::SIZEOF_LONG].unpack1('L!') == 0
    end
    X11.XTestFakeKeyEvent(connection, key, 0, 0)
    X11.XSync(connection, 0)
    Timeout.timeout(10) { sleep 0.02 until get_windows.call.empty? }
    urgent = X11.XInternAtom(connection, '_MORTAR_URGENT_WORKSPACES', 0)
    X11.XChangeProperty(connection, root, urgent, 6, 32, 0, [1].pack('L!'), 1)
    X11.XSync(connection, 0)
    Timeout.timeout(10) { sleep 0.02 until get_windows.call.size == 1 }
    X11.XTestFakeKeyEvent(connection, key, 1, 0)
    X11.XSync(connection, 0)
    X11.XChangeProperty(connection, root, urgent, 6, 32, 0, '', 0)
    X11.XSync(connection, 0)
    sleep 0.2
    abort 'modifier did not keep bar visible' unless get_windows.call.size == 1
    X11.XTestFakeKeyEvent(connection, key, 0, 0)
    X11.XSync(connection, 0)
    Timeout.timeout(10) { sleep 0.02 until get_windows.call.empty? }
    X11.XChangeProperty(connection, root, urgent, 6, 32, 0, [1].pack('L!'), 1)
    X11.XSync(connection, 0)
    Timeout.timeout(10) { sleep 0.02 until get_windows.call.size == 1 }
    X11.XChangeProperty(connection, root, urgent, 6, 32, 0, '', 0)
    X11.XSync(connection, 0)
    Timeout.timeout(10) { sleep 0.02 until get_windows.call.empty? }
    X11.XCloseDisplay(connection)
    Process.kill('TERM', pid)
    Process.wait(pid)
  end

  [false, true].each do |leader_exits|
    child_reader, child_writer = IO.pipe
    code = "child = fork { loop { sleep 1 } }; STDERR.puts(child); STDERR.flush; #{leader_exits ? 'exit!' : 'loop { sleep 1 }'}"
    pid = Process.spawn({ 'DISPLAY' => ":#{Integer(display)}" }, bar, RbConfig.ruby, '-e', code, err: child_writer)
    child_writer.close
    descendant = Integer(Timeout.timeout(10) { child_reader.gets })
    sleep 1.2 if leader_exits
    Process.kill('KILL', pid)
    Process.wait(pid)
    Timeout.timeout(10) do
      loop do
        state = File.read("/proc/#{descendant}/stat").split[2] rescue 'Z'
        break if state == 'Z'
        sleep 0.05
      end
    end
    child_reader.close
  end

  child_reader, child_writer = IO.pipe
  code = 'child = fork { loop { sleep 1 } }; STDERR.puts(child); STDERR.flush; loop { sleep 1 }'
  pid = Process.spawn({ 'DISPLAY' => ":#{Integer(display)}" }, bar, RbConfig.ruby, '-e', code, err: child_writer)
  child_writer.close
  descendant = Integer(Timeout.timeout(10) { child_reader.gets })
  watchdog = Integer(File.read("/proc/#{descendant}/stat").split[4])
  Process.kill('KILL', watchdog)
  Timeout.timeout(10) { Process.wait(pid) }
  Timeout.timeout(10) do
    loop do
      state = File.read("/proc/#{descendant}/stat").split[2] rescue 'Z'
      break if state == 'Z'
      sleep 0.05
    end
  end
  child_reader.close

  child_reader, child_writer = IO.pipe
  code = 'child = fork { loop { sleep 1 } }; STDERR.puts(child); STDERR.flush; STDOUT.write(%Q({"version":1}\n[invalid]\n)); STDOUT.flush; loop { sleep 1 }'
  pid = Process.spawn({ 'DISPLAY' => ":#{Integer(display)}" }, bar, RbConfig.ruby, '-e', code, err: child_writer)
  child_writer.close
  descendant = Integer(Timeout.timeout(10) { child_reader.gets })
  diagnostics = Thread.new { child_reader.read }
  Timeout.timeout(10) do
    loop do
      state = File.read("/proc/#{descendant}/stat").split[2] rescue 'Z'
      break if state == 'Z'
      sleep 0.05
    end
  end
  Process.kill('TERM', pid)
  _, status = Process.wait2(pid)
  output = diagnostics.value
  abort "bar failed: #{output}" unless status.success?
  child_reader.close
ensure
  reader.close
  Process.kill('TERM', server) rescue nil
  Process.wait(server)
end
