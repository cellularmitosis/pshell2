#!/bin/bash

set -e -o pipefail

if test -z "$PICO_SDK_PATH" ; then
    echo "Error: PICO_SDK_PATH not set." >&2
    echo "Download https://github.com/raspberrypi/pico-sdk" >&2
    echo "and export PICO_SDK_PATH from your shell rc file." >&2
    exit 1
fi

mkdir -p build
cd build
cmake ..
make
