#pragma once

#include <QByteArrayView>
#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace filemanager::platform {

// Complete serialized replies: do not mix with buffered stdout writes in the same mode.
inline bool writeHelperReply(QByteArrayView reply, FILE* destination = stdout)
{
    if (destination != stdout && destination != stderr)
        return false;
#ifdef Q_OS_WIN
    const HANDLE output = GetStdHandle(destination == stderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (!output || output == INVALID_HANDLE_VALUE)
        return false;
    if (GetFileType(output) == FILE_TYPE_PIPE) {
        // QProcess supplies overlapped pipe handles on Windows. C stdio does not
        // supply OVERLAPPED or retain its buffer until a pending write completes.
        // An event also works for synchronous pipes (e.g. the deployment runner).
        const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event)
            return false;
        bool success = true;
        qsizetype offset = 0;
        while (offset < reply.size()) {
            OVERLAPPED operation{};
            operation.hEvent = event;
            const DWORD amount = static_cast<DWORD>(qMin<qsizetype>(65536, reply.size() - offset));
            const BOOL started = WriteFile(output, reply.data() + offset, amount, nullptr, &operation);
            if (!started && GetLastError() != ERROR_IO_PENDING) {
                success = false;
                break;
            }
            DWORD written = 0;
            // The caller's existing process deadline bounds this helper. Never
            // release the reply buffer or OVERLAPPED while the OS still uses it.
            if (!GetOverlappedResult(output, &operation, &written, TRUE)
                    || written == 0 || written > amount) {
                success = false;
                break;
            }
            offset += written;
        }
        CloseHandle(event);
        return success;
    }
#endif
    return std::fwrite(reply.data(), 1, static_cast<size_t>(reply.size()), destination)
            == static_cast<size_t>(reply.size()) && std::fflush(destination) == 0;
}

} // namespace filemanager::platform
