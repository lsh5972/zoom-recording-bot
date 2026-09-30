# frozen_string_literal: true

module ZoomBot
  # HTTP and response decoding only. No Zoom authentication policy here.
  class HttpClient
    def request(uri, request)
      response = Net::HTTP.start(uri.host, uri.port, use_ssl: true,
                                open_timeout: 5, read_timeout: 15, write_timeout: 15) do |http|
        http.request(request)
      end
      status = response.code.to_i
      body = JSON.parse(response.body.to_s)
      raise ApiError.new(status, body.is_a?(Hash) ? body['code'] : nil) unless status.between?(200, 299)
      raise Error, 'Zoom API returned a non-object JSON response' unless body.is_a?(Hash)

      body
    rescue JSON::ParserError
      raise ApiError.new(status, nil) unless status.between?(200, 299)

      raise Error, 'Zoom API returned invalid JSON'
    rescue Timeout::Error, SocketError, SystemCallError, OpenSSL::SSL::SSLError, EOFError, IOError
      raise Error, 'Zoom API connection failed'
    end
  end
end
