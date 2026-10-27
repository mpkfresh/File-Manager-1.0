#include "search/windowssearch.h"

#include <QDir>
#include <QFileInfo>
#include <QUrl>

#ifdef Q_OS_WIN
#include <windows.h>
#include <msdasc.h>
#include <oledb.h>
#include <oledberr.h>
#include <searchapi.h>
#include <wrl/client.h>
#endif

namespace filemanager::search {
namespace {

#ifdef Q_OS_WIN
using Microsoft::WRL::ComPtr;

struct ComApartment final
{
    explicit ComApartment(bool ownsInitialization) : ownsInitialization(ownsInitialization) {}
    ~ComApartment() { if (ownsInitialization) CoUninitialize(); }
    bool ownsInitialization = false;
};

QString sqlLiteral(QString value)
{
    value.replace(QLatin1Char('\''), QStringLiteral("''"));
    return value;
}

QString fuzzyLikePattern(const QString& name)
{
    QString pattern = "%";
    for (const QChar character : name.trimmed()) {
        if (character == '[')
            pattern += QStringLiteral("[[]");
        else if (character == '%')
            pattern += QStringLiteral("[%]");
        else if (character == '_')
            pattern += QStringLiteral("[_]");
        else if (character == QLatin1Char('\''))
            pattern += QStringLiteral("''");
        else
            pattern += character;
        pattern += '%';
    }
    return pattern;
}

WindowsSearchResult querySta(const QString& root, const QString& queryText,
    int maximumCandidates, bool advancedQuery, const std::atomic_bool* cancellation)
{
    WindowsSearchResult result;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        result.diagnostic = QStringLiteral("COM initialization failed (0x%1).")
            .arg(static_cast<quint32>(initialized), 8, 16, QLatin1Char('0'));
        return result;
    }
    ComApartment apartment(SUCCEEDED(initialized));
    if (cancellation && cancellation->load()) {
        result.cancelled = true;
        return result;
    }

    ComPtr<ISearchManager> manager;
    HRESULT status = CoCreateInstance(CLSID_CSearchManager, nullptr, CLSCTX_LOCAL_SERVER,
        IID_PPV_ARGS(&manager));
    ComPtr<ISearchCatalogManager> catalog;
    if (SUCCEEDED(status))
        status = manager->GetCatalog(L"SystemIndex", &catalog);
    ComPtr<ISearchCrawlScopeManager> scopeManager;
    if (SUCCEEDED(status))
        status = catalog->GetCrawlScopeManager(&scopeManager);
    const QString scopeUrl = QUrl::fromLocalFile(
        QDir(root).absolutePath() + QLatin1Char('/')).toString(QUrl::FullyEncoded);
    BOOL included = FALSE;
    if (SUCCEEDED(status)) {
        status = scopeManager->IncludedInCrawlScope(
            reinterpret_cast<LPCWSTR>(scopeUrl.utf16()), &included);
    }
    if (FAILED(status) || !included) {
        result.diagnostic = FAILED(status)
            ? QStringLiteral("Windows Search scope detection failed (0x%1).")
                .arg(static_cast<quint32>(status), 8, 16, QLatin1Char('0'))
            : QStringLiteral("The folder is not included in Windows Search.");
        return result;
    }

