project('mortar', 'cpp', version: '0.1.0', default_options: ['cpp_std=c++23', 'warning_level=3'])

xcb = dependency('xcb')
xcb_randr = dependency('xcb-randr')
xcb_xkb = dependency('xcb-xkb')
xkbcommon = dependency('xkbcommon-x11')
mortar = executable('mortar', 'mortar/main.cpp', 'mortar/layout.cpp', 'mortar/workspaces.cpp', 'mortar/keyboard.cpp', dependencies: [xcb, xcb_randr, xcb_xkb, xkbcommon], install: true)

if get_option('tests')
  size_hints_test = executable('test-size-hints', 'tests/size-hints.cpp', 'mortar/layout.cpp', 'mortar/workspaces.cpp', 'mortar/keyboard.cpp', dependencies: [xcb, xcb_randr, xcb_xkb, xkbcommon])
  test('size-hints', size_hints_test)
  layout_test = executable('test-layout', 'tests/layout.cpp', 'mortar/layout.cpp')
  test('layout', layout_test)
  workspaces_test = executable('test-workspaces', 'tests/workspaces.cpp', 'mortar/workspaces.cpp', 'mortar/layout.cpp')
  test('workspaces', workspaces_test)
  keyboard_test = executable('test-keyboard', 'tests/keyboard.cpp', 'mortar/keyboard.cpp', dependencies: [xcb, xcb_xkb, xkbcommon])
  test('keyboard', find_program('ruby'), args: ['-e', '''
require 'timeout'
reader, writer = IO.pipe
server = Process.spawn('Xvfb', '-displayfd', '3', '-screen', '0', '800x600x24', 3 => writer, out: File::NULL, err: File::NULL)
writer.close
begin
  display = Timeout.timeout(10) { reader.gets }
  abort 'Xvfb failed to start' unless display
  exit(system({ 'DISPLAY' => ":#{Integer(display)}" }, ARGV.fetch(0)) ? 0 : 1)
ensure
  reader.close
  Process.kill('TERM', server) rescue nil
  Process.wait(server)
end
''', keyboard_test])
  xcb_xtest = dependency('xcb-xtest')
  client = executable('test-client', 'tests/client.cpp', dependencies: [xcb, xcb_xtest])
  test('x11', find_program('ruby'), args: [files('tests/x11.rb'), mortar, client], timeout: 180)
endif
