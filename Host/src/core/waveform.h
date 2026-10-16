#pragma once
#include "core/protocol.h"
#include "core/types.h"
#include <QDateTime>
#include <QVector>
#include <array>
#include <memory>
#include <optional>

namespace mca {

// Reassembly key and storage are namespace-level types so that MSVC accepts
// the container members unambiguously.
struct WaveformCaptureKey {
    quint8 deviceId = 0;
    quint32 sessionId = 0;
    quint32 configVersion = 0;
    quint32 captureId = 0;
};

struct WaveformFragmentSlot {
    bool present = false;
    quint16 sampleCount = 0;
    std::array<quint16, WaveformFragmentMax> samples{};
};

struct WaveformPendingCapture {
    WaveformCaptureKey key;
    // Named `parts` rather than `slots`, because `slots` is a Qt keyword.
    QVector<WaveformFragmentSlot> parts;
    int presentCount = 0;
    WaveformFragment meta;      // header captured from the first fragment
    bool metaValid = false;
    QDateTime firstSeen;
    qint64 order = 0;
};

// ---------------------------------------------------------------------------
// Waveform reassembler
//
// Fragments arrive over UDP, so they can be reordered, duplicated, lost, or
// delivered late from a previous session.  The reassembler is the only place
// that decides whether a record is complete:
//
//   * the reassembly key is (device id, session id, config version,
//     capture id), so fragments of different captures can never be mixed;
//   * a duplicate fragment with identical content is ignored, while two
//     fragments that disagree about the same index void the whole capture;
//   * fragments may arrive in any order, and a record is only published once
//     all 25 fragments covering exactly 12500 samples are present;
//   * at most four incomplete records are tracked, the oldest is evicted
//     first and the eviction is counted;
//   * a record that is not complete one second after its first fragment is
//     dropped and counted.  Nothing is ever zero-filled or published partial.
// ---------------------------------------------------------------------------
class WaveformReassembler {
public:
    struct Statistics {
        quint64 fragments = 0;
        quint64 duplicateFragments = 0;
        quint64 conflictingFragments = 0;
        quint64 staleFragments = 0;
        quint64 completedCaptures = 0;
        quint64 discardedCaptures = 0;
        quint64 evictedCaptures = 0;
        quint64 timedOutCaptures = 0;
    };

    static constexpr int MaximumPending = 4;
    static constexpr int TimeoutMilliseconds = 1000;

    explicit WaveformReassembler(int timeoutMs = TimeoutMilliseconds);

    // Adds one decoded fragment.  On completion the assembled record is
    // returned; otherwise the result is empty.
    std::optional<std::shared_ptr<const WaveformCapture>>
    add(const WaveformFragment &fragment, quint8 deviceId,
        quint32 packetConfigVersion, QString *error = nullptr);

    // Drops records that have been incomplete for too long.  Returns the
    // number of records that were discarded.
    int expire(const QDateTime &now = QDateTime::currentDateTime());

    // Drops every pending record and starts a new session, so data from an
    // old session can never complete after a restart.
    void reset(quint32 newSessionId = 0);
    void clear();

    const Statistics &statistics() const { return m_statistics; }
    int pendingCount() const { return m_pending.size(); }
    quint32 sessionId() const { return m_sessionId; }
    bool sessionKnown() const { return m_sessionKnown; }

    // Rejects a fragment whose fixed geometry or sample payload is not
    // exactly the documented one.
    static bool validate(const WaveformFragment &fragment, QString *error);

private:
    static bool sameKey(const WaveformCaptureKey &a,
                        const WaveformCaptureKey &b) {
        return a.deviceId == b.deviceId && a.sessionId == b.sessionId
            && a.configVersion == b.configVersion
            && a.captureId == b.captureId;
    }

    int findOrCreate(const WaveformCaptureKey &key,
                     const WaveformFragment &fragment);
    void evictOldest();

    QVector<WaveformPendingCapture> m_pending;
    Statistics m_statistics;
    int m_timeoutMs;
    quint32 m_sessionId = 0;
    bool m_sessionKnown = false;
    qint64 m_orderCounter = 0;
};

// Writes a complete waveform as UTF-8 CSV.  The first line is always
// `# MCA_WAVEFORM_CSV_V1`, followed by comment lines carrying the capture
// metadata, then exactly `totalSamples` data rows with the columns
// `sample_index,time_ns,adc_code,overrange`.  Raw ADC codes are written
// unchanged.  The file is written to a temporary sibling and renamed, so a
// failed export never leaves a truncated file behind.
bool writeWaveformCsv(const QString &path, const WaveformCapture &capture,
                      QString *error = nullptr);

} // namespace mca
