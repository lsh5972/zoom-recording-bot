# frozen_string_literal: true

module ZoomBot
  class DeliveryError < Error
    attr_reader :retryable

    def initialize(message, retryable:)
      @retryable = retryable
      super(message)
    end
  end

  # Multipart transport only. Delivery progress belongs to ChunkDelivery.
  class ChunkHttpClient
    def initialize(endpoint:, bearer_token: nil)
      @uri = URI(endpoint.to_s)
      local_http = @uri.scheme == 'http' && %w[127.0.0.1 localhost ::1].include?(@uri.host)
      unless @uri.host && (@uri.scheme == 'https' || local_http) && !@uri.userinfo && !@uri.fragment
        raise Error, 'Chunk endpoint must use HTTPS, or HTTP on loopback for local testing'
      end
      @bearer_token = bearer_token
    rescue URI::InvalidURIError
      raise Error, 'Invalid chunk endpoint'
    end

    def post(event, wav_path)
      request = Net::HTTP::Post.new(@uri)
      request['Idempotency-Key'] = event.fetch('data').fetch('chunk_id')
      request['Authorization'] = "Bearer #{@bearer_token}" unless @bearer_token.to_s.empty?
      File.open(wav_path, 'rb') do |audio|
        request.set_form([
          ['metadata', JSON.generate(event), { content_type: 'application/json' }],
          ['audio', audio, { filename: File.basename(wav_path),
                             content_type: 'audio/wav' }]
        ], 'multipart/form-data')
        response = Net::HTTP.start(@uri.host, @uri.port, use_ssl: @uri.scheme == 'https',
                                  open_timeout: 5, read_timeout: 30, write_timeout: 30) do |http|
          http.request(request)
        end
        status = response.code.to_i
        return if status.between?(200, 299)

        raise DeliveryError.new("Chunk endpoint returned HTTP #{status}",
                                retryable: status == 408 || status == 429 || status >= 500)
      end
    rescue Timeout::Error, SocketError, SystemCallError, OpenSSL::SSL::SSLError, EOFError, IOError
      raise DeliveryError.new('Chunk delivery connection failed', retryable: true)
    end

    def inspect
      '#<ZoomBot::ChunkHttpClient [REDACTED]>'
    end
  end
end
