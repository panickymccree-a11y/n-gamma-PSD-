#include "analysis/analysis.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace mca {
namespace {

constexpr double SqrtTwo = 1.4142135623730950488;
constexpr double SqrtTwoPi = 2.5066282746310005024;
constexpr double MinimumSigma = 0.15;
constexpr int MaximumFitIterations = 120;

struct GaussianFitModel {
    int left = 0;
    int right = 0;
    int peakCount = 1;
    bool linearBackground = true;

    int backgroundParameters() const {
        return linearBackground ? 2 : 1;
    }

    int parameterCount() const {
        return backgroundParameters() + peakCount * 3;
    }

    int peakOffset(int peak) const {
        return backgroundParameters() + peak * 3;
    }
};

double gaussianBinFraction(double x, double centroid, double sigma) {
    sigma = qMax(MinimumSigma, sigma);
    const double scale = sigma * SqrtTwo;
    return 0.5 * (std::erf((x + 0.5 - centroid) / scale) -
                  std::erf((x - 0.5 - centroid) / scale));
}

void projectParameters(QVector<double> &parameters,
                       const GaussianFitModel &model) {
    const double span = qMax(1, model.right - model.left);
    parameters[0] = qMax(0.0, parameters[0]);
    if (model.linearBackground) {
        const double endBackground =
            parameters[0] + parameters[1] * span;
        if (endBackground < 0)
            parameters[1] = -parameters[0] / span;
    }
    const double maximumSigma =
        qMax(MinimumSigma, double(model.right - model.left + 1));
    for (int peak = 0; peak < model.peakCount; ++peak) {
        const int offset = model.peakOffset(peak);
        parameters[offset] = qMax(0.0, parameters[offset]);
        parameters[offset + 1] =
            qBound(double(model.left) - 0.5,
                   parameters[offset + 1],
                   double(model.right) + 0.5);
        parameters[offset + 2] =
            qBound(MinimumSigma, parameters[offset + 2],
                   maximumSigma);
    }
}

double modelValue(const QVector<double> &parameters,
                  const GaussianFitModel &model, double x) {
    double value = parameters[0];
    if (model.linearBackground)
        value += parameters[1] * (x - model.left);
    for (int peak = 0; peak < model.peakCount; ++peak) {
        const int offset = model.peakOffset(peak);
        value += parameters[offset] *
                 gaussianBinFraction(x, parameters[offset + 1],
                                     parameters[offset + 2]);
    }
    return qMax(1.0e-9, value);
}

double poissonResidual(double observed, double expected) {
    expected = qMax(1.0e-12, expected);
    double deviance = 0;
    if (observed > 0) {
        deviance = 2.0 * (expected - observed +
                          observed * std::log(observed / expected));
    } else {
        deviance = 2.0 * expected;
    }
    const double magnitude = std::sqrt(qMax(0.0, deviance));
    return expected >= observed ? magnitude : -magnitude;
}

QVector<double> residuals(const QVector<quint64> &spectrum,
                          const QVector<double> &parameters,
                          const GaussianFitModel &model) {
    QVector<double> output;
    output.reserve(model.right - model.left + 1);
    for (int x = model.left; x <= model.right; ++x) {
        output.push_back(poissonResidual(
            double(spectrum[x]), modelValue(parameters, model, x)));
    }
    return output;
}

double squaredNorm(const QVector<double> &values) {
    double result = 0;
    for (double value : values)
        result += value * value;
    return result;
}

bool solveLinearSystem(QVector<QVector<double>> matrix,
                       QVector<double> rightHandSide,
                       QVector<double> *solution) {
    const int size = matrix.size();
    if (!solution || rightHandSide.size() != size)
        return false;
    for (int pivotColumn = 0; pivotColumn < size; ++pivotColumn) {
        int pivotRow = pivotColumn;
        for (int row = pivotColumn + 1; row < size; ++row) {
            if (std::abs(matrix[row][pivotColumn]) >
                std::abs(matrix[pivotRow][pivotColumn]))
                pivotRow = row;
        }
        if (std::abs(matrix[pivotRow][pivotColumn]) < 1.0e-12)
            return false;
        if (pivotRow != pivotColumn) {
            matrix.swapItemsAt(pivotRow, pivotColumn);
            std::swap(rightHandSide[pivotRow],
                      rightHandSide[pivotColumn]);
        }
        const double divisor = matrix[pivotColumn][pivotColumn];
        for (int column = pivotColumn; column < size; ++column)
            matrix[pivotColumn][column] /= divisor;
        rightHandSide[pivotColumn] /= divisor;
        for (int row = 0; row < size; ++row) {
            if (row == pivotColumn)
                continue;
            const double factor = matrix[row][pivotColumn];
            if (factor == 0)
                continue;
            for (int column = pivotColumn; column < size; ++column)
                matrix[row][column] -=
                    factor * matrix[pivotColumn][column];
            rightHandSide[row] -= factor * rightHandSide[pivotColumn];
        }
    }
    *solution = std::move(rightHandSide);
    return true;
}

struct OptimizationResult {
    bool valid = false;
    double objective = std::numeric_limits<double>::infinity();
    QVector<double> parameters;
};

OptimizationResult optimizeGaussian(
    const QVector<quint64> &spectrum, const GaussianFitModel &model,
    QVector<double> parameters) {
    projectParameters(parameters, model);
    QVector<double> currentResiduals =
        residuals(spectrum, parameters, model);
    double currentObjective = squaredNorm(currentResiduals);
    double damping = 1.0e-2;
    bool acceptedStep = false;

    for (int iteration = 0; iteration < MaximumFitIterations;
         ++iteration) {
        const int sampleCount = currentResiduals.size();
        const int parameterCount = parameters.size();
        QVector<QVector<double>> jacobian(
            sampleCount, QVector<double>(parameterCount, 0));

        for (int parameter = 0; parameter < parameterCount;
             ++parameter) {
            const double step =
                1.0e-4 * (std::abs(parameters[parameter]) + 1.0);
            QVector<double> plus = parameters;
            QVector<double> minus = parameters;
            plus[parameter] += step;
            minus[parameter] -= step;
            projectParameters(plus, model);
            projectParameters(minus, model);
            const double denominator =
                plus[parameter] - minus[parameter];
            if (std::abs(denominator) < 1.0e-14)
                continue;
            const QVector<double> plusResiduals =
                residuals(spectrum, plus, model);
            const QVector<double> minusResiduals =
                residuals(spectrum, minus, model);
            for (int sample = 0; sample < sampleCount; ++sample) {
                jacobian[sample][parameter] =
                    (plusResiduals[sample] - minusResiduals[sample]) /
                    denominator;
            }
        }

        QVector<QVector<double>> normal(
            parameters.size(),
            QVector<double>(parameters.size(), 0));
        QVector<double> gradient(parameters.size(), 0);
        for (int row = 0; row < parameters.size(); ++row) {
            for (int sample = 0; sample < currentResiduals.size();
                 ++sample) {
                gradient[row] +=
                    jacobian[sample][row] * currentResiduals[sample];
            }
            for (int column = 0; column < parameters.size(); ++column) {
                for (int sample = 0;
                     sample < currentResiduals.size(); ++sample) {
                    normal[row][column] +=
                        jacobian[sample][row] *
                        jacobian[sample][column];
                }
            }
            normal[row][row] +=
                damping * (normal[row][row] + 1.0e-6);
            gradient[row] = -gradient[row];
        }

        QVector<double> delta;
        if (!solveLinearSystem(normal, gradient, &delta)) {
            damping *= 10.0;
            if (damping > 1.0e12)
                break;
            continue;
        }

        QVector<double> trial = parameters;
        double deltaNorm = 0;
        double parameterNorm = 0;
        for (int parameter = 0; parameter < trial.size();
             ++parameter) {
            trial[parameter] += delta[parameter];
            deltaNorm += delta[parameter] * delta[parameter];
            parameterNorm +=
                parameters[parameter] * parameters[parameter];
        }
        projectParameters(trial, model);
        const QVector<double> trialResiduals =
            residuals(spectrum, trial, model);
        const double trialObjective = squaredNorm(trialResiduals);

        if (std::isfinite(trialObjective) &&
            trialObjective < currentObjective) {
            const double improvement =
                currentObjective - trialObjective;
            parameters = std::move(trial);
            currentResiduals = trialResiduals;
            currentObjective = trialObjective;
            acceptedStep = true;
            damping = qMax(1.0e-12, damping * 0.3);
            if (improvement <=
                    1.0e-9 * (1.0 + currentObjective) ||
                std::sqrt(deltaNorm) <=
                    1.0e-7 * (1.0 + std::sqrt(parameterNorm)))
                break;
        } else {
            damping *= 10.0;
            if (damping > 1.0e12)
                break;
        }
    }

    OptimizationResult result;
    result.valid = acceptedStep && std::isfinite(currentObjective);
    result.objective = currentObjective;
    result.parameters = std::move(parameters);
    return result;
}

QVector<int> initialPeakCenters(const QVector<quint64> &spectrum,
                                int left, int right,
                                const QVector<double> &background,
                                int peakCount) {
    QVector<double> net(right - left + 1, 0);
    for (int x = left; x <= right; ++x)
        net[x - left] =
            qMax(0.0, double(spectrum[x]) - background[x - left]);

    QVector<int> candidates;
    for (int x = left; x <= right; ++x) {
        const double value = net[x - left];
        const double previous =
            x == left ? -1.0 : net[x - left - 1];
        const double next =
            x == right ? -1.0 : net[x - left + 1];
        if (value >= previous && value >= next)
            candidates.push_back(x);
    }
    std::sort(candidates.begin(), candidates.end(),
              [&](int first, int second) {
        return net[first - left] > net[second - left];
    });

    QVector<int> selected;
    for (int candidate : candidates) {
        bool separated = true;
        for (int existing : selected)
            separated &= std::abs(existing - candidate) >= 1;
        if (separated)
            selected.push_back(candidate);
        if (selected.size() == peakCount)
            break;
    }
    for (int x = left;
         selected.size() < peakCount && x <= right; ++x) {
        if (!selected.contains(x))
            selected.push_back(x);
    }
    std::sort(selected.begin(), selected.end());
    return selected;
}

} // namespace

