FROM docker:28.5.1-cli AS docker-cli
FROM ruby:3.2.5-slim-bookworm
COPY --from=docker-cli /usr/local/bin/docker /usr/local/bin/docker
WORKDIR /app
COPY lib/ /app/lib/
COPY bin/zoom-bot /app/bin/zoom-bot
ENTRYPOINT ["ruby", "/app/bin/zoom-bot"]
