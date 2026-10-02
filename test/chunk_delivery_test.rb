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

  class CheckpointDelivery < ZoomBot::ChunkDelivery
    attr_reader :checkpoints

    def initialize(**options)
      super
      @checkpoints = []
    end

    private

    def save_offset(offset)
      super
      @checkpoints << offset
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

  def test_raw_events_are_checkpointed_as_a_batch_without_skipping_partial_lines
    Dir.mktmpdir do |output|
      events = [{ type: 'sdk.raw_audio.one_way' }] * 100
      events << write_chunk(output, 'one')
      events.concat([{ type: 'sdk.raw_share.frame' }] * 100)
      journal(output, events)
      complete_size = File.size(File.join(output, 'events.jsonl'))
      partial = JSON.generate(write_chunk(output, 'two'))
      File.open(File.join(output, 'events.jsonl'), 'a') { |file| file.write(partial) }
      receiver = Receiver.new
      sender = CheckpointDelivery.new(output: output, client: receiver)

      assert_equal 1, sender.drain
      assert_equal 2, sender.checkpoints.size
      assert_equal complete_size, JSON.parse(File.read(File.join(output, 'delivery.json'))).fetch('offset')
      assert_equal 0, sender.drain
      assert_equal 2, sender.checkpoints.size

      File.open(File.join(output, 'events.jsonl'), 'a') { |file| file.write("\n") }
      assert_equal 1, ZoomBot::ChunkDelivery.new(output: output, client: receiver).drain
      assert_equal %w[one two], receiver.requests.map { |event, _| event['data']['chunk_id'] }
    end
  end

  def test_successful_chunk_is_checkpointed_before_a_later_delivery_fails
    Dir.mktmpdir do |output|
      first = write_chunk(output, 'one')
      journal(output, [first, { type: 'speech_on' }, write_chunk(output, 'two')])
      receiver = Receiver.new
      post = receiver.method(:post)
      receiver.define_singleton_method(:post) do |event, path|
        self.failure = ZoomBot::DeliveryError.new('HTTP 503', retryable: true) if event['data']['chunk_id'] == 'two'
        post.call(event, path)
      end
      sender = ZoomBot::ChunkDelivery.new(output: output, client: receiver)
      assert_raises(ZoomBot::DeliveryError) { sender.drain }
      assert_equal (JSON.generate(first) + "\n").bytesize,
                   JSON.parse(File.read(File.join(output, 'delivery.json'))).fetch('offset')

      restarted = Receiver.new
      assert_equal 1, ZoomBot::ChunkDelivery.new(output: output, client: restarted).drain
      assert_equal ['two'], restarted.requests.map { |event, _| event['data']['chunk_id'] }
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

  def test_recording_filename_with_korean_speaker_is_delivered_with_metadata
    Dir.mktmpdir do |output|
      event = chunk('one')
      event['data'].merge!('recording_id' => 'recording-2', 'speaker_label' => '홍길동',
                          'display_name' => '홍길동', 'file_sequence' => 3,
                          'wav_path' => 'recording-2__홍길동__00:00:01-00:00:02__chunk-3.wav')
      File.binwrite(File.join(output, event['data']['wav_path']), 'RIFF-cloud-file')
      journal(output, [event])
      receiver = Receiver.new
      assert_equal 1, ZoomBot::ChunkDelivery.new(output: output, client: receiver).drain
      assert_equal 'recording-2', receiver.requests[0][0]['data']['recording_id']
      assert_equal '홍길동', receiver.requests[0][0]['data']['display_name']
      assert_equal 'RIFF-cloud-file', receiver.requests[0][1]
    end
  end

  def test_recording_speaker_paths_and_file_sequences_are_validated
    ['../private', "a\nb", 'a:b', '', 'a' * 81].each do |label|
      Dir.mktmpdir do |output|
        event = chunk('one')
        event['data'].merge!('recording_id' => 'recording-2', 'speaker_label' => label, 'file_sequence' => 3)
        journal(output, [event])
        receiver = Receiver.new
        assert_raises(ZoomBot::Error) { ZoomBot::ChunkDelivery.new(output: output, client: receiver).drain }
        assert_empty receiver.requests
      end
    end
  end

  def test_unrecorded_gmt9_midnight_filename_keeps_absolute_metadata
    Dir.mktmpdir do |output|
      event = chunk('one')
      event['data'].merge!('recording_id' => nil, 'timestamp_origin' => 'wall_clock_gmt9',
                          'speaker_label' => '홍길동', 'file_sequence' => 1,
                          'start_ms' => 53998000, 'end_ms' => 54003000,
                          'wav_path' => 'unrecorded__홍길동__23:59:58-00:00:03__chunk-1.wav')
      File.binwrite(File.join(output, event['data']['wav_path']), 'RIFF-midnight')
      journal(output, [event])
      receiver = Receiver.new
      assert_equal 1, ZoomBot::ChunkDelivery.new(output: output, client: receiver).drain
      assert_equal 53998000, receiver.requests[0][0]['data']['start_ms']
      assert_equal 54003000, receiver.requests[0][0]['data']['end_ms']
      assert_nil receiver.requests[0][0]['data']['recording_id']
    end
  end
end
