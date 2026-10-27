if(NOT DEFINED BUILD_DIR OR NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "BUILD_DIR and SOURCE_DIR are required")
endif()

set(expected_dlls
    avcodec-61.dll
    avformat-61.dll
    avutil-59.dll
    d3dcompiler_47.dll
    dxcompiler.dll
    dxil.dll
    generic/qtuiotouchplugin.dll
    iconengines/qsvgicon.dll
    imageformats/qgif.dll
    imageformats/qico.dll
    imageformats/qjpeg.dll
    imageformats/qsvg.dll
    multimedia/ffmpegmediaplugin.dll
    multimedia/windowsmediaplugin.dll
    networkinformation/qnetworklistmanager.dll
    opengl32sw.dll
    platforms/qwindows.dll
    Qt6Core.dll
    Qt6Gui.dll
    Qt6Multimedia.dll
    Qt6Network.dll
    Qt6Sql.dll
    Qt6Svg.dll
    Qt6Widgets.dll
    sqldrivers/qsqlite.dll
    styles/qmodernwindowsstyle.dll
    swresample-5.dll
    swscale-8.dll
    tls/qcertonlybackend.dll
    tls/qschannelbackend.dll)

file(GLOB root_dlls RELATIVE "${BUILD_DIR}" "${BUILD_DIR}/*.dll")
set(actual_dlls ${root_dlls})
foreach(plugin_dir generic iconengines imageformats multimedia networkinformation platforms
        sqldrivers styles tls)
    file(GLOB plugin_dlls RELATIVE "${BUILD_DIR}" "${BUILD_DIR}/${plugin_dir}/*.dll")
    list(APPEND actual_dlls ${plugin_dlls})
endforeach()
list(SORT actual_dlls)
list(SORT expected_dlls)
if(NOT actual_dlls STREQUAL expected_dlls)
    message(FATAL_ERROR
        "Release DLL inventory changed without review.\nExpected: ${expected_dlls}\nActual: ${actual_dlls}")
endif()

foreach(required_file
        "${BUILD_DIR}/LICENSE"
        "${BUILD_DIR}/THIRD_PARTY_NOTICES.md"
        "${BUILD_DIR}/SECURITY.md"
        "${BUILD_DIR}/licenses/Qt-LICENSE.txt"
        "${BUILD_DIR}/licenses/FFmpeg-LGPL-2.1.txt"
        "${BUILD_DIR}/licenses/Signalsmith-MIT.txt"
        "${BUILD_DIR}/licenses/Mozilla-Public-License-2.0.txt"
        "${BUILD_DIR}/licenses/Qt-ThirdParty-Attributions.txt"
        "${BUILD_DIR}/licenses/Windows-SDK-Third-Party-Notices.rtf"
        "${BUILD_DIR}/sbom/qtbase-6.11.2.cdx.json"
        "${BUILD_DIR}/sbom/qtmultimedia-6.11.2.cdx.json"
        "${BUILD_DIR}/sbom/qtsvg-6.11.2.cdx.json")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Required release compliance artifact is missing: ${required_file}")
    endif()
endforeach()

file(READ "${BUILD_DIR}/licenses/Signalsmith-MIT.txt" signalsmith_license)
file(SHA256 "${SOURCE_DIR}/resources/licenses/Signalsmith-MIT.txt" signalsmith_license_source_sha256)
file(SHA256 "${BUILD_DIR}/licenses/Signalsmith-MIT.txt" signalsmith_license_deployed_sha256)
if(NOT signalsmith_license_source_sha256 STREQUAL signalsmith_license_deployed_sha256)
    message(FATAL_ERROR "Deployed Signalsmith license differs from the reviewed source text")
endif()
foreach(required_text "MIT License" "Geraint Luff / Signalsmith Audio Ltd.")
    string(FIND "${signalsmith_license}" "${required_text}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Packaged Signalsmith license is incomplete: ${required_text}")
    endif()
endforeach()

file(READ "${BUILD_DIR}/licenses/Mozilla-Public-License-2.0.txt" mpl_license)
file(SHA256 "${SOURCE_DIR}/resources/licenses/Mozilla-Public-License-2.0.txt" mpl_license_source_sha256)
file(SHA256 "${BUILD_DIR}/licenses/Mozilla-Public-License-2.0.txt" mpl_license_deployed_sha256)
if(NOT mpl_license_source_sha256 STREQUAL mpl_license_deployed_sha256)
    message(FATAL_ERROR "Deployed MPL 2.0 license differs from the reviewed source text")
endif()
foreach(required_text "Mozilla Public License Version 2.0" "Exhibit A - Source Code Form License Notice")
    string(FIND "${mpl_license}" "${required_text}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Packaged MPL 2.0 license is incomplete: ${required_text}")
    endif()
endforeach()

file(SHA256 "${SOURCE_DIR}/resources/licenses/Qt-ThirdParty-Attributions.txt" qt_attributions_source_sha256)
file(SHA256 "${BUILD_DIR}/licenses/Qt-ThirdParty-Attributions.txt" qt_attributions_deployed_sha256)
if(NOT qt_attributions_source_sha256 STREQUAL qt_attributions_deployed_sha256)
    message(FATAL_ERROR "Deployed Qt attribution appendix differs from the reviewed source text")
