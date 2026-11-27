// Compare SNR estimators on real symbols, because the one in the decoder may be biased.
//
// The decoder reports SNR from the M2M4 moment estimator:
//
//     S = sqrt(2*M2^2 - M4)      M2 = E[|y|^2], M4 = E[|y|^4]
//     N = M2 - S
//
// That estimator is derived for a complex Gaussian signal. For BPSK in circular complex noise the
// derivation does come out exactly -- with y = +/-A + n, M2 = S + N and M4 = S^2 + 4SN + 2N^2, so
// the formula recovers S and N -- but it assumes the samples are independent, and these are not.
// They come out of a root-raised-cosine matched filter, which correlates the noise across samples,
// and correlated noise changes the fourth moment while leaving the second alone. The bias that
// produces is a constant offset, which is what a reading that is consistently low looks like.
//
// BPSK admits an estimator with no such assumption. The constellation is real: the imaginary part
// carries no signal at all, whatever the filter did to it. So
//
//     sigma^2 = E[Im(y)^2]                  the imaginary part is pure noise
//     S       = E[Re(y)^2] - sigma^2        the real part is signal plus the same noise
//     SNR     = S / (2*sigma^2)
//
// and every quantity in it is a second moment, which the filter does not bias the same way a fourth
// moment is biased. It also needs no decisions, so it does not degrade at low SNR the way a
// decision-directed estimate does.
//
// This prints both, along with the moments they come from, so the difference can be seen rather
// than argued about.
//
// Usage: NadirStation_SnrProbe <file.cs16> [--rate 2000000] [--symbol-rate 128000] [--seconds 20]
//                       [--skip 0] [--dump symbols.bin]

#include "../src/dsp.h"

#include <QCoreApplication>
#include <QString>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

// The estimator the decoder uses.
void m2m4(const std::vector<std::complex<float>>& s, double* signal, double* noise) {
    if (s.empty()) {
        *signal = 0.0;
        *noise = 0.0;
        return;
    }
    double m2 = 0.0, m4 = 0.0;
    for (const auto& y : s) {
        const double p = std::norm(y);
        m2 += p;
        m4 += p * p;
    }
    m2 /= static_cast<double>(s.size());
    m4 /= static_cast<double>(s.size());
    *signal = std::sqrt(std::max(0.0, 2.0 * m2 * m2 - m4));
    *noise = m2 - *signal;
}

// The estimator that uses the fact that a BPSK constellation is real.
void realAxis(const std::vector<std::complex<float>>& s, double* signal, double* noise) {
    if (s.empty()) {
        *signal = 0.0;
        *noise = 0.0;
        return;
    }
    double re2 = 0.0, im2 = 0.0;
    for (const auto& y : s) {
        re2 += static_cast<double>(y.real()) * y.real();
        im2 += static_cast<double>(y.imag()) * y.imag();
    }
    re2 /= static_cast<double>(s.size());
    im2 /= static_cast<double>(s.size());
    *noise = 2.0 * im2;                    // total noise power, both dimensions
    *signal = std::max(0.0, re2 - im2);    // the real axis minus the noise on it
}

