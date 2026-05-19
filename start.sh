#!/bin/bash
while ! xdpyinfo > /dev/null 2>&1; do
    sleep 1
done
sleep 2
exec "$(dirname "$0")/build/recorder" --gui
