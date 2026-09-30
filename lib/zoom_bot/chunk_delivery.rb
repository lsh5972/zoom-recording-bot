# frozen_string_literal: true

module ZoomBot
  # Reads only complete journal lines. A failed POST leaves the cursor in place.
  class ChunkDelivery
    def initialize(output:, client:)
      @output = File.realpath(output)
      @client = client
      @state_path = File.join(@output, 'delivery.json')
    end

    def drain
      File.open(File.join(@output, 'delivery.lock'), File::RDWR | File::CREAT, 0o600) do |lock|
        raise Error, 'Another chunk sender is running for this session' unless lock.flock(File::LOCK_EX | File::LOCK_NB)

        return 0 unless File.exist?(File.join(@output, 'events.jsonl'))

        delivered = 0
        File.open(File.join(@output, 'events.jsonl'), 'rb') do |journal|
          offset = load_offset
          raise Error, 'Event journal is shorter than saved delivery progress' if offset > journal.size

          journal.seek(offset)
          while (line = journal.gets)
            break unless line.end_with?("\n")

            event = JSON.parse(line)
            if event.fetch('type') == 'audio.chunk_ready'
              @client.post(event, wav_path(event.fetch('data')))
              delivered += 1
            end
            save_offset(journal.pos)
          end
        end
        delivered
      end
    rescue JSON::ParserError, KeyError, TypeError
      raise Error, 'Invalid event journal or delivery progress'
    end

    private

    def wav_path(metadata)
      id = metadata.fetch('chunk_id')
      participant = metadata.fetch('participant_session_id')
      sequence = metadata.fetch('sequence')
      start_ms = metadata.fetch('start_ms')
      end_ms = metadata.fetch('end_ms')
      unless [id, participant].all? { |value| value.is_a?(String) && value.match?(/\A[a-zA-Z0-9_-]+\z/) } &&
             sequence.is_a?(Integer) && sequence.positive? && start_ms.is_a?(Integer) && start_ms >= 0 &&
             end_ms.is_a?(Integer) && end_ms >= start_ms
        raise Error, 'Invalid finalized WAV path'
      end
      relative = "#{participant}__#{clock_label(start_ms)}-#{clock_label(end_ms)}__chunk-#{sequence}.wav"
      raise Error, 'Invalid finalized WAV path' unless metadata.fetch('wav_path') == relative

      path = File.realpath(File.join(@output, metadata.fetch('wav_path')))
      unless path == File.join(@output, relative)
        raise Error, 'Finalized WAV must remain inside the meeting output directory'
      end

      path
    rescue Errno::ENOENT
      raise Error, 'Finalized WAV is missing; delivery progress retained'
    end

    def clock_label(milliseconds)
      seconds = milliseconds / 1000
      format('%02d:%02d:%02d', seconds / 3600, seconds / 60 % 60, seconds % 60)
    end

    def load_offset
      return 0 unless File.exist?(@state_path)

      offset = JSON.parse(File.read(@state_path)).fetch('offset')
      raise Error, 'Invalid delivery offset' unless offset.is_a?(Integer) && offset >= 0

      offset
    end

    def save_offset(offset)
      temporary = "#{@state_path}.tmp"
      File.open(temporary, File::WRONLY | File::CREAT | File::TRUNC, 0o600) do |file|
        file.write(JSON.generate(offset: offset))
        file.flush
        file.fsync
      end
      File.rename(temporary, @state_path)
      File.open(@output, 'r') { |directory| directory.fsync }
    end
  end
end
