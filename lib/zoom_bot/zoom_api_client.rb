# frozen_string_literal: true

module ZoomBot
  class ZoomApiClient
    def initialize(tokens, http: HttpClient.new)
      @tokens = tokens
      @http = http
    end

    def meeting(meeting_id)
      get("/meetings/#{meeting_id}")
    end

    def host_zak(host_id)
      encoded_id = URI.encode_www_form_component(host_id).gsub('+', '%20')
      body = get("/users/#{encoded_id}/token?type=zak")
      token = body['token']
      raise Error, 'Zoom user token response lacks ZAK' unless token.is_a?(String) && !token.empty?

      token
    end

    private

    def get(path)
      attempts = 0
      begin
        attempts += 1
        uri = URI("https://api.zoom.us/v2#{path}")
        request = Net::HTTP::Get.new(uri)
        request['Authorization'] = "Bearer #{@tokens.token}"
        @http.request(uri, request)
      rescue ApiError => error
        if error.status == 401 && attempts == 1
          @tokens.invalidate
          retry
        end
        raise
      end
    end
  end
end
