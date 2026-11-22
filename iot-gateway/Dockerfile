# Match the json-c 0.19 API/behavior validated by SIM-09 on Arch Linux.
FROM archlinux:base AS dependencies
RUN pacman -Syu --noconfirm --needed ca-certificates json-c mosquitto curl sqlite \
    && pacman -Scc --noconfirm

FROM dependencies AS build
RUN pacman -S --noconfirm --needed gcc make pkgconf \
    && pacman -Scc --noconfirm
WORKDIR /src
COPY Makefile ./
COPY include/ include/
COPY src/ src/
RUN make all

FROM dependencies
RUN groupadd --gid 10001 gateway \
    && useradd --uid 10001 --gid 10001 --no-create-home --shell /usr/bin/nologin gateway \
    && install -d -o gateway -g gateway -m 0750 /data
COPY --from=build /src/build/iot-gateway /usr/local/bin/iot-gateway
USER 10001:10001
WORKDIR /data
STOPSIGNAL SIGTERM
ENTRYPOINT ["/usr/local/bin/iot-gateway"]
