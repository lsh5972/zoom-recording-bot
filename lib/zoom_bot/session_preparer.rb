# frozen_string_literal: true

module ZoomBot
  class SessionPreparer
    def initialize(api, signatures, root:, clock: -> { Time.now })
      @api = api
      @signatures = signatures
      @root = File.expand_path(root)
      @clock = clock
    end

    def prepare(input)
      meeting_id = self.class.normalize_id(input)
      meeting = @api.meeting(meeting_id)
      raise Error, 'Zoom returned a different meeting ID' unless meeting['id'].to_s == meeting_id
      host_id = meeting['host_id']
      raise Error, 'Meeting response lacks host_id' unless host_id.is_a?(String) && !host_id.empty?

      # The host comes from Zoom, never from a caller-provided user ID.
      zak = @api.host_zak(host_id)
      session_id = SecureRandom.uuid
      credentials = {
        schema_version: 1, session_id: session_id, meeting_id: meeting_id, host_user_id: host_id,
        passcode: meeting.fetch('password', '').to_s, display_name: 'Meeting Recorder',
        sdk_jwt: @signatures.issue, user_zak: zak, prepared_at: @clock.call.utc.iso8601
      }

      directory = File.join(@root, session_id)
      FileUtils.mkdir_p(directory, mode: 0o700)
      FileUtils.mkdir_p(File.join(directory, 'output'), mode: 0o700)
      write(File.join(directory, 'join.json'), credentials)
      metadata = { schema_version: 1, session_id: session_id, meeting_id: meeting_id,
                   host_user_id: host_id, state: 'prepared', prepared_at: credentials[:prepared_at] }
      write(File.join(directory, 'session.json'), metadata)
      { session_id: session_id, directory: directory }
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
