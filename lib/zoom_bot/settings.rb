# frozen_string_literal: true

module ZoomBot
  class Settings
    KEYS = %w[
      ZOOM_SDK_CLIENT_ID ZOOM_SDK_CLIENT_SECRET
      ZOOM_S2S_ACCOUNT_ID ZOOM_S2S_CLIENT_ID ZOOM_S2S_CLIENT_SECRET
    ].freeze

    def initialize(env = ENV)
      missing = KEYS.select { |key| env[key].to_s.strip.empty? }
      raise Error, "Missing environment variables: #{missing.join(', ')}" unless missing.empty?

      @values = KEYS.to_h { |key| [key, env.fetch(key)] }
    end

    def fetch(key)
      @values.fetch(key)
    end

    def inspect
      '#<ZoomBot::Settings [REDACTED]>'
    end
  end
end
