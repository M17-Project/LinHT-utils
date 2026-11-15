#!/bin/bash
# Print the battery voltage in mV.
# Rev C has no battery divider on the SoM ADC, so read VBAT from the
# BQ25792 charger (I2C bus 2, address 0x6b) through bq25792_status.py.
# Needs root for /dev/i2c-2.

dir=$(dirname "$(readlink -f "$0")")
python3 "$dir/bq25792_status.py" --json | python3 -c 'import json, sys
print(round(json.load(sys.stdin)["adc"]["vbat_V"] * 1000))'