RoiStats AnalysisService::roiStats(const QVector<quint64> &s, int left, int right) {
    RoiStats r; if (s.isEmpty()) return r;
    left = qBound(0, left, s.size() - 1); right = qBound(left, right, s.size() - 1);
    const double y0 = s[left], y1 = s[right]; double weighted = 0, netSum = 0; r.maximumChannel = left;
    for (int x = left; x <= right; ++x) {
        r.gross += s[x]; if (s[x] > s[r.maximumChannel]) r.maximumChannel = x;
        const double bg = y0 + (y1 - y0) * (x - left) / qMax(1, right - left);
        const double net = qMax(0.0, double(s[x]) - bg); weighted += x * net; netSum += net; r.background += bg;
    }
    r.net = double(r.gross) - r.background; r.centroid = netSum > 0 ? weighted / netSum : 0;
    return r;
}

FitResult AnalysisService::fitGaussian(const QVector<quint64> &s, int left, int right,
                                       int peakCount, bool linearBackground, const QVector<int> &initialCenters) {
    FitResult out;
    if (s.isEmpty()) {
        out.message = QStringLiteral("能谱为空");
        return out;
    }
    left = qBound(0, left, s.size() - 1);
    right = qBound(left, right, s.size() - 1);
    peakCount = qBound(1, peakCount, 4);
    GaussianFitModel model{left, right, peakCount,
                           linearBackground};
    const int sampleCount = right - left + 1;
    if (sampleCount <= model.parameterCount()) {
        out.message = QStringLiteral(
            "ROI 仅有 %1 个点，不足以拟合 %2 个峰和%3；"
            "请减少峰数量或扩大 ROI")
            .arg(sampleCount)
            .arg(peakCount)
            .arg(linearBackground ? QStringLiteral("线性背景")
                                  : QStringLiteral("常数背景"));
        return out;
    }

    const int edgeSamples = qMin(2, sampleCount);
    double leftBackground = 0;
    double rightBackground = 0;
    for (int offset = 0; offset < edgeSamples; ++offset) {
        leftBackground += double(s[left + offset]);
        rightBackground += double(s[right - offset]);
    }
    leftBackground /= edgeSamples;
    rightBackground /= edgeSamples;
    if (!linearBackground) {
        const auto minimum = std::min_element(
            s.cbegin() + left, s.cbegin() + right + 1);
        leftBackground = rightBackground = double(*minimum);
    }

    QVector<double> background(sampleCount, leftBackground);
    if (linearBackground) {
        for (int x = left; x <= right; ++x) {
            background[x - left] =
                leftBackground +
                (rightBackground - leftBackground) *
                    double(x - left) / qMax(1, right - left);
        }
    }

    double totalNet = 0;
    double maximumNet = 0;
    for (int x = left; x <= right; ++x) {
        const double net =
            qMax(0.0, double(s[x]) - background[x - left]);
        totalNet += net;
        maximumNet = qMax(maximumNet, net);
    }
    if (totalNet <= 0 || maximumNet <= 0) {
        out.message = QStringLiteral("ROI 内未找到高于背景的有效峰");
        return out;
    }

    QVector<int> centers = initialPeakCenters(s, left, right, background, peakCount);
    if (initialCenters.size()==peakCount && std::all_of(initialCenters.cbegin(),initialCenters.cend(),
        [left,right](int x){return x>=left && x<=right;})) centers=initialCenters;
    const std::array<double, 3> sigmaStarts{
        0.35, 0.8,
        qMax(1.5, double(sampleCount) /
                      qMax(6, peakCount * 6))
    };
    OptimizationResult best;
    for (double sigmaStart : sigmaStarts) {
        QVector<double> parameters(model.parameterCount(), 0);
        parameters[0] = leftBackground;
        if (linearBackground) {
            parameters[1] =
                (rightBackground - leftBackground) /
                qMax(1, right - left);
        }
        for (int peak = 0; peak < peakCount; ++peak) {
            const int offset = model.peakOffset(peak);
            const int center = centers.value(
                peak, left + (peak + 1) * sampleCount /
                                (peakCount + 1));
            const double centerNet =
                qMax(0.0, double(s[center]) -
                              background[center - left]);
            parameters[offset + 1] = center;
            parameters[offset + 2] = sigmaStart;
            parameters[offset] = qMax(
                totalNet / peakCount,
                centerNet / qMax(
                    1.0e-6,
                    gaussianBinFraction(center, center,
                                        sigmaStart)));
        }
        const OptimizationResult candidate =
            optimizeGaussian(s, model, parameters);
        if (candidate.valid &&
            candidate.objective < best.objective)
            best = candidate;
    }
    if (!best.valid) {
        out.message = QStringLiteral(
            "Gaussian 拟合未收敛；请调整 ROI 或减少峰数量");
        return out;
    }

    out.background0 = best.parameters[0];
    out.background1 =
        linearBackground ? best.parameters[1] : 0;
    for (int peak = 0; peak < peakCount; ++peak) {
        const int offset = model.peakOffset(peak);
        PeakResult result;
        result.area = best.parameters[offset];
        result.centroid = best.parameters[offset + 1];
        result.sigma = best.parameters[offset + 2];
        result.fwhm = 2.354820045 * result.sigma;
        result.height = result.area *
            gaussianBinFraction(result.centroid,
                                result.centroid,
                                result.sigma);
        if (!std::isfinite(result.area) ||
            !std::isfinite(result.centroid) ||
            !std::isfinite(result.sigma) ||
            result.area <= 0) {
            out.message = QStringLiteral(
                "Gaussian 拟合得到无效参数；请调整 ROI");
            out.peaks.clear();
            return out;
        }
        out.peaks.push_back(result);
    }
    std::sort(out.peaks.begin(), out.peaks.end(),
              [](const PeakResult &first,
                 const PeakResult &second) {
        return first.centroid < second.centroid;
    });

    double pearsonChiSquare = 0;
    for (int x = left; x <= right; ++x) {
        const double expected =
            modelValue(best.parameters, model, x);
        const double difference = double(s[x]) - expected;
        pearsonChiSquare +=
            difference * difference / qMax(1.0e-9, expected);
    }
    const int degreesOfFreedom =
        sampleCount - model.parameterCount();
    out.reducedChiSquare =
        pearsonChiSquare / qMax(1, degreesOfFreedom);

    const int curveSegments =
        qMin(4096, qMax(16, (right - left) * 16));
    out.curve.reserve(curveSegments + 1);
    for (int point = 0; point <= curveSegments; ++point) {
        const double x = left +
            double(right - left) * point / curveSegments;
        out.curve.push_back(
            {x, modelValue(best.parameters, model, x)});
    }
    out.valid = true;
    out.message = QStringLiteral("拟合完成");
    return out;
}

