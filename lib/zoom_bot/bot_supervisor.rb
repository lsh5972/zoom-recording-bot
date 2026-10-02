# frozen_string_literal: true

module ZoomBot
  # Starts an already-built native worker image. This does not implement Zoom capture.
  class BotSupervisor
    def initialize(image:, command: ->(*args) { Open3.capture3(*args) })
      raise Error, 'Set ZOOM_BOT_IMAGE after building the native SDK worker' if image.to_s.empty?
      raise Error, 'Invalid worker image name' unless image.match?(/\A[a-zA-Z0-9][a-zA-Z0-9._\/:@-]*\z/)

      @image = image
      @command = command
    end

    def check_image!
      _out, _err, status = @command.call('docker', 'image', 'inspect', @image)
      raise Error, 'Native worker image unavailable; SDK worker must be built first' unless status.success?
    end

    def start(session)
      directory = File.realpath(session.fetch(:directory))
      name = "zoom-bot-#{session.fetch(:session_id)}"
      # User tokens travel in a private read-only file, never argv or Docker env.
      _out, _err, status = @command.call(
        'docker', 'run', '--detach', '--pull=never', '--rm', '--name', name,
        '--label', "zoom-bot.session=#{session.fetch(:session_id)}",
        '--mount', "type=bind,src=#{directory}/join.json,dst=/run/zoom-bot/join.json,readonly",
        '--mount', "type=bind,src=#{directory}/output,dst=/data",
        @image, '--config', '/run/zoom-bot/join.json', '--output', '/data'
      )
      raise Error, 'Docker failed to launch the worker; private session files retained' unless status.success?

      name
    end

    def stop(session_id)
      raise Error, 'Invalid session ID' unless session_id.match?(/\A[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}\z/)

      _out, _err, status = @command.call('docker', 'stop', '--time', '30', "zoom-bot-#{session_id}")
      raise Error, 'Docker failed to stop the worker' unless status.success?
    end
  end
end
