#pragma once

#include "core/types.h"
#include <QString>

namespace mca {

class SpectrumFile {
public:
    static bool writeV1(const QString &path, const SpectrumSnapshot &snapshot,
                        QString *error = nullptr);
    static std::shared_ptr<const SpectrumSnapshot>
    readV1(const QString &path, QString *error = nullptr);
};

} // namespace mca

