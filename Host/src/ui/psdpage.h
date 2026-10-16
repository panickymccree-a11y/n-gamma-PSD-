#pragma once
#include "core/psd.h"
#include <QWidget>
class QDoubleSpinBox;
class QCheckBox;
class QLabel;
class QPushButton;
namespace mca {
class PsdPlot;
class PsdPage : public QWidget {
    Q_OBJECT
public:
    explicit PsdPage(QWidget *parent=nullptr);
    ChannelConfig channelConfig(ChannelConfig shared) const;
    bool validate(QString *error) const;
    void begin(const ChannelConfig &config, bool filtered);
    void addEvents(const QVector<PsdEvent> &events);
    PsdSession *session() { return &m_session; }
    void setRunning(bool running);
    void clear();
    void setTransportStatus(quint64 lost, quint64 dropped);
signals:
    void errorOccurred(const QString &error);
private:
    void updateSettings();
    PsdSession m_session;
    PsdSettings m_settings;
    QDoubleSpinBox *m_total,*m_pre,*m_start,*m_end,*m_xmin,*m_xmax,*m_rmin,*m_rmax;
    QDoubleSpinBox *m_roiMin,*m_roiMax,*m_fitMin,*m_fitMax,*m_threshold;
    QCheckBox *m_direction,*m_fit;
    QLabel *m_status,*m_gates,*m_transport;
    QPushButton *m_record,*m_open;
    PsdPlot *m_plot;
    QString m_recordPath;
    bool m_running=false;
    std::shared_ptr<const PsdSnapshot> m_snapshot;
};
}
