require 'tmpdir'
require 'timeout'
require 'fcntl'
require 'socket'

Dir.mktmpdir('mortar-test-') do |dir|
  xvfb = nil
  wm = nil
  begin
    config_dir = File.join(dir, 'mortar')
    Dir.mkdir(config_dir)
    config = File.join(config_dir, 'config.scm')
    marker = File.join(dir, 'side-effect')
    side_effect = "(call-with-output-file #{marker.dump} (lambda (port) (display \"executed\" port)))\n"
    File.write(config, side_effect)
    invalid_env = { 'DISPLAY' => '', 'XDG_CONFIG_HOME' => dir, 'HOME' => dir }
    system(invalid_env, ARGV.fetch(0), out: File::NULL, err: File::NULL) and
      abort 'Expected failure without an X server'
    abort 'Configuration executed without an X server' if File.exist?(marker)
    defaults = File.read(File.expand_path('../config.scm', __dir__))
    File.write(config, defaults)
    reader, writer = IO.pipe
    xvfb = Process.spawn('Xvfb', '-displayfd', '3', '-screen', '0', '800x600x24', '-extension', 'RANDR',
                         3 => writer, out: File::NULL, err: File::NULL)
    writer.close
    display = Timeout.timeout(10) { reader.gets }
    reader.close
    abort 'Xvfb without RandR failed to start' unless display
    env = { 'DISPLAY' => ":#{Integer(display)}", 'XDG_CONFIG_HOME' => dir, 'HOME' => dir }
    client_input, client_writer = IO.pipe
    client_reader, client_output = IO.pipe
    client = Process.spawn(env, ARGV.fetch(1), 'startup-failure', in: client_input, out: client_output)
    client_input.close
    client_output.close
    abort 'Client failed to prepare startup failure check' unless
      Timeout.timeout(10) { client_reader.gets } == "startup failure ready\n"
    error_path = File.join(dir, 'no-randr.log')
    wm = Process.spawn(env, ARGV.fetch(0), out: File::NULL, err: error_path)
    _, status = Timeout.timeout(10) { Process.wait2(wm) }
    wm = nil
    abort 'Window manager unexpectedly accepted keyboard grab conflict without RandR' if status.success?
    abort 'Window manager failed for an unexpected reason' unless File.read(error_path).include?('key grabs failed')
    client_writer.puts
    client_writer.close
    client_reader.close
    _, status = Timeout.timeout(10) { Process.wait2(client) }
    client = nil
    abort 'Startup failure left stale EWMH state or unmapped an existing window' unless status.success?
    Process.kill('TERM', xvfb)
    Process.wait(xvfb)
    xvfb = nil
    reader, writer = IO.pipe
    xvfb = Process.spawn('Xvfb', '-displayfd', '3', '-screen', '0', '800x600x24',
                         3 => writer, out: File::NULL, err: File::NULL)
    writer.close
    display = Timeout.timeout(10) { reader.gets }
    reader.close
    abort 'Xvfb failed to start' unless display
    launch_log = File.join(dir, 'launch.log')
    %w[x-terminal-emulator dmenu_run mortar-test-spawn].each { |name| File.symlink(File.expand_path(ARGV.fetch(1)), File.join(dir, name)) }
    env = { 'DISPLAY' => ":#{Integer(display)}", 'XDG_CONFIG_HOME' => dir, 'HOME' => dir,
            'PATH' => "#{dir}:#{ENV.fetch('PATH')}", 'MORTAR_TEST_LAUNCH' => launch_log }
    File.symlink(File.expand_path(ARGV.fetch(2)), File.join(dir, 'mortarbar'))
    restart_marker = File.join(dir, 'restart-marker')
    socket_path = File.join(dir, 'floating.sock')
    File.write(config, "(bar-height 24)\n(bar-position 'top)\n" \
                       "(call-with-output-file #{restart_marker.dump} (lambda (port) (display \"ready\" port)))\n")
    wm = Process.spawn(env.merge('MORTARSOCK' => socket_path), ARGV.fetch(0), out: File::NULL, err: File::NULL, in: File::NULL)
    abort 'Automatic mortarbar did not reserve space' unless system(env, ARGV.fetch(1), 'bar-reservation')
    abort 'Workspace names or bar clicks failed' unless system(env, ARGV.fetch(1), 'bar-workspaces')
    client_input, client_writer = IO.pipe
    client_reader, client_output = IO.pipe
    client = Process.spawn(env, ARGV.fetch(1), 'floating-center', in: client_input, out: client_output)
    client_input.close
    client_output.close
    abort 'Floating window was not centered in the usable area' unless
      Timeout.timeout(10) { client_reader.gets } == "floating ready\n"
    File.delete(restart_marker)
    socket = UNIXSocket.new(socket_path)
    request = 'eval (restart)'
    socket.write([request.bytesize].pack('N') + request)
    socket.close
    Timeout.timeout(10) { sleep 0.02 until File.exist?(restart_marker) }
    abort 'Window manager did not restart' unless system(env, ARGV.fetch(1), 'bar-reservation')
    client_writer.puts
    client_writer.close
    client_reader.close
    _, status = Timeout.timeout(10) { Process.wait2(client) }
    client = nil
    abort 'Floating window moved after restart' unless status.success?
    File.write(config, defaults)
    Process.kill('TERM', wm)
    _, status = Timeout.timeout(10) { Process.wait2(wm) }
    wm = nil
    abort 'Window manager failed during bar shutdown' unless status.success?
    File.unlink(File.join(dir, 'mortarbar'))
    marker_bar = File.join(dir, 'bar-pid')
    File.write(File.join(dir, 'mortarbar'), "#!/bin/sh\ntrap '' TERM\nprintf '%s' $$ > \"#{marker_bar}\"\nwhile :; do sleep 1; done\n")
    File.chmod(0755, File.join(dir, 'mortarbar'))
    input_reader, input_writer = IO.pipe
    output_reader, output_writer = IO.pipe
    output_writer.fcntl(Fcntl::F_SETFL, output_writer.fcntl(Fcntl::F_GETFL) & ~File::NONBLOCK)
    before = output_writer.fcntl(Fcntl::F_GETFL)
    wm = Process.spawn(env, ARGV.fetch(0), in: input_reader, out: output_writer, err: File::NULL)
    Timeout.timeout(10) { sleep 0.02 until File.exist?(marker_bar) }
    abort "mortarbar modified WM stdout flags: #{before} #{output_writer.fcntl(Fcntl::F_GETFL)}" unless output_writer.fcntl(Fcntl::F_GETFL) == before
    bar_pid = Integer(File.read(marker_bar))
    Process.kill('TERM', wm)
    _, status = Timeout.timeout(5) { Process.wait2(wm) }
    wm = nil
    abort 'Window manager failed to stop unresponsive mortarbar' unless status.success?
    begin
      Process.kill(0, bar_pid)
      abort 'mortarbar survived shutdown'
    rescue Errno::ESRCH
    end
    [input_reader, input_writer, output_reader, output_writer].each(&:close)
    File.unlink(File.join(dir, 'mortarbar'))
    env['PATH'] = env.fetch('PATH').split(File::PATH_SEPARATOR).reject { |path| File.executable?(File.join(path, 'mortarbar')) }.join(File::PATH_SEPARATOR)
    client_input, client_writer = IO.pipe
    client_reader, client_output = IO.pipe
    client = Process.spawn(env, ARGV.fetch(1), 'grab-conflict', in: client_input, out: client_output)
    client_input.close
    client_output.close
    abort 'Client failed to grab Alt+1' unless
      Timeout.timeout(10) { client_reader.gets } == "grab conflict ready\n"
    error_path = File.join(dir, 'grab-conflict.log')
    wm = Process.spawn(env, ARGV.fetch(0), out: File::NULL, err: error_path)
    _, status = Timeout.timeout(10) { Process.wait2(wm) }
    wm = nil
    abort 'Window manager unexpectedly accepted keyboard grab conflict' if status.success?
    abort 'Window manager failed for an unexpected reason' unless
      File.read(error_path).include?('key grabs failed')
    client_writer.puts
    client_writer.close
    _, status = Timeout.timeout(10) { Process.wait2(client) }
    client = nil
    abort 'Existing client did not remain mapped' unless status.success?
    client_reader.close
    client_input, client_writer = IO.pipe
    client_reader, client_output = IO.pipe
    client = Process.spawn(env, ARGV.fetch(1), in: client_input, out: client_output)
    client_input.close
    client_output.close
    abort 'Client failed to create existing window' unless
      Timeout.timeout(10) { client_reader.gets } == "existing window ready\n"
    wm_error_path = File.join(dir, 'wm.log')
    File.write(config, defaults + side_effect)
    socket_path = File.join(dir, 'exit.sock')
    wm = Process.spawn(env.merge('MORTARSOCK' => socket_path), ARGV.fetch(0), out: File::NULL, err: wm_error_path)
    client_writer.puts
    abort 'Window manager failed to manage existing window' unless
      Timeout.timeout(10) { client_reader.gets } == "window manager ready\n"
    abort 'Valid configuration side effect did not execute' unless File.exist?(marker)
    File.delete(marker)
    error_path = File.join(dir, 'second.log')
    File.write(config, side_effect)
    success = system(env, ARGV.fetch(0), out: File::NULL, err: error_path)
    abort 'Configuration executed while another window manager exists' if File.exist?(marker)
    File.write(config, defaults)
    abort 'Second window manager unexpectedly succeeded' if success
    abort 'Second window manager failed for an unexpected reason' unless
      File.read(error_path).include?('cannot acquire window management')
    abort 'Client failed to prepare lifecycle checks' unless
      Timeout.timeout(30) { client_reader.gets } == "lifecycle ready\n"
    socket = UNIXSocket.new(socket_path)
    request = 'eval (exit)'
    socket.write([request.bytesize].pack('N') + request)
    _, status = Timeout.timeout(10) { Process.wait2(wm) }
    socket.close
    wm = nil
    abort "Window manager shutdown failed:\n#{File.read(wm_error_path)}" unless status.success?
    client_writer.puts
    abort 'Client failed shutdown checks' unless
      Timeout.timeout(20) { client_reader.gets } == "shutdown checked\n"
    wm = Process.spawn(env, ARGV.fetch(0), out: File::NULL, err: wm_error_path)
    client_writer.puts
    client_writer.close
    client_reader.close
    _, status = Timeout.timeout(30) { Process.wait2(client) }
    client = nil
    abort 'Client geometry checks failed' unless status.success?
    failures = %w[switch move caps num both move-caps move-num move-both remap].reject do |mode|
      system(env, ARGV.fetch(1), mode, out: File::NULL)
    end
    abort "Keyboard checks failed: #{failures.join(', ')}" unless failures.empty?
    %w[transient dialog floating-toggle fullscreen scratchpad workspace-pointer].each do |mode|
      abort "Floating window check failed: #{mode}\nMortar stderr:\n#{File.read(wm_error_path)}" unless
        system(env, ARGV.fetch(1), mode, out: File::NULL)
    end
    unless system(env, ARGV.fetch(1), 'milestone', out: File::NULL)
      records = File.exist?(launch_log) ? File.read(launch_log) : '(missing)'
      abort "Milestone keyboard checks failed\nLaunch log:\n#{records}\nMortar stderr:\n#{File.read(wm_error_path)}"
    end
    abort "Default launch did not execute exact commands\nLaunch log:\n#{File.read(launch_log)}\nMortar stderr:\n#{File.read(wm_error_path)}" unless
      File.readlines(launch_log).sort == ["dmenu_run 1\n", "x-terminal-emulator 1\n"]
    Process.kill('TERM', wm)
    _, status = Timeout.timeout(10) { Process.wait2(wm) }
    wm = nil
    abort "Window manager shutdown failed:\n#{File.read(wm_error_path)}" unless status.success?
    abort 'French keyboard setup failed' unless system(env, 'setxkbmap', '-layout', 'fr', out: File::NULL)
    wm = Process.spawn(env, ARGV.fetch(0), out: File::NULL, err: wm_error_path)
    %w[switch move].each do |mode|
      abort "French keyboard check failed: #{mode}" unless system(env, ARGV.fetch(1), mode, out: File::NULL)
    end
    abort 'Keyboard group setup failed' unless
      system(env, 'setxkbmap', '-layout', 'us,fr', '-option', 'grp:alt_shift_toggle', out: File::NULL)
    abort 'Keyboard group check failed' unless system(env, ARGV.fetch(1), 'group', out: File::NULL)
    Process.kill('TERM', wm)
    _, status = Timeout.timeout(10) { Process.wait2(wm) }
    wm = nil
    abort "Window manager shutdown failed:\n#{File.read(wm_error_path)}" unless status.success?
    abort 'Keyboard reset failed' unless system(env, 'setxkbmap', '-layout', 'us', out: File::NULL)
    File.write(config, "(bind-key 50 8 (lambda () (workspace 3)))\n(error \"broken configuration\")\n")
    error_path = File.join(dir, 'config.log')
    wm = Process.spawn(env, ARGV.fetch(0), out: File::NULL, err: error_path)
    _, status = Timeout.timeout(10) { Process.wait2(wm) }
    wm = nil
    abort 'Broken configuration unexpectedly succeeded' if status.success?
    abort 'Broken configuration was not reported' if File.empty?(error_path)
    File.delete(config)
    wm = Process.spawn(env, ARGV.fetch(0), out: File::NULL, err: error_path)
    _, status = Timeout.timeout(10) { Process.wait2(wm) }
    wm = nil
    abort 'Missing configuration unexpectedly succeeded' if status.success?
    abort 'Missing configuration was not reported' if File.empty?(error_path)
    File.write(config, "(bind-key 49 8 (lambda () (error \"callback failure\")))\n" \
                       "(bind-key 50 8 (lambda () (workspace 3)))\n" \
                       "(bind-key 65475 8 (lambda () (workspace 3)))\n")
    wm = Process.spawn(env, ARGV.fetch(0), out: File::NULL, err: error_path)
    abort "Configuration callback checks failed:\n#{File.read(error_path)}" unless system(env, ARGV.fetch(1), 'config', out: File::NULL)
    abort 'Arbitrary key binding failed' unless system(env, ARGV.fetch(1), 'arbitrary', out: File::NULL)
    Process.kill('TERM', wm)
    _, status = Timeout.timeout(10) { Process.wait2(wm) }
    wm = nil
    abort 'Window manager shutdown failed' unless status.success?
    [['(gaps 10 5)', 'gaps'], ['(gaps 65535 65535)', 'gaps-large']].each do |setting, mode|
      File.write(config, "#{setting}\n")
      wm = Process.spawn(env, ARGV.fetch(0), out: File::NULL, err: error_path)
      abort "Gap geometry check failed: #{mode}" unless system(env, ARGV.fetch(1), mode)
      Process.kill('TERM', wm)
      _, status = Timeout.timeout(10) { Process.wait2(wm) }
      wm = nil
      abort 'Window manager shutdown failed after gap checks' unless status.success?
    end
    File.write(config, defaults + "(border 3 #x123456 #x654321)\n")
    wm = Process.spawn(env, ARGV.fetch(0), out: File::NULL, err: error_path)
    abort 'Border checks failed' unless system(env, ARGV.fetch(1), 'borders')
    abort 'Parent border and split checks failed' unless system(env, ARGV.fetch(1), 'parent-borders')
    abort 'ARGB border check failed' unless system(env, ARGV.fetch(1), 'argb-border')
    Process.kill('TERM', wm)
    _, status = Timeout.timeout(10) { Process.wait2(wm) }
    wm = nil
    abort 'Window manager shutdown failed after border checks' unless status.success?
    File.write(config, <<~SCHEME)
      (bind-key '(alt b) (lambda () (split 'vertical)))
      (bind-key '(alt z) (lambda () (toggle-split)))
      (bind-key 99 8 (lambda () (focus 'up)))
      (bind-key 101 8 (lambda () (focus 'down)))
      (bind-key 102 8 (lambda () (split 'horizontal)))
      (bind-key 103 8 (lambda () (focus 'left)))
      (bind-key 105 8 (lambda () (focus 'right)))
      (bind-key 112 8 (lambda () (move 'left)))
      (bind-key 117 8 (lambda () (resize 'right 10)))
      (bind-key 121 8 (lambda () (move 'right)))
      (bind-key 110 8 (lambda () (close)))
      (bind-key 111 8 (lambda () (spawn "mortar-test-spawn" "a b" "; touch forbidden")))
    SCHEME
    wm = Process.spawn(env, ARGV.fetch(0), out: File::NULL, err: error_path)
    unless system(env, ARGV.fetch(1), 'scheme-actions', out: File::NULL)
      records = File.exist?(launch_log) ? File.read(launch_log) : '(missing)'
      abort "Scheme action checks failed\nLaunch log:\n#{records}\nMortar stderr:\n#{File.read(error_path)}"
    end
    Process.kill('TERM', wm)
    _, status = Timeout.timeout(10) { Process.wait2(wm) }
    wm = nil
    abort "Window manager shutdown failed after callback error:\n#{File.read(error_path)}" unless status.success?
  ensure
    [client, wm, xvfb].compact.each do |pid|
      begin
        Process.kill('TERM', pid)
      rescue Errno::ESRCH
      end
      begin
        Process.wait(pid)
      rescue Errno::ECHILD
      end
    end
  end
end
