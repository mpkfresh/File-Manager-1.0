#include <QCoreApplication>
#include <QDir>

#include <cstdio>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shlobj_core.h>
#include <shobjidl_core.h>

namespace {

IContextMenu2* context2 = nullptr;
IContextMenu3* context3 = nullptr;

LRESULT CALLBACK menuWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (context3 && (message == WM_INITMENUPOPUP || message == WM_DRAWITEM
            || message == WM_MEASUREITEM || message == WM_MENUCHAR)) {
        LRESULT result = 0;
        if (SUCCEEDED(context3->HandleMenuMsg2(message, wParam, lParam, &result)))
            return result;
    } else if (context2 && (message == WM_INITMENUPOPUP || message == WM_DRAWITEM
            || message == WM_MEASUREITEM)) {
        if (SUCCEEDED(context2->HandleMenuMsg(message, wParam, lParam)))
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void printError(const char* message, HRESULT code)
{
    std::fprintf(stderr, "%s (0x%08lx)\n", message, static_cast<unsigned long>(code));
}

} // namespace
#endif

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() == 2 && arguments.at(1) == "--validate")
        return 0;
#ifndef Q_OS_WIN
    Q_UNUSED(arguments);
    return 2;
#else
    const bool probe = arguments.size() >= 3 && arguments.at(1) == "--probe";
    const bool popup = arguments.size() >= 5 && arguments.at(1) == "--popup";
    if ((!probe && !popup) || arguments.size() > (probe ? 34 : 36))
        return 2;
    bool xOk = false;
    bool yOk = false;
    const int x = popup ? arguments.at(2).toInt(&xOk) : 0;
    const int y = popup ? arguments.at(3).toInt(&yOk) : 0;
    if (probe) { xOk = true; yOk = true; }
    if (!xOk || !yOk)
        return 3;
    const HRESULT initialized = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized)) {
        printError("Could not initialize the isolated Shell apartment", initialized);
        return 4;
    }

    std::vector<PIDLIST_ABSOLUTE> absoluteItems;
    std::vector<PCUITEMID_CHILD> childItems;
    PIDLIST_ABSOLUTE commonParent = nullptr;
    IShellFolder* parentFolder = nullptr;
    IContextMenu* context = nullptr;
    HMENU menu = nullptr;
    HWND window = nullptr;
    int resultCode = 5;
    constexpr UINT firstCommand = 1;
    constexpr UINT lastCommand = 0x7fff;

    const int firstPath = probe ? 2 : 4;
    for (int index = firstPath; index < arguments.size(); ++index) {
        PIDLIST_ABSOLUTE absolute = nullptr;
        const QString path = QDir::toNativeSeparators(arguments.at(index));
        IShellItem* shellItem = nullptr;
        HRESULT resolved = SHCreateItemFromParsingName(
            reinterpret_cast<LPCWSTR>(path.utf16()), nullptr, IID_PPV_ARGS(&shellItem));
        if (SUCCEEDED(resolved) && shellItem)
            resolved = SHGetIDListFromObject(shellItem, &absolute);
        if (shellItem)
            shellItem->Release();
        if (FAILED(resolved) || !absolute) {
            printError("Windows could not resolve a selected item", resolved);
            goto cleanup;
        }
        absoluteItems.push_back(absolute);
        PIDLIST_ABSOLUTE parent = ILCloneFull(absolute);
        if (!parent || !ILRemoveLastID(parent)) {
            if (parent) CoTaskMemFree(parent);
            std::fprintf(stderr, "Could not resolve the selected item's parent.\n");
            goto cleanup;
        }
        if (!commonParent)
            commonParent = parent;
        else {
            const bool same = ILIsEqual(commonParent, parent) != FALSE;
            CoTaskMemFree(parent);
            if (!same) {
                std::fprintf(stderr, "Windows commands require selections from one folder.\n");
                goto cleanup;
            }
        }
    }

    {
        PCUITEMID_CHILD firstChild = nullptr;
        HRESULT result = SHBindToParent(absoluteItems.front(), IID_PPV_ARGS(&parentFolder), &firstChild);
        if (FAILED(result) || !parentFolder) {
            printError("Windows could not bind the selected folder", result);
            goto cleanup;
        }
        for (const auto absolute : absoluteItems) {
            PCUITEMID_CHILD child = nullptr;
            IShellFolder* checkFolder = nullptr;
            result = SHBindToParent(absolute, IID_PPV_ARGS(&checkFolder), &child);
            if (FAILED(result) || !checkFolder || !child) {
                if (checkFolder) checkFolder->Release();
                printError("Windows could not bind a selected item", result);
                goto cleanup;
            }
            checkFolder->Release();
            childItems.push_back(child);
        }
        result = parentFolder->GetUIObjectOf(nullptr, static_cast<UINT>(childItems.size()),
            childItems.data(), IID_IContextMenu, nullptr, reinterpret_cast<void**>(&context));
        if (FAILED(result) || !context) {
            printError("Windows did not provide commands for this selection", result);
            goto cleanup;
        }
    }

    menu = CreatePopupMenu();
    if (!menu) {
        std::fprintf(stderr, "Could not create the isolated Windows command menu.\n");
        goto cleanup;
    }
    {
        const HRESULT result = context->QueryContextMenu(menu, 0, firstCommand, lastCommand, CMF_EXPLORE);
        if (FAILED(result)) {
            printError("A Windows command extension failed while building its menu", result);
            goto cleanup;
        }
    }
    if (probe) {
        resultCode = 0;
        goto cleanup;
    }
    context->QueryInterface(IID_PPV_ARGS(&context3));
    context->QueryInterface(IID_PPV_ARGS(&context2));

    {
        const wchar_t* className = L"FileManagerContextMenuHostWindow";
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = menuWindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = className;
        RegisterClassW(&windowClass);
        window = CreateWindowExW(WS_EX_TOOLWINDOW, className, L"", WS_POPUP,
            x, y, 1, 1, nullptr, nullptr, windowClass.hInstance, nullptr);
        if (!window) {
            std::fprintf(stderr, "Could not create the isolated menu owner.\n");
            goto cleanup;
        }
    }
    SetForegroundWindow(window);
    {
        const UINT selected = TrackPopupMenuEx(menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, x, y, window, nullptr);
        PostMessageW(window, WM_NULL, 0, 0);
        if (selected == 0) {
            resultCode = 10;
            goto cleanup;
        }
        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
        invoke.hwnd = window;
        invoke.lpVerb = MAKEINTRESOURCEA(selected - firstCommand);
        invoke.lpVerbW = MAKEINTRESOURCEW(selected - firstCommand);
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = POINT{x, y};
        const HRESULT invoked = context->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
        if (FAILED(invoked)) {
            printError("The selected Windows command failed", invoked);
            goto cleanup;
        }
    }
    resultCode = 0;

cleanup:
    if (window) DestroyWindow(window);
    if (context3) { context3->Release(); context3 = nullptr; }
    if (context2) { context2->Release(); context2 = nullptr; }
    if (menu) DestroyMenu(menu);
    if (context) context->Release();
    if (parentFolder) parentFolder->Release();
    if (commonParent) CoTaskMemFree(commonParent);
    for (const auto item : absoluteItems) CoTaskMemFree(item);
    CoUninitialize();
    return resultCode;
#endif
}
