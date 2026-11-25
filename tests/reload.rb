require 'tmpdir'
require 'timeout'

Dir.mktmpdir('mortar-reload-') do |dir|
  xvfb = wm = client = conflict = nil
  begin
    reader, writer = IO.pipe
    xvfb = Process.spawn('Xvfb', '-displayfd', '3', '-screen', '0', '800x600x24',
                         3 => writer, out: File::NULL, err: File::NULL)
    writer.close
    display = Timeout.timeout(10) { reader.gets }
    reader.close
    abort 'Xvfb failed to start' unless display
    config_dir = File.join(dir, 'mortar')
    Dir.mkdir(config_dir)
    config = File.join(config_dir, 'config.scm')
    log = File.join(dir, 'callbacks')
    staged = File.join(dir, 'staged')
    errors = File.join(dir, 'wm.log')
    bar_log = File.join(dir, 'bars')
    bar = File.join(dir, 'mortarbar')
    File.write(bar, "#!/bin/sh\ntrap '' TERM\nprintf '%s %s\\n' \"$$\" \"$*\" >> #{bar_log}\nwhile :; do sleep 1; done\n")
    File.chmod(0755, bar)
    env = { 'PATH' => "#{dir}:#{ENV.fetch('PATH')}", 'DISPLAY' => ":#{Integer(display)}", 'XDG_CONFIG_HOME' => dir, 'HOME' => dir }
    binding = ->(key, value) { "(bind-key #{key.ord} 8 (lambda () (call-with-output-file #{log.dump} (lambda (p) (display #{value.dump} p)))))\n" }
    File.write(config, binding.call('z', 'old') + '(bar-status "old")')
    wm = Process.spawn(env, ARGV.fetch(0), out: File::NULL, err: errors)
    input, output = IO.pipe
    response, child_output = IO.pipe
    client = Process.spawn(env, ARGV.fetch(1), 'reload-keys', in: input, out: child_output)
    input.close
    child_output.close
    abort 'Key client failed' unless Timeout.timeout(10) { response.gets } == "ready\n"
    send_key = lambda do |key|
      output.puts(key)
      abort 'Key client stopped' unless Timeout.timeout(10) { response.gets } == "sent\n"
    end
    wait_for = lambda do |message, &condition|
      Timeout.timeout(5) { sleep 0.02 until condition.call }
    rescue Timeout::Error
      abort "#{message}\n#{File.read(errors)}"
    end
    check = lambda do |key, value|
      File.delete(log) if File.exist?(log)
      wait_for.call("Expected #{value}") do
        send_key.call(key)
        File.exist?(log) && File.read(log) == value
      end
    end
    wait_for.call('Initial binding did not become active') do
      send_key.call('z')
      File.exist?(log) && File.read(log) == 'old'
    end
    wait_for.call('Initial bar did not start') { File.exist?(bar_log) }
    old_bar = Integer(File.read(bar_log).split.first)
    File.write(config, binding.call('x', 'new') + '(bar-status "new")')
    send_key.call('c')
    check.call('x', 'new')
    wait_for.call('Bar did not restart') { File.readlines(bar_log).size == 2 }
    abort 'Bar arguments did not change' unless File.readlines(bar_log).last.split.last == 'new'
    begin
      Process.kill(0, old_bar)
      abort 'Old bar survived reload'
    rescue Errno::ESRCH
    end
    File.write(config, binding.call('x', 'new') + "(bar-status \"new\") (bar-position 'bottom) (bar-font \"sans 12\") (bar-colors 1 2 3 4)")
    send_key.call('c')
    wait_for.call('Bar appearance did not restart bar') { File.readlines(bar_log).size == 3 }
    abort 'Bar settings not passed' unless File.readlines(bar_log).last.include?('--mortar-settings bottom sans 12 1 2 3 4 -- new')
    File.write(config, binding.call('z', 'bad') + "(bar-position 'top) (error \"reload failed\")\n")
    send_key.call('c')
    wait_for.call('Failed reload was not reported') { File.read(errors).include?('failed to reload configuration') }
    check.call('x', 'new')
    abort 'Failed reload restarted bar' unless File.readlines(bar_log).size == 3
    conflict_input, conflict_writer = IO.pipe
    conflict_reader, conflict_output = IO.pipe
    conflict = Process.spawn(env, ARGV.fetch(1), 'reload-conflict', in: conflict_input, out: conflict_output)
    conflict_input.close
    conflict_output.close
    abort 'Conflict client failed' unless Timeout.timeout(10) { conflict_reader.gets } == "ready\n"
    File.write(config, binding.call('z', 'conflict') + binding.call('x', 'conflict') +
                       "(call-with-output-file #{staged.dump} (lambda (p) (display \"yes\" p)))\n")
    send_key.call('c')
    wait_for.call('Reload did not stage configuration') { File.exist?(staged) }
    check.call('x', 'new')
    conflict_writer.puts
    conflict_writer.close
    _, status = Process.wait2(conflict)
    conflict = nil
    abort 'Conflict client failed' unless status.success?
  ensure
    output&.close unless output&.closed?
    [conflict, client, wm, xvfb].compact.each do |pid|
      begin
        Process.kill('TERM', pid)
        Process.wait(pid)
      rescue Errno::ESRCH, Errno::ECHILD
      end
    end
  end
end
