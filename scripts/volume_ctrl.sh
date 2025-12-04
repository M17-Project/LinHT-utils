#!/bin/bash

b=5000 #log base
last_vol=-1

while true; do
    raw=$(< /sys/bus/iio/devices/iio:device0/in_voltage0_raw)

    #compute with awk instead of bc
    vol=$(awk -v v="$raw" -v b="$b" '
        BEGIN {
            out = 100 + log(1+(v/3000)*(b-1))/log(b) * (255-100) + 1
            printf "%d", out
        }')

    #only apply change if necessary
    if [ "$vol" -ne "$last_vol" ]; then
        amixer -q -c wm8960audio set Playback "$vol"
        last_vol=$vol
    fi

    sleep 0.2
done