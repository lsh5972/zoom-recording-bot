# frozen_string_literal: true

require_relative 'test_helper'

class SessionTest < Minitest::Test
  include TestFixtures

  def preparer(root, http, screenshot_interval_seconds: 1)
    tokens = Struct.new(:token).new('access')
    ZoomBot::SessionPreparer.new(ZoomBot::ZoomApiClient.new(tokens, http: http),
                                ZoomBot::SdkSignature.new(settings), root: root,
                                bot_user_email: settings.fetch('ZOOM_BOT_USER_EMAIL'),
                                display_name: settings.fetch('ZOOM_BOT_DISPLAY_NAME'),
                                screenshot_interval_seconds: screenshot_interval_seconds)
  end

  def test_prepare_uses_bot_email_zak_and_keeps_actual_host_and_secrets_private
    http = FakeHttp.new({ 'id' => 123456789, 'host_id' => 'actual-host', 'start_time' => '2026-10-01T10:00:00.123Z', 'password' => 'meeting-secret' },
                        { 'token' => 'secret-zak' })
    Dir.mktmpdir do |root|
      result = preparer(root, http).prepare('123 456 789')
      path = File.join(result[:directory], 'join.json')
      join = JSON.parse(File.read(path))
      assert_equal result[:session_id], join['session_id']
      assert_equal File.join(root, '123456789'), result[:directory]
      assert_equal '123456789', result[:meeting_id]
      assert_equal 'actual-host', join['host_user_id']
      assert_equal 'recorder+bot@example.com', join['bot_user_email']
      assert_equal 'secret-zak', join['user_zak']
      assert_equal 'meeting-secret', join['passcode']
      assert_equal 1790848800123, join['meeting_start_unix_ms']
      assert_equal 1, join['screenshot_interval_seconds']
      assert_equal 0o600, File.stat(path).mode & 0o777
      assert_equal 0o700, File.stat(result[:directory]).mode & 0o777
      assert_equal '/v2/users/recorder%2Bbot%40example.com/token', http.requests.last.first.path
      refute_includes File.read(File.join(result[:directory], 'session.json')), 'secret'
      refute_includes File.read(path), settings.fetch('ZOOM_S2S_CLIENT_SECRET')
      refute_includes File.read(path), settings.fetch('ZOOM_SDK_CLIENT_SECRET')
      refute_includes result.inspect, 'secret-zak'
    end
  end

  def test_screenshot_interval_reaches_private_worker_configuration
    values = ZoomBot::Settings::KEYS.to_h { |key| [key, settings.fetch(key)] }
    values['ZOOM_SCREENSHOT_INTERVAL_SECONDS'] = '5'
    configured = ZoomBot::Settings.new(values)
    http = FakeHttp.new({ 'id' => 123456789, 'host_id' => 'host', 'start_time' => '2026-10-01T10:00:00Z' },
                        { 'token' => 'zak' })
    Dir.mktmpdir do |root|
      result = preparer(root, http, screenshot_interval_seconds: configured.fetch('ZOOM_SCREENSHOT_INTERVAL_SECONDS')).prepare('123456789')
      assert_equal 5, JSON.parse(File.read(File.join(result[:directory], 'join.json')))['screenshot_interval_seconds']
    end
  end

  def test_invalid_meeting_id_makes_no_request
    http = FakeHttp.new
    Dir.mktmpdir do |root|
      ['../123456789', '123', '123456789;echo secret', '123456789?x=y'].each do |id|
        assert_raises(ZoomBot::Error) { preparer(root, http).prepare(id) }
      end
      assert_empty http.requests
      assert_empty Dir.children(root)
    end
  end

  def test_missing_or_invalid_start_time_fails_before_zak_or_private_files
    [nil, 'not-a-time', 123, '2026-10-01T10:00:00'].each do |start_time|
      http = FakeHttp.new({ 'id' => 123456789, 'host_id' => 'host', 'start_time' => start_time })
      Dir.mktmpdir do |root|
        error = assert_raises(ZoomBot::Error) { preparer(root, http).prepare('123456789') }
        assert_includes error.message, 'start_time'
        assert_equal 1, http.requests.length
        assert_empty Dir.children(root)
      end
    end
  end

  def test_missing_or_mismatched_meeting_host_never_requests_zak
    [{ 'id' => 123456789 }, { 'id' => 987654321, 'host_id' => 'wrong-host' }].each do |meeting|
      http = FakeHttp.new(meeting)
      Dir.mktmpdir do |root|
        assert_raises(ZoomBot::Error) { preparer(root, http).prepare('123456789') }
        assert_equal 1, http.requests.length
        assert_empty Dir.children(root)
      end
    end
  end

  def test_repeated_meeting_id_does_not_overwrite_recording_or_request_another_zak
    http = FakeHttp.new({ 'id' => 123456789, 'host_id' => 'host', 'start_time' => '2026-10-01T10:00:00Z' }, { 'token' => 'zak-one' },
                        { 'id' => 987654321, 'host_id' => 'host', 'start_time' => '2026-10-01T10:00:00Z' }, { 'token' => 'zak-two' })
    Dir.mktmpdir do |root|
      service = preparer(root, http)
      one = service.prepare('123456789')
      File.binwrite(File.join(one[:directory], 'output', 'existing.wav'), 'existing PCM')
      assert_raises(ZoomBot::Error) { service.prepare('123456789') }
      assert_equal 2, http.requests.length
      assert_equal 'existing PCM', File.binread(File.join(one[:directory], 'output', 'existing.wav'))
      two = service.prepare('987654321')
      refute_equal one[:session_id], two[:session_id]
      assert_equal 'zak-one', JSON.parse(File.read(File.join(one[:directory], 'join.json')))['user_zak']
      assert_equal 'zak-two', JSON.parse(File.read(File.join(two[:directory], 'join.json')))['user_zak']
      assert_equal one.merge(directory: File.realpath(one[:directory])),
                   ZoomBot::SessionPreparer.locate('123 456 789', root: root)
      assert_equal two.merge(directory: File.realpath(two[:directory])),
                   ZoomBot::SessionPreparer.locate('987654321', root: root)
    end
  end

  def test_lookup_preserves_old_uuid_directories_and_rejects_mismatched_metadata
    Dir.mktmpdir do |root|
      uuid = '00000000-0000-4000-8000-000000000001'
      legacy = File.join(root, uuid)
      Dir.mkdir(legacy)
      File.write(File.join(legacy, 'session.json'), JSON.generate(session_id: uuid, meeting_id: '123456789'))
      assert_equal uuid, ZoomBot::SessionPreparer.locate(uuid, root: root)[:session_id]
      meeting = File.join(root, '123456789')
      Dir.mkdir(meeting)
      File.write(File.join(meeting, 'session.json'), JSON.generate(session_id: uuid, meeting_id: '987654321'))
      assert_raises(ZoomBot::Error) { ZoomBot::SessionPreparer.locate('123456789', root: root) }
      assert_raises(ZoomBot::Error) { ZoomBot::SessionPreparer.locate('../123456789', root: root) }
    end
  end

  def test_container_launch_exposes_only_file_paths_and_retains_output
    calls = []
    success = Struct.new(:success?).new(true)
    command = ->(*args) { calls << args; ['', '', success] }
    supervisor = ZoomBot::BotSupervisor.new(image: 'zoom-bot-worker:test', command: command)
    http = FakeHttp.new({ 'id' => 123456789, 'host_id' => 'host', 'start_time' => '2026-10-01T10:00:00Z' }, { 'token' => 'never-in-argv' })
    Dir.mktmpdir do |root|
      session = preparer(root, http).prepare('123456789')
      supervisor.check_image!
      name = supervisor.start(session)
      assert_equal "zoom-bot-#{session[:session_id]}", name
      args = calls.last
      assert_equal %w[docker run --detach --pull=never], args.first(4)
      assert args.any? { |argument| argument.end_with?('join.json,readonly') }
      refute_includes args.join(' '), 'never-in-argv'
      assert_includes args, '--rm'
      supervisor.stop(session[:session_id])
      assert File.directory?(File.join(session[:directory], 'output'))
    end
  end

  def test_failed_container_launch_preserves_session_without_echoing_docker_output
    failure = Struct.new(:success?).new(false)
    command = ->(*_args) { ['private-output', 'private-error', failure] }
    supervisor = ZoomBot::BotSupervisor.new(image: 'zoom-bot-worker:test', command: command)
    assert_raises(ZoomBot::Error) { supervisor.check_image! }
    http = FakeHttp.new({ 'id' => 123456789, 'host_id' => 'host', 'start_time' => '2026-10-01T10:00:00Z' }, { 'token' => 'secret-zak' })
    Dir.mktmpdir do |root|
      session = preparer(root, http).prepare('123456789')
      error = assert_raises(ZoomBot::Error) { supervisor.start(session) }
      refute_includes error.message, 'private-output'
      refute_includes error.message, 'private-error'
      assert File.exist?(File.join(session[:directory], 'join.json'))
    end
  end

  def test_stop_rejects_arbitrary_container_names_without_running_docker
    calls = []
    command = ->(*args) { calls << args }
    supervisor = ZoomBot::BotSupervisor.new(image: 'unused', command: command)
    assert_raises(ZoomBot::Error) { supervisor.stop('some-other-service') }
    assert_empty calls
  end
end
