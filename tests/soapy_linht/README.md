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

and provides them to applications such as:

* [OpenWebRX](https://www.openwebrx.de/)
* [GNU Radio](https://wiki.gnuradio.org/index.php/Soapy)
* [rtl_433](https://github.com/merbanan/rtl_433)
* any SDR tool that supports SoapySDR

It enables LinHT to behave like a conventional local or network-attached SDR
receiver.

## Features

* **CF32 output** (native float complex samples)
* Automatic **DC offset removal**
* **FIFO-based streaming**
* Works through **SoapyRemote** (SoapySDRServer)
* Compatible with OpenWebRX
* Designed for **LinHT handheld SDR** (M17 Project)
* RX only, no setFrequency or any other SX1255 parameters (yet)

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

Install dependencies (Debian/Ubuntu):

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

## How It Works Internally

The driver:

1. Subscribes to LinHT ZMQ baseband stream (`ipc:///tmp/bsb_rx`)
2. Processes each 1024-IQ-sample block through:
   * integer > float conversion
   * FIR equalizer (inverse-sinc for SX1255)
   * DC removal
   * FIFO buffering
3. Feeds exactly **numElems** samples to SoapySDR, matching SoapyRemote UDP MTU (≈178 complex samples)

## Contact

Part of the **M17 Project / LinHT** ecosystem
[https://github.com/M17-Project](https://github.com/M17-Project)
