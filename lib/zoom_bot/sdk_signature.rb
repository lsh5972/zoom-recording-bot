# frozen_string_literal: true

module ZoomBot
  class SdkSignature
    def initialize(settings, clock: -> { Time.now.to_i })
      @settings = settings
      @clock = clock
    end

    def issue
      issued_at = @clock.call - 30
      expires_at = issued_at + 3600
      header = encode(alg: 'HS256', typ: 'JWT')
      payload = encode(appKey: @settings.fetch('ZOOM_SDK_CLIENT_ID'), iat: issued_at,
                       exp: expires_at, tokenExp: expires_at)
      signing_input = "#{header}.#{payload}"
      signature = OpenSSL::HMAC.digest('SHA256', @settings.fetch('ZOOM_SDK_CLIENT_SECRET'), signing_input)
      "#{signing_input}.#{Base64.urlsafe_encode64(signature, padding: false)}"
    end

    private

    def encode(value)
      Base64.urlsafe_encode64(JSON.generate(value), padding: false)
    end
  end
end
