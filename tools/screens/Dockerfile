# The screens harness builds and runs the studio's drawing code on Linux, never on the host.
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends g++ python3 && rm -rf /var/lib/apt/lists/*
WORKDIR /work
