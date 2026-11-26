project('mortar', 'cpp', version: '0.1.0', default_options: ['cpp_std=c++23', 'warning_level=3'])

xcb = dependency('xcb')
xcb_randr = dependency('xcb-randr')
xcb_xkb = dependency('xcb-xkb')
xkbcommon = dependency('xkbcommon-x11')
guile = dependency('guile-3.0').as_system('system')
pcre2 = dependency('libpcre2-8')
libjq = dependency('libjq', required: false)
if not libjq.found()
  libjq = meson.get_compiler('cpp').find_library('jq')
endif
libjq = libjq.as_system('system')
pangocairo = dependency('pangocairo')
fontconfig = dependency('fontconfig')
mortarbar = executable('mortarbar', 'mortarbar/main.cpp', 'mortarbar/status.cpp', 'mortar/randr.cpp', dependencies: [xcb, xcb_randr, xcb_xkb, libjq, pangocairo, fontconfig], install: true)
mortar = executable('mortar', 'mortar/main.cpp', 'mortar/restart.cpp', 'mortar/randr.cpp', 'mortar/outputs.cpp', 'mortar/scratchpad.cpp', 'mortar/ipc.cpp', 'mortar/actions.cpp', 'mortar/layout.cpp', 'mortar/workspaces.cpp', 'mortar/keyboard.cpp', 'mortar/config.cpp', dependencies: [xcb, xcb_randr, xcb_xkb, xkbcommon, guile, pcre2], install: true)