double db(double signal, double noise) {
    if (signal <= 0.0 || noise <= 0.0) return 0.0;
    return 10.0 * std::log10(signal / noise);
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    QString path, dumpPath;
    double rate = 2.0e6, symbolRate = 128000.0, seconds = 20.0, skip = 0.0;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QStringLiteral("--rate") && i + 1 < argc)
            rate = QString::fromLocal8Bit(argv[++i]).toDouble();
        else if (a == QStringLiteral("--symbol-rate") && i + 1 < argc)
            symbolRate = QString::fromLocal8Bit(argv[++i]).toDouble();
        else if (a == QStringLiteral("--seconds") && i + 1 < argc)
            seconds = QString::fromLocal8Bit(argv[++i]).toDouble();
        else if (a == QStringLiteral("--skip") && i + 1 < argc)
            skip = QString::fromLocal8Bit(argv[++i]).toDouble();
        else if (a == QStringLiteral("--dump") && i + 1 < argc)
            dumpPath = QString::fromLocal8Bit(argv[++i]);
        else if (path.isEmpty())
            path = a;
    }
    if (path.isEmpty()) {
        std::cout << "usage: NadirStation_SnrProbe <file.cs16> [--rate Hz] [--symbol-rate Hz]\n"
                     "                      [--seconds N] [--skip S] [--dump symbols.bin]\n";
        return 2;
    }

    std::ifstream in(path.toLocal8Bit().constData(), std::ios::binary);
    if (!in) {
        std::cerr << "cannot open " << path.toLocal8Bit().constData() << "\n";
        return 3;
    }
    if (skip > 0.0)
        in.seekg(static_cast<std::streamoff>(skip * rate * 4), std::ios::beg);

    nadirstation::dsp::PskDemod demod(static_cast<float>(rate), static_cast<float>(symbolRate));
    demod.reset();

    const std::size_t block = 1u << 16;
    const std::size_t total = static_cast<std::size_t>(seconds * rate);
    std::size_t done = 0;
    std::vector<std::int16_t> raw(block * 2);
    std::vector<std::complex<float>> all;

    while (done < total) {
        const std::size_t want = std::min(block, total - done);
        in.read(reinterpret_cast<char*>(raw.data()),
                static_cast<std::streamsize>(want * 4));
        const std::size_t got = static_cast<std::size_t>(in.gcount()) / 4;
        if (got == 0) break;
        std::vector<std::complex<float>> iq;
        iq.reserve(got);
        for (std::size_t i = 0; i < got; ++i)
            iq.emplace_back(static_cast<float>(raw[2 * i]) / 32768.0f,
                            static_cast<float>(raw[2 * i + 1]) / 32768.0f);
        const auto r = demod.process(iq);
        // PERBLOCK: the reading per block, because a value that flashes low is a property of some
        // blocks and averaging hides which.
        if (!r.symbols.empty()) {
            double a11 = 0.0, a22 = 0.0, a12 = 0.0;
            for (const auto& y : r.symbols) {
                a11 += static_cast<double>(y.real()) * y.real();
                a22 += static_cast<double>(y.imag()) * y.imag();
                a12 += static_cast<double>(y.real()) * y.imag();
            }
            const double m = static_cast<double>(r.symbols.size());
            a11 /= m; a22 /= m; a12 /= m;
            const double hf = 0.5 * (a11 + a22);
            const double sp = std::sqrt(0.25 * (a11 - a22) * (a11 - a22) + a12 * a12);
            const double lmin = std::max(0.0, hf - sp), lmax = hf + sp;
            const double sig = std::max(0.0, lmax - lmin), noi = 2.0 * lmin;
            const double d = (sig > 0.0 && noi > 0.0) ? 10.0 * std::log10(sig / noi) : 0.0;
            // The eigenvalue ratio says how far the constellation is from occupying one axis.
            const double ratio = lmax > 0.0 ? lmin / lmax : 1.0;
            std::printf("PERBLOCK t=%7.3f n=%6zu snr=%6.2f dB  lmin/lmax=%.4f  |Re|/|Im| rms=%.3f\n",
                        static_cast<double>(done) / rate, r.symbols.size(), d, ratio,
                        a11 > 0.0 ? std::sqrt(a22 / a11) : 0.0);
            std::fflush(stdout);
        }
        all.insert(all.end(), r.symbols.begin(), r.symbols.end());
        done += got;
    }

    if (all.empty()) {
        std::cerr << "no symbols\n";
        return 4;
    }

    // Drop the first fifth: the loops need to converge before the constellation means anything, and
    // an unconverged opening drags both estimates down.
    const std::size_t from = all.size() / 5;
    std::vector<std::complex<float>> settled(all.begin() + static_cast<std::ptrdiff_t>(from),
                                             all.end());

    double s1 = 0.0, n1 = 0.0, s2 = 0.0, n2 = 0.0;
    m2m4(settled, &s1, &n1);
    realAxis(settled, &s2, &n2);

    double re2 = 0.0, im2 = 0.0;
    for (const auto& y : settled) {
        re2 += static_cast<double>(y.real()) * y.real();
        im2 += static_cast<double>(y.imag()) * y.imag();
    }
    re2 /= static_cast<double>(settled.size());
    im2 /= static_cast<double>(settled.size());

    std::printf("symbols            %zu (of %zu, first fifth dropped)\n", settled.size(), all.size());
    std::printf("E[Re^2]            %.6f\n", re2);
    std::printf("E[Im^2]            %.6f\n", im2);
    std::printf("\n");
    std::printf("M2M4      signal %10.6f  noise %10.6f  -> %6.2f dB\n", s1, n1, db(s1, n1));
    std::printf("real-axis signal %10.6f  noise %10.6f  -> %6.2f dB\n", s2, n2, db(s2, n2));
    std::printf("\n");
    std::printf("difference         %6.2f dB\n", db(s2, n2) - db(s1, n1));

    if (!dumpPath.isEmpty()) {
        std::ofstream out(dumpPath.toLocal8Bit().constData(), std::ios::binary);
        out.write(reinterpret_cast<const char*>(all.data()),
                  static_cast<std::streamsize>(all.size() * sizeof(std::complex<float>)));
        std::printf("\nwrote %zu symbols (%zu bytes) to %s\n", all.size(),
                    all.size() * sizeof(std::complex<float>), dumpPath.toLocal8Bit().constData());
    }
    return 0;
}
