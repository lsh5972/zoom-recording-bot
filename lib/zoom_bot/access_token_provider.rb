# frozen_string_literal: true

module ZoomBot
  class AccessTokenProvider
    def initialize(settings, http: HttpClient.new, clock: -> { Time.now.to_i })
      @settings = settings
      @http = http
      @clock = clock
      @expires_at = 0
    end

    def token
      return @token if @token && @clock.call < @expires_at - 30

      uri = URI('https://zoom.us/oauth/token')
      request = Net::HTTP::Post.new(uri)
      request.basic_auth(@settings.fetch('ZOOM_S2S_CLIENT_ID'), @settings.fetch('ZOOM_S2S_CLIENT_SECRET'))
      request.set_form_data(grant_type: 'account_credentials', account_id: @settings.fetch('ZOOM_S2S_ACCOUNT_ID'))
      issued_at = @clock.call
      body = @http.request(uri, request)
      @token = body['access_token']
      lifetime = body['expires_in']
      unless @token.is_a?(String) && !@token.empty? && lifetime.is_a?(Numeric) && lifetime.positive?
        invalidate
        raise Error, 'Zoom OAuth response lacks a valid access token or expiry'
      end
      @expires_at = issued_at + lifetime
      @token
    end

    def invalidate
      @token = nil
      @expires_at = 0
    end

    def inspect
      '#<ZoomBot::AccessTokenProvider [REDACTED]>'
    end
  end
end