if get_option('tests')
  test('mortarbar', find_program('ruby'), args: [files('tests/mortarbar.rb'), mortarbar])
  bar_outputs_test = executable('test-mortarbar-outputs', 'mortarbar/main.cpp', 'mortarbar/status.cpp', 'tests/mortarbar-outputs.cpp', dependencies: [xcb, xcb_randr, xcb_xkb, libjq, pangocairo, fontconfig])
  test('mortarbar-outputs', find_program('ruby'), args: [files('tests/mortarbar.rb'), bar_outputs_test, 'outputs'])
  status_test = executable('test-status', 'tests/status.cpp', 'mortarbar/status.cpp', dependencies: libjq)
  test('status', status_test)
  ipc_test = executable('test-ipc', 'tests/ipc.cpp', 'mortar/ipc.cpp')
  test('ipc', ipc_test)
  config_test = executable('test-config', 'tests/config.cpp', 'mortar/config.cpp', dependencies: [guile, xkbcommon, pcre2])
  test('config', config_test)
  size_hints_test = executable('test-size-hints', 'tests/size-hints.cpp', 'mortar/restart.cpp', 'mortar/randr.cpp', 'mortar/outputs.cpp', 'mortar/scratchpad.cpp', 'mortar/ipc.cpp', 'mortar/actions.cpp', 'mortar/layout.cpp', 'mortar/workspaces.cpp', 'mortar/keyboard.cpp', 'mortar/config.cpp', dependencies: [xcb, xcb_randr, xcb_xkb, xkbcommon, guile, pcre2])
  test('size-hints', size_hints_test)
  floating_size_hints_test = executable('test-floating-size-hints', 'tests/floating-size-hints.cpp')
  test('floating-size-hints', floating_size_hints_test)
  scratchpad_test = executable('test-scratchpad', 'tests/scratchpad.cpp', 'mortar/scratchpad.cpp')
  test('scratchpad', scratchpad_test)
  test('gaps', executable('test-gaps', 'tests/gaps.cpp'))
  layout_test = executable('test-layout', 'tests/layout.cpp', 'mortar/layout.cpp')
  test('layout', layout_test)
  restart_test = executable('test-restart', 'tests/restart.cpp', 'mortar/restart.cpp', 'mortar/workspaces.cpp', 'mortar/layout.cpp')
  test('restart', restart_test)
  workspaces_test = executable('test-workspaces', 'tests/workspaces.cpp', 'mortar/workspaces.cpp', 'mortar/layout.cpp')
  test('workspaces', workspaces_test)
  outputs_test = executable('test-outputs', 'tests/outputs.cpp', 'mortar/outputs.cpp')
  test('outputs', outputs_test)
  randr_test = executable('test-randr', 'tests/randr.cpp', 'mortar/randr.cpp', 'mortar/outputs.cpp', dependencies: [xcb, xcb_randr])
  test('randr', find_program('ruby'), args: ['-e', '''
require 'timeout'
reader, writer = IO.pipe
server = Process.spawn('Xvfb', '-displayfd', '3', '-screen', '0', '800x600x24', *ARGV.drop(1), 3 => writer, out: File::NULL, err: File::NULL)
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
''', randr_test])
  test('randr-fallback', find_program('ruby'), args: ['-e', '''
require 'timeout'
reader, writer = IO.pipe
server = Process.spawn('Xvfb', '-displayfd', '3', '-screen', '0', '800x600x24', '-extension', 'RANDR', 3 => writer, out: File::NULL, err: File::NULL)
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
''', randr_test])
  selection_test = executable('test-wm-selection', 'tests/wm-selection.cpp', dependencies: xcb)
  test('wm-selection', find_program('ruby'), args: ['-e', '''
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
''', selection_test])
  test('wm-selection-loss', find_program('ruby'), args: ['-e', '''
require 'timeout'
require 'tmpdir'
reader, writer = IO.pipe
server = Process.spawn('Xvfb', '-displayfd', '3', '-screen', '0', '800x600x24', 3 => writer, out: File::NULL, err: File::NULL)
writer.close
wm = nil
begin
  display = Timeout.timeout(10) { reader.gets }
  abort 'Xvfb failed to start' unless display
  Dir.mktmpdir do |dir|
    env = { 'DISPLAY' => ":#{Integer(display)}", 'HOME' => dir, 'XDG_CONFIG_HOME' => dir, 'XDG_RUNTIME_DIR' => dir }
    wm = Process.spawn(env, ARGV.fetch(0), out: File::NULL, err: File::NULL)
    abort 'selection takeover failed' unless system(env, ARGV.fetch(1), 'takeover')
    Timeout.timeout(10) { Process.wait(wm) }
    wm = nil
  end
ensure
  Process.kill('TERM', wm) rescue nil if wm
  Process.wait(wm) rescue nil if wm
  reader.close
  Process.kill('TERM', server) rescue nil
  Process.wait(server)
end
''', mortar, selection_test])
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
  actions_test = executable('test-actions', 'tests/actions.cpp', 'mortar/actions.cpp', dependencies: xcb)
  test('actions', find_program('ruby'), args: ['-e', '''
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
''', actions_test])
  xcb_xtest = dependency('xcb-xtest')
  client = executable('test-client', 'tests/client.cpp', dependencies: [xcb, xcb_xtest])
  test('ipc-integration', find_program('ruby'), args: [files('tests/ipc-integration.rb'), mortar, client])
  ewmh_test = executable('test-ewmh', 'tests/ewmh.cpp', dependencies: xcb)
  test('ewmh', find_program('ruby'), args: ['-e', '''
require 'timeout'
require 'tmpdir'
require 'fileutils'
reader, writer = IO.pipe
server = Process.spawn('Xvfb', '-displayfd', '3', '-screen', '0', '800x600x24', 3 => writer, out: File::NULL, err: File::NULL)
writer.close
wm = nil
begin
  display = Timeout.timeout(10) { reader.gets }
  abort 'Xvfb failed to start' unless display
  Dir.mktmpdir do |dir|
    FileUtils.mkdir_p(File.join(dir, 'mortar'))
    File.write(File.join(dir, 'mortar', 'config.scm'), "(bar-output \"mortar-test-no-such-output\")\n")
    env = { 'DISPLAY' => ":#{Integer(display)}", 'HOME' => dir, 'XDG_CONFIG_HOME' => dir }
    wm = Process.spawn(env, ARGV.fetch(0), out: File::NULL, err: File::NULL)
    exit(system(env, ARGV.fetch(1)) ? 0 : 1)
  end
ensure
  Process.kill('TERM', wm) rescue nil if wm
  Process.wait(wm) rescue nil if wm
  reader.close
  Process.kill('TERM', server) rescue nil
  Process.wait(server)
end
''', mortar, ewmh_test])
  rules_test = executable('test-window-rules', 'tests/window-rules.cpp', dependencies: xcb)
  test('window-rules', find_program('ruby'), args: [files('tests/window-rules.rb'), mortar, rules_test])
  test('reload', find_program('ruby'), args: [files('tests/reload.rb'), mortar, client])
  test('x11', find_program('ruby'), args: [files('tests/x11.rb'), mortar, client, mortarbar], timeout: 180)
endif
