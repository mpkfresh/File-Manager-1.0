#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QVector>

namespace filemanager::integration
{

struct KaiProposedFile
{
    QString relativePath;
    QByteArray bytes;
    QByteArray sha256;
};

struct KaiHandoffRequest
{
    QVector<KaiProposedFile> files;
};

// Parses an untrusted, create-only request. It conveys data, never authority:
// a receiver must separately obtain a folder-scoped grant, recheck the target
// and exact bytes, show a final preview, and require explicit confirmation.
bool parseKaiHandoffRequest(QByteArrayView wire, KaiHandoffRequest* request,
                            QString* error = nullptr);

} // namespace filemanager::integration