endif()
file(READ "${BUILD_DIR}/licenses/Qt-ThirdParty-Attributions.txt" qt_attributions)
foreach(required_text "Qt 6.11.2 third-party attribution appendix" "PCRE2" "Freetype 2"
        "HarfBuzz-NG" "LibPNG" "libpsl")
    string(FIND "${qt_attributions}" "${required_text}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Packaged Qt attribution appendix is incomplete: ${required_text}")
    endif()
endforeach()

set(expected_windows_sdk_notice_sha256
    "EBB6640A8246B39B70322C5CEC9AAD7A71E86EA7B86C9C604D3A7F3FF1B30E89")
file(SHA256 "${SOURCE_DIR}/resources/licenses/Windows-SDK-Third-Party-Notices.rtf"
    windows_sdk_notice_source_sha256)
file(SHA256 "${BUILD_DIR}/licenses/Windows-SDK-Third-Party-Notices.rtf"
    windows_sdk_notice_deployed_sha256)
string(TOUPPER "${windows_sdk_notice_source_sha256}" windows_sdk_notice_source_sha256)
string(TOUPPER "${windows_sdk_notice_deployed_sha256}" windows_sdk_notice_deployed_sha256)
if(NOT windows_sdk_notice_source_sha256 STREQUAL expected_windows_sdk_notice_sha256 OR
        NOT windows_sdk_notice_deployed_sha256 STREQUAL expected_windows_sdk_notice_sha256)
    message(FATAL_ERROR "Deployed Windows SDK third-party notices differ from the reviewed SDK copy")
endif()

file(READ "${BUILD_DIR}/licenses/FFmpeg-LGPL-2.1.txt" ffmpeg_license)
file(SHA256 "${SOURCE_DIR}/resources/licenses/FFmpeg-LGPL-2.1.txt" ffmpeg_license_source_sha256)
file(SHA256 "${BUILD_DIR}/licenses/FFmpeg-LGPL-2.1.txt" ffmpeg_license_deployed_sha256)
if(NOT ffmpeg_license_source_sha256 STREQUAL ffmpeg_license_deployed_sha256)
    message(FATAL_ERROR "Deployed FFmpeg license differs from the reviewed source text")
endif()
foreach(required_text "GNU LESSER GENERAL PUBLIC LICENSE" "Version 2.1, February 1999")
    string(FIND "${ffmpeg_license}" "${required_text}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Packaged FFmpeg license is incomplete: ${required_text}")
    endif()
endforeach()

file(READ "${BUILD_DIR}/LICENSE" project_license)
foreach(required_license_text "MIT License" "Copyright (c) 2026 Kiatami")
    string(FIND "${project_license}" "${required_license_text}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Packaged project license is incomplete: ${required_license_text}")
    endif()
endforeach()

execute_process(
    COMMAND "${BUILD_DIR}/FileManager.exe" --deployment-probe
    WORKING_DIRECTORY "${BUILD_DIR}"
    RESULT_VARIABLE deployment_probe_result
    TIMEOUT 20)
if(NOT deployment_probe_result EQUAL 0)
    message(FATAL_ERROR
        "The packaged-runtime deployment probe failed with exit code ${deployment_probe_result}")
endif()

if(NOT EXISTS "${BUILD_DIR}/FileManagerMaintenance.exe")
    message(FATAL_ERROR "The uninstall maintenance executable is missing from the deployment.")
endif()

file(READ "${SOURCE_DIR}/docs/DEPENDENCIES.md" dependency_inventory)
file(READ "${SOURCE_DIR}/THIRD_PARTY_NOTICES.md" third_party_notices)
file(SHA256 "${SOURCE_DIR}/THIRD_PARTY_NOTICES.md" source_notice_sha256)
file(SHA256 "${BUILD_DIR}/THIRD_PARTY_NOTICES.md" deployed_notice_sha256)
if(NOT source_notice_sha256 STREQUAL deployed_notice_sha256)
    message(FATAL_ERROR "Deployed third-party notice differs from current source.")
endif()
foreach(required_notice "Signalsmith Stretch" "DR Libs" "Two-Level Segregated Fit"
        "Boost Software License" "Independent JPEG Group" "XSVG" "Public Suffix List"
        "Windows SDK graphics redistributables")
    string(FIND "${third_party_notices}" "${required_notice}" notice_position)
    if(notice_position EQUAL -1)
        message(FATAL_ERROR "Third-party notice is missing: ${required_notice}")
    endif()
endforeach()
foreach(required_text "Qt" "6.11.2" "FFmpeg" "7.1.3" "minizip-ng"
        "7b2387161c542fa9f427352dcdef76097d0d692b" "zlib-ng"
        "12731092979c6d07f42da27da673a9f6c7b13586")
    string(FIND "${dependency_inventory}${third_party_notices}" "${required_text}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Dependency notice is missing: ${required_text}")
    endif()
endforeach()
