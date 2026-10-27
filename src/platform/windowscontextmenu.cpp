#include "platform/windowscontextmenu.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>

namespace filemanager::platform {

ContextMenuHostResult showWindowsContextMenuIsolated(const QStringList& absolutePaths,
    int screenX, int screenY, int timeoutMilliseconds, const QString& helperExecutable)
{
    if (absolutePaths.isEmpty() || absolutePaths.size() > 32)
        return {false, false, 0, "Windows commands require between 1 and 32 selected items."};
    if (timeoutMilliseconds < 1000 || timeoutMilliseconds > 300000)
        return {false, false, 0, "The Windows command timeout is outside policy."};
    QStringList arguments{"--popup", QString::number(screenX), QString::number(screenY)};
    qsizetype commandBytes = 0;
    for (const QString& path : absolutePaths) {
        const QFileInfo info(path);
        if (!info.isAbsolute() || !info.exists())
            return {false, false, 0, "A selected item is unavailable or not an absolute path."};
        const QString absolute = info.absoluteFilePath();
        commandBytes += absolute.size() * 2;
        if (commandBytes > 24 * 1024)
            return {false, false, 0, "The selected paths exceed the isolated command-line limit."};
        arguments.append(absolute);
    }
    const QString executable = helperExecutable.isEmpty()
        ? QDir(QCoreApplication::applicationDirPath()).filePath(
#ifdef Q_OS_WIN
            "FileManagerContextMenuHost.exe")
#else
            "FileManagerContextMenuHost")
#endif
        : helperExecutable;
    QProcess helper;
    helper.start(executable, arguments, QIODevice::ReadOnly);
    if (!helper.waitForStarted(1000))
        return {false, false, 0, "The isolated Windows command helper could not start."};
    if (!helper.waitForFinished(timeoutMilliseconds)) {
        helper.kill(); helper.waitForFinished(1000);
        return {false, false, 0, "The isolated Windows command helper timed out."};
    }
    const int code = helper.exitCode();
    if (helper.exitStatus() != QProcess::NormalExit)
        return {false, false, code, "The isolated Windows command helper crashed."};
    if (code == 10)
        return {true, true, code, {}};
    if (code != 0) {
        const QString detail = QString::fromLocal8Bit(helper.readAllStandardError()).trimmed().left(4096);
        return {false, false, code,
            detail.isEmpty() ? "The isolated Windows command helper failed." : detail};
    }
    return {true, false, 0, {}};
}

} // namespace filemanager::platform
