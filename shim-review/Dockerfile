FROM ubuntu:24.04

# Install build dependencies for rhboot/shim
RUN apt-get update && apt-get install -y --no-install-recommends \
    git \
    make \
    gcc \
    binutils \
    gnu-efi \
    libelf-dev \
    libssl-dev \
    pesign \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Copy vendor certificate into the image at build time
COPY MOK.cer /build/MOK.cer

WORKDIR /build

# Clone rhboot/shim at the exact version used for submission (v16.1 = current main)
RUN git clone --depth=1 https://github.com/rhboot/shim.git shim-src \
    && cd shim-src \
    && git submodule update --init gnu-efi

# Build shimx64.efi and mmx64.efi with our vendor certificate
WORKDIR /build/shim-src
RUN make VENDOR_CERT_FILE=/build/MOK.cer ARCH=x86_64 shimx64.efi mmx64.efi 2>&1 | tee /build/build.log

# Copy outputs to /output
RUN mkdir -p /output \
    && cp shimx64.efi mmx64.efi /output/ \
    && sha256sum /output/shimx64.efi /output/mmx64.efi | tee /output/SHA256SUMS \
    && echo "--- Build complete ---" >> /build/build.log \
    && cp /build/build.log /output/build.log

# To reproduce the build:
#   docker build -t impossible-os-shim .
#   docker run --rm -v $(pwd)/output:/output impossible-os-shim cp -r /output/. /output/
#
# Then verify:
#   sha256sum output/shimx64.efi
#   # Expected: d7e21770b1c8f2b977db1d533f7bba3d0de3d212e83ffd35c2509de970d6bd2f
