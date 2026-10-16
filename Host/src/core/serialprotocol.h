#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVector>
#include <array>
#include <optional>

namespace mca {

constexpr quint32 SerialMagic = 0x4d435332u; // MCS2
constexpr quint8 SerialVersion = 2;
constexpr quint32 SerialFirmwareVersion = 0x0002'0600u;
constexpr int SerialHeaderBytes = 16;
constexpr int SerialCrcBytes = 4;
constexpr int SerialMaximumPayload = 1064;
constexpr int SerialSpectrumHeaderBytes = 40;
constexpr int SerialBaudRate = 500'000;

enum class SerialMessageType : quint8 {
    HelloRequest = 0x01,
    ControlRequest = 0x02,
    Ack = 0x03,
    HelloResponse = 0x81,
    ControlResponse = 0x82,
    Status = 0x83,
    Spectrum = 0x84,
    Error = 0x85
};

struct SerialFrame {
    SerialMessageType type = SerialMessageType::Error;
    quint8 flags = 0;
    quint32 sequence = 0;
    QByteArray payload;
};

struct SerialHello {
    quint32 baudRate = 0;
    quint16 binCount = 0;
    quint8 peakShift = 0;
    quint8 integralShift = 0;
    quint16 chunkBins = 0;
    quint32 sampleRateHz = 0;
    quint16 featureFlags = 0;
    quint32 firmwareVersion = 0;
};

struct SerialStatus {
    quint8 formatVersion = 0;
    bool running = false;
    bool baselineReady = false;
    bool baselineAutomatic = false;
    bool frontendInitDone = false;
    bool rangeBusy = false;
    bool filterEnabled = false;
    bool peakInterpolationEnabled = false;
    bool externalArmed = false;
    bool externalWindowActive = false;
    quint8 baselineState = 0;
    quint8 mode = 0;
    quint32 configVersion = 0;
    quint32 outputEvents = 0;
    quint32 rejectedEvents = 0;
    quint32 droppedEvents = 0;
    quint32 crcErrors = 0;
    quint32 frameErrors = 0;
    quint32 retries = 0;
    quint16 baseline = 0;
    quint16 manualBaseline = 0;
    quint16 recoveryTolerance = 0;
    quint8 actualRange = 0;
    quint32 externalTriggerCount = 0;
    quint64 timestamp = 0;
};

struct SerialSpectrumChunk {
    quint32 session = 0;
    quint32 configVersion = 0;
    quint32 epoch = 0;
    quint8 kind = 0;
    quint8 flags = 0;
    quint8 binShift = 0;
    quint16 totalBins = 0;
    quint16 startBin = 0;
    quint16 binCount = 0;
    quint16 chunkIndex = 0;
    quint16 chunkTotal = 0;
    quint64 intervalTicks = 0;
    quint32 intervalEvents = 0;
    quint32 droppedEvents = 0;
    QVector<quint32> counts;
};

class SerialFrameCodec {
public:
    static quint32 crc32(const QByteArray &bytes);
    static QByteArray encode(SerialMessageType type, quint32 sequence,
                             const QByteArray &payload = {}, quint8 flags = 0);
    QVector<SerialFrame> feed(const QByteArray &bytes, QStringList *errors = nullptr);
    void clear();
private:
    QByteArray m_buffer;
};

bool decodeSerialHello(const QByteArray &payload, SerialHello &hello,
                       QString *error = nullptr);
bool decodeSerialStatus(const QByteArray &payload, SerialStatus &status,
                        QString *error = nullptr);
bool decodeSerialSpectrum(const QByteArray &payload, SerialSpectrumChunk &chunk,
                          QString *error = nullptr);
QByteArray makeSerialAck(quint32 frameSequence,
                         const SerialSpectrumChunk &chunk);

// Collects both kinds for one epoch and publishes only after all chunks are
// present.  Returned vectors are deltas and must be added to the live 64-bit
// spectra by the acquisition worker.
class SerialSpectrumAssembler {
public:
    struct Epoch {
        quint32 session = 0;
        quint32 epoch = 0;
        quint8 peakShift = 0;
        quint8 integralShift = 0;
        quint64 intervalTicks = 0;
        quint32 intervalEvents = 0;
        quint32 droppedEvents = 0;
        quint8 flags = 0;
        QVector<quint32> peak;
        QVector<quint32> integral;
    };
    std::optional<Epoch> add(const SerialSpectrumChunk &chunk,
                             QString *error = nullptr);
    void clear();
private:
    struct Partial {
        Epoch epoch;
        QVector<bool> peakChunks;
        QVector<bool> integralChunks;
    };
    QHash<quint64, Partial> m_partial;
};

} // namespace mca
