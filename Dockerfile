FROM ubuntu:24.04 as build

RUN apt-get update && \
    apt-get install -y \
	gcc-mips-linux-gnu \
        binutils-mips-linux-gnu \
        bsdextrautils \
        build-essential \
        git \
        pkgconf \
        python3

RUN mkdir /sm64
WORKDIR /sm64
ENV PATH="/sm64/tools:${PATH}"

CMD echo 'Usage: docker run --rm -v ${PWD}:/sm64 sm64 make VERSION=us -j4\n' \
         'See https://github.com/n64decomp/sm64/blob/master/README.md for more information'
