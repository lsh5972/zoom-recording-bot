# frozen_string_literal: true

require_relative 'test_helper'

class HttpClientTest < Minitest::Test
  def request_with(status, body)
    response = Struct.new(:code, :body).new(status.to_s, body)
    connection = Object.new
    connection.define_singleton_method(:request) { |_request| response }
    transport = ->(*_args, **_options, &block) { block.call(connection) }
    uri = URI('https://api.zoom.us/v2/meetings/123456789')
    Net::HTTP.stub(:start, transport) do
      ZoomBot::HttpClient.new.request(uri, Net::HTTP::Get.new(uri))
    end
  end

  def test_decodes_successful_object
    assert_equal({ 'id' => 123456789 }, request_with(200, '{"id":123456789}'))
  end

  def test_api_error_retains_status_and_code_without_response_secrets
    error = assert_raises(ZoomBot::ApiError) do
      request_with(403, '{"code":200,"message":"private-token"}')
    end
    assert_equal 403, error.status
    assert_includes error.message, '200'
    refute_includes error.message, 'private-token'

    error = assert_raises(ZoomBot::ApiError) do
      request_with(401, '{"code":"private-token"}')
    end
    refute_includes error.message, 'private-token'
  end

  def test_invalid_or_non_object_responses_fail_without_echoing_body
    ['private-token', '["private-token"]'].each do |body|
      error = assert_raises(ZoomBot::Error) { request_with(200, body) }
      refute_includes error.message, 'private-token'
    end
    error = assert_raises(ZoomBot::ApiError) { request_with(502, '<html>private-token</html>') }
    assert_equal 502, error.status
    refute_includes error.message, 'private-token'
  end

  def test_transport_error_does_not_expose_request_details
    transport = ->(*_args, **_options) { raise IOError, 'private-token' }
    uri = URI('https://api.zoom.us/v2/meetings/123456789')
    Net::HTTP.stub(:start, transport) do
      error = assert_raises(ZoomBot::Error) do
        ZoomBot::HttpClient.new.request(uri, Net::HTTP::Get.new(uri))
      end
      assert_equal 'Zoom API connection failed', error.message
    end
  end
end