    ComPtr<ISearchQueryHelper> helper;
    status = catalog->GetQueryHelper(&helper);
    QString sql;
    LPWSTR generatedSql = nullptr;
    if (SUCCEEDED(status) && advancedQuery) {
        status = helper->put_QuerySyntax(SEARCH_ADVANCED_QUERY_SYNTAX);
        if (SUCCEEDED(status))
            status = helper->put_QuerySelectColumns(L"System.ItemPathDisplay");
        const QString restriction = QStringLiteral("SCOPE='%1'").arg(sqlLiteral(scopeUrl));
        if (SUCCEEDED(status)) {
            status = helper->put_QueryWhereRestrictions(
                reinterpret_cast<LPCWSTR>(restriction.utf16()));
        }
        if (SUCCEEDED(status))
            status = helper->put_QueryMaxResults(maximumCandidates);
        if (SUCCEEDED(status)) {
            status = helper->GenerateSQLFromUserQuery(
                reinterpret_cast<LPCWSTR>(queryText.utf16()), &generatedSql);
        }
        if (SUCCEEDED(status) && generatedSql)
            sql = QString::fromWCharArray(generatedSql);
        if (generatedSql)
            CoTaskMemFree(generatedSql);
        if (FAILED(status) || sql.isEmpty()) {
            result.diagnostic = QStringLiteral("Windows Search could not parse the AQS query (0x%1).")
                .arg(static_cast<quint32>(status), 8, 16, QLatin1Char('0'));
            return result;
        }
    }
    LPWSTR connectionString = nullptr;
    if (SUCCEEDED(status))
        status = helper->get_ConnectionString(&connectionString);
    if (FAILED(status) || !connectionString) {
        result.diagnostic = QStringLiteral("Windows Search connection metadata failed (0x%1).")
            .arg(static_cast<quint32>(status), 8, 16, QLatin1Char('0'));
        if (connectionString)
            CoTaskMemFree(connectionString);
        return result;
    }

    ComPtr<IDataInitialize> dataInitialize;
    status = CoCreateInstance(CLSID_MSDAINITIALIZE, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dataInitialize));
    ComPtr<IDBInitialize> database;
    if (SUCCEEDED(status)) {
        status = dataInitialize->GetDataSource(nullptr, CLSCTX_INPROC_SERVER,
            connectionString, __uuidof(IDBInitialize),
            reinterpret_cast<IUnknown**>(database.GetAddressOf()));
    }
    CoTaskMemFree(connectionString);
    if (SUCCEEDED(status))
        status = database->Initialize();
    ComPtr<IDBCreateSession> sessionFactory;
    if (SUCCEEDED(status))
        status = database.As(&sessionFactory);
    ComPtr<IDBCreateCommand> commandFactory;
    if (SUCCEEDED(status))
        status = sessionFactory->CreateSession(nullptr, __uuidof(IDBCreateCommand),
            reinterpret_cast<IUnknown**>(commandFactory.GetAddressOf()));
    ComPtr<ICommandText> command;
    if (SUCCEEDED(status))
        status = commandFactory->CreateCommand(nullptr, __uuidof(ICommandText),
            reinterpret_cast<IUnknown**>(command.GetAddressOf()));
    if (!advancedQuery) {
        sql = QStringLiteral(
            "SELECT TOP %1 System.ItemPathDisplay FROM SYSTEMINDEX "
            "WHERE SCOPE='%2' AND System.ItemNameDisplay LIKE '%3'")
            .arg(maximumCandidates)
            .arg(sqlLiteral(scopeUrl), fuzzyLikePattern(queryText));
    }
    if (SUCCEEDED(status)) {
        status = command->SetCommandText(DBGUID_DEFAULT,
            reinterpret_cast<LPCOLESTR>(sql.utf16()));
    }
    ComPtr<IRowset> rowset;
    if (SUCCEEDED(status)) {
        DBROWCOUNT affected = 0;
        ComPtr<IUnknown> rows;
        status = command->Execute(nullptr, IID_IRowset, nullptr, &affected, &rows);
        if (SUCCEEDED(status))
            status = rows.As(&rowset);
    }
    ComPtr<IAccessor> accessor;
    if (SUCCEEDED(status))
        status = rowset.As(&accessor);

    constexpr DBLENGTH MaximumPathBytes = 65536;
    const DBBYTEOFFSET lengthOffset = MaximumPathBytes;
    const DBBYTEOFFSET statusOffset = lengthOffset + sizeof(DBLENGTH);
    const DBLENGTH rowBytes = statusOffset + sizeof(DBSTATUS);
    DBBINDING binding{};
    binding.iOrdinal = 1;
    binding.obValue = 0;
    binding.obLength = lengthOffset;
    binding.obStatus = statusOffset;
    binding.dwPart = DBPART_VALUE | DBPART_LENGTH | DBPART_STATUS;
    binding.dwMemOwner = DBMEMOWNER_CLIENTOWNED;
    binding.eParamIO = DBPARAMIO_NOTPARAM;
    binding.cbMaxLen = MaximumPathBytes;
    binding.wType = DBTYPE_WSTR;
    HACCESSOR rowAccessor = 0;
    if (SUCCEEDED(status))
        status = accessor->CreateAccessor(DBACCESSOR_ROWDATA, 1, &binding,
            rowBytes, &rowAccessor, nullptr);
    if (FAILED(status)) {
        result.diagnostic = QStringLiteral("Windows Search query failed (0x%1).")
            .arg(static_cast<quint32>(status), 8, 16, QLatin1Char('0'));
        if (database)
            database->Uninitialize();
        return result;
    }

