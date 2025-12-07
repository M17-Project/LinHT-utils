#include <SoapySDR/Device.hpp>
#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Constants.h>
#include <SoapySDR/Errors.h>

#include <zmq.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <sx1255.h>
}

#include "fir.h"

static const size_t ZMQ_LEN_INTS = 2048;  // 2048 int32_t → 1024 complex samples
static const size_t ZMQ_COMPLEX_SAMPLES = ZMQ_LEN_INTS / 2;
static const double LINHT_SAMPLE_RATE = 500000.0; // 500 kSa/s
static const double LINHT_CENTER_FREQ = 433.475e6;

// SX1255 is a global singleton (the chip is only one and is shared).
// SoapySDR may create *multiple* LinHTZmqDevice instances for one client
// (e.g. during negotiation), and each instance calls the constructor/destructor.
// Without this reference counter we would call sx1255_init()/sx1255_cleanup()
// multiple times and break the GPIO/SPI state for others.
// `g_sx1255_users` ensures that SX1255 is initialized only once and cleaned up
// only when the last device instance is destroyed.
static int g_sx1255_users = 0;

// Opaque stream state for this driver
struct LinHTZmqStream
{
    bool active;
    std::string format;
    LinHTFir fir;
    float dc_i = 0.0f;
    float dc_q = 0.0f;
    std::deque<std::complex<float>> fifo;
};

class LinHTZmqDevice : public SoapySDR::Device
{
public:
    explicit LinHTZmqDevice(const SoapySDR::Kwargs &args);
    ~LinHTZmqDevice();

    // Identification ----------------------------------------------------
    std::string getDriverKey(void) const
    {
        return "linht";
    }

    std::string getHardwareKey(void) const
    {
        return "LinHT";
    }

    SoapySDR::Kwargs getHardwareInfo(void) const
    {
        SoapySDR::Kwargs info;
        info["origin"] = "SoapySDR driver for LinHT";
        info["endpoint"] = endpoint;
        info["fixed_sample_rate"] = std::to_string(LINHT_SAMPLE_RATE);
        info["fixed_center_freq"] = std::to_string(centerFreqHz);
        return info;
    }

    // Channels ----------------------------------------------------------
    size_t getNumChannels(const int direction) const
    {
        if (direction == SOAPY_SDR_RX) return 1; // single RX channel
        return 0;                                // no TX support (yet)
    }

    bool getFullDuplex(const int /*direction*/, const size_t /*channel*/) const
    {
        return false;
    }

    // Stream formats ----------------------------------------------------
    std::vector<std::string> getStreamFormats(const int direction, const size_t channel) const
    {
        std::vector<std::string> formats;
        if (direction == SOAPY_SDR_RX && channel == 0)
        {
            // Native format: complex float (I, Q)
            formats.push_back(SOAPY_SDR_CF32);
            // rtl_433
            formats.push_back(SOAPY_SDR_CS16);
        }
        return formats;
    }

    std::string getNativeStreamFormat(const int direction,
                                      const size_t channel,
                                      double &fullScale) const
    {
        if (direction == SOAPY_SDR_RX && channel == 0)
        {
            fullScale = 1.0f;
            return SOAPY_SDR_CF32;
        }
        fullScale = 0.0f;
        return "";
    }

    SoapySDR::ArgInfoList getStreamArgsInfo(const int /*direction*/,
                                            const size_t /*channel*/) const
    {
        // No special stream args for now
        return SoapySDR::ArgInfoList();
    }

    // Frequency API -----------------------------------------------------
    void setFrequency(const int direction,
                      const size_t channel,
                      const std::string &name,
                      const double frequency,
                      const SoapySDR::Kwargs & /*args*/)
    {
        if (direction != SOAPY_SDR_RX || channel != 0) return;

        if (name.empty() || name == "RF")
        {
            centerFreqHz = frequency;
            applyHardwareFrequency();
        }
    }

    double getFrequency(const int direction,
                        const size_t channel,
                        const std::string &name) const
    {
        if (direction != SOAPY_SDR_RX || channel != 0) return 0.0;
        if (name.empty() || name == "RF") return centerFreqHz;
        return 0.0;
    }

    std::vector<std::string> listFrequencies(const int direction,
                                             const size_t channel) const
    {
        if (direction == SOAPY_SDR_RX && channel == 0)
        {
            return {"RF"};
        }
        return {};
    }

    SoapySDR::RangeList getFrequencyRange(const int direction,
                                          const size_t channel,
                                          const std::string &name) const
    {
        SoapySDR::RangeList ranges;
        if (direction == SOAPY_SDR_RX && channel == 0 &&
            (name.empty() || name == "RF"))
        {
            // Arbitrary narrow range around 433 MHz (purely informational)
            ranges.emplace_back(420.0e6, 470.0e6);
        }
        return ranges;
    }

    // Sample rate -------------------------------------------------------
    void setSampleRate(const int /*direction*/,
                       const size_t /*channel*/,
                       const double /*rate*/)
    {
        // Not supported, fixed rate.
    }

    double getSampleRate(const int direction,
                         const size_t channel) const
    {
        if (direction == SOAPY_SDR_RX && channel == 0)
        {
            return LINHT_SAMPLE_RATE;
        }
        return 0.0;
    }

    std::vector<double> listSampleRates(const int direction,
                                        const size_t channel) const
    {
        if (direction == SOAPY_SDR_RX && channel == 0)
        {
            return {LINHT_SAMPLE_RATE};
        }
        return {};
    }

    SoapySDR::RangeList getSampleRateRange(const int direction,
                                           const size_t channel) const
    {
        SoapySDR::RangeList ranges;
        if (direction == SOAPY_SDR_RX && channel == 0)
        {
            ranges.emplace_back(LINHT_SAMPLE_RATE, LINHT_SAMPLE_RATE);
        }
        return ranges;
    }

    // Antennas ----------------------------------------------------------
    std::vector<std::string> listAntennas(const int direction,
                                          const size_t channel) const
    {
        if (direction == SOAPY_SDR_RX && channel == 0)
        {
            return {"RX"};
        }
        return {};
    }

    void setAntenna(const int /*direction*/,
                    const size_t /*channel*/,
                    const std::string & /*name*/)
    {
        // Single fixed antenna.
    }

    std::string getAntenna(const int direction,
                           const size_t channel) const
    {
        if (direction == SOAPY_SDR_RX && channel == 0) return "RX";
        return "";
    }

    // Stream API --------------------------------------------------------
    SoapySDR::Stream *setupStream(const int direction,
                                  const std::string &format,
                                  const std::vector<size_t> &channels,
                                  const SoapySDR::Kwargs & /*args*/)
    {
        if (direction != SOAPY_SDR_RX)
        {
            throw std::runtime_error("LinHTZmq: only RX direction is supported");
        }

        if (format != SOAPY_SDR_CF32 && format != SOAPY_SDR_CS16)
        {
            throw std::runtime_error("LinHT: supported formats are CF32 and CS16!");
        }

        if (!channels.empty() && (channels.size() != 1 || channels[0] != 0))
        {
            throw std::runtime_error("LinHTZmq: only channel 0 is supported");
        }

        auto *st = new LinHTZmqStream();
        st->active = false;
        st->format = format;
        return reinterpret_cast<SoapySDR::Stream *>(st);
    }

    void closeStream(SoapySDR::Stream *stream)
    {
        if (stream == nullptr) return;
        auto *st = reinterpret_cast<LinHTZmqStream *>(stream);
        delete st;
    }

    size_t getStreamMTU(SoapySDR::Stream * /*stream*/) const
    {
        // MTU in elements (complex samples)
        return ZMQ_COMPLEX_SAMPLES;
    }

