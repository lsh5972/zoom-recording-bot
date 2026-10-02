# frozen_string_literal: true

require_relative 'test_helper'
require 'rbconfig'

class CliTest < Minitest::Test
  def test_stop_uses_configured_runs_directory
    Dir.mktmpdir('zoom bot cli ') do |root|
      runs = File.join(root, 'recordings')
      meeting = File.join(runs, '123456789')
      FileUtils.mkdir_p(meeting)
      session_id = '00000000-0000-4000-8000-000000000001'
      File.write(File.join(meeting, 'session.json'), JSON.generate(session_id: session_id, meeting_id: '123456789'))
      docker = File.join(root, 'docker')
      File.write(docker, "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$ZOOM_TEST_COMMAND_LOG\"\n")
      File.chmod(0o755, docker)
      log = File.join(root, 'docker-args')
      env = { 'ZOOM_RUNS_DIR' => runs, 'ZOOM_TEST_COMMAND_LOG' => log, 'PATH' => "#{root}:#{ENV.fetch('PATH')}" }
      out, err, status = Open3.capture3(env, RbConfig.ruby, File.expand_path('../bin/zoom-bot', __dir__), 'stop', '123456789')
      assert status.success?, err
      assert_includes out, 'Container stopped'
      assert_equal ['stop', '--time', '30', "zoom-bot-#{session_id}"], File.readlines(log, chomp: true)
      assert File.exist?(File.join(meeting, 'session.json'))
    end
  end

  def test_docker_wrapper_uses_vm_socket_for_desktop_and_context_socket_for_linux
    Dir.mktmpdir do |root|
      docker = File.join(root, 'docker')
      File.write(docker, <<~SH)
        #!/bin/sh
        case "$1" in
          context) printf '%s\\n' "$ZOOM_TEST_ENDPOINT" ;;
          info) printf '%s\\n' "$ZOOM_TEST_OS" ;;
          compose) printf '%s\\n' "$ZOOM_DOCKER_SOCKET" > "$ZOOM_TEST_COMMAND_LOG" ;;
          *) exit 1 ;;
        esac
      SH
      File.chmod(0o755, docker)
      log = File.join(root, 'socket')
      env = { 'DOCKER_HOST' => nil, 'PATH' => "#{root}:#{ENV.fetch('PATH')}", 'ZOOM_TEST_COMMAND_LOG' => log }
      [
        ['Docker Desktop', 'unix:///Users/example/.docker/run/docker.sock', '/var/run/docker.sock'],
        ['Ubuntu 22.04', 'unix:///run/user/1000/docker.sock', '/run/user/1000/docker.sock']
      ].each do |system, endpoint, expected|
        out, err, status = Open3.capture3(env.merge('ZOOM_TEST_OS' => system, 'ZOOM_TEST_ENDPOINT' => endpoint),
                                         'sh', File.expand_path('../bin/zoom-bot-docker', __dir__), 'check')
        assert status.success?, "#{out}#{err}"
        assert_equal expected, File.read(log).strip
      end
    end
  end
end