    result.usedIndex = true;
    QByteArray row(static_cast<qsizetype>(rowBytes), Qt::Uninitialized);
    while (!cancellation || !cancellation->load()) {
        DBCOUNTITEM obtained = 0;
        HROW* handles = nullptr;
        status = rowset->GetNextRows(DB_NULL_HCHAPTER, 0, 1, &obtained, &handles);
        if (status == DB_S_ENDOFROWSET || obtained == 0)
            break;
        if (FAILED(status)) {
            result.usedIndex = false;
            result.paths.clear();
            result.diagnostic = QStringLiteral("Windows Search row retrieval failed (0x%1).")
                .arg(static_cast<quint32>(status), 8, 16, QLatin1Char('0'));
            break;
        }
        row.fill(0);
        const HRESULT dataStatus = rowset->GetData(handles[0], rowAccessor, row.data());
        const auto fieldStatus = *reinterpret_cast<const DBSTATUS*>(row.constData() + statusOffset);
        const auto fieldLength = *reinterpret_cast<const DBLENGTH*>(row.constData() + lengthOffset);
        if (SUCCEEDED(dataStatus) && fieldStatus == DBSTATUS_S_OK
                && fieldLength > 0 && fieldLength < MaximumPathBytes) {
            result.paths.append(QString::fromWCharArray(
                reinterpret_cast<const wchar_t*>(row.constData()),
                static_cast<qsizetype>(fieldLength / sizeof(wchar_t))));
        }
        rowset->ReleaseRows(obtained, handles, nullptr, nullptr, nullptr);
        CoTaskMemFree(handles);
    }
    result.cancelled = cancellation && cancellation->load();
    accessor->ReleaseAccessor(rowAccessor, nullptr);
    if (database)
        database->Uninitialize();
    return result;
}
#endif

} // namespace

WindowsSearchResult WindowsSearch::query(const QString& root, const QString& name,
    int maximumCandidates, const std::atomic_bool* cancellation)
{
    if (cancellation && cancellation->load())
        return {false, true, {}, QStringLiteral("Windows Search was cancelled before starting.")};
    if (root.trimmed().isEmpty() || name.trimmed().isEmpty()
            || maximumCandidates <= 0 || maximumCandidates > 20000) {
        return {false, cancellation && cancellation->load(), {},
            QStringLiteral("The Windows Search request is outside policy.")};
    }
#ifdef Q_OS_WIN
    return querySta(root, name, maximumCandidates, false, cancellation);
#else
    Q_UNUSED(cancellation);
    return {false, false, {}, QStringLiteral("Windows Search is unavailable on this platform.")};
#endif
}

WindowsSearchResult WindowsSearch::queryAqs(const QString& root, const QString& advancedQuery,
    int maximumCandidates, const std::atomic_bool* cancellation)
{
    if (cancellation && cancellation->load())
        return {false, true, {}, QStringLiteral("Windows AQS search was cancelled before starting.")};
    const QFileInfo rootInfo(root);
    const QString queryText = advancedQuery.trimmed();
    if (!rootInfo.isAbsolute() || !rootInfo.isDir() || queryText.isEmpty()
            || queryText.size() > 4096 || queryText.contains(QChar::Null)
            || maximumCandidates <= 0 || maximumCandidates > 20000) {
        return {false, false, {},
            QStringLiteral("The Windows AQS request is outside policy.")};
    }
#ifdef Q_OS_WIN
    return querySta(rootInfo.absoluteFilePath(), queryText,
        maximumCandidates, true, cancellation);
#else
    Q_UNUSED(cancellation);
    return {false, false, {}, QStringLiteral("Windows Search is unavailable on this platform.")};
#endif
}

} // namespace filemanager::search
