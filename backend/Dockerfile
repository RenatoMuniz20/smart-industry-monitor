FROM archlinux:base AS dependencies
RUN pacman -Syu --noconfirm --needed libmicrohttpd curl json-c ca-certificates \
    && pacman -Scc --noconfirm

FROM dependencies AS build
RUN pacman -S --noconfirm --needed gcc make \
    && pacman -Scc --noconfirm
WORKDIR /src
COPY Makefile ./
COPY include/ include/
COPY src/ src/
RUN make all

FROM dependencies
RUN groupadd --gid 10001 backend \
    && useradd --uid 10001 --gid 10001 --no-create-home --shell /usr/bin/nologin backend
COPY --from=build /src/build/backend /usr/local/bin/backend
USER 10001:10001
STOPSIGNAL SIGTERM
ENTRYPOINT ["/usr/local/bin/backend"]