Calibration AnalysisService::calibrate(const QVector<QPointF> &p, Calibration::Order order, bool *ok) {
    Calibration c; c.order=order; const int degree=int(order); const int n=p.size();
    if (n < degree+1) { if(ok)*ok=false; return c; }
    double a[3][4]{};
    for(int row=0;row<=degree;++row){ for(int col=0;col<=degree;++col)
        for(const auto&v:p) a[row][col]+=std::pow(v.x(),row+col);
        for(const auto&v:p) a[row][degree+1]+=v.y()*std::pow(v.x(),row); }
    for(int i=0;i<=degree;++i){ int pivot=i; for(int r=i+1;r<=degree;++r) if(std::abs(a[r][i])>std::abs(a[pivot][i]))pivot=r;
        for(int k=i;k<=degree+1;++k)std::swap(a[i][k],a[pivot][k]); if(std::abs(a[i][i])<1e-12){if(ok)*ok=false;return c;}
        const double d=a[i][i]; for(int k=i;k<=degree+1;++k)a[i][k]/=d;
        for(int r=0;r<=degree;++r)if(r!=i){const double f=a[r][i];for(int k=i;k<=degree+1;++k)a[r][k]-=f*a[i][k];}}
    c.coefficients.fill(0,3); for(int i=0;i<=degree;++i)c.coefficients[i]=a[i][degree+1]; if(ok)*ok=true; return c;
}

} // namespace mca
