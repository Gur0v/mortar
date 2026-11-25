require 'tmpdir'
require 'timeout'

Dir.mktmpdir('mortar-test-') do |dir|
  xvfb = nil
  wm = nil
  begin
    system({ 'DISPLAY' => '' }, ARGV.fetch(0), out: File::NULL, err: File::NULL) and
      abort 'Expected failure without an X server'
    reader, writer = IO.pipe
    xvfb = Process.spawn('Xvfb', '-displayfd', '3', '-screen', '0', '800x600x24',
                         3 => writer, out: File::NULL, err: File::NULL)
    writer.close
    display = Timeout.timeout(10) { reader.gets }
    reader.close
    abort 'Xvfb failed to start' unless display
    env = { 'DISPLAY' => ":#{Integer(display)}" }
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
      File.read(error_path).include?('keyboard setup failed')
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
    wm = Process.spawn(env, ARGV.fetch(0), out: File::NULL, err: File::NULL)
    client_writer.puts
    abort 'Window manager failed to manage existing window' unless
      Timeout.timeout(10) { client_reader.gets } == "window manager ready\n"
    error_path = File.join(dir, 'second.log')
    success = system(env, ARGV.fetch(0), out: File::NULL, err: error_path)
    abort 'Second window manager unexpectedly succeeded' if success
    abort 'Second window manager failed for an unexpected reason' unless
      File.read(error_path).include?('cannot acquire window management')
    abort 'Client failed to prepare lifecycle checks' unless
      Timeout.timeout(30) { client_reader.gets } == "lifecycle ready\n"
    Process.kill('TERM', wm)
    _, status = Timeout.timeout(10) { Process.wait2(wm) }
    wm = nil
    abort 'Window manager shutdown failed' unless status.success?
    client_writer.puts
    abort 'Client failed shutdown checks' unless
      Timeout.timeout(20) { client_reader.gets } == "shutdown checked\n"
    wm = Process.spawn(env, ARGV.fetch(0), out: File::NULL, err: File::NULL)
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
    Process.kill('TERM', wm)
    _, status = Timeout.timeout(10) { Process.wait2(wm) }
    wm = nil
    abort 'Window manager shutdown failed' unless status.success?
    abort 'French keyboard setup failed' unless system(env, 'setxkbmap', '-layout', 'fr', out: File::NULL)
    wm = Process.spawn(env, ARGV.fetch(0), out: File::NULL, err: File::NULL)
    %w[switch move].each do |mode|
      abort "French keyboard check failed: #{mode}" unless system(env, ARGV.fetch(1), mode, out: File::NULL)
    end
    abort 'Keyboard group setup failed' unless
      system(env, 'setxkbmap', '-layout', 'us,fr', '-option', 'grp:alt_shift_toggle', out: File::NULL)
    abort 'Keyboard group check failed' unless system(env, ARGV.fetch(1), 'group', out: File::NULL)
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
