# The pinned CI image: the whole CI toolchain, installed once.
#
# Published by .github/workflows/ci-image.yml to ghcr.io/rizonesoft/impossible-os-ci
# when this file or the toolchain changes. The CI jobs run in it by digest, so a
# digest change in the workflows is the only way the toolchain moves. Build it
# from the repository root:
#
#     docker build -f scripts/ci/image/Dockerfile -t impossible-os-ci .
FROM ubuntu:26.04@sha256:065aa8626841f16057b78b027b464cd59c4fac36c204c72dcbf48d66f5fa8a96

LABEL org.opencontainers.image.source="https://github.com/rizonesoft/impossible-os" \
      org.opencontainers.image.description="Impossible OS CI toolchain" \
      org.opencontainers.image.licenses="GPL-3.0-only"

COPY scripts/ci/toolchain.sh scripts/ci/toolchain-packages.txt /tmp/ci/
RUN bash /tmp/ci/toolchain.sh && rm -rf /tmp/ci

# The suites run unprivileged, as they do on a developer host: several assert that
# a permission is refused, which root never sees. uid 1001 is the hosted runner's
# own user, so the job's workspace mount is writable.
RUN useradd -m -u 1001 -s /bin/bash runner
