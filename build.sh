docker run --rm \
      -v "$(pwd)":/sm64 \
      -w /sm64 \
      --user $(id -u):$(id -g) \
      sm64 \
      make VERSION=us -j4 COMPARE=0 COMPILER=gcc CROSS=mips-linux-gnu-gcc
