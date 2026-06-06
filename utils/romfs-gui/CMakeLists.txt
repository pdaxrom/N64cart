cmake_minimum_required(VERSION 3.16)

set(_romfs_gui_vcpkg_search_roots)
if(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}")
    file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" _romfs_gui_env_vcpkg_root)
    list(APPEND _romfs_gui_vcpkg_search_roots "${_romfs_gui_env_vcpkg_root}")
endif()
list(APPEND _romfs_gui_vcpkg_search_roots
    "${CMAKE_CURRENT_LIST_DIR}/../../../vcpkg"
    "${CMAKE_CURRENT_LIST_DIR}/../../../../vcpkg"
)

set(_romfs_gui_default_vcpkg_root "")
foreach(_romfs_gui_candidate_root IN LISTS _romfs_gui_vcpkg_search_roots)
    get_filename_component(_romfs_gui_candidate_root "${_romfs_gui_candidate_root}" ABSOLUTE)
    if(EXISTS "${_romfs_gui_candidate_root}/scripts/buildsystems/vcpkg.cmake")
        set(_romfs_gui_default_vcpkg_root "${_romfs_gui_candidate_root}")
        break()
    endif()
endforeach()

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND _romfs_gui_default_vcpkg_root)
    set(CMAKE_TOOLCHAIN_FILE
        "${_romfs_gui_default_vcpkg_root}/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH "vcpkg toolchain file"
    )
    message(STATUS "romfs-gui: auto-detected vcpkg toolchain at ${CMAKE_TOOLCHAIN_FILE}")
endif()

if(NOT DEFINED VCPKG_MANIFEST_DIR AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/vcpkg.json")
    set(VCPKG_MANIFEST_DIR
        "${CMAKE_CURRENT_LIST_DIR}"
        CACHE PATH "vcpkg manifest directory"
    )
endif()

if(NOT DEFINED VCPKG_MANIFEST_INSTALL)
    set(VCPKG_MANIFEST_INSTALL ON CACHE BOOL "Install vcpkg manifest dependencies automatically")
    message(STATUS "romfs-gui: enabled automatic vcpkg manifest install")
endif()

if(WIN32 AND NOT DEFINED VCPKG_TARGET_TRIPLET)
    string(TOLOWER "${CMAKE_PREFIX_PATH};${Qt6_DIR}" _romfs_gui_qt_hints_lower)
    if(_romfs_gui_qt_hints_lower MATCHES "mingw")
        set(VCPKG_TARGET_TRIPLET "x64-mingw-dynamic" CACHE STRING "vcpkg target triplet")
        message(STATUS "romfs-gui: defaulting VCPKG_TARGET_TRIPLET to ${VCPKG_TARGET_TRIPLET}")
    elseif(_romfs_gui_qt_hints_lower MATCHES "msvc")
        set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "vcpkg target triplet")
        message(STATUS "romfs-gui: defaulting VCPKG_TARGET_TRIPLET to ${VCPKG_TARGET_TRIPLET}")
    endif()
endif()

project(romfs_gui LANGUAGES C CXX)

set(ROMFS_GUI_APP_NAME "ROMFS Manager")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network LinguistTools)
find_package(Git QUIET)
set(ROMFS_GUI_LIBUSB_INCLUDE_DIRS "")
set(ROMFS_GUI_LIBUSB_LINK_LIBRARIES "")
set(LIBUSB_PREFIX "")
set(LIBUSB_LIBDIR "")
set(LIBUSB_BINDIR "")

