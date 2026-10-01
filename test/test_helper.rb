# frozen_string_literal: true

require 'minitest/autorun'
require 'tmpdir'
require_relative '../lib/zoom_bot'

module TestFixtures
  def settings
    values = ZoomBot::Settings::KEYS.to_h { |key| [key, "fixture-#{key}"] }
    values['ZOOM_BOT_USER_EMAIL'] = 'recorder+bot@example.com'
    ZoomBot::Settings.new(values)
  end

  class FakeHttp
    attr_reader :requests

    def initialize(*responses)
      @responses = responses
      @requests = []
    end

    def request(uri, request)
      @requests << [uri, request]
      response = @responses.shift
      raise 'Unexpected request' if response.nil?
      raise response if response.is_a?(Exception)

      response
    end
  end
end
