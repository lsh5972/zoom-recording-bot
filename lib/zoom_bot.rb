# frozen_string_literal: true

require 'json'
require 'net/http'
require 'uri'
require 'openssl'
require 'base64'
require 'fileutils'
require 'securerandom'
require 'time'
require 'open3'

module ZoomBot
  class Error < StandardError; end

  class ApiError < Error
    attr_reader :status

    def initialize(status, code)
      @status = status
      # Deliberately exclude response text: upstream responses can contain secrets.
      suffix = code.to_s.match?(/\A\d+\z/) ? " (Zoom code #{code})" : ''
      super("Zoom API returned HTTP #{status}#{suffix}")
    end
  end
end

require_relative 'zoom_bot/settings'
require_relative 'zoom_bot/http_client'
require_relative 'zoom_bot/access_token_provider'
require_relative 'zoom_bot/zoom_api_client'
require_relative 'zoom_bot/sdk_signature'
require_relative 'zoom_bot/session_preparer'
require_relative 'zoom_bot/bot_supervisor'
require_relative 'zoom_bot/chunk_http_client'
require_relative 'zoom_bot/chunk_delivery'
