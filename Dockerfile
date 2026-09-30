FROM ubuntu:22.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libgtkmm-3.0-1v5 libx11-xcb1 libxcb-xfixes0 libxcb-shape0 \
    libxcb-shm0 libxcb-randr0 libxcb-image0 libxcb-keysyms1 libxcb-xtest0 \
    libdbus-1-3 libglib2.0-0 libgbm1 libxfixes3 libgl1 libdrm2 libgssapi-krb5-2 \
    libegl1 libsdl2-2.0-0 libcurl4 libasound2 libasound2-plugins libsndfile1 \
    dbus pulseaudio pulseaudio-utils \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

FROM runtime AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config python3 libglib2.0-dev libsndfile1-dev \
    && apt-get clean && rm -rf /var/lib/apt/lists/*
COPY worker/ /src/worker/
COPY scripts/ /src/scripts/

# Builds the real adapter against public API definitions, linked to test doubles only.
FROM build AS api-check
COPY vendor/zoom-api/h/ /opt/zoom-api/h/
RUN cmake -S /src/worker -B /build -DZOOM_API_HEADERS=/opt/zoom-api/h \
    && cmake --build /build -j 4 && ctest --test-dir /build --output-on-failure
CMD ["ctest", "--test-dir", "/build", "--output-on-failure"]

FROM build AS sdk-build
# Official SDK distribution: h/, libmeetingsdk.so, qt_libs/, and accompanying resources.
COPY vendor/zoom-sdk/ /opt/zoom-sdk/
RUN test -f /opt/zoom-sdk/libmeetingsdk.so \
    && ln -sf libmeetingsdk.so /opt/zoom-sdk/libmeetingsdk.so.1 \
    && cmake -S /src/worker -B /build -DZOOM_SDK_ROOT=/opt/zoom-sdk \
    && cmake --build /build -j 4 && ctest --test-dir /build --output-on-failure

FROM runtime AS worker
COPY --from=sdk-build /build/zoom-bot-worker /app/zoom-bot-worker
COPY --from=sdk-build /opt/zoom-sdk/ /opt/zoom-sdk/
COPY docker/entrypoint.sh /app/entrypoint.sh
RUN chmod 755 /app/entrypoint.sh
ENV LD_LIBRARY_PATH=/opt/zoom-sdk:/opt/zoom-sdk/qt_libs:/opt/zoom-sdk/qt_libs/Qt/lib
WORKDIR /opt/zoom-sdk
ENTRYPOINT ["/app/entrypoint.sh"]
