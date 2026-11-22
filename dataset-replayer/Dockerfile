FROM archlinux:base AS dependencies
RUN pacman -Syu --noconfirm --needed bash mosquitto \
    && pacman -Scc --noconfirm
RUN bash -c '((BASH_VERSINFO[0] > 5 || (BASH_VERSINFO[0] == 5 && BASH_VERSINFO[1] >= 1)))'

FROM dependencies AS build
RUN pacman -S --noconfirm --needed gcc make pkgconf \
    && pacman -Scc --noconfirm
WORKDIR /src
COPY Makefile ./
COPY include/ include/
COPY src/ src/
RUN make all

FROM dependencies
RUN groupadd --gid 10001 replayer \
    && useradd --uid 10001 --gid 10001 --no-create-home --shell /usr/bin/nologin replayer
COPY --from=build /src/build/dataset-replayer /app/dataset-replayer/build/dataset-replayer
COPY scripts/run-ten-machines.sh /app/dataset-replayer/scripts/run-ten-machines.sh
USER 10001:10001
WORKDIR /app
STOPSIGNAL SIGTERM
ENTRYPOINT ["/usr/bin/bash", "/app/dataset-replayer/scripts/run-ten-machines.sh"]
