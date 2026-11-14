#!/bin/sh
# LinHT volume control: map the panel knob onto the TLV320AIC3100 speaker path.
#
# HARDWARE
#   knob wiper -> imx93-adc in_voltage0, 12 bit, 0.4394 mV/LSB (0..1.8 V).
#   Measured travel 0..2955 counts. gui_test reads the same channel for its
#   on-screen volume indicator, so keep them consistent.
#
# CODEC
#   'Speaker Analog'  0..127, a uniform 0.5 dB/step fader:
#                     dB = (value - 127) * 0.5   -> 127 = 0 dB, 0 = -63.5 dB
#   'Speaker Driver'  0..3 = +6 / +12 / +18 / +24 dB class-D gain
#   Net output = Speaker Analog + Speaker Driver.
#
# LEVELS measured on hardware and confirmed by ear:
#   -13.0 dB (analog  89, driver 0) comfortable listening level, the default
#   +14.5 dB (analog 120, driver 2) loudest setting free of audible distortion
#   +18 dB and above distort, so driver 3 is never selected.
#
# WHY THE DRIVER IS KEPT LOW
#   The analog fader sits before the class-D driver, so driver gain amplifies
#   the output stage's own noise. Attenuating early and amplifying late is the
#   noisier arrangement, so for any requested level this picks the lowest
#   driver gain that can still reach it; the driver only steps up once the
#   analog fader has run out of range.
#
# WHY THE BOTTOM OF THE KNOB DOES NOT MUTE THE SPEAKER
#   Fully attenuating is deliberate: powering the output path down costs about
#   10 dB on the microphone noise floor, because DAPM then drops the codec's
#   bias (measured: ADC speech-band floor -56 dBFS with the output mixer
#   powered, -47 dBFS without). Switching the speaker off to save power would
#   quietly degrade transmit audio. Silence here means fully attenuated.

set -u

CARD=AUDIO
KNOB=/sys/bus/iio/devices/iio:device0/in_voltage0_raw

KNOB_MIN=20           # measured travel is 0..2955, trimmed at both ends so
KNOB_MAX=2900         # the extremes stay reachable despite pot/ADC tolerance
SILENT_FRAC=0.03      # bottom 3% of travel = fully attenuated
DB_MIN=-40.0          # quietest level in the fade
DB_MAX=14.5           # loudest distortion-free level
POLL=0.2

[ -r "$KNOB" ] || { echo "volume_ctrl: cannot read $KNOB" >&2; exit 1; }
amixer -c "$CARD" sget 'Speaker Analog' >/dev/null 2>&1 || {
    echo "volume_ctrl: no 'Speaker Analog' control on card '$CARD'" >&2
    exit 1
}

last_analog=-1
last_driver=-1

while :; do
    raw=$(cat "$KNOB" 2>/dev/null) || raw=''
    case "$raw" in ''|*[!0-9]*) sleep "$POLL"; continue ;; esac

    out=$(awk -v raw="$raw" -v kmin="$KNOB_MIN" -v kmax="$KNOB_MAX" \
              -v sf="$SILENT_FRAC" -v dmin="$DB_MIN" -v dmax="$DB_MAX" '
        BEGIN {
            p = (raw - kmin) / (kmax - kmin)
            if (p < 0) p = 0
            if (p > 1) p = 1

            if (p < sf) { print 0, 0; exit }          # driver 0, fully attenuated

            db = dmin + (p - sf) / (1 - sf) * (dmax - dmin)

            if      (db <=  6) drv = 0                # lowest class-D gain that
            else if (db <= 12) drv = 1                # can reach this level
            else               drv = 2
            drv_db = 6 + 6 * drv

            analog = int(127 + (db - drv_db) * 2 + 0.5)
            if (analog < 0)   analog = 0
            if (analog > 127) analog = 127
            print drv, analog
        }')
    [ -n "$out" ] || { sleep "$POLL"; continue; }
    driver=${out% *}
    analog=${out#* }
    case "$driver$analog" in ''|*[!0-9]*) sleep "$POLL"; continue ;; esac

    # Apply whichever change reduces gain first, so crossing a driver boundary
    # never produces a momentary jump in level.
    if [ "$driver" -lt "$last_driver" ]; then
        amixer -q -c "$CARD" sset 'Speaker Driver' "$driver" on
        last_driver=$driver
    fi
    if [ "$analog" -ne "$last_analog" ]; then
        amixer -q -c "$CARD" sset 'Speaker Analog' "$analog"
        last_analog=$analog
    fi
    if [ "$driver" -ne "$last_driver" ]; then
        amixer -q -c "$CARD" sset 'Speaker Driver' "$driver" on
        last_driver=$driver
    fi

    sleep "$POLL"
done
