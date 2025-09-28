#!/bin/bash

echo "scale=2; $(< /sys/class/thermal/thermal_zone0/temp) / 1000" | bc -l
