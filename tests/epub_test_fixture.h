#pragma once

#include <QByteArray>
#include <QString>

namespace bookTestFixtures {
bool writeEpub(const QString& path, const QByteArray& containerXml,
    const QByteArray& packageXml);
}