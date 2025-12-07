#pragma once

#include <array>
#include <complex>
#include <cstddef>

class LinHTFir
{
public:
    static constexpr std::size_t NUM_TAPS = 91;
    static constexpr std::size_t HISTORY_LEN = NUM_TAPS;

    LinHTFir();
    void reset();
    std::complex<float> processSample(std::complex<float> x);

private:
    std::array<std::complex<float>, HISTORY_LEN> ring;
    size_t pos = 0;
};
