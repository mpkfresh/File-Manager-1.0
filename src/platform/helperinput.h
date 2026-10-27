#pragma once

#include <QtGlobal>
#include <array>
#include <cstdio>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace filemanager::platform {

// Returns available bytes, zero on EOF, or -1 on failure. Do not mix with buffered stdin.
inline qint64 readHelperInput(char* destination, qsizetype maximum)
{
    if (!destination || maximum <= 0)
        return -1;
#ifdef Q_OS_WIN
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (!input || input == INVALID_HANDLE_VALUE)
        return -1;
    if (GetFileType(input) == FILE_TYPE_PIPE) {
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent)
            return -1;
        const DWORD amount = static_cast<DWORD>(qMin<qsizetype>(maximum, 65536));
        const BOOL started = ReadFile(input, destination, amount, nullptr, &operation);
        DWORD error = started ? ERROR_SUCCESS : GetLastError();
        DWORD received = 0;
        if (started || error == ERROR_IO_PENDING) {
            // Caller-owned process deadlines bound the wait. Keep the destination
            // and OVERLAPPED alive until the kernel has completed the operation.
            error = GetOverlappedResult(input, &operation, &received, TRUE)
                ? ERROR_SUCCESS : GetLastError();
        }
        CloseHandle(operation.hEvent);
        if (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF)
            return 0;
        return error == ERROR_SUCCESS && received <= amount ? static_cast<qint64>(received) : -1;
    }
#endif
    const size_t received = std::fread(destination, 1, static_cast<size_t>(maximum), stdin);
    return received ? static_cast<qint64>(received) : std::ferror(stdin) ? -1 : 0;
}

class HelperInputReader final
{
public:
    // Byte values are unsigned; -1 means EOF and -2 means an input failure.
    int next()
    {
        if (offset_ == size_) {
            const qint64 received = readHelperInput(buffer_.data(), buffer_.size());
            if (received <= 0)
                return received == 0 ? -1 : -2;
            offset_ = 0;
            size_ = static_cast<qsizetype>(received);
        }
        return static_cast<unsigned char>(buffer_[offset_++]);
    }

private:
    std::array<char, 4096> buffer_{};
    qsizetype offset_ = 0;
    qsizetype size_ = 0;
};

} // namespace filemanager::platform
