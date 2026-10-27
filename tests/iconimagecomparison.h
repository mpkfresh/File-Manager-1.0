#pragma once

#include <QImage>
#include <cstdlib>

namespace filemanager::tests {

inline bool sameVisibleImage(const QImage& expected, const QImage& actual)
{
    if (expected.isNull() || actual.isNull() || expected.size() != actual.size())
        return false;
    const QImage left = expected.convertToFormat(QImage::Format_RGBA8888);
    const QImage right = actual.convertToFormat(QImage::Format_RGBA8888);
    // Native icon extraction and Qt's premultiplied-alpha conversion can
    // differ by one or two RGB levels while preserving visible geometry.
    // Alpha and any larger visible color difference remain exact failures.
    for (int y = 0; y < left.height(); ++y) {
        const uchar* a = left.constScanLine(y);
        const uchar* b = right.constScanLine(y);
        for (int x = 0; x < left.width(); ++x) {
            const int offset = x * 4;
            if (a[offset + 3] != b[offset + 3])
                return false;
            if (a[offset + 3] != 0) {
                for (int channel = 0; channel < 3; ++channel) {
                    if (std::abs(int(a[offset + channel]) - int(b[offset + channel])) > 2)
                        return false;
                }
            }
        }
    }
    return true;
}

} // namespace filemanager::tests
