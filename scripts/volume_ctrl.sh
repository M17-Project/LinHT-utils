#!/bin/bash

b=100 #log base

while true
do
    vol=$(< /sys/bus/iio/devices/iio:device0/in_voltage0_raw)
    vol=$(echo "100 + l(1+($vol/3000)*($b-1))/l($b) * (255-100) + 1" | bc -l)
    vol=$(printf "%.0f" "$vol")
    amixer -q -c wm8960audio set Playback $vol
    #echo $vol
    sleep 0.2
done