set(LIBUSB_RUNTIME_FILES "")
set(LIBUSB_RUNTIME_FILE_RELEASE "")
set(LIBUSB_RUNTIME_FILE_DEBUG "")
if(WIN32)
    include(FindPackageHandleStandardArgs)

    set(_romfs_gui_vcpkg_root "")
    set(_romfs_gui_vcpkg_installed_dir "")
    set(_romfs_gui_vcpkg_triplet "${VCPKG_TARGET_TRIPLET}")
    if(NOT _romfs_gui_vcpkg_triplet)
        set(_romfs_gui_vcpkg_triplet "x64-mingw-dynamic")
    endif()

    if(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}")
        file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" _romfs_gui_vcpkg_root)
    endif()
    if(NOT _romfs_gui_vcpkg_root AND CMAKE_TOOLCHAIN_FILE)
        get_filename_component(_romfs_gui_toolchain_dir "${CMAKE_TOOLCHAIN_FILE}" DIRECTORY)
        get_filename_component(_romfs_gui_vcpkg_root "${_romfs_gui_toolchain_dir}/../.." ABSOLUTE)
        if(NOT EXISTS "${_romfs_gui_vcpkg_root}")
            set(_romfs_gui_vcpkg_root "")
        endif()
    endif()
    if(NOT _romfs_gui_vcpkg_root)
        foreach(_romfs_gui_candidate_root IN LISTS _romfs_gui_vcpkg_search_roots)
            get_filename_component(_romfs_gui_candidate_root "${_romfs_gui_candidate_root}" ABSOLUTE)
            if(EXISTS "${_romfs_gui_candidate_root}")
                set(_romfs_gui_vcpkg_root "${_romfs_gui_candidate_root}")
                break()
            endif()
        endforeach()
        if(NOT _romfs_gui_vcpkg_root)
            set(_romfs_gui_vcpkg_root "")
        endif()
    endif()

    if(VCPKG_INSTALLED_DIR)
        file(TO_CMAKE_PATH "${VCPKG_INSTALLED_DIR}" _romfs_gui_vcpkg_installed_dir)
    elseif(_romfs_gui_vcpkg_root)
        set(_romfs_gui_vcpkg_installed_dir "${_romfs_gui_vcpkg_root}/installed")
    endif()

    set(_libusb_prefix_hints)
    if(_romfs_gui_vcpkg_installed_dir AND _romfs_gui_vcpkg_triplet)
        list(APPEND _libusb_prefix_hints
            "${_romfs_gui_vcpkg_installed_dir}/${_romfs_gui_vcpkg_triplet}"
        )
    endif()
    if(_romfs_gui_vcpkg_root)
        list(APPEND _libusb_prefix_hints "${_romfs_gui_vcpkg_root}")
    endif()
    foreach(_cmake_prefix IN LISTS CMAKE_PREFIX_PATH)
        list(APPEND _libusb_prefix_hints "${_cmake_prefix}")
    endforeach()
    list(REMOVE_DUPLICATES _libusb_prefix_hints)

    find_path(LIBUSB_INCLUDE_DIR
        NAMES libusb.h
        HINTS ${_libusb_prefix_hints}
        PATH_SUFFIXES include/libusb-1.0 include
    )
    find_library(LIBUSB_LIBRARY_RELEASE
        NAMES usb-1.0 libusb-1.0
        HINTS ${_libusb_prefix_hints}
        PATH_SUFFIXES lib bin
    )
    find_library(LIBUSB_LIBRARY_DEBUG
        NAMES usb-1.0 libusb-1.0
        HINTS ${_libusb_prefix_hints}
        PATH_SUFFIXES debug/lib debug/bin lib bin
    )

    set(_libusb_link_release_selected "${LIBUSB_LIBRARY_RELEASE}")
    if(NOT _libusb_link_release_selected)
        set(_libusb_link_release_selected "${LIBUSB_LIBRARY_DEBUG}")
    endif()

    set(_libusb_link_debug_selected "${LIBUSB_LIBRARY_DEBUG}")
    if(NOT _libusb_link_debug_selected)
        set(_libusb_link_debug_selected "${LIBUSB_LIBRARY_RELEASE}")
    endif()

    find_package_handle_standard_args(LibUSB
        REQUIRED_VARS
            LIBUSB_INCLUDE_DIR
            _libusb_link_release_selected
    )

    set(ROMFS_GUI_LIBUSB_INCLUDE_DIRS "${LIBUSB_INCLUDE_DIR}")
    set(ROMFS_GUI_LIBUSB_LINK_LIBRARIES
        "$<IF:$<CONFIG:Debug>,${_libusb_link_debug_selected},${_libusb_link_release_selected}>"
    )

    set(_libusb_runtime_release_hints)
    set(_libusb_runtime_debug_hints)
    foreach(_cmake_prefix IN LISTS _libusb_prefix_hints)
        list(APPEND _libusb_runtime_release_hints
            "${_cmake_prefix}/bin"
            "${_cmake_prefix}/debug/bin"
            "${_cmake_prefix}/tools/libusb"
            "${_cmake_prefix}/tools/libusb/bin"
        )
        list(APPEND _libusb_runtime_debug_hints
            "${_cmake_prefix}/debug/bin"
            "${_cmake_prefix}/bin"
            "${_cmake_prefix}/tools/libusb"
            "${_cmake_prefix}/tools/libusb/bin"
        )
    endforeach()
    foreach(_libusb_link_file IN ITEMS
            "${_libusb_link_release_selected}"
            "${_libusb_link_debug_selected}")
        if(_libusb_link_file)
            get_filename_component(_libusb_link_dir "${_libusb_link_file}" DIRECTORY)
            list(APPEND _libusb_runtime_release_hints
                "${_libusb_link_dir}"
                "${_libusb_link_dir}/../bin"
                "${_libusb_link_dir}/../debug/bin"
                "${_libusb_link_dir}/../tools/libusb"
            )
            list(APPEND _libusb_runtime_debug_hints
                "${_libusb_link_dir}/../debug/bin"
                "${_libusb_link_dir}/../bin"
                "${_libusb_link_dir}"
                "${_libusb_link_dir}/../tools/libusb"
            )
        endif()
    endforeach()
    if(LIBUSB_INCLUDE_DIR)
        get_filename_component(_libusb_include_prefix "${LIBUSB_INCLUDE_DIR}" DIRECTORY)
        get_filename_component(_libusb_include_prefix "${_libusb_include_prefix}" DIRECTORY)
        list(APPEND _libusb_runtime_release_hints
            "${_libusb_include_prefix}/bin"
            "${_libusb_include_prefix}/debug/bin"
            "${_libusb_include_prefix}/tools/libusb"
        )
        list(APPEND _libusb_runtime_debug_hints
            "${_libusb_include_prefix}/debug/bin"
            "${_libusb_include_prefix}/bin"
            "${_libusb_include_prefix}/tools/libusb"
        )
    endif()
    list(REMOVE_DUPLICATES _libusb_runtime_release_hints)
    list(REMOVE_DUPLICATES _libusb_runtime_debug_hints)

    foreach(_libusb_runtime_dir IN LISTS _libusb_runtime_release_hints)
        if(NOT LIBUSB_RUNTIME_FILE_RELEASE)
            foreach(_libusb_runtime_name IN ITEMS libusb-1.0.dll liblibusb-1.0.dll)
                if(EXISTS "${_libusb_runtime_dir}/${_libusb_runtime_name}")
                    set(LIBUSB_RUNTIME_FILE_RELEASE "${_libusb_runtime_dir}/${_libusb_runtime_name}")
                    break()
                endif()
            endforeach()
        endif()
    endforeach()
    foreach(_libusb_runtime_dir IN LISTS _libusb_runtime_debug_hints)
        if(NOT LIBUSB_RUNTIME_FILE_DEBUG)
            foreach(_libusb_runtime_name IN ITEMS libusb-1.0.dll liblibusb-1.0.dll)
                if(EXISTS "${_libusb_runtime_dir}/${_libusb_runtime_name}")
                    set(LIBUSB_RUNTIME_FILE_DEBUG "${_libusb_runtime_dir}/${_libusb_runtime_name}")
                    break()
                endif()
            endforeach()
        endif()
    endforeach()

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
else()
    set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH ON)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
    pkg_get_variable(LIBUSB_PREFIX libusb-1.0 prefix)
    pkg_get_variable(LIBUSB_LIBDIR libusb-1.0 libdir)
    pkg_get_variable(LIBUSB_BINDIR libusb-1.0 bindir)
    set(ROMFS_GUI_LIBUSB_LINK_LIBRARIES PkgConfig::LIBUSB)
endif()

if(APPLE AND LIBUSB_LIBDIR)
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
    translations/romfs-gui_de.ts
    translations/romfs-gui_en.ts
    translations/romfs-gui_es.ts
    translations/romfs-gui_fa.ts
    translations/romfs-gui_hy.ts
    translations/romfs-gui_pt_BR.ts
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
    ${ROMFS_GUI_LIBUSB_INCLUDE_DIRS}
)

target_link_libraries(romfs-gui PRIVATE
    Qt6::Widgets
    Qt6::Network
    ${ROMFS_GUI_LIBUSB_LINK_LIBRARIES}
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
