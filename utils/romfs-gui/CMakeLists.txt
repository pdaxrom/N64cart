cmake_minimum_required(VERSION 3.16)

project(romfs_gui LANGUAGES C CXX)

set(ROMFS_GUI_APP_NAME "ROMFS Manager")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network LinguistTools)
find_package(Git QUIET)
set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH ON)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
pkg_get_variable(LIBUSB_PREFIX libusb-1.0 prefix)
pkg_get_variable(LIBUSB_LIBDIR libusb-1.0 libdir)
pkg_get_variable(LIBUSB_BINDIR libusb-1.0 bindir)

set(LIBUSB_RUNTIME_FILES "")
set(LIBUSB_RUNTIME_FILE_RELEASE "")
set(LIBUSB_RUNTIME_FILE_DEBUG "")
if(WIN32)
    set(_libusb_runtime_release_hints)
    set(_libusb_runtime_debug_hints)
    if(LIBUSB_BINDIR)
        list(APPEND _libusb_runtime_release_hints "${LIBUSB_BINDIR}")
        list(APPEND _libusb_runtime_debug_hints "${LIBUSB_BINDIR}")
    endif()
    if(LIBUSB_PREFIX)
        list(APPEND _libusb_runtime_release_hints
            "${LIBUSB_PREFIX}/bin"
            "${LIBUSB_PREFIX}/debug/bin"
        )
        list(APPEND _libusb_runtime_debug_hints
            "${LIBUSB_PREFIX}/debug/bin"
            "${LIBUSB_PREFIX}/bin"
        )
    endif()
    if(LIBUSB_LIBDIR)
        list(APPEND _libusb_runtime_release_hints
            "${LIBUSB_LIBDIR}/../bin"
            "${LIBUSB_LIBDIR}"
            "${LIBUSB_LIBDIR}/../debug/bin"
        )
        list(APPEND _libusb_runtime_debug_hints
            "${LIBUSB_LIBDIR}/../debug/bin"
            "${LIBUSB_LIBDIR}/../bin"
            "${LIBUSB_LIBDIR}"
        )
    endif()
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        list(APPEND _libusb_runtime_release_hints
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin"
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin"
        )
        list(APPEND _libusb_runtime_debug_hints
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin"
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin"
        )
    endif()
    list(REMOVE_DUPLICATES _libusb_runtime_release_hints)
    list(REMOVE_DUPLICATES _libusb_runtime_debug_hints)

    find_file(LIBUSB_RUNTIME_FILE_RELEASE
        NAMES libusb-1.0.dll liblibusb-1.0.dll
        HINTS ${_libusb_runtime_release_hints}
        PATH_SUFFIXES .
        NO_DEFAULT_PATH
    )
    find_file(LIBUSB_RUNTIME_FILE_DEBUG
        NAMES libusb-1.0.dll liblibusb-1.0.dll
        HINTS ${_libusb_runtime_debug_hints}
        PATH_SUFFIXES .
        NO_DEFAULT_PATH
    )

    set(_libusb_runtime_release_selected "${LIBUSB_RUNTIME_FILE_RELEASE}")
    if(NOT _libusb_runtime_release_selected)
        set(_libusb_runtime_release_selected "${LIBUSB_RUNTIME_FILE_DEBUG}")
    endif()

    set(_libusb_runtime_debug_selected "${LIBUSB_RUNTIME_FILE_DEBUG}")
    if(NOT _libusb_runtime_debug_selected)
        set(_libusb_runtime_debug_selected "${LIBUSB_RUNTIME_FILE_RELEASE}")
    endif()

    foreach(_runtime_file IN ITEMS
            "${_libusb_runtime_release_selected}"
            "${_libusb_runtime_debug_selected}")
        if(_runtime_file)
            list(APPEND LIBUSB_RUNTIME_FILES "${_runtime_file}")
        endif()
    endforeach()
elseif(APPLE AND LIBUSB_LIBDIR)
    file(GLOB LIBUSB_RUNTIME_FILES CONFIGURE_DEPENDS
        "${LIBUSB_LIBDIR}/libusb-1.0*.dylib"
    )
elseif(UNIX AND LIBUSB_LIBDIR)
    file(GLOB LIBUSB_RUNTIME_FILES CONFIGURE_DEPENDS
        "${LIBUSB_LIBDIR}/libusb-1.0.so*"
    )
endif()
list(REMOVE_DUPLICATES LIBUSB_RUNTIME_FILES)

set(ROMFS_GUI_GIT_HASH "")
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../.."
        OUTPUT_VARIABLE ROMFS_GUI_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()

set(ROMFS_SOURCES
    ../../fw/romfs/romfs.c
    ../simple-connection-lib/src/tcp.c
    ../simple-connection-lib/src/getrandom.c
    ../simple-connection-lib/src/base64.c
)

