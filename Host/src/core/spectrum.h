#pragma once
#include "core/types.h"
#include <QReadWriteLock>

namespace mca {

class SpectrumAccumulator {
public:
    explicit SpectrumAccumulator(int channels = 8,
                                 int peakBins = DefaultPeakBins,
                                 int peakFractionBits = 4,
                                 quint32 sampleRateHz = 80'000'000);
    void clear();
    quint64 add(const QVector<EventRecord> &events, bool includePileup = true);
    void addSerialDelta(const QVector<quint32> &peak,
                        const QVector<quint32> &integral,
                        quint64 events, quint8 peakShift,
                        quint8 integralShift);
    void addModuleObservables(const QVector<ModuleEvent> &events);
    void addCoincidenceObservables(const QVector<CoincidenceEvent> &events);
    std::shared_ptr<const SpectrumSnapshot> snapshot() const;
    // The UDP event peak uses a negotiated fixed-point format.  Capability
    // registers are read asynchronously after the accumulator is created,
    // so update the decoder when the authoritative format arrives.
    void setPeakFractionBits(int bits);
    void setIntegralShift(int shift);
    void setAutoIntegralShift(bool enabled);
    int integralShift() const { return m_integralShift; }
    bool isAutoIntegralShift() const { return m_autoShift; }
    int channelCount() const { return m_channels; }
private:
    void rebinIntegral(int newShift);
    int computeOptimalShift() const;

    int m_channels;
    int m_peakBins;
    int m_peakFractionBits;
    int m_integralShift = 0;
    bool m_autoShift = true;
    quint64 m_maxIntegral = 0;
    mutable QReadWriteLock m_lock;
    SpectrumSnapshot m_data;
};

} // namespace mca
