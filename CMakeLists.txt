cmake_minimum_required(VERSION 3.21)
set(FILE_MANAGER_BUILD_VERSION "1.0.0" CACHE STRING "Three- or four-part File Manager build version")
if(NOT FILE_MANAGER_BUILD_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+(\\.[0-9]+)?$")
    message(FATAL_ERROR "FILE_MANAGER_BUILD_VERSION must have three or four numeric components")
endif()
project(FileManager VERSION ${FILE_MANAGER_BUILD_VERSION} LANGUAGES C CXX)

# Windows fixed version fields must agree with the displayed version, including
# the optional fourth component used by development builds.
set(FILE_MANAGER_VERSION_REVISION 0)
if(NOT "${PROJECT_VERSION_TWEAK}" STREQUAL "")
    set(FILE_MANAGER_VERSION_REVISION "${PROJECT_VERSION_TWEAK}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(FILE_MANAGER_PRODUCT_NAME "File Manager" CACHE STRING "User-facing product name")
set(FILE_MANAGER_PUBLISHER "Kiatami" CACHE STRING "Publisher shown in Windows metadata")
set(FILE_MANAGER_RELEASE_CHANNEL "" CACHE STRING "User-facing release channel")
if(FILE_MANAGER_RELEASE_CHANNEL STREQUAL "")
    set(FILE_MANAGER_DISPLAY_VERSION "${PROJECT_VERSION}")
elseif(FILE_MANAGER_RELEASE_CHANNEL MATCHES "^rc\\.[0-9]+$")
    set(FILE_MANAGER_DISPLAY_VERSION "${PROJECT_VERSION}-${FILE_MANAGER_RELEASE_CHANNEL}")
else()
    set(FILE_MANAGER_DISPLAY_VERSION "${PROJECT_VERSION} ${FILE_MANAGER_RELEASE_CHANNEL}")
endif()

find_package(Qt6 6.11.2 EXACT REQUIRED COMPONENTS Concurrent Core Gui Multimedia Network Sql Widgets)

include(FetchContent)
option(FILE_MANAGER_BUILD_TESTS "Build File Manager verification tests" ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build dependencies statically" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "Disable dependency tests" FORCE)
set(ZLIB_COMPAT ON CACHE BOOL "Expose the zlib-compatible API" FORCE)
unset(ZLIB_ENABLE_TESTS CACHE)
set(FILE_MANAGER_ZLIB_NG_COMMIT 12731092979c6d07f42da27da673a9f6c7b13586)
FetchContent_Declare(zlib_ng
    GIT_REPOSITORY https://github.com/zlib-ng/zlib-ng.git
    GIT_TAG ${FILE_MANAGER_ZLIB_NG_COMMIT}
    GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(zlib_ng)
get_target_property(FILE_MANAGER_ZLIB_TARGET zlibstatic ALIASED_TARGET)
if(NOT FILE_MANAGER_ZLIB_TARGET)
    set(FILE_MANAGER_ZLIB_TARGET zlibstatic)
endif()
if(NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB ALIAS ${FILE_MANAGER_ZLIB_TARGET})
endif()
set(ZLIB_FOUND TRUE CACHE BOOL "Use the pinned zlib-ng target" FORCE)
set(ZLIB_LIBRARY "${zlib_ng_BINARY_DIR}/zlibstatic.lib" CACHE FILEPATH
    "Pinned zlib-ng library" FORCE)
set(ZLIB_LIBRARIES ZLIB::ZLIB CACHE STRING "Pinned zlib-ng target" FORCE)
set(ZLIB_INCLUDE_DIR "${zlib_ng_SOURCE_DIR};${zlib_ng_BINARY_DIR}" CACHE STRING
    "Pinned zlib-ng include directories" FORCE)
set(ZLIB_INCLUDE_DIRS "${zlib_ng_SOURCE_DIR};${zlib_ng_BINARY_DIR}" CACHE STRING
    "Pinned zlib-ng include directories" FORCE)

set(MZ_COMPAT OFF CACHE BOOL "Disable legacy minizip compatibility API" FORCE)
set(MZ_ZLIB ON CACHE BOOL "Enable DEFLATE decompression" FORCE)
set(MZ_ZLIB_FLAVOR zlib CACHE STRING "Use the pinned zlib-compatible target" FORCE)
set(MZ_BZIP2 OFF CACHE BOOL "Disable unused BZIP2 support" FORCE)
set(MZ_LZMA OFF CACHE BOOL "Disable unused LZMA support" FORCE)
set(MZ_PPMD OFF CACHE BOOL "Disable unused PPMD support" FORCE)
set(MZ_ZSTD OFF CACHE BOOL "Disable unused Zstandard support" FORCE)
set(MZ_FETCH_LIBS OFF CACHE BOOL "Never fetch unpinned transitive dependencies" FORCE)
set(MZ_PKCRYPT OFF CACHE BOOL "Disable archive encryption" FORCE)
set(MZ_WZAES OFF CACHE BOOL "Disable archive encryption" FORCE)
set(MZ_OPENSSL OFF CACHE BOOL "Disable OpenSSL integration" FORCE)
set(MZ_DECOMPRESS_ONLY ON CACHE BOOL "Build read-only decompression support" FORCE)
set(MZ_BUILD_TESTS OFF CACHE BOOL "Disable minizip-ng tools" FORCE)
set(MZ_BUILD_UNIT_TESTS OFF CACHE BOOL "Disable minizip-ng unit tests" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "Do not install bundled dependency artifacts" FORCE)
set(FILE_MANAGER_MINIZIP_NG_COMMIT 7b2387161c542fa9f427352dcdef76097d0d692b)
FetchContent_Declare(minizip_ng
    GIT_REPOSITORY https://github.com/zlib-ng/minizip-ng.git
    GIT_TAG ${FILE_MANAGER_MINIZIP_NG_COMMIT}
    GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(minizip_ng)
if(NOT IS_DIRECTORY "${zlib_ng_SOURCE_DIR}"
        OR NOT IS_DIRECTORY "${minizip_ng_SOURCE_DIR}")
    message(FATAL_ERROR "The fetched dependency source directories are unavailable")
endif()
# Record the directories CMake actually consumed. V1 verifies their Git trees
# against the pinned revisions; this file is build evidence, not a payload file.
file(WRITE "${CMAKE_BINARY_DIR}/file-manager-dependency-sources.txt"
    "zlib_ng_source=${zlib_ng_SOURCE_DIR}\n"
    "zlib_ng_commit=${FILE_MANAGER_ZLIB_NG_COMMIT}\n"
    "minizip_ng_source=${minizip_ng_SOURCE_DIR}\n"
    "minizip_ng_commit=${FILE_MANAGER_MINIZIP_NG_COMMIT}\n")

set(FILE_MANAGER_LIBRARY_SOURCES
    resources/resources.qrc
    src/mainwindow.cpp
    src/mainwindow.h
    src/commands/commandregistry.cpp
    src/commands/commandregistry.h
    src/diagnostics/diagnosticbundle.cpp
    src/diagnostics/diagnosticbundle.h
    src/diagnostics/diagnosticlog.cpp
    src/diagnostics/diagnosticlog.h
    src/diagnostics/asyncdiagnosticlog.cpp
    src/diagnostics/asyncdiagnosticlog.h
    src/diagnostics/diagnostichost.cpp
    src/diagnostics/diagnostichost.h
    src/settings/portablepreferences.cpp
    src/settings/portablepreferences.h
    src/settings/settingshealth.cpp
    src/settings/settingshealth.h
    src/files/fileoperations.cpp
    src/files/fileoperations.h
    src/files/fileanalysis.cpp
    src/files/fileanalysis.h
    src/files/batchrename.cpp
    src/files/batchrename.h
    src/files/sourcesnapshot.cpp
    src/files/sourcesnapshot.h
    src/files/tvorganizer.cpp
    src/files/tvorganizer.h
    src/files/bookorganizer.cpp
    src/files/bookorganizer.h
    src/files/epubseriesmetadata.cpp
    src/files/epubseriesmetadata.h
    src/tags/tagcatalog.cpp
    src/tags/tagcatalog.h
    # Keep legacy destination-rule validation so older encrypted label catalogs
    # can load and be cleared. The rule editor and tag-move planner are not
    # linked into the 1.0 app; labels remain identifiers, not move commands.
    src/tags/tagrules.cpp
    src/tags/tagrules.h
    src/files/reviewedtransfer.cpp
    src/files/reviewedtransfer.h
    src/files/analysisservice.cpp
    src/files/analysisservice.h
    src/files/duplicatefinder.cpp
    src/files/duplicatefinder.h
    src/files/duplicateservice.cpp
    src/files/duplicateservice.h
    src/files/nameordering.cpp
    src/files/nameordering.h
    src/files/storagevolumes.cpp
    src/files/storagevolumes.h
    src/files/networkdiscoveryservice.cpp
    src/files/networkdiscoveryservice.h
    src/files/operationjournal.cpp
    src/files/operationjournal.h
    src/files/sessionundo.cpp
    src/files/sessionundo.h
    src/files/filenamevalidator.cpp
    src/files/filenamevalidator.h
    src/files/scopeenumerator.cpp
    src/files/scopeenumerator.h
    src/files/transfercontroller.cpp
    src/files/transfercontroller.h
    src/files/transferqueue.cpp
    src/files/transferqueue.h
    src/files/ziparchive.cpp
    src/files/ziparchive.h
    src/search/fuzzymatcher.cpp
    src/search/fuzzymatcher.h
    src/search/searchservice.cpp
    src/search/searchservice.h
    src/search/sessionsearchhistory.cpp
    src/search/sessionsearchhistory.h
    src/search/savedsearchstore.cpp
    src/search/savedsearchstore.h
    src/search/windowssearch.cpp
    src/search/windowssearch.h
    src/search/metadataindex.cpp
    src/search/metadataindex.h
    src/search/contentsearchservice.cpp
    src/search/contentsearchservice.h
    src/settings/sessionstore.cpp
    src/settings/sessionstore.h
    src/settings/startuppreferences.cpp
    src/settings/startuppreferences.h
    src/settings/contextmenupermissions.cpp
    src/settings/contextmenupermissions.h
    src/settings/metadatapermissions.cpp
    src/settings/metadatapermissions.h
    src/settings/shellpermissions.cpp
    src/settings/shellpermissions.h
    src/settings/favoritegroups.cpp
    src/settings/favoritegroups.h
    src/ui/filepane.cpp
    src/ui/filepane.h
    src/ui/localdirectorymodel.cpp
    src/ui/localdirectorymodel.h
    src/platform/directorywatcher.cpp
    src/platform/directorywatcher.h
    src/platform/directorywatchhost.cpp
    src/ui/presentationtiming.h
    src/ui/breadcrumbbar.cpp
    src/ui/breadcrumbbar.h
    src/ui/homeview.cpp
    src/ui/homeview.h
    src/ui/tabbedpanel.cpp
    src/ui/tabbedpanel.h
    src/ui/transferannouncements.h
    src/ui/localizedformat.h
    src/ui/applicationtheme.cpp
    src/ui/applicationtheme.h
    src/ui/brandidentity.cpp
    src/ui/brandidentity.h
    src/ui/appearancesettings.cpp
    src/ui/appearancesettings.h
    src/ui/batchrenamedialog.cpp
    src/ui/batchrenamedialog.h
    src/ui/tvorganizerdialog.cpp
    src/ui/tvorganizerdialog.h
    src/ui/tageditordialog.cpp
    src/ui/tageditordialog.h
    src/ui/conflictdialog.cpp
    src/ui/conflictdialog.h
    src/ui/contentsearchdialog.cpp
    src/ui/contentsearchdialog.h
    src/ui/filedetailsdialog.cpp
    src/ui/filedetailsdialog.h
    src/ui/filesearchdialog.cpp
    src/ui/filesearchdialog.h
    src/ui/operationcenterdialog.cpp
    src/ui/operationcenterdialog.h
    src/ui/preferencesdialog.cpp
    src/ui/preferencesdialog.h
    src/ui/firstlaunchintegrationdialog.cpp
    src/ui/firstlaunchintegrationdialog.h
    src/ui/iconfactory.cpp
    src/ui/iconfactory.h
    src/ui/navigationtree.cpp
    src/ui/navigationtree.h
    src/ui/shellnamespacebrowser.cpp
    src/ui/shellnamespacebrowser.h
    src/ui/searchresultbar.cpp
    src/ui/searchresultbar.h
    src/ui/shortcuteditordialog.cpp
    src/ui/shortcuteditordialog.h
    src/ui/fileitemdelegate.cpp
    src/ui/fileiconimage.cpp
    src/ui/fileiconimage.h
    src/ui/backgroundfileicons.cpp
    src/ui/backgroundfileicons.h
    src/platform/fileicon.cpp
    src/platform/fileicon.h
    src/ui/fileitemdelegate.h
    src/workspace/layoutmodel.cpp
    src/workspace/layoutmodel.h
    src/workspace/workspaceview.cpp
    src/workspace/workspaceview.h
    src/preview/previewservice.cpp
    src/preview/previewservice.h
    src/preview/previewcontroller.cpp
    src/preview/previewcontroller.h
    src/preview/sandboxedpreviewprocess.cpp
    src/preview/sandboxedpreviewprocess.h
    src/preview/installcleanup.cpp
    src/preview/installcleanup.h
    src/preview/previewidentity.h
    src/preview/thumbnailprovider.cpp
    src/preview/thumbnailprovider.h
    src/preview/imagemetadataprovider.cpp
    src/preview/fileiconscheduler.cpp
    src/preview/fileiconscheduler.h
    src/preview/imagemetadataprovider.h
    src/preview/documentmetadataprovider.cpp
    src/preview/documentmetadataprovider.h
    src/preview/mediametadataprovider.cpp
    src/preview/mediametadataprovider.h
    src/preview/shellmetadataprovider.cpp
    src/preview/shellmetadataprovider.h
    src/preview/cloudstatusprovider.cpp
    src/preview/cloudstatusprovider.h
    src/platform/windowswindowchrome.cpp
    src/platform/windowswindowchrome.h
    src/platform/singleinstancecoordinator.cpp
    src/platform/singleinstancecoordinator.h
    src/platform/folderlaunch.cpp
    src/platform/folderlaunch.h
    src/platform/knownfolders.cpp
    src/platform/knownfolders.h
    src/platform/pathnavigation.cpp
    src/platform/pathnavigation.h
      src/platform/shellfileoperation.cpp
      src/platform/shellfileoperation.h
      src/platform/recyclebinoperation.cpp
      src/platform/recyclebinoperation.h
    src/platform/shellclipboard.cpp
    src/platform/shellclipboard.h
    src/platform/shellitem.cpp
    src/platform/shellitem.h
    src/platform/nativefilecopy.cpp
    src/platform/nativefilecopy.h
    src/platform/cloudfilestatus.cpp
    src/platform/cloudfilestatus.h
    src/platform/networkdriveui.cpp
    src/platform/networkdriveui.h
    src/platform/guardedfilerename.cpp
    src/platform/guardedfilerename.h
    src/platform/systemopen.cpp
    src/platform/systemopen.h
    src/platform/shelllink.cpp
    src/platform/shelllink.h
    src/platform/windowscontextmenu.cpp
    src/platform/windowscontextmenu.h
    src/platform/windowsshare.cpp
    src/platform/windowsshare.h
    src/platform/archivehandoff.cpp
    src/platform/archivehandoff.h
    src/platform/windowsfolderintegration.cpp
    src/platform/windowsfolderintegration.h
    src/platform/folderrouting.cpp
    src/platform/folderrouting.h
    src/platform/wineintegration.cpp
    src/platform/wineintegration.h
    src/platform/winekeystate.h
)

qt_add_library(FileManagerLib STATIC ${FILE_MANAGER_LIBRARY_SOURCES})
target_link_libraries(FileManagerLib PUBLIC
    Qt6::Concurrent Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Network Qt6::Sql Qt6::Widgets
    MINIZIP::minizip)
if(WIN32)
    target_link_libraries(FileManagerLib PUBLIC bcrypt cldapi crypt32 dwmapi mpr pathcch propsys shell32 ole32
        runtimeobject searchsdk userenv uuid windowsapp)
endif()
target_include_directories(FileManagerLib PUBLIC src)
target_compile_definitions(FileManagerLib PUBLIC
    FILE_MANAGER_VERSION="${PROJECT_VERSION}"
    FILE_MANAGER_DISPLAY_VERSION="${FILE_MANAGER_DISPLAY_VERSION}"
    FILE_MANAGER_PRODUCT_NAME="${FILE_MANAGER_PRODUCT_NAME}"
    FILE_MANAGER_PUBLISHER="${FILE_MANAGER_PUBLISHER}"
    FILE_MANAGER_RELEASE_CHANNEL="${FILE_MANAGER_RELEASE_CHANNEL}"
)

qt_add_executable(FileManager WIN32 src/main.cpp)
target_link_libraries(FileManager PRIVATE FileManagerLib)

qt_add_executable(FileManagerShellHost WIN32 src/platform/shellnamespacehost.cpp)
target_link_libraries(FileManagerShellHost PRIVATE FileManagerLib)

qt_add_executable(FileManagerPreviewHost WIN32 src/preview/previewhost.cpp)
target_link_libraries(FileManagerPreviewHost PRIVATE FileManagerLib)

qt_add_executable(FileManagerContextMenuHost WIN32 src/platform/contextmenuhost.cpp)
target_link_libraries(FileManagerContextMenuHost PRIVATE Qt6::Core)
if(WIN32)
    target_link_libraries(FileManagerContextMenuHost PRIVATE shell32 ole32 uuid)
endif()

qt_add_executable(FileManagerMaintenance WIN32
    src/platform/folderrouting.cpp
    src/platform/windowsfolderintegration.cpp
    src/platform/windowsfolderintegration.h
    src/platform/wineintegration.cpp
    src/maintenance/main.cpp
    src/preview/installcleanup.cpp
    src/preview/installcleanup.h
    src/preview/previewidentity.h)
target_include_directories(FileManagerMaintenance PRIVATE src)
target_link_libraries(FileManagerMaintenance PRIVATE Qt6::Core)
target_compile_definitions(FileManagerMaintenance PRIVATE
    FILE_MANAGER_PRODUCT_NAME="${FILE_MANAGER_PRODUCT_NAME}"
    FILE_MANAGER_PUBLISHER="${FILE_MANAGER_PUBLISHER}")
if(WIN32)
    target_link_libraries(FileManagerMaintenance PRIVATE userenv user32 advapi32 shell32)
endif()

if(MSVC)
    function(file_manager_add_version_resource target description original_filename)
        set(FILE_MANAGER_FILE_DESCRIPTION "${description}")
        set(FILE_MANAGER_ORIGINAL_FILENAME "${original_filename}")
        configure_file(resources/windows-version.rc.in
            "${CMAKE_CURRENT_BINARY_DIR}/generated/${target}-version.rc" @ONLY)
        set_source_files_properties(
            "${CMAKE_CURRENT_BINARY_DIR}/generated/${target}-version.rc" PROPERTIES
            OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/branding/app-icon.ico")
        target_sources(${target} PRIVATE
            "${CMAKE_CURRENT_BINARY_DIR}/generated/${target}-version.rc")
    endfunction()
    file_manager_add_version_resource(FileManager "${FILE_MANAGER_PRODUCT_NAME}"
        "FileManager.exe")
    file_manager_add_version_resource(FileManagerShellHost
        "${FILE_MANAGER_PRODUCT_NAME} Shell namespace host" "FileManagerShellHost.exe")
    file_manager_add_version_resource(FileManagerPreviewHost
        "${FILE_MANAGER_PRODUCT_NAME} preview host" "FileManagerPreviewHost.exe")
    file_manager_add_version_resource(FileManagerContextMenuHost
        "${FILE_MANAGER_PRODUCT_NAME} context-menu host" "FileManagerContextMenuHost.exe")
    file_manager_add_version_resource(FileManagerMaintenance
        "${FILE_MANAGER_PRODUCT_NAME} uninstall maintenance" "FileManagerMaintenance.exe")
endif()

if(MSVC)
    set(FILE_MANAGER_WINDOWS_MANIFEST
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows.manifest")
    foreach(target FileManager FileManagerShellHost FileManagerPreviewHost FileManagerContextMenuHost
            FileManagerMaintenance)
        target_link_options(${target} PRIVATE "/MANIFEST:EMBED"
            "/MANIFESTINPUT:${FILE_MANAGER_WINDOWS_MANIFEST}")
        # Linker input options alone do not track changes to the manifest file.
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
            "${FILE_MANAGER_WINDOWS_MANIFEST}")
    endforeach()
endif()

function(file_manager_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

file_manager_enable_warnings(FileManagerLib)
file_manager_enable_warnings(FileManager)
file_manager_enable_warnings(FileManagerShellHost)
file_manager_enable_warnings(FileManagerPreviewHost)
file_manager_enable_warnings(FileManagerContextMenuHost)
file_manager_enable_warnings(FileManagerMaintenance)

include(CTest)
if(FILE_MANAGER_BUILD_TESTS)
    enable_testing()
    qt_add_executable(FileManagerKaiHandoffContractTests
        tests/kaihandoffcontract_tests.cpp
        src/integration/kaihandoffcontract.cpp
        src/files/filenamevalidator.cpp)
    target_include_directories(FileManagerKaiHandoffContractTests PRIVATE src)
    target_link_libraries(FileManagerKaiHandoffContractTests PRIVATE Qt6::Core)
    file_manager_enable_warnings(FileManagerKaiHandoffContractTests)
    add_test(NAME FileManagerKaiHandoffContractTests COMMAND FileManagerKaiHandoffContractTests)
    qt_add_executable(FileManagerKaiSessionGrantTests
        tests/kaisessiongrants_tests.cpp
        src/integration/kaisessiongrants.cpp
        src/files/sourcesnapshot.cpp)
    target_include_directories(FileManagerKaiSessionGrantTests PRIVATE src)
    target_link_libraries(FileManagerKaiSessionGrantTests PRIVATE Qt6::Core)
    file_manager_enable_warnings(FileManagerKaiSessionGrantTests)
    add_test(NAME FileManagerKaiSessionGrantTests COMMAND FileManagerKaiSessionGrantTests)
    qt_add_executable(FileManagerCoreTests
        tests/core_tests.cpp
        tests/tvorganizer_tests.cpp
        tests/bookorganizer_tests.cpp
        tests/epub_test_fixture.cpp
        tests/epubseriesmetadata_tests.cpp
        tests/tagcatalog_tests.cpp
        tests/tagmetadata_tests.cpp
        src/tags/tagcatalog.cpp
        src/preview/documentmetadataprovider.cpp
        src/preview/sandboxedpreviewprocess.cpp
        src/preview/previewidentity.h
        src/tags/tagrules.cpp
        src/search/fuzzymatcher.cpp
        src/files/fileoperations.cpp
        src/files/fileanalysis.cpp
        src/files/batchrename.cpp
        src/files/sourcesnapshot.cpp
        src/files/tvorganizer.cpp
        src/files/bookorganizer.cpp
        src/files/epubseriesmetadata.cpp
        src/files/duplicatefinder.cpp
        src/files/nameordering.cpp
        src/files/storagevolumes.cpp
        src/files/sessionundo.cpp
        src/files/filenamevalidator.cpp
        src/files/scopeenumerator.cpp
        src/platform/nativefilecopy.cpp
        src/platform/cloudfilestatus.cpp
        src/platform/guardedfilerename.cpp
        src/platform/recyclebinoperation.cpp
        src/platform/shelllink.cpp
        src/workspace/layoutmodel.cpp
        src/settings/favoritegroups.cpp
        src/ui/applicationtheme.h
        src/diagnostics/diagnosticbundle.cpp
        src/diagnostics/diagnosticbundle.h
        src/diagnostics/diagnosticlog.cpp
        src/diagnostics/diagnosticlog.h
        src/settings/portablepreferences.cpp
        src/settings/portablepreferences.h
        src/settings/settingshealth.cpp
        src/settings/settingshealth.h
    )
    target_include_directories(FileManagerCoreTests PRIVATE src)
    target_link_libraries(FileManagerCoreTests PRIVATE Qt6::Core Qt6::Gui MINIZIP::minizip)
    if(WIN32)
        target_link_libraries(FileManagerCoreTests PRIVATE bcrypt cldapi crypt32 shell32 ole32
            userenv advapi32)
        add_dependencies(FileManagerCoreTests FileManagerPreviewHost)
    endif()
    file_manager_enable_warnings(FileManagerCoreTests)
    add_test(NAME FileManagerCoreTests COMMAND FileManagerCoreTests)
    add_test(NAME FileManagerMediaOrganizerCoreTests
        COMMAND FileManagerCoreTests --media-organizer-only)
    set_tests_properties(FileManagerMediaOrganizerCoreTests PROPERTIES
        LABELS "focused;media-organizer")
    add_test(NAME FileManagerTagCoreTests
        COMMAND FileManagerCoreTests --tags-only)
    set_tests_properties(FileManagerTagCoreTests PROPERTIES
        LABELS "focused;tags")

    if(WIN32)
        qt_add_executable(FileManagerFolderBenchmark
            tests/folder_performance_benchmark.cpp
            src/files/scopeenumerator.cpp
            src/files/filenamevalidator.cpp
            src/files/sourcesnapshot.cpp
            src/platform/nativefilecopy.cpp
        )
        target_include_directories(FileManagerFolderBenchmark PRIVATE src)
        target_link_libraries(FileManagerFolderBenchmark PRIVATE Qt6::Core shell32 ole32 psapi)
        file_manager_enable_warnings(FileManagerFolderBenchmark)
    endif()

    qt_add_executable(FileManagerWorkspaceTests
        tests/workspace_tests.cpp
        tests/reviewedtransfer_tests.cpp
        tests/guardedfilerename_tests.cpp
        tests/guardeddirectory_tests.cpp
        tests/guardedcrossvolume_tests.cpp
    )
    qt_add_executable(FileManagerPreviewHelperFixture tests/preview_helper_fixture.cpp)
    target_link_libraries(FileManagerPreviewHelperFixture PRIVATE Qt6::Core)
    file_manager_enable_warnings(FileManagerPreviewHelperFixture)
    target_link_libraries(FileManagerWorkspaceTests PRIVATE FileManagerLib)
    add_dependencies(FileManagerWorkspaceTests FileManagerShellHost FileManagerPreviewHost
        FileManagerContextMenuHost
        FileManagerPreviewHelperFixture)
    file_manager_enable_warnings(FileManagerWorkspaceTests)
    add_test(NAME FileManagerWorkspaceTests COMMAND FileManagerWorkspaceTests)
    qt_add_executable(FileManagerIconSchedulerTests tests/fileiconscheduler_tests.cpp)
    target_link_libraries(FileManagerIconSchedulerTests PRIVATE FileManagerLib)
    file_manager_enable_warnings(FileManagerIconSchedulerTests)
    add_test(NAME FileManagerIconSchedulerTests COMMAND FileManagerIconSchedulerTests)
    qt_add_executable(FileManagerIconRenderingTests tests/fileiconrendering_tests.cpp
        tests/backgroundfileicons_checks.cpp)
    target_link_libraries(FileManagerIconRenderingTests PRIVATE FileManagerLib)
    add_dependencies(FileManagerIconRenderingTests FileManagerShellHost)
    file_manager_enable_warnings(FileManagerIconRenderingTests)
    add_test(NAME FileManagerIconRenderingTests COMMAND FileManagerIconRenderingTests)
    set(FILE_MANAGER_TEST_TEMP_DIR "${CMAKE_CURRENT_BINARY_DIR}/test-temp")
    set(FILE_MANAGER_TEST_LOCAL_APP_DATA "${FILE_MANAGER_TEST_TEMP_DIR}/local-app-data")
    set(FILE_MANAGER_TEST_ROAMING_APP_DATA "${FILE_MANAGER_TEST_TEMP_DIR}/roaming-app-data")
    file(MAKE_DIRECTORY "${FILE_MANAGER_TEST_TEMP_DIR}"
        "${FILE_MANAGER_TEST_LOCAL_APP_DATA}" "${FILE_MANAGER_TEST_ROAMING_APP_DATA}")
    set(FILE_MANAGER_TEST_ENVIRONMENT
        "TMP=${FILE_MANAGER_TEST_TEMP_DIR};TEMP=${FILE_MANAGER_TEST_TEMP_DIR};LOCALAPPDATA=${FILE_MANAGER_TEST_LOCAL_APP_DATA};APPDATA=${FILE_MANAGER_TEST_ROAMING_APP_DATA}")
    set_tests_properties(FileManagerWorkspaceTests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        ENVIRONMENT "${FILE_MANAGER_TEST_ENVIRONMENT}")

    if(WIN32)
        # Explicitly built/run diagnostic; never part of the release suite or package.
        add_executable(FileManagerIconExtractionProbe EXCLUDE_FROM_ALL
            tests/icon_extraction_probe.cpp src/platform/fileicon.cpp)
        target_include_directories(FileManagerIconExtractionProbe PRIVATE src)
        target_link_libraries(FileManagerIconExtractionProbe PRIVATE
            Qt6::Widgets shell32 comctl32 ole32 user32 gdi32)
        file_manager_enable_warnings(FileManagerIconExtractionProbe)
        if(MSVC)
            target_link_options(FileManagerIconExtractionProbe PRIVATE "/MANIFEST:EMBED"
                "/MANIFESTINPUT:${FILE_MANAGER_WINDOWS_MANIFEST}")
            set_property(TARGET FileManagerIconExtractionProbe APPEND PROPERTY LINK_DEPENDS
                "${FILE_MANAGER_WINDOWS_MANIFEST}")
        endif()

        qt_add_executable(FileManagerDirectoryWatcherTests tests/directorywatcher_tests.cpp)
        target_link_libraries(FileManagerDirectoryWatcherTests PRIVATE FileManagerLib)
        add_dependencies(FileManagerDirectoryWatcherTests FileManagerShellHost)
        file_manager_enable_warnings(FileManagerDirectoryWatcherTests)
        add_test(NAME FileManagerDirectoryWatcherTests COMMAND FileManagerDirectoryWatcherTests)
        set_tests_properties(FileManagerDirectoryWatcherTests PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            ENVIRONMENT "${FILE_MANAGER_TEST_ENVIRONMENT}")
    endif()

    if(WIN32)
        qt_add_executable(FileManagerReviewedTransferBenchmark
            tests/reviewed_transfer_benchmark.cpp)
        target_link_libraries(FileManagerReviewedTransferBenchmark PRIVATE FileManagerLib)
        file_manager_enable_warnings(FileManagerReviewedTransferBenchmark)

        qt_add_executable(FileManagerNavigationBenchmark
            tests/navigation_performance_benchmark.cpp)
        target_link_libraries(FileManagerNavigationBenchmark PRIVATE FileManagerLib)
        file_manager_enable_warnings(FileManagerNavigationBenchmark)

        qt_add_executable(FileManagerLaunchBenchmark
            tests/launch_performance_benchmark.cpp)
        target_link_libraries(FileManagerLaunchBenchmark PRIVATE FileManagerLib)
        file_manager_enable_warnings(FileManagerLaunchBenchmark)

        qt_add_executable(FileManagerShellNavigationBenchmark
            tests/shell_navigation_performance_benchmark.cpp)
        target_link_libraries(FileManagerShellNavigationBenchmark PRIVATE FileManagerLib)
        add_dependencies(FileManagerShellNavigationBenchmark FileManagerShellHost)
        file_manager_enable_warnings(FileManagerShellNavigationBenchmark)
    endif()

    qt_add_executable(FileManagerWorkspaceRestoreTests tests/workspace_restore_tests.cpp)
    qt_add_executable(FileManagerAsyncDiagnosticsTests tests/asyncdiagnostics_tests.cpp)
    target_link_libraries(FileManagerAsyncDiagnosticsTests PRIVATE FileManagerLib)
    add_dependencies(FileManagerAsyncDiagnosticsTests FileManagerShellHost)
    file_manager_enable_warnings(FileManagerAsyncDiagnosticsTests)
    add_test(NAME FileManagerAsyncDiagnosticsTests COMMAND FileManagerAsyncDiagnosticsTests)
    set_tests_properties(FileManagerAsyncDiagnosticsTests PROPERTIES
        ENVIRONMENT "${FILE_MANAGER_TEST_ENVIRONMENT}")
    target_link_libraries(FileManagerWorkspaceRestoreTests PRIVATE FileManagerLib)
    file_manager_enable_warnings(FileManagerWorkspaceRestoreTests)
    add_test(NAME FileManagerWorkspaceRestoreTests COMMAND FileManagerWorkspaceRestoreTests)

    qt_add_executable(FileManagerKeyboardTests tests/filepane_keyboard_tests.cpp)
    target_link_libraries(FileManagerKeyboardTests PRIVATE FileManagerLib)
    file_manager_enable_warnings(FileManagerKeyboardTests)
    add_test(NAME FileManagerKeyboardTests COMMAND FileManagerKeyboardTests)

    qt_add_executable(FileManagerVisualTests tests/visual_tests.cpp tests/tvorganizer_ui_tests.cpp
        tests/tageditor_ui_tests.cpp tests/epub_test_fixture.cpp)
    target_link_libraries(FileManagerVisualTests PRIVATE FileManagerLib)
    add_dependencies(FileManagerVisualTests FileManagerPreviewHost FileManagerShellHost)
    file_manager_enable_warnings(FileManagerVisualTests)
    target_compile_definitions(FileManagerVisualTests PRIVATE
        FILE_MANAGER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME FileManagerVisualTests COMMAND FileManagerVisualTests)
    add_test(NAME FileManagerMediaOrganizerVisualTests
        COMMAND FileManagerVisualTests --media-organizer-only)
    set_tests_properties(FileManagerMediaOrganizerVisualTests PROPERTIES
        LABELS "focused;media-organizer")
    add_test(NAME FileManagerTagVisualTests
        COMMAND FileManagerVisualTests --tags-only)
    set_tests_properties(FileManagerTagVisualTests PROPERTIES
        LABELS "focused;tags")
    add_test(NAME FileManagerDpi150Tests COMMAND FileManagerVisualTests --invariants-only)
    set_tests_properties(FileManagerDpi150Tests PROPERTIES ENVIRONMENT
        "QT_SCALE_FACTOR=1.5;${FILE_MANAGER_TEST_ENVIRONMENT}")
    add_test(NAME FileManagerDpi200Tests COMMAND FileManagerVisualTests --invariants-only)
    set_tests_properties(FileManagerDpi200Tests PROPERTIES ENVIRONMENT
        "QT_SCALE_FACTOR=2.0;${FILE_MANAGER_TEST_ENVIRONMENT}")
    set_tests_properties(FileManagerIconRenderingTests FileManagerIconSchedulerTests FileManagerCoreTests
        FileManagerMediaOrganizerCoreTests FileManagerTagCoreTests FileManagerWorkspaceRestoreTests
        FileManagerKeyboardTests FileManagerVisualTests FileManagerMediaOrganizerVisualTests
        FileManagerTagVisualTests PROPERTIES
        ENVIRONMENT "${FILE_MANAGER_TEST_ENVIRONMENT}")
    if(MSVC)
        foreach(target FileManagerIconRenderingTests FileManagerIconSchedulerTests FileManagerCoreTests FileManagerWorkspaceTests FileManagerWorkspaceRestoreTests
                FileManagerKeyboardTests FileManagerVisualTests FileManagerDirectoryWatcherTests
                FileManagerAsyncDiagnosticsTests FileManagerFolderBenchmark FileManagerReviewedTransferBenchmark
                FileManagerNavigationBenchmark FileManagerLaunchBenchmark FileManagerShellNavigationBenchmark)
            target_link_options(${target} PRIVATE "/MANIFEST:EMBED"
                "/MANIFESTINPUT:${FILE_MANAGER_WINDOWS_MANIFEST}")
            set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
                "${FILE_MANAGER_WINDOWS_MANIFEST}")
        endforeach()
    endif()
endif()

if(WIN32)
    # Keep the release notice current even when only its text changes and the
    # application has no C++ reason to relink. Ninja reruns CMake for changes
    # to configure_file inputs; the POST_BUILD copy below still covers other
    # generator output directories.
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
        "${CMAKE_CURRENT_BINARY_DIR}/THIRD_PARTY_NOTICES.md" COPYONLY)
    get_filename_component(FILE_MANAGER_QT_PREFIX "${Qt6_DIR}/../../.." ABSOLUTE)
    set(WINDEPLOYQT_EXECUTABLE "${FILE_MANAGER_QT_PREFIX}/bin/windeployqt.exe")
    if(NOT EXISTS "${WINDEPLOYQT_EXECUTABLE}")
        message(FATAL_ERROR "The selected Qt kit has no bin/windeployqt.exe: ${FILE_MANAGER_QT_PREFIX}")
    endif()
    set(FILE_MANAGER_WINDEPLOYQT_OPTIONS --no-translations --compiler-runtime
        --exclude-plugins qsqlibase,qsqlmimer,qsqloci,qsqlodbc,qsqlpsql)
    # Keep the verbatim reviewed Qt license in source so relocated Qt kits
    # deploy the same notice without depending on an installer's parent tree.
    set(FILE_MANAGER_QT_LICENSE_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/licenses/Qt-LICENSE.txt")
    if(NOT EXISTS "${FILE_MANAGER_QT_LICENSE_FILE}")
        message(FATAL_ERROR "The reviewed Qt license notice is missing.")
    endif()
    add_custom_command(TARGET FileManager POST_BUILD
        COMMAND "${WINDEPLOYQT_EXECUTABLE}" ${FILE_MANAGER_WINDEPLOYQT_OPTIONS}
            "$<TARGET_FILE:FileManager>"
        COMMAND "${CMAKE_COMMAND}" -E rm -f
            "$<TARGET_FILE_DIR:FileManager>/sqldrivers/qsqlibase.dll"
            "$<TARGET_FILE_DIR:FileManager>/sqldrivers/qsqlmimer.dll"
            "$<TARGET_FILE_DIR:FileManager>/sqldrivers/qsqloci.dll"
            "$<TARGET_FILE_DIR:FileManager>/sqldrivers/qsqlodbc.dll"
            "$<TARGET_FILE_DIR:FileManager>/sqldrivers/qsqlpsql.dll"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:FileManager>/licenses"
            "$<TARGET_FILE_DIR:FileManager>/sbom"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
            "$<TARGET_FILE_DIR:FileManager>/LICENSE"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
            "$<TARGET_FILE_DIR:FileManager>/THIRD_PARTY_NOTICES.md"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/SECURITY.md"
            "$<TARGET_FILE_DIR:FileManager>/SECURITY.md"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${FILE_MANAGER_QT_LICENSE_FILE}"
            "$<TARGET_FILE_DIR:FileManager>/licenses/Qt-LICENSE.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/licenses/FFmpeg-LGPL-2.1.txt"
            "$<TARGET_FILE_DIR:FileManager>/licenses/FFmpeg-LGPL-2.1.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/licenses/Signalsmith-MIT.txt"
            "$<TARGET_FILE_DIR:FileManager>/licenses/Signalsmith-MIT.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/licenses/Mozilla-Public-License-2.0.txt"
            "$<TARGET_FILE_DIR:FileManager>/licenses/Mozilla-Public-License-2.0.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/licenses/Qt-ThirdParty-Attributions.txt"
            "$<TARGET_FILE_DIR:FileManager>/licenses/Qt-ThirdParty-Attributions.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/licenses/Windows-SDK-Third-Party-Notices.rtf"
            "$<TARGET_FILE_DIR:FileManager>/licenses/Windows-SDK-Third-Party-Notices.rtf"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${FILE_MANAGER_QT_PREFIX}/sbom/qtbase-6.11.2.cdx.json"
            "$<TARGET_FILE_DIR:FileManager>/sbom/qtbase-6.11.2.cdx.json"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${FILE_MANAGER_QT_PREFIX}/sbom/qtmultimedia-6.11.2.cdx.json"
            "$<TARGET_FILE_DIR:FileManager>/sbom/qtmultimedia-6.11.2.cdx.json"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${FILE_MANAGER_QT_PREFIX}/sbom/qtsvg-6.11.2.cdx.json"
            "$<TARGET_FILE_DIR:FileManager>/sbom/qtsvg-6.11.2.cdx.json"
        VERBATIM)
    add_custom_command(TARGET FileManagerPreviewHost POST_BUILD
        COMMAND "${WINDEPLOYQT_EXECUTABLE}" ${FILE_MANAGER_WINDEPLOYQT_OPTIONS}
            "$<TARGET_FILE:FileManagerPreviewHost>"
        VERBATIM)
    if(FILE_MANAGER_BUILD_TESTS)
        add_custom_command(TARGET FileManagerIconRenderingTests POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}" ${FILE_MANAGER_WINDEPLOYQT_OPTIONS}
                "$<TARGET_FILE:FileManagerIconRenderingTests>"
            VERBATIM)
        add_custom_command(TARGET FileManagerIconSchedulerTests POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}" ${FILE_MANAGER_WINDEPLOYQT_OPTIONS}
                "$<TARGET_FILE:FileManagerIconSchedulerTests>"
            VERBATIM)
        add_custom_command(TARGET FileManagerWorkspaceTests POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}" ${FILE_MANAGER_WINDEPLOYQT_OPTIONS}
                "$<TARGET_FILE:FileManagerWorkspaceTests>"
            VERBATIM)
        add_custom_command(TARGET FileManagerWorkspaceRestoreTests POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}" ${FILE_MANAGER_WINDEPLOYQT_OPTIONS}
                "$<TARGET_FILE:FileManagerWorkspaceRestoreTests>"
            VERBATIM)
        add_custom_command(TARGET FileManagerKeyboardTests POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}" ${FILE_MANAGER_WINDEPLOYQT_OPTIONS}
                "$<TARGET_FILE:FileManagerKeyboardTests>"
            VERBATIM)
        add_custom_command(TARGET FileManagerVisualTests POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}" ${FILE_MANAGER_WINDEPLOYQT_OPTIONS}
                "$<TARGET_FILE:FileManagerVisualTests>"
            VERBATIM)
    endif()
endif()

if(FILE_MANAGER_BUILD_TESTS AND WIN32)
    find_program(FILE_MANAGER_POWERSHELL NAMES pwsh.exe powershell.exe REQUIRED)
    add_test(NAME FileManagerReleaseSignatureTests
        COMMAND "${FILE_MANAGER_POWERSHELL}" -NoProfile -NonInteractive
            -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_signature_tests.ps1")
    add_test(NAME FileManagerPackageIdentityTests
        COMMAND "${FILE_MANAGER_POWERSHELL}" -NoProfile -NonInteractive
            -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_identity_tests.ps1")
    add_test(NAME FileManagerDependencyTests
        COMMAND "${CMAKE_COMMAND}"
            -DBUILD_DIR=$<TARGET_FILE_DIR:FileManager>
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_release_dependencies.cmake")
endif()
