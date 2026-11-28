#pragma once
// Qt meta-types for the IQ blocks and spectra the receivers pass across threads. In one header so
// every file that declares such a signal sees them before moc instantiates the type -- a receiver
// whose moc file sorts before nadirstationdecoder's otherwise breaks the build.
#include <QMetaType>
#include <complex>
#include <memory>
#include <vector>

Q_DECLARE_METATYPE(std::shared_ptr<std::vector<std::complex<float>>>)
Q_DECLARE_METATYPE(std::vector<float>)
