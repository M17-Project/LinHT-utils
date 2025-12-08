# SoapyLinHT

**SoapySDR Driver for LinHT Baseband (SX1255 over ZMQ)**

SoapyLinHT is a lightweight, high-performance **SoapySDR driver** that exposes
raw IQ baseband samples from the **LinHT handheld SDR (SX1255)** to any SDR
application through the standard [SoapySDR API](https://github.com/pothosware/SoapySDR).

The driver receives IQ samples from the LinHT internal **ZMQ stream**
(`ipc:///tmp/bsb_rx`), applies:

* **inverse-sinc equalization** (SX1255 compensation FIR)
* **DC offset removal**
* **float (CF32) normalization**
* optional conversion to **CS16** for tools like rtl_433

This allows LinHT to behave like a conventional local or network SDR receiver,
usable by:

* [OpenWebRX](https://www.openwebrx.de/)
* [GNU Radio](https://wiki.gnuradio.org/index.php/Soapy)
* [rtl_433](https://github.com/merbanan/rtl_433)
* any SDR tool supporting the SoapySDR API

It enables LinHT to behave like a conventional local or network-attached SDR
receiver.

## Features

* **CF32** (native float complex samples)
* **CS16** compatible path for rtl_433
* SX1255 **frequency tuning**
* SX1255 **gain control** (LNA, PGA, DAC, MIX)
* Automatic **DC offset removal**
* Integrated **inverse-sinc FIR equalizer**
* FIFO-based streaming for consistent MTU handling
* Works both locally and via **SoapyRemote**
* Designed specifically for the **LinHT SDR**
* **RX only** (TX will come later)

## Limitations

* RX only
* Sample rate fixed at **500 kSa/s**
* Only one hardware RX stream
* Bandwidth control not implemented (fixed by hardware)

## Repository Structure

```
soapy_linht/
 ├── CMakeLists.txt
 ├── main.cpp            # The driver implementation
 ├── fir.h / fir.cpp     # SX1255 inverse-sinc FIR filter
 └── README.md
```

## Build Instructions

### Prerequisites

You need:

* C++17 compiler
* CMake ≥ 3.10
* SoapySDR development headers
* ZeroMQ (libzmq, libczmq or similar)
* LinHT SX1255 control library (`libsx1255.so`)

Install dependencies:

```bash
sudo apt install cmake g++ libsoapysdr-dev libzmq3-dev
```

### Build

```bash
git clone https://github.com/M17-Project/LinHT-utils.git
cd LinHT-utils/soapy_linht
mkdir build && cd build
cmake ..
make -j
```

### Using Without System Installation

You can load the plugin directly from the build directory:

```bash
export SOAPY_SDR_PLUGIN_PATH=$PWD
SoapySDRUtil --find="driver=linht"
```

## Running with SoapyRemote

Start the Soapy server on the LinHT device:

```bash
export SOAPY_SDR_PLUGIN_PATH=/path/to/soapy_linht/build
SoapySDRServer --bind
```

Then on a remote machine:

```bash
SoapySDRUtil --find="driver=remote,remote=linht_device_ip:55132,remote:driver=linht"
```

## Using with OpenWebRX

Example OpenWebRX source configuration:

```
    "sdrs": {
        "fe5e859c-a717-437d-b2e2-311bf685a419": {
            "name": "LinHT",
            "type": "soapy_remote",
            "profiles": {
                "f81cc87e-6334-462b-a17e-e21cd154d04c": {
                    "name": "433MHz",
                    "center_freq": 432500000,
                    "samp_rate": 500000,
                    "start_freq": 432500000,
                    "start_mod": "m17"
                }
            },
            "device": "",
            "remote": "10.17.17.17:55132",
            "remote_driver": "linht"
        }
    },
```

## Using with rtl_433

Example command

```
$ rtl_433 \
-d "driver=remote,remote=10.17.17.17:55132,remote:driver=linht" \
-f 433850k \
-s 500000 \
-M stats -M level -M noise
```

## SX1255 Hardware Control

The driver supports:

* Frequency tuning
* Gain control

Gain is mapped to SX1255 HW blocks:

| Name    | Description                 | Range             |
| ------- | --------------------------- | ----------------- |
| **LNA** | RF preamp                   | 0–48 dB           |
| **PGA** | Programmable gain amplifier | 0–30 dB           |
| **DAC** | DAC attenuation             | 0, -3, -6, -9 dB  |
| **MIX** | Mixer gain                  | −37.5 to −7.5 dB  |

You can manually set gain:

```bash
SoapySDRUtil --set="LNA=30"
```

## How It Works Internally

The driver:

1. Subscribes to LinHT ZMQ baseband stream (`ipc:///tmp/bsb_rx`)
2. Processes each 1024-IQ-sample block through:
   * integer > float conversion
   * FIR equalizer (inverse-sinc for SX1255)
   * DC removal
   * FIFO buffering
3. Feeds exactly **numElems** samples to SoapySDR, matching SoapyRemote UDP MTU (≈178 complex samples)
4. Hardware control (frequency + gains) goes directly to SX1255 SPI/GPIO

## Contact

Part of the **M17 Project / LinHT** ecosystem
[https://github.com/M17-Project](https://github.com/M17-Project)
