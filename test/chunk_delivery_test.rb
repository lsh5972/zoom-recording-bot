# frozen_string_literal: true

require_relative 'test_helper'

class ChunkDeliveryTest < Minitest::Test
  class Receiver
    attr_reader :requests
    attr_accessor :failure

    def initialize
      @requests = []
    end

    def post(event, wav_path)
      @requests << [event, File.binread(wav_path)]
      raise @failure if @failure
    end
  end

  def chunk(id)
    { 'type' => 'audio.chunk_ready', 'session_id' => 'session',
      'data' => { 'chunk_id' => id, 'participant_session_id' => id, 'sequence' => 1,
                  'start_ms' => 1000, 'end_ms' => 2000,
                  'wav_path' => "#{id}__00:00:01-00:00:02__chunk-1.wav" } }
  end

  def write_chunk(output, id)
    event = chunk(id)
    path = File.join(output, event['data']['wav_path'])
    FileUtils.mkdir_p(File.dirname(path))
    File.binwrite(path, "RIFF-fixture-#{id}")
    event
  end

  def journal(output, events)
    File.write(File.join(output, 'events.jsonl'), events.map { |event| JSON.generate(event) + "\n" }.join)
  end

  def test_each_chunk_is_delivered_once_after_success_and_restart
    Dir.mktmpdir do |output|
      journal(output, [{ type: 'speech_on' }, write_chunk(output, 'one'), write_chunk(output, 'two')])
      receiver = Receiver.new
      sender = ZoomBot::ChunkDelivery.new(output: output, client: receiver)
      assert_equal 2, sender.drain
      assert_equal 0, ZoomBot::ChunkDelivery.new(output: output, client: receiver).drain
      assert_equal %w[one two], receiver.requests.map { |event, _| event['data']['chunk_id'] }
      assert_equal 0o600, File.stat(File.join(output, 'delivery.json')).mode & 0o777
    end
  end

  def test_failed_delivery_retries_same_id_without_advancing_cursor
    Dir.mktmpdir do |output|
      journal(output, [write_chunk(output, 'one'), write_chunk(output, 'two')])
      receiver = Receiver.new
      receiver.failure = ZoomBot::DeliveryError.new('HTTP 503', retryable: true)
      sender = ZoomBot::ChunkDelivery.new(output: output, client: receiver)
      assert_raises(ZoomBot::DeliveryError) { sender.drain }
      refute File.exist?(File.join(output, 'delivery.json'))
      receiver.failure = nil
      assert_equal 2, ZoomBot::ChunkDelivery.new(output: output, client: receiver).drain
      assert_equal %w[one one two], receiver.requests.map { |event, _| event['data']['chunk_id'] }
    end
  end

  def test_incomplete_last_line_is_ignored_until_completed
    Dir.mktmpdir do |output|
      event = JSON.generate(write_chunk(output, 'one'))
      File.write(File.join(output, 'events.jsonl'), event[0...-2])
      receiver = Receiver.new
      sender = ZoomBot::ChunkDelivery.new(output: output, client: receiver)
      assert_equal 0, sender.drain
      assert_empty receiver.requests
      File.open(File.join(output, 'events.jsonl'), 'a') { |file| file.write(event[-2..] + "\n") }
      assert_equal 1, sender.drain
    end
  end

  def test_missing_file_does_not_skip_chunk
    Dir.mktmpdir do |output|
      journal(output, [chunk('missing')])
      receiver = Receiver.new
      sender = ZoomBot::ChunkDelivery.new(output: output, client: receiver)
      assert_raises(ZoomBot::Error) { sender.drain }
      assert_empty receiver.requests
      journal(output, [write_chunk(output, 'missing')])
      assert_equal 1, sender.drain
    end
  end

  def test_paths_cannot_escape_session_output
    Dir.mktmpdir do |output|
      event = chunk('one')
      event['data']['wav_path'] = '../join.json'
      journal(output, [event])
      receiver = Receiver.new
      sender = ZoomBot::ChunkDelivery.new(output: output, client: receiver)
      assert_raises(ZoomBot::Error) { sender.drain }
      assert_empty receiver.requests
      Dir.mktmpdir do |outside|
        filename = chunk('one')['data']['wav_path']
        File.write(File.join(outside, filename), 'private-data')
        File.symlink(File.join(outside, filename), File.join(output, filename))
        journal(output, [chunk('one')])
        assert_raises(ZoomBot::Error) { sender.drain }
        assert_empty receiver.requests
      end
    end
  end

  def test_concurrent_sender_is_rejected
    Dir.mktmpdir do |output|
      File.open(File.join(output, 'delivery.lock'), 'w') do |lock|
        lock.flock(File::LOCK_EX)
        sender = ZoomBot::ChunkDelivery.new(output: output, client: Receiver.new)
        assert_raises(ZoomBot::Error) { sender.drain }
      end
    end
  end
end