    int activateStream(SoapySDR::Stream *stream,
                       const int /*flags*/ = 0,
                       const long long /*timeNs*/ = 0,
                       const size_t /*numElems*/ = 0)
    {
        auto *st = reinterpret_cast<LinHTZmqStream *>(stream);
        if (!st) return SOAPY_SDR_STREAM_ERROR;
        st->active = true;
        st->fir.reset();
        st->dc_i = st->dc_q = 0.0f;
        st->fifo.clear();

        return 0;
    }

    int deactivateStream(SoapySDR::Stream *stream,
                         const int /*flags*/ = 0,
                         const long long /*timeNs*/ = 0)
    {
        auto *st = reinterpret_cast<LinHTZmqStream *>(stream);
        if (!st) return SOAPY_SDR_STREAM_ERROR;
        st->active = false;
        return 0;
    }

    int readStream(SoapySDR::Stream *stream,
                   void *const *buffs,
                   const size_t numElems,
                   int &flags,
                   long long &timeNs,
                   const long timeoutUs = 100000)
    {
        auto *st = reinterpret_cast<LinHTZmqStream *>(stream);
        if(!st || !st->active)
        {
            return SOAPY_SDR_TIMEOUT;
        }

        if(!buffs || !buffs[0] || numElems == 0)
        {
            return SOAPY_SDR_STREAM_ERROR;
        }

        // Ensure FIFO has enough samples.
        while(st->fifo.size() < numElems)
        {
            // Poll ZMQ.
            zmq_pollitem_t item;
            item.socket = zmqSub;
            item.fd = 0;
            item.events = ZMQ_POLLIN;
            item.revents = 0;

            long timeoutMs =
                (timeoutUs < 0) ? -1 :
                (timeoutUs == 0) ? 0 :
                (timeoutUs + 999) / 1000;

            int pollRet = zmq_poll(&item, 1, timeoutMs);
            if(pollRet < 0)
            {
                if(zmq_errno() == EINTR)
                {
                    // Try again.
                    continue;
                }
                return SOAPY_SDR_STREAM_ERROR;
            }

            if(pollRet == 0)
            {
                return SOAPY_SDR_TIMEOUT;
            }

            // Read one full ZMQ block (1024 complex).
            int32_t rxBuf[ZMQ_LEN_INTS];
            int rc = zmq_recv(zmqSub, rxBuf, sizeof(rxBuf), 0);
            if(rc <= 0)
            {
                return SOAPY_SDR_TIMEOUT;
            }
            else if(rc != sizeof(rxBuf))
            {
                throw std::runtime_error("LinHTZmq: ZMQ data truncated!");
            }

            size_t nInts = rc / sizeof(int32_t);
            size_t nComplex = nInts / 2;

            const float scale = powf(2.0f, -31.0f);
            const float alpha = 1e-4f;

            for(size_t i = 0; i < nComplex; i++)
            {
                // int32 -> float
                float I = rxBuf[2*i + 0] * scale;
                float Q = rxBuf[2*i + 1] * scale;

                // FIR
                auto y = st->fir.processSample({I, Q});

                // DC removal
                st->dc_i = (1.f - alpha)*st->dc_i + alpha*y.real();
                st->dc_q = (1.f - alpha)*st->dc_q + alpha*y.imag();

                st->fifo.emplace_back(y.real() - st->dc_i,
                                      y.imag() - st->dc_q);
            }
        }

        if(st->format == SOAPY_SDR_CF32)
        {
            auto *out = reinterpret_cast<std::complex<float> *>(buffs[0]);

            // Output exactly numElems.
            for(size_t i = 0; i < numElems; i++)
            {
                out[i] = st->fifo.front();
                st->fifo.pop_front();
            }
        }
        else if(st->format == SOAPY_SDR_CS16) {
            auto *out = reinterpret_cast<std::complex<int16_t> *>(buffs[0]);

            // Output exactly numElems.
            for(size_t i = 0; i < numElems; i++)
            {
                // Get CF32 sample from FIFO
                const std::complex<float> s = st->fifo.front();
                st->fifo.pop_front();

                // Clamp to [-1.0, 1.0] to avoid overflow
                // FIXME: is this needed?
                float re = std::clamp(s.real(), -1.0f, 1.0f);
                float im = std::clamp(s.imag(), -1.0f, 1.0f);

                // Scale to int16_t range and round
                int16_t ire = static_cast<int16_t>(std::lrint(re * 32767.0f));
                int16_t iim = static_cast<int16_t>(std::lrint(im * 32767.0f));

                // Store as CS16 (interleaved I/Q)
                out[i] = std::complex<int16_t>(ire, iim);
            }
        }

        flags = 0;
        timeNs = 0;
        return (int)numElems;
    }


