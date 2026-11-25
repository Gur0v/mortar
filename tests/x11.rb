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
    wm = Process.spawn(env, ARGV.fetch(0), out: File::NULL, err: File::NULL)
    sleep 0.2
    Process.kill(0, wm)
    error_path = File.join(dir, 'second.log')
    success = system(env, ARGV.fetch(0), out: File::NULL, err: error_path)
    abort 'Second window manager unexpectedly succeeded' if success
    abort 'Second window manager failed for an unexpected reason' unless
      File.read(error_path).include?('cannot acquire window management')
    abort 'Client was not tiled' unless system(env, ARGV.fetch(1))
  ensure
    [wm, xvfb].compact.each do |pid|
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
