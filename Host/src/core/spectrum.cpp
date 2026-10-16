#include "core/spectrum.h"
#include <QReadLocker>
#include <QWriteLocker>
#include <algorithm>
#include <cmath>

namespace mca {

SpectrumAccumulator::SpectrumAccumulator(int channels, int peakBins,
                                         int peakFractionBits,
                                         quint32 sampleRateHz)
    : m_channels(qBound(1, channels, MaxChannels)),
      m_peakBins(qBound(256, peakBins, 65536)),
      m_peakFractionBits(qBound(0, peakFractionBits, 15)) {
    m_data.channelCount = m_channels;
    m_data.sampleRateHz = sampleRateHz;
    m_data.peakFractionBits = quint8(m_peakFractionBits);
    int adcBits = 0;
    for (int bins = m_peakBins; bins > 1; bins >>= 1)
        ++adcBits;
    m_data.adcBits = quint8(adcBits);
    m_data.peak.resize(m_channels); m_data.integral.resize(m_channels);
    m_data.moduleEnergy.resize((m_channels + 3) / 4);
    for (int c = 0; c < m_channels; ++c) {
        m_data.peak[c].fill(0, m_peakBins);
        m_data.integral[c].fill(0, DefaultIntegralBins);
    }
    for (auto &energy : m_data.moduleEnergy)
        energy.fill(0, DefaultIntegralBins);
}

void SpectrumAccumulator::clear() {
    QWriteLocker guard(&m_lock);
    for (auto &v : m_data.peak) v.fill(0);
    for (auto &v : m_data.integral) v.fill(0);
    for (auto &v : m_data.moduleEnergy) v.fill(0);
    m_data.modulePositions.clear();
    m_data.coincidenceEnergy.clear();
    m_data.coincidenceTime.clear();
    m_data.channelEvents.fill(0); m_data.totalEvents = 0; ++m_data.version;
    m_maxIntegral = 0;
    if (m_autoShift) m_integralShift = 0;
}

void SpectrumAccumulator::setIntegralShift(int shift) {
    QWriteLocker guard(&m_lock);
    m_autoShift = false;
    int newShift = qBound(0, shift, 23);
    rebinIntegral(newShift);
}

void SpectrumAccumulator::setAutoIntegralShift(bool enabled) {
    QWriteLocker guard(&m_lock);
    m_autoShift = enabled;
    if (enabled) {
        int optimal = computeOptimalShift();
        rebinIntegral(optimal);
    }
}

int SpectrumAccumulator::computeOptimalShift() const {
    if (m_maxIntegral == 0) return 0;
    int bitWidth = 0;
    quint64 v = m_maxIntegral;
    while (v > 0) { v >>= 1; ++bitWidth; }
    // Keep maxIntegral >> shift < DefaultIntegralBins (16384 = 2^14)
    return qMax(0, bitWidth - 14);
}

void SpectrumAccumulator::rebinIntegral(int newShift) {
    if (newShift == m_integralShift) return;
    const int oldShift = m_integralShift;
    for (int c = 0; c < m_channels; ++c) {
        QVector<quint64> newIntegral(DefaultIntegralBins, 0);
        if (newShift > oldShift) {
            // Merging: shift increased, bins combine (exact)
            const int mergeFactor = 1 << (newShift - oldShift);
            for (int i = 0; i < DefaultIntegralBins; ++i) {
                if (m_data.integral[c][i] == 0) continue;
                const int newBin = i / mergeFactor;
                if (newBin < DefaultIntegralBins)
                    newIntegral[newBin] += m_data.integral[c][i];
            }
        } else {
            // Splitting: shift decreased, bins distributed (approximate)
            const int splitFactor = 1 << (oldShift - newShift);
            for (int i = 0; i < DefaultIntegralBins; ++i) {
                if (m_data.integral[c][i] == 0) continue;
                const quint64 count = m_data.integral[c][i];
                const quint64 perBin = count / splitFactor;
                const quint64 remainder = count % splitFactor;
                for (int j = 0; j < splitFactor; ++j) {
                    const int newBin = i * splitFactor + j;
                    if (newBin < DefaultIntegralBins)
                        newIntegral[newBin] += perBin + (j < int(remainder) ? 1 : 0);
                }
            }
        }
        m_data.integral[c] = std::move(newIntegral);
    }
    m_integralShift = newShift;
}

quint64 SpectrumAccumulator::add(const QVector<EventRecord> &events, bool includePileup) {
    QWriteLocker guard(&m_lock);
    quint64 accepted = 0;
    for (const auto &e : events) {
        if (e.channel >= m_channels) continue;
        if (e.pileup && !includePileup) continue;
        // Track max integral for auto-shift computation
        if (m_autoShift && e.integral > m_maxIntegral) {
            m_maxIntegral = e.integral;
            const int optimal = computeOptimalShift();
            if (optimal != m_integralShift) rebinIntegral(optimal);
        }
        const int peakBin = qMin<int>(e.peak >> m_peakFractionBits,
                                      m_peakBins - 1);
        const int integralBin = qMin<int>(e.integral >> m_integralShift, DefaultIntegralBins - 1);
        ++m_data.peak[e.channel][peakBin]; ++m_data.integral[e.channel][integralBin];
        ++m_data.channelEvents[e.channel]; ++m_data.totalEvents; ++accepted;
    }
    if (accepted) ++m_data.version;
    return accepted;
}

void SpectrumAccumulator::addSerialDelta(const QVector<quint32> &peak,
                                         const QVector<quint32> &integral,
                                         quint64 events, quint8 peakShift,
                                         quint8 integralShift) {
    QWriteLocker guard(&m_lock);
    if (m_channels < 1 || peak.size() != m_peakBins ||
        integral.size() > DefaultIntegralBins)
        return;
    if (m_data.integral[0].size() != integral.size()) {
        for (auto &spectrum : m_data.integral)
            spectrum.fill(0, integral.size());
    }
    m_data.peakBinShift = peakShift;
    m_data.integralBinShift = integralShift;
    for (int i = 0; i < peak.size(); ++i)
        m_data.peak[0][i] += peak[i];
    for (int i = 0; i < integral.size(); ++i)
        m_data.integral[0][i] += integral[i];
    m_data.channelEvents[0] += events;
    m_data.totalEvents += events;
    ++m_data.version;
}

void SpectrumAccumulator::addModuleObservables(const QVector<ModuleEvent> &events) {
    QWriteLocker guard(&m_lock);
    for (const auto &event : events) {
        if (event.module < m_data.moduleEnergy.size()) {
            const int bin = qMin<int>(event.energy >> m_integralShift,
                                      DefaultIntegralBins - 1);
            ++m_data.moduleEnergy[event.module][bin];
        }
        if (m_data.modulePositions.size() < 200'000)
            m_data.modulePositions.push_back(QPointF(event.x, event.y));
    }
    if (!events.isEmpty()) ++m_data.version;
}

void SpectrumAccumulator::addCoincidenceObservables(
    const QVector<CoincidenceEvent> &events) {
    QWriteLocker guard(&m_lock);
    for (const auto &event : events) {
        if (m_data.coincidenceEnergy.size() < 200'000)
            m_data.coincidenceEnergy.push_back(
                QPointF(event.scatter.energy, event.absorber.energy));
        if (m_data.coincidenceTime.size() < 200'000)
            m_data.coincidenceTime.push_back(event.layerTime);
    }
    if (!events.isEmpty()) ++m_data.version;
}

std::shared_ptr<const SpectrumSnapshot> SpectrumAccumulator::snapshot() const {
    QReadLocker guard(&m_lock);
    return std::make_shared<const SpectrumSnapshot>(m_data);
}

void SpectrumAccumulator::setPeakFractionBits(int bits) {
    QWriteLocker guard(&m_lock);
    m_peakFractionBits = qBound(0, bits, 15);
    m_data.peakFractionBits = quint8(m_peakFractionBits);
}

} // namespace mca