qt_add_executable(romfs-gui
    WIN32
    MACOSX_BUNDLE
    MANUAL_FINALIZATION
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    resources.qrc
    settingsdialog.cpp
    settingsdialog.h
    settingsdialog.ui
    romfsiconview.cpp
    romfsiconview.h
    romfsdevice.cpp
    romfsdevice.h
    romfsmodel.cpp
    romfsmodel.h
    romfsbridge.cpp
    romfsbridge.h
    romfstransport.h
    usbtransport.cpp
    usbtransport.h
    remotetransport.cpp
    remotetransport.h
    romfsview.cpp
    romfsview.h
    ${ROMFS_SOURCES}
)

set(TS_FILES
    translations/romfs-gui_en.ts
    translations/romfs-gui_ru.ts
)

target_sources(romfs-gui PRIVATE
    ${TS_FILES}
)

set(ROMFS_TRANSLATION_LANGS "")
foreach(ts_file ${TS_FILES})
    get_filename_component(ts_name "${ts_file}" NAME_WE)
    string(REGEX MATCH "_([A-Za-z0-9_]+)$" _match "${ts_name}")
    if(CMAKE_MATCH_1)
        list(APPEND ROMFS_TRANSLATION_LANGS "${CMAKE_MATCH_1}")
    endif()
endforeach()
list(REMOVE_DUPLICATES ROMFS_TRANSLATION_LANGS)

