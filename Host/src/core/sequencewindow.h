#pragma once
#include "core/types.h"

namespace mca {
// A slot is retired only when it leaves the 1024-packet repair window.
class SequenceWindow {
public:
    static constexpr quint32 Size = 1024;
    void clear() { m_slots.fill(Empty); m_initialized=false; m_pending=0; }
    bool accept(quint32 sequence, qsizetype events, PacketStats &stats) {
        ++stats.received;
        if (!m_initialized) {
            clear(); m_initialized=true; m_highest=sequence;
            m_slots[sequence % Size]=Seen;
        } else {
            const qint32 ahead=qint32(sequence-m_highest);
            if (ahead>0) {
                if (quint32(ahead)>=Size) {
                    stats.confirmedMissing+=m_pending+quint64(ahead)-Size;
                    m_slots.fill(Pending); m_pending=Size-1;
                    m_slots[sequence % Size]=Seen;
                } else {
                    for (quint32 i=1;i<=quint32(ahead);++i) {
                        auto &slot=m_slots[(m_highest+i)%Size];
                        if (slot==Pending) { ++stats.confirmedMissing; --m_pending; }
                        slot=i==quint32(ahead) ? Seen : Pending;
                        if (slot==Pending) ++m_pending;
                    }
                }
                m_highest=sequence;
            } else {
                const quint32 age=m_highest-sequence;
                if (age>=Size) { ++stats.latePackets; update(stats); return false; }
                auto &slot=m_slots[sequence%Size];
                if (slot==Seen) {
                    ++stats.duplicate; stats.duplicateEvents+=quint64(qMax<qsizetype>(0,events));
                    update(stats); return false;
                }
                ++stats.reordered;
                if (slot==Pending) --m_pending;
                slot=Seen;
            }
        }
        update(stats); return true;
    }
private:
    enum Slot : quint8 { Empty, Pending, Seen };
    std::array<Slot,Size> m_slots{};
    bool m_initialized=false;
    quint32 m_highest=0, m_pending=0;
    void update(PacketStats &s) const {
        s.lastSequence=m_highest; s.hasSequence=m_initialized;
        s.pendingMissing=m_pending; s.lost=s.confirmedMissing;
    }
};
}
