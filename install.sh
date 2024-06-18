#!/bin/bash

set -e -o pipefail

if test "$(uname -s)" != "Darwin" ; then
    echo "Error: install.sh is only tested on macOS." >&2
    exit 1
fi

if ! test -e /Volumes/RPI-RP2 ; then
    echo "Note: /Volumes/RPI-RP2 not found." >&2
    echo "Hold the BOOTSEL button and reset your pico." >&2
    sleep 1
    while test ! -e /Volumes/RPI-RP2 ; do
        echo "Waiting for /Volumes/RPI-RP2..." >&2
        sleep 1
    done
fi

if test -d /Volumes/RPI-RP2/pshell_usb.uf2 ; then
    rm -rf /Volumes/RPI-RP2/pshell_usb.uf2
fi
cp -v build/pshell_usb.uf2 /Volumes/RPI-RP2/
