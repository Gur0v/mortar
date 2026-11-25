require 'rbconfig'
require 'timeout'

bar = ARGV.fetch(0)
reader, writer = IO.pipe
server = Process.spawn('Xvfb', '-displayfd', '3', '-screen', '0', '800x600x24', 3 => writer, out: File::NULL, err: File::NULL)
writer.close
begin
  display = Timeout.timeout(10) { reader.gets }
  abort 'Xvfb failed to start' unless display
  reader_status, writer_status = IO.pipe
  code = 'STDERR.write(ARGV.fetch(0)); STDOUT.write(%q({"version":1,"click_events":true}
[[{"full_text":"status","name":"test"}]]
)); STDOUT.flush; STDIN.read'
  pid = Process.spawn({ 'DISPLAY' => ":#{Integer(display)}" }, bar, RbConfig.ruby, '-e', code, 'a b', err: writer_status)
  writer_status.close
  abort 'status arguments changed' unless Timeout.timeout(10) { reader_status.read(3) } == 'a b'
  reader_status.close
  sleep 0.2
  abort 'bar exited unexpectedly' unless Process.waitpid(pid, Process::WNOHANG).nil?
  Process.kill('TERM', pid)
  Process.wait(pid)

  child_reader, child_writer = IO.pipe
  code = 'child = fork { loop { sleep 1 } }; STDERR.puts(child); STDERR.flush; STDOUT.write("bad\n"); STDOUT.flush; loop { sleep 1 }'
  pid = Process.spawn({ 'DISPLAY' => ":#{Integer(display)}" }, bar, RbConfig.ruby, '-e', code, err: child_writer)
  child_writer.close
  descendant = Integer(Timeout.timeout(10) { child_reader.gets })
  Timeout.timeout(10) do
    loop do
      state = File.read("/proc/#{descendant}/stat").split[2] rescue 'Z'
      break if state == 'Z'
      sleep 0.05
    end
  end
  Process.kill('TERM', pid)
  Process.wait(pid)
  child_reader.close
ensure
  reader.close
  Process.kill('TERM', server) rescue nil
  Process.wait(server)
end
