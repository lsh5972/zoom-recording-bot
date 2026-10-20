# frozen_string_literal: true

require_relative 'test_helper'
require 'rbconfig'

class CliTest < Minitest::Test
  def test_run_passes_host_network_option_to_worker
    Dir.mktmpdir('zoom bot run ') do |root|
      bootstrap = File.join(root, 'zoom_fixture.rb')
      File.write(bootstrap, <<~RUBY)
        require #{File.expand_path('../lib/zoom_bot', __dir__).dump}
        module ZoomBot
          class ZoomApiClient
            def meeting(id)
              { 'id' => id.to_i, 'host_id' => 'host', 'start_time' => '2026-10-01T10:00:00Z' }
            end
            def user_zak(_email)
              'fixture-zak'
            end
          end
        end
      RUBY
      docker = File.join(root, 'docker')
      File.write(docker, "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$ZOOM_TEST_COMMAND_LOG\"\n")
      File.chmod(0o755, docker)
      log = File.join(root, 'docker-args')
      runs = File.join(root, 'recordings')
      env = ZoomBot::Settings::KEYS.to_h { |key| [key, "fixture-#{key}"] }.merge(
        'ZOOM_BOT_USER_EMAIL' => 'bot@example.com', 'ZOOM_BOT_IMAGE' => 'zoom-bot-worker:test',
        'ZOOM_RUNS_DIR' => runs, 'ZOOM_TEST_COMMAND_LOG' => log, 'PATH' => "#{root}:#{ENV.fetch('PATH')}"
      )
      out, err, status = Open3.capture3(env, RbConfig.ruby, '-r', bootstrap,
                                      File.expand_path('../bin/zoom-bot', __dir__), 'run', '123456789', '--network=host')
      assert status.success?, "#{out}#{err}"
      assert_includes out, 'Container launched'
      args = File.readlines(log, chomp: true)
      assert_equal 'run', args.first
      assert_equal 1, args.count('--network=host')
      assert_operator args.index('--network=host'), :<, args.index('zoom-bot-worker:test')
      assert_includes args, '--rm'
      refute_includes args.join(' '), 'fixture-zak'
      assert File.directory?(File.join(runs, '123456789', 'output'))
    end
  end

  def test_run_rejects_unsupported_or_duplicate_network_options
    Dir.mktmpdir do |root|
      env = ZoomBot::Settings::KEYS.to_h { |key| [key, nil] }.merge('ZOOM_RUNS_DIR' => root)
      [['--network=bridge'], ['--network=host', '--network=host']].each do |options|
        _out, err, status = Open3.capture3(env, RbConfig.ruby, File.expand_path('../bin/zoom-bot', __dir__),
                                         'run', '123456789', *options)
        refute status.success?
        assert_includes err, 'Usage:'
        assert_empty Dir.children(root)
      end
    end
  end

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

  def test_docker_wrapper_forwards_worker_network_option
    Dir.mktmpdir do |root|
      docker = File.join(root, 'docker')
      File.write(docker, <<~SH)
        #!/bin/sh
        case "$1" in
          context) printf '%s\\n' 'unix:///var/run/docker.sock' ;;
          info) printf '%s\\n' 'Ubuntu 22.04' ;;
          compose) printf '%s\\n' "$@" > "$ZOOM_TEST_COMMAND_LOG" ;;
          *) exit 1 ;;
        esac
      SH
      File.chmod(0o755, docker)
      log = File.join(root, 'compose-args')
      env = { 'DOCKER_HOST' => nil, 'PATH' => "#{root}:#{ENV.fetch('PATH')}", 'ZOOM_TEST_COMMAND_LOG' => log }
      out, err, status = Open3.capture3(env, 'sh', File.expand_path('../bin/zoom-bot-docker', __dir__),
                                      'run', '123456789', '--network=host')
      assert status.success?, "#{out}#{err}"
      assert_equal ['controller', 'run', '123456789', '--network=host'], File.readlines(log, chomp: true).last(4)
    end
  end
end
