# frozen_string_literal: true

require_relative 'test_helper'

class AuthTest < Minitest::Test
  include TestFixtures

  def test_screenshot_interval_defaults_and_rejects_invalid_values
    values = ZoomBot::Settings::KEYS.to_h { |key| [key, settings.fetch(key)] }
    [nil, '', '  ', '1', ' 5 '].each do |value|
      configured = ZoomBot::Settings.new(values.merge('ZOOM_SCREENSHOT_INTERVAL_SECONDS' => value))
      assert_equal(value.to_s.strip == '5' ? 5 : 1, configured.fetch('ZOOM_SCREENSHOT_INTERVAL_SECONDS'))
    end
    ['0', '-1', '1.5', 'NaN', 'private-value', '2147483648'].each do |value|
      error = assert_raises(ZoomBot::Error) { ZoomBot::Settings.new(values.merge('ZOOM_SCREENSHOT_INTERVAL_SECONDS' => value)) }
      assert_equal 'ZOOM_SCREENSHOT_INTERVAL_SECONDS must be a positive integer', error.message
    end
  end

  def test_access_token_uses_s2s_credentials_and_refreshes_before_expiry
    time = 1000
    http = FakeHttp.new({ 'access_token' => 'one', 'expires_in' => 3600 },
                        { 'access_token' => 'two', 'expires_in' => 3600 })
    provider = ZoomBot::AccessTokenProvider.new(settings, http: http, clock: -> { time })
    assert_equal 'one', provider.token
    assert_equal 'one', provider.token
    assert_equal 1, http.requests.length
    uri, request = http.requests.first
    assert_equal 'https://zoom.us/oauth/token', uri.to_s
    form = URI.decode_www_form(request.body).to_h
    assert_equal 'account_credentials', form['grant_type']
    assert_equal settings.fetch('ZOOM_S2S_ACCOUNT_ID'), form['account_id']
    basic = Base64.decode64(request['Authorization'].split.last)
    assert_equal "#{settings.fetch('ZOOM_S2S_CLIENT_ID')}:#{settings.fetch('ZOOM_S2S_CLIENT_SECRET')}", basic
    time = 4570
    assert_equal 'two', provider.token
  end

  def test_native_signature_verifies_with_sdk_secret_and_not_s2s_secret
    jwt = ZoomBot::SdkSignature.new(settings, clock: -> { 10_000 }).issue
    header, payload, signature = jwt.split('.')
    assert_equal({ 'alg' => 'HS256', 'typ' => 'JWT' }, JSON.parse(Base64.urlsafe_decode64(header)))
    claims = JSON.parse(Base64.urlsafe_decode64(payload))
    assert_equal settings.fetch('ZOOM_SDK_CLIENT_ID'), claims['appKey']
    assert_equal 9970, claims['iat']
    assert_equal 3600, claims['exp'] - claims['iat']
    assert_equal claims['exp'], claims['tokenExp']
    decoded = Base64.urlsafe_decode64(signature)
    assert_equal OpenSSL::HMAC.digest('SHA256', settings.fetch('ZOOM_SDK_CLIENT_SECRET'), "#{header}.#{payload}"), decoded
    refute_equal OpenSSL::HMAC.digest('SHA256', settings.fetch('ZOOM_S2S_CLIENT_SECRET'), "#{header}.#{payload}"), decoded
    refute claims.key?('role'), 'A native JWT role claim must not be mistaken for actual host privileges'
  end

  def test_retry_401_refreshes_token_only_once
    oauth = FakeHttp.new({ 'access_token' => 'old', 'expires_in' => 3600 },
                         { 'access_token' => 'new', 'expires_in' => 3600 })
    provider = ZoomBot::AccessTokenProvider.new(settings, http: oauth)
    http = FakeHttp.new(ZoomBot::ApiError.new(401, 124), { 'id' => 123456789 })
    api = ZoomBot::ZoomApiClient.new(provider, http: http)
    assert_equal 123456789, api.meeting('123456789')['id']
    assert_equal %w[old new], http.requests.map { |_, request| request['Authorization'].split.last }

    failing = FakeHttp.new(ZoomBot::ApiError.new(401, 124), ZoomBot::ApiError.new(401, 124))
    provider = Struct.new(:token) { def invalidate; end }.new('bad')
    assert_raises(ZoomBot::ApiError) { ZoomBot::ZoomApiClient.new(provider, http: failing).meeting('123456789') }
    assert_equal 2, failing.requests.length
  end

  def test_zak_targets_host_id_with_correct_query
    provider = Struct.new(:token).new('access')
    http = FakeHttp.new({ 'token' => 'host-zak' })
    api = ZoomBot::ZoomApiClient.new(provider, http: http)
    assert_equal 'host-zak', api.user_zak('host/name+one')
    assert_equal 'https://api.zoom.us/v2/users/host%2Fname%2Bone/token?type=zak', http.requests.first.first.to_s
  end

  def test_zak_issuance_needs_only_s2s_credentials
    credentials = {
      'ZOOM_S2S_ACCOUNT_ID' => 'account', 'ZOOM_S2S_CLIENT_ID' => 'client',
      'ZOOM_S2S_CLIENT_SECRET' => 's2s-only-secret'
    }
    oauth = FakeHttp.new({ 'access_token' => 's2s-access', 'expires_in' => 3600 })
    http = FakeHttp.new({ 'id' => 123456789, 'host_id' => 'actual-host' }, { 'token' => 'host-zak' })
    tokens = ZoomBot::AccessTokenProvider.new(credentials, http: oauth)
    api = ZoomBot::ZoomApiClient.new(tokens, http: http)
    host = api.meeting('123456789').fetch('host_id')
    assert_equal 'host-zak', api.user_zak(host)
    assert_equal 1, oauth.requests.length
    assert_equal 2, http.requests.length
    assert_equal 'Bearer s2s-access', http.requests.last.last['Authorization']
  end

  def test_missing_credentials_report_names_without_values
    error = assert_raises(ZoomBot::Error) { ZoomBot::Settings.new({ 'ZOOM_SDK_CLIENT_ID' => 'private-value' }) }
    assert_includes error.message, 'ZOOM_S2S_ACCOUNT_ID'
    refute_includes error.message, 'private-value'
    refute_includes settings.inspect, 'fixture-'
  end

  def test_invalid_bot_email_reports_no_value
    values = ZoomBot::Settings::KEYS.to_h { |key| [key, settings.fetch(key)] }
    ['private-not-an-email', "bot@example.com\nextra", 'bot@@example.com'].each do |email|
      values['ZOOM_BOT_USER_EMAIL'] = email
      error = assert_raises(ZoomBot::Error) { ZoomBot::Settings.new(values) }
      assert_includes error.message, 'ZOOM_BOT_USER_EMAIL'
      refute_includes error.message, email
    end
  end

  def test_invalid_oauth_response_fails_closed
    http = FakeHttp.new({ 'access_token' => 'secret' })
    error = assert_raises(ZoomBot::Error) { ZoomBot::AccessTokenProvider.new(settings, http: http).token }
    refute_includes error.message, 'secret'
  end
end
