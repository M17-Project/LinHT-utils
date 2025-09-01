#!/bin/bash

v=`echo "$(</sys/bus/iio/devices/iio:device0/in_voltage1_raw)/4096 * 1.8 * (39+10)/10 * 1000" | bc -l`
v=$(printf "%.0f" "$v")
echo $v
