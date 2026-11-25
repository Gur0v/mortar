require 'rbconfig'
require 'timeout'
require 'fiddle/import'

module X11
  extend Fiddle::Importer
  libdir = IO.popen(['pkg-config', '--variable=libdir', 'x11'], &:read).strip
  dlload File.join(libdir, 'libX11.so.6')
  extern 'void *XOpenDisplay(char*)'
  extern 'unsigned long XDefaultRootWindow(void*)'
  extern 'int XQueryTree(void*, unsigned long, void*, void*, void*, void*)'
  extern 'int XFree(void*)'
  extern 'void *XGetImage(void*, unsigned long, int, int, unsigned int, unsigned int, unsigned long, int)'
  extern 'unsigned long XGetPixel(void*, int, int)'
  extern 'int XDestroyImage(void*)'
  extern 'int XCloseDisplay(void*)'
  extern 'unsigned long XInternAtom(void*, char*, int)'
  extern 'int XChangeProperty(void*, unsigned long, unsigned long, unsigned long, int, int, void*, int)'
  extern 'int XSync(void*, int)'
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
[[{"full_text":"a very long status value that must be shortened","short_text":"S","min_width":40,"align":"right","urgent":true,"separator":false,"separator_block_width":15},{"full_text":"<span foreground=\"#ff0000\">é漢</span>","markup":"pango","name":"test","color":"#ffffff","background":"#00ff00","border":"#0000ff","border_top":2}]]
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
  abort 'bar window count incorrect' unless count[0, Fiddle::SIZEOF_INT].unpack1('I') == (multi ? 2 : 1)
  list = Fiddle::Pointer.new(children[0, Fiddle::SIZEOF_VOIDP].unpack1('J'))
  windows = list[0, (multi ? 2 : 1) * Fiddle::SIZEOF_LONG].unpack('L!*')
  window = windows.last
  X11.XFree(list)
  strut = X11.XInternAtom(connection, '_NET_WM_STRUT_PARTIAL', 0)
  actual_type = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
  actual_format = Fiddle::Pointer.malloc(Fiddle::SIZEOF_INT)
  items = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
  remaining = Fiddle::Pointer.malloc(Fiddle::SIZEOF_LONG)
  data = Fiddle::Pointer.malloc(Fiddle::SIZEOF_VOIDP)
  reservations = windows.map do |bar_window|
  result = X11.XGetWindowProperty(connection, bar_window, strut, 0, 12, 0, 6, actual_type, actual_format, items, remaining, data)
  abort 'bar strut missing' unless result == 0 && items[0, Fiddle::SIZEOF_LONG].unpack1('L!') == 12
  values = Fiddle::Pointer.new(data[0, Fiddle::SIZEOF_VOIDP].unpack1('J'))
  reservation = values[0, 12 * Fiddle::SIZEOF_LONG].unpack('L!*')
  X11.XFree(values)
  reservation
  end
  spans = reservations.map { |reservation| [reservation[2], reservation[8], reservation[9]] }.sort
  abort 'bar strut incorrect' unless spans == (multi ? [[24, 0, 399], [24, 400, 799]] : [[24, 0, 799]])
  names = X11.XInternAtom(connection, '_NET_DESKTOP_NAMES', 0)
  utf8 = X11.XInternAtom(connection, 'UTF8_STRING', 0)
  long_name = ('W' * 400 + "\0").b
  X11.XChangeProperty(connection, root, names, utf8, 8, 0, long_name, long_name.bytesize)
  X11.XSync(connection, 0)
  sleep 0.2
  image = X11.XGetImage(connection, window, 0, 0, (multi ? 400 : 800), 24, -1, 2)
  abort 'cannot capture bar' if image.null?
  colors = (multi ? (200...400) : (600...800)).flat_map { |x| (0...24).map { |y| X11.XGetPixel(image, x, y) & 0xffffff } }
  abort 'urgent background missing' unless colors.include?(0x900000)
  abort 'urgent border missing' unless colors.include?(0x2f343a)
  abort 'disabled separator drawn' if colors.include?(0x666666)
  abort 'status background missing' unless colors.include?(0x00ff00)
  abort 'status border missing' unless colors.include?(0x0000ff)
  abort 'status foreground missing' unless colors.any? { |pixel| ((pixel >> 16) & 0xff) > ((pixel >> 8) & 0xff) + 32 }
  X11.XDestroyImage(image)
  X11.XCloseDisplay(connection)
  Process.kill('TERM', pid)
  Process.wait(pid)

  child_reader, child_writer = IO.pipe
  code = 'child = fork { loop { sleep 1 } }; STDERR.puts(child); STDERR.flush; STDOUT.write("bad\n"); STDOUT.flush; loop { sleep 1 }'
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
