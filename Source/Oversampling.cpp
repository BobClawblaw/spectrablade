#include "Oversampling.h"

namespace
{
    double besselI0 (double x)
    {
        // The series, until a term no longer changes the sum.
        auto sum = 1.0, term = 1.0;
        const auto q = x * x / 4.0;

        for (int k = 1; k < 200; ++k)
        {
            term *= q / (k * k);
            sum += term;

            if (term < sum * 1.0e-17)
                break;
        }

        return sum;
    }
}

const std::array<double, Oversampler4::taps>& Oversampler4::kernelTaps()
{
    // A sinc cut at the original Nyquist (an eighth of the high rate) under a Kaiser window
    // (beta 8), scaled to pass DC at unity.
    static const auto kernel = []
    {
        std::array<double, taps> h {};
        constexpr double beta = 8.0, cutoff = 0.5 / factor;
        constexpr int centre = (taps - 1) / 2;
        const auto norm = besselI0 (beta);
        auto sum = 0.0;

        for (int k = 0; k < taps; ++k)
        {
            const auto n = k - centre;
            const auto x = 2.0 * cutoff * n;
            const auto sinc = n == 0 ? 1.0 : std::sin (juce::MathConstants<double>::pi * x) / (juce::MathConstants<double>::pi * x);
            const auto r = (double) n / centre;
            h[(size_t) k] = sinc * besselI0 (beta * std::sqrt (juce::jmax (0.0, 1.0 - r * r))) / norm;
            sum += h[(size_t) k];
        }

        for (auto& v : h)
            v /= sum;

        return h;
    }();

    return kernel;
}

void Oversampler4::reset() noexcept
{
    lowHistory.fill (0.0);
    highHistory.fill (0.0);
    lowPos = highPos = 0;
}

double Oversampler4::responseDb (double f)
{
    const auto& h = kernelTaps();
    std::complex<double> sum;

    for (int k = 0; k < taps; ++k)
        sum += h[(size_t) k] * std::polar (1.0, -juce::MathConstants<double>::twoPi * f * k);

    return 20.0 * std::log10 (std::max (1.0e-12, std::abs (sum)));
}
