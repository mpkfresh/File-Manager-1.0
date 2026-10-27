#include "files/nameordering.h"

#include <QCollator>

namespace filemanager::files {

int naturalNameCompare(QStringView left, QStringView right)
{
    thread_local QCollator foldedCollator = [] {
        QCollator value;
        value.setNumericMode(true);
        value.setCaseSensitivity(Qt::CaseInsensitive);
        return value;
    }();
    thread_local QCollator caseCollator = [] {
        QCollator value;
        value.setNumericMode(true);
        value.setCaseSensitivity(Qt::CaseSensitive);
        return value;
    }();
    const int folded = foldedCollator.compare(left, right);
    if (folded != 0)
        return folded;
    return caseCollator.compare(left, right);
}

} // namespace filemanager::files
