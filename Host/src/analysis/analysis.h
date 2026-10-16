#pragma once
#include "core/types.h"

namespace mca {

class AnalysisService {
public:
    static RoiStats roiStats(const QVector<quint64> &spectrum, int left, int right);
    static FitResult fitGaussian(const QVector<quint64> &spectrum, int left, int right,
                                 int peakCount = 1, bool linearBackground = true,
                                 const QVector<int> &initialCenters = {});
    static Calibration calibrate(const QVector<QPointF> &points, Calibration::Order order, bool *ok = nullptr);
};

} // namespace mca
