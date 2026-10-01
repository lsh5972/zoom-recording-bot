# frozen_string_literal: true

module ZoomBot
  class Settings
    KEYS = %w[
      ZOOM_SDK_CLIENT_ID ZOOM_SDK_CLIENT_SECRET
      ZOOM_S2S_ACCOUNT_ID ZOOM_S2S_CLIENT_ID ZOOM_S2S_CLIENT_SECRET
      ZOOM_BOT_USER_EMAIL
    ].freeze

    def initialize(env = ENV)
      missing = KEYS.select { |key| env[key].to_s.strip.empty? }
      raise Error, "Missing environment variables: #{missing.join(', ')}" unless missing.empty?

      @values = KEYS.to_h { |key| [key, env.fetch(key)] }
      @values['ZOOM_BOT_USER_EMAIL'] = @values.fetch('ZOOM_BOT_USER_EMAIL').strip
      unless @values.fetch('ZOOM_BOT_USER_EMAIL').match?(/\A[^\s@]+@[^\s@]+\z/)
        raise Error, 'ZOOM_BOT_USER_EMAIL must be an email address'
      end
    end

    def fetch(key)
      @values.fetch(key)
    end

    def inspect
      '#<ZoomBot::Settings [REDACTED]>'
    end
  end
end