    // TODO: TX not implemented, yet.
    int writeStream(SoapySDR::Stream * /*stream*/,
                    const void *const * /*buffs*/,
                    const size_t /*numElems*/,
                    int & /*flags*/,
                    const long long /*timeNs*/ = 0,
                    const long /*timeoutUs*/ = 100000)
    {
        return SOAPY_SDR_STREAM_ERROR;
    }

private:
    void *zmqCtx;
    void *zmqSub;
    std::string endpoint;
    double centerFreqHz;

    // SX1255 related
    // SX1255 related (default values are set in constructor,
    // findLinHTZmq() may override them via Kwargs)
    bool rfCtrlAvailable;
    std::string spiDevice;
    std::string gpioChip;
    int resetPinOffset;

    void applyHardwareFrequency()
    {
        if (!rfCtrlAvailable)
        {
            return;
        }

        double freq = centerFreqHz;
        if (freq < 420.0e6) freq = 420.0e6;
        if (freq > 470.0e6) freq = 470.0e6;

        uint32_t f_hz = static_cast<uint32_t>(freq + 0.5);

        sx1255_set_rx_freq(f_hz);
        // sx1255_set_tx_freq(f_hz);

        std::cerr << "LinHTZmq: SX1255 tuned to " << f_hz / 1e6 << " MHz\n";
    }
};

LinHTZmqDevice::LinHTZmqDevice(const SoapySDR::Kwargs &args)
    : zmqCtx(nullptr)
    , zmqSub(nullptr)
    , endpoint("ipc:///tmp/bsb_rx")
    , centerFreqHz(LINHT_CENTER_FREQ)
    , rfCtrlAvailable(false)
    , spiDevice("/dev/spidev0.0")
    , gpioChip("/dev/gpiochip0")
    , resetPinOffset(22)
{
    auto epIt = args.find("rx_endpoint");
    if(epIt != args.end())
        endpoint = epIt->second;

    zmqCtx = zmq_ctx_new();
    if(!zmqCtx)
        throw std::runtime_error("LinHTZmq: failed to create ZMQ context");

    zmqSub = zmq_socket(zmqCtx, ZMQ_SUB);
    if(!zmqSub)
    {
        zmq_ctx_term(zmqCtx);
        throw std::runtime_error("LinHTZmq: failed to create ZMQ SUB socket");
    }

    int rc = zmq_setsockopt(zmqSub, ZMQ_SUBSCRIBE, "", 0);
    if(rc != 0)
    {
        zmq_close(zmqSub);
        zmq_ctx_term(zmqCtx);
        throw std::runtime_error("LinHTZmq: zmq_setsockopt(ZMQ_SUBSCRIBE) failed");
    }

    rc = zmq_connect(zmqSub, endpoint.c_str());
    if(rc != 0)
    {
        zmq_close(zmqSub);
        zmq_ctx_term(zmqCtx);
        throw std::runtime_error("LinHTZmq: failed to connect to endpoint " + endpoint);
    }

    std::cerr << "LinHTZmq: connected to " << endpoint
              << " (CF32, " << LINHT_SAMPLE_RATE/1000.0 << " kSa/s, "
              << centerFreqHz/1e6 << " MHz)\n";

    // SX1255 config.
    auto spiIt = args.find("sx1255_spi");
    if(spiIt != args.end())
    {
        spiDevice = spiIt->second;
    }

    auto gpioIt = args.find("sx1255_gpio");
    if(gpioIt != args.end())
    {
        gpioChip = gpioIt->second;
    }

    auto rstIt = args.find("sx1255_reset");
    if(rstIt != args.end())
    {
        const std::string &val = rstIt->second;
        char *end = nullptr;
        long v = std::strtol(val.c_str(), &end, 10);

        if (end != val.c_str() && *end == '\0' &&
            v >= 0 && v <= std::numeric_limits<int>::max())
        {
            resetPinOffset = static_cast<int>(v);
        }
        else
        {
            std::cerr << "LinHTZmq: invalid sx1255_reset '" << val
                      << "', keeping default " << resetPinOffset << "\n";
        }
    }

    // --- SX1255 init ---
    if (g_sx1255_users == 0) {
        rc = sx1255_init(spiDevice.c_str(), gpioChip.c_str(), resetPinOffset);
        if(rc != 0)
        {
            std::cerr << "LinHTZmq: sx1255_init("
                      << spiDevice << ", " << gpioChip
                      << ", " << resetPinOffset << ") failed, rc=" << rc << "\n";
            rfCtrlAvailable = false;
            return;
        }

        std::cerr
            << "LinHTZmq: using SX1255 spi="
            << spiDevice
            << " gpio=" << gpioChip
            << " reset=" << resetPinOffset << "\n";

        sx1255_reset();
        sx1255_set_rate(SX1255_RATE_500K);
        sx1255_set_rx_pll_bw(75);
        sx1255_set_tx_pll_bw(75);
        sx1255_enable_rx(true);
        // sx1255_enable_tx(true);
    }

    g_sx1255_users++;
    rfCtrlAvailable = true;
    applyHardwareFrequency();
}