set(QT_TRANSLATION_DIR "")
if(TARGET Qt6::qmake)
    get_target_property(QT6_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
    if(QT6_QMAKE_EXECUTABLE)
        execute_process(
            COMMAND "${QT6_QMAKE_EXECUTABLE}" -query QT_INSTALL_TRANSLATIONS
            OUTPUT_VARIABLE QT_TRANSLATION_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
    endif()
endif()

set(QTBASE_QM_FILES "")
if(QT_TRANSLATION_DIR AND ROMFS_TRANSLATION_LANGS)
    foreach(lang ${ROMFS_TRANSLATION_LANGS})
        set(candidate "${QT_TRANSLATION_DIR}/qtbase_${lang}.qm")
        if(EXISTS "${candidate}")
            list(APPEND QTBASE_QM_FILES "${candidate}")
        endif()
    endforeach()
endif()

qt_add_lupdate(romfs-gui
    TS_FILES ${TS_FILES}
)

qt_add_lrelease(romfs-gui
    TS_FILES ${TS_FILES}
    QM_FILES_OUTPUT_VARIABLE ROMFS_QM_FILES
)

# MANUAL_FINALIZATION

if(APPLE)
    set_target_properties(romfs-gui PROPERTIES
        OUTPUT_NAME "${ROMFS_GUI_APP_NAME}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.pdaxrom.romfsmanager"
        MACOSX_BUNDLE_BUNDLE_NAME "${ROMFS_GUI_APP_NAME}"
        MACOSX_BUNDLE_INFO_STRING "${ROMFS_GUI_APP_NAME}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
        MACOSX_BUNDLE_ICON_FILE "icon.icns"
    )

    set(MACOSX_ICON_FILE "${CMAKE_CURRENT_SOURCE_DIR}/icons/icon.icns")
    set_source_files_properties(${MACOSX_ICON_FILE} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    target_sources(romfs-gui PRIVATE ${MACOSX_ICON_FILE})
else()
    set_target_properties(romfs-gui PROPERTIES
        OUTPUT_NAME "${ROMFS_GUI_APP_NAME}"
    )
endif()

target_include_directories(romfs-gui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../../fw/romfs
    ../simple-connection-lib/src
    ../
)

target_link_libraries(romfs-gui PRIVATE
    Qt6::Widgets
    Qt6::Network
    PkgConfig::LIBUSB
)

if(ROMFS_GUI_GIT_HASH)
    target_compile_definitions(romfs-gui PRIVATE ROMFS_GUI_GIT_HASH="${ROMFS_GUI_GIT_HASH}")
endif()

if(LIBUSB_LIBDIR AND UNIX)
    set_property(TARGET romfs-gui APPEND PROPERTY BUILD_RPATH "${LIBUSB_LIBDIR}")
endif()

if(ROMFS_QM_FILES OR QTBASE_QM_FILES)
    if(APPLE)
        set(_translation_dest "$<TARGET_BUNDLE_DIR:romfs-gui>/Contents/Resources/translations")
    else()
        set(_translation_dest "$<TARGET_FILE_DIR:romfs-gui>/translations")
    endif()

    set(_translation_commands
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_translation_dest}"
    )
    if(ROMFS_QM_FILES)
        list(APPEND _translation_commands
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ROMFS_QM_FILES} "${_translation_dest}"
        )
    endif()
    if(QTBASE_QM_FILES)
        list(APPEND _translation_commands
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${QTBASE_QM_FILES} "${_translation_dest}"
        )
    endif()

    add_custom_command(TARGET romfs-gui POST_BUILD
        ${_translation_commands}
        COMMENT "Copying translations into runtime directory"
        VERBATIM
    )
endif()

if (WIN32)
    target_link_libraries(romfs-gui PRIVATE ws2_32)
    target_sources(romfs-gui PRIVATE romfs-gui.rc)
    if(_libusb_runtime_release_selected OR _libusb_runtime_debug_selected)
        add_custom_command(TARGET romfs-gui POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<IF:$<CONFIG:Debug>,${_libusb_runtime_debug_selected},${_libusb_runtime_release_selected}>"
                    "$<TARGET_FILE_DIR:romfs-gui>"
            COMMENT "Copying libusb runtime DLL"
            VERBATIM
        )
    else()
        message(WARNING "libusb runtime DLL was not found during configure. The application may fail to start until libusb-1.0.dll is copied next to the executable.")
    endif()
endif()

set(_qt_bin_dir "")
set(_qt_qmake "")
if(TARGET Qt6::qmake)
    get_target_property(_qt_qmake Qt6::qmake IMPORTED_LOCATION)
endif()

set(QT_QMAKE_EXECUTABLE "${_qt_qmake}")
if(_qt_qmake)
    get_filename_component(_qt_bin_dir "${_qt_qmake}" DIRECTORY)
endif()

if(NOT QT_QMAKE_EXECUTABLE)
    set(_qt_qmake_hints)
    if(_qt_bin_dir)
        list(APPEND _qt_qmake_hints "${_qt_bin_dir}")
    endif()
    if(Qt6_DIR)
        get_filename_component(_qt_prefix_from_dir "${Qt6_DIR}" DIRECTORY)
        get_filename_component(_qt_prefix_from_dir "${_qt_prefix_from_dir}" DIRECTORY)
        get_filename_component(_qt_prefix_from_dir "${_qt_prefix_from_dir}" DIRECTORY)
        list(APPEND _qt_qmake_hints "${_qt_prefix_from_dir}/bin")
    endif()
    foreach(_cmake_prefix IN LISTS CMAKE_PREFIX_PATH)
        list(APPEND _qt_qmake_hints "${_cmake_prefix}/bin")
    endforeach()

    find_program(QT_QMAKE_EXECUTABLE
        NAMES qmake qmake6 qmake-qt6
        HINTS ${_qt_qmake_hints}
    )
    if(QT_QMAKE_EXECUTABLE AND NOT _qt_bin_dir)
        get_filename_component(_qt_bin_dir "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
    endif()
endif()

qt_finalize_executable(romfs-gui)

if(WIN32)
    find_program(QT_WINDEPLOYQT_EXECUTABLE
        NAMES windeployqt windeployqt6
        HINTS "${_qt_bin_dir}"
    )
    if(QT_WINDEPLOYQT_EXECUTABLE)
        set(_deploy_commands
            COMMAND "${QT_WINDEPLOYQT_EXECUTABLE}"
                    "$<TARGET_FILE:romfs-gui>"
        )
        if(_libusb_runtime_release_selected OR _libusb_runtime_debug_selected)
            list(APPEND _deploy_commands
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<IF:$<CONFIG:Debug>,${_libusb_runtime_debug_selected},${_libusb_runtime_release_selected}>"
                        "$<TARGET_FILE_DIR:romfs-gui>"
            )
        endif()
        add_custom_target(deploy
            ${_deploy_commands}
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            DEPENDS romfs-gui
            COMMENT "Running windeployqt on ${ROMFS_GUI_APP_NAME}"
            VERBATIM
        )
    endif()
elseif(APPLE)
    find_program(QT_MACDEPLOYQT_EXECUTABLE
        NAMES macdeployqt
        HINTS "${_qt_bin_dir}"
    )
    if(QT_MACDEPLOYQT_EXECUTABLE)
        set(_deploy_commands
            COMMAND "${QT_MACDEPLOYQT_EXECUTABLE}"
                    "$<TARGET_BUNDLE_DIR:romfs-gui>"
                    -no-strip
        )
        if(LIBUSB_RUNTIME_FILES)
            set(_deploy_commands
                COMMAND ${CMAKE_COMMAND} -E make_directory
                        "$<TARGET_BUNDLE_DIR:romfs-gui>/Contents/Frameworks"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${LIBUSB_RUNTIME_FILES}
                        "$<TARGET_BUNDLE_DIR:romfs-gui>/Contents/Frameworks"
                ${_deploy_commands}
            )
        endif()
        add_custom_target(deploy
            ${_deploy_commands}
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            DEPENDS romfs-gui
            COMMENT "Running macdeployqt on ${ROMFS_GUI_APP_NAME}"
            VERBATIM
        )
    endif()
elseif(UNIX)
    set(ROMFS_GUI_LINUX_DEPLOY_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/deploy-linux.sh")
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/scripts/deploy-linux.sh.in
                   ${ROMFS_GUI_LINUX_DEPLOY_SCRIPT}
                   @ONLY)
    add_custom_target(deploy
        COMMAND bash "${ROMFS_GUI_LINUX_DEPLOY_SCRIPT}"
                "$<TARGET_FILE:romfs-gui>"
                "${CMAKE_CURRENT_BINARY_DIR}/deploy/${PROJECT_NAME}-linux-portable"
                "${CMAKE_CURRENT_BINARY_DIR}/deploy/${PROJECT_NAME}-linux-portable.tar.gz"
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        DEPENDS romfs-gui
        COMMENT "Bundling portable Linux package for ${ROMFS_GUI_APP_NAME}"
        VERBATIM
    )
endif()
