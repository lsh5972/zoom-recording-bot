# frozen_string_literal: true

require_relative 'test_helper'
require 'socket'

class ChunkHttpClientTest < Minitest::Test
  def with_receiver(status)
    server = TCPServer.new('127.0.0.1', 0)
    received = Queue.new
    thread = Thread.new do
      connection = server.accept
      begin
        request_line = connection.gets
        headers = {}
        while (line = connection.gets) && line != "\r\n"
          key, value = line.split(':', 2)
          headers[key.downcase] = value.strip
        end
        body = connection.read(Integer(headers.fetch('content-length')))
        received << [request_line, headers, body]
        response_body = 'private-response'
        connection.write("HTTP/1.1 #{status} Result\r\nContent-Length: #{response_body.bytesize}\r\nConnection: close\r\n\r\n#{response_body}")
      ensure
        connection.close
      end
    end
    yield "http://127.0.0.1:#{server.addr[1]}/chunks", received
    raise 'Test receiver did not complete' unless thread.join(2)
    thread.value
  ensure
    server&.close
    thread&.kill if thread&.alive?
  end

  def event
    { 'type' => 'audio.chunk_ready', 'session_id' => 'session',
      'data' => { 'chunk_id' => 'session-user-42-join-1-chunk-3', 'participant_session_id' => 'user-42-join-1' } }
  end

  def with_audio
    Dir.mktmpdir do |root|
      path = File.join(root, 'user-42-join-1__00:01:12-00:01:38__chunk-3.wav')
      bytes = "RIFF\x00\x01\xffWAVE".b
      File.binwrite(path, bytes)
      yield path, bytes
    end
  end

  def test_multipart_preserves_speaker_timestamp_filename_metadata_and_binary_audio
    with_receiver(204) do |endpoint, received|
      with_audio do |path, bytes|
        client = ZoomBot::ChunkHttpClient.new(endpoint: endpoint, bearer_token: 'fixture-bearer')
        client.post(event, path)
        request, headers, body = received.pop
        assert_equal "POST /chunks HTTP/1.1\r\n", request
        assert_equal event['data']['chunk_id'], headers['idempotency-key']
        assert_equal 'Bearer fixture-bearer', headers['authorization']
        boundary = headers['content-type'].split('boundary=', 2).last.delete('"')
        parts = body.split("--#{boundary}")
        metadata = parts.find { |part| part.include?('name="metadata"') }
        assert_equal event, JSON.parse(metadata.split("\r\n\r\n", 2).last.delete_suffix("\r\n"))
        audio = parts.find { |part| part.include?('name="audio"') }
        assert_includes audio, "filename=\"#{File.basename(path)}\""
        assert_includes audio, 'Content-Type: audio/wav'
        assert_equal bytes, audio.split("\r\n\r\n", 2).last.delete_suffix("\r\n").b
      end
    end
  end

  def test_http_failure_classifies_retry_without_exposing_response_or_credentials
    [400, 429, 503].each do |status|
      with_receiver(status) do |endpoint, _received|
        with_audio do |path, _bytes|
          client = ZoomBot::ChunkHttpClient.new(endpoint: endpoint, bearer_token: 'fixture-bearer')
          error = assert_raises(ZoomBot::DeliveryError) { client.post(event, path) }
          assert_equal status != 400, error.retryable
          assert_includes error.message, status.to_s
          refute_includes error.message, 'private-response'
          refute_includes error.message, 'fixture-bearer'
          refute_includes client.inspect, endpoint
        end
      end
    end
  end

  def test_remote_plain_http_and_embedded_credentials_are_rejected_without_network
    ['http://example.com/chunks', 'https://user:private@receiver.example/chunks', 'not a URL'].each do |endpoint|
      error = assert_raises(ZoomBot::Error) { ZoomBot::ChunkHttpClient.new(endpoint: endpoint) }
      refute_includes error.message, 'private'
    end
  end
end
