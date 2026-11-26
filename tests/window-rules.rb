require 'tmpdir'
require 'timeout'

Dir.mktmpdir('mortar-rules-') do |dir|
  xvfb = wm = nil
  reader, writer = IO.pipe
  begin
    xvfb = Process.spawn('Xvfb', '-displayfd', '3', '-screen', '0', '800x600x24',
                         3 => writer, out: File::NULL, err: File::NULL)
    writer.close
    display = Timeout.timeout(10) { reader.gets }
    abort 'Xvfb failed to start' unless display
    Dir.mkdir(File.join(dir, 'mortar'))
    File.write(File.join(dir, 'mortar', 'config.scm'), <<~SCHEME)
      (window-rule "Float" #f 'floating)
      (window-rule "Remote" 2 #f)
      (window-rule "Override" 2 'floating)
      (window-rule "Override" 1 'tiled)
      (window-rule #f #f 'floating "special")
      (window-rule #f 2 #f #f "Target")
      (window-rule "Combined" 3 #f "special" "Target")
      (window-rule-regex "^Regex" 2 'floating "^term" "file$")
      (window-rule "Regex.*" 3 #f)
    SCHEME
    env = { 'DISPLAY' => ":#{Integer(display)}", 'XDG_CONFIG_HOME' => dir, 'HOME' => dir,
            'PATH' => '/nonexistent' }
    wm = Process.spawn(env, ARGV.fetch(0), out: File::NULL, err: File::NULL)
    abort 'Window rule test failed' unless system(env, ARGV.fetch(1))
  ensure
    reader.close
    writer.close unless writer.closed?
    [wm, xvfb].compact.each do |pid|
      Process.kill('TERM', pid) rescue nil
      Process.wait(pid) rescue nil
    end
  end
end
