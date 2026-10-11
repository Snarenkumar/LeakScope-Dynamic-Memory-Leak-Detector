FROM ubuntu:22.04

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install g++, make, and binutils (provides addr2line)
RUN apt-get update && \
    apt-get install -y --no-install-recommends g++ make binutils && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy project files
COPY . /app

# Build shared library and sample binaries
RUN make clean || true
RUN make

# Default command: run automated test suite, then demo symbolized output
CMD ["sh", "-c", "make test && echo '' && make demo"]
