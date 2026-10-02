# frozen_string_literal: true

module ZoomBot
  class SessionPreparer
    def initialize(api, signatures, root:, bot_user_email:, clock: -> { Time.now })
      @api = api
      @signatures = signatures
      @root = File.expand_path(root)
      @bot_user_email = bot_user_email
      @clock = clock
    end

    def prepare(input)
      meeting_id = self.class.normalize_id(input)
      directory = File.join(@root, meeting_id)
      raise Error, 'Meeting directory already exists; existing recording files retained' if File.exist?(directory)
      meeting = @api.meeting(meeting_id)
      raise Error, 'Zoom returned a different meeting ID' unless meeting['id'].to_s == meeting_id
      host_id = meeting['host_id']
      raise Error, 'Meeting response lacks host_id' unless host_id.is_a?(String) && !host_id.empty?
      begin
        start_time = meeting.fetch('start_time')
        unless start_time.is_a?(String) && start_time.match?(/(?:Z|[+-]\d{2}:?\d{2})\z/)
          raise ArgumentError
        end
        meeting_start_unix_ms = (Time.iso8601(start_time).to_r * 1000).floor
        raise ArgumentError unless meeting_start_unix_ms.positive?
      rescue KeyError, ArgumentError
        raise Error, 'Meeting response lacks a valid start_time for the first recording'
      end

      zak = @api.user_zak(@bot_user_email)
      session_id = SecureRandom.uuid
      credentials = {
        schema_version: 1, session_id: session_id, meeting_id: meeting_id, host_user_id: host_id,
        bot_user_email: @bot_user_email, meeting_start_unix_ms: meeting_start_unix_ms,
        passcode: meeting.fetch('password', '').to_s, display_name: 'Meeting Recorder',
        sdk_jwt: @signatures.issue, user_zak: zak, prepared_at: @clock.call.utc.iso8601
      }

      FileUtils.mkdir_p(@root, mode: 0o700)
      begin
        Dir.mkdir(directory, 0o700)
      rescue Errno::EEXIST
        raise Error, 'Meeting directory already exists; existing recording files retained'
      end
      FileUtils.mkdir_p(File.join(directory, 'output'), mode: 0o700)
      write(File.join(directory, 'join.json'), credentials)
      metadata = { schema_version: 1, session_id: session_id, meeting_id: meeting_id,
                   host_user_id: host_id, bot_user_email: @bot_user_email, meeting_start_unix_ms: meeting_start_unix_ms,
                   state: 'prepared', prepared_at: credentials[:prepared_at] }
      write(File.join(directory, 'session.json'), metadata)
      { session_id: session_id, meeting_id: meeting_id, directory: directory }
    end

    def self.locate(input, root:)
      uuid = /\A[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}\z/
      identifier = input.to_s.match?(uuid) ? input.to_s : normalize_id(input)
      directory = File.realpath(File.join(root, identifier))
      metadata = JSON.parse(File.read(File.join(directory, 'session.json')))
      session_id = metadata.fetch('session_id')
      unless session_id.is_a?(String) && session_id.match?(uuid) &&
             (identifier == session_id || identifier == metadata.fetch('meeting_id'))
        raise Error, 'Session metadata does not match its meeting directory'
      end
      { session_id: session_id, meeting_id: metadata.fetch('meeting_id'), directory: directory }
    rescue JSON::ParserError, KeyError, TypeError
      raise Error, 'Invalid private session metadata'
    end

    def self.normalize_id(input)
      id = input.to_s.delete(' -')
      raise Error, 'Meeting ID must contain 9 to 11 digits' unless id.match?(/\A[1-9]\d{8,10}\z/)

      id
    end

    private

    def write(path, value)
      File.open(path, File::WRONLY | File::CREAT | File::EXCL, 0o600) do |file|
        file.write(JSON.generate(value))
      end
    end
  end
end