LinHTZmqDevice::~LinHTZmqDevice()
{
    if (zmqSub)
    {
        zmq_close(zmqSub);
        zmqSub = nullptr;
    }
    if (zmqCtx)
    {
        zmq_ctx_term(zmqCtx);
        zmqCtx = nullptr;
    }

    g_sx1255_users--;
    if (g_sx1255_users == 0)
    {
        sx1255_cleanup();
    }
    rfCtrlAvailable = false;
}

static SoapySDR::KwargsList findLinHTZmq(const SoapySDR::Kwargs &args)
{
    SoapySDR::KwargsList results;

    // Filtr na driver=linht (pokud uživatel něco zadal)
    auto it = args.find("driver");
    if (it != args.end() && it->second != "linht")
        return results;

    SoapySDR::Kwargs dev;
    dev["driver"] = "linht";
    dev["label"] = "LinHT ZMQ";

    // Defaulty pro SX1255 + RX endpoint
    dev["rx_endpoint"]  = "ipc:///tmp/bsb_rx";
    dev["sx1255_spi"]   = "/dev/spidev0.0";
    dev["sx1255_gpio"]  = "/dev/gpiochip0";
    dev["sx1255_reset"] = "22";

    // Když uživatel v původních args něco přepíše, respektuj to:
    auto epIt = args.find("rx_endpoint");
    if (epIt != args.end())
        dev["rx_endpoint"] = epIt->second;

    auto spiIt = args.find("sx1255_spi");
    if (spiIt != args.end())
        dev["sx1255_spi"] = spiIt->second;

    auto gpioIt = args.find("sx1255_gpio");
    if (gpioIt != args.end())
        dev["sx1255_gpio"] = gpioIt->second;

    auto rstIt = args.find("sx1255_reset");
    if (rstIt != args.end())
        dev["sx1255_reset"] = rstIt->second;

    results.push_back(dev);
    return results;
}

static SoapySDR::Device *makeLinHTZmq(const SoapySDR::Kwargs &args)
{
    return new LinHTZmqDevice(args);
}

static SoapySDR::Registry linhtZmqReg(
    "linht",
    &findLinHTZmq,
    &makeLinHTZmq,
    SOAPY_SDR_ABI_VERSION);
