cmake_minimum_required(VERSION 3.16)

project(romfs_gui LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

option(ENABLE_SYSTEM_LIBUSB "Enable system libusb" OFF)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network LinguistTools)

if(WIN32)
    set(LIBUSB_INCLUDE_DIRS "vcpkg/installed/x64-windows/include/libusb-1.0")
    set(LIBUSB_LIBRARY_DIRS "vcpkg/installed/x64-windows/lib")
    set(LIBUSB_LIBRARIES "usb-1.0")
elseif(APPLE AND NOT ENABLE_SYSTEM_LIBUSB)
    include(ExternalProject)

    ExternalProject_Add(
        libusb
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/../libusb-1.0.26
        BINARY_DIR ${CMAKE_BINARY_DIR}/_build/libusb-1.0.26
        INSTALL_DIR ${CMAKE_BINARY_DIR}/_install

        CONFIGURE_COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_BINARY_DIR}/_build/libusb-1.0.26 ${CMAKE_SOURCE_DIR}/../libusb-1.0.26/configure --prefix=${CMAKE_BINARY_DIR}/_install
        BUILD_COMMAND make
        INSTALL_COMMAND make install
    )

    set(LIBUSB_INCLUDE_DIRS "${CMAKE_BINARY_DIR}/_install/include/libusb-1.0")
    set(LIBUSB_LIBRARY_DIRS "${CMAKE_BINARY_DIR}/_install/lib")
    set(LIBUSB_LIBRARIES "usb-1.0")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
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
    if(NOT ENABLE_SYSTEM_LIBUSB)
        add_dependencies(romfs-gui libusb)
    endif()

    set_target_properties(romfs-gui PROPERTIES
        OUTPUT_NAME "ROMFS Manager"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.pdaxrom.romfsmanager"
        MACOSX_BUNDLE_BUNDLE_NAME "ROMFS Manager"
        MACOSX_BUNDLE_INFO_STRING "ROMFS Manager"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
        MACOSX_BUNDLE_ICON_FILE "icon.icns"
    )

    set(MACOSX_ICON_FILE "${CMAKE_CURRENT_SOURCE_DIR}/icons/icon.icns")
    set_source_files_properties(${MACOSX_ICON_FILE} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    target_sources(romfs-gui PRIVATE ${MACOSX_ICON_FILE})

    if(ROMFS_QM_FILES OR QTBASE_QM_FILES)
        set(_translation_dest "$<TARGET_BUNDLE_DIR:romfs-gui>/Contents/Resources/translations")
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
            COMMENT "Copying translations into app bundle"
            VERBATIM
        )
    endif()
else()
    set_target_properties(romfs-gui PROPERTIES
        OUTPUT_NAME "ROMFS Manager"
    )
endif()

target_include_directories(romfs-gui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../../fw/romfs
    ../simple-connection-lib/src
    ../
    ${LIBUSB_INCLUDE_DIRS}
)

target_link_directories(romfs-gui PRIVATE
    ${LIBUSB_LIBRARY_DIRS}
)

target_link_libraries(romfs-gui PRIVATE
    Qt6::Widgets
    Qt6::Network
    ${LIBUSB_LIBRARIES}
)

if (WIN32)
    target_link_libraries(romfs-gui PRIVATE ws2_32)
    target_sources(romfs-gui PRIVATE romfs-gui.rc)
endif()

# qt_finalize_executable(romfs-gui)

# set(QT_DEPLOY_BIN_DIR "")
# if(TARGET Qt6::qmake)
#     get_target_property(QT6_QMAKE_PATH Qt6::qmake IMPORTED_LOCATION)
#     if(QT6_QMAKE_PATH)
#         get_filename_component(QT_DEPLOY_BIN_DIR "${QT6_QMAKE_PATH}" DIRECTORY)
#     endif()
# endif()

# if(APPLE AND QT_DEPLOY_BIN_DIR)
#     set(QT_DEPLOY_TOOL "${QT_DEPLOY_BIN_DIR}/macdeployqt")
#     add_custom_command(TARGET romfs-gui POST_BUILD
#         COMMAND "${QT_DEPLOY_TOOL}" "$<TARGET_BUNDLE_DIR:romfs-gui>" -always-overwrite
#         COMMENT "Deploying Qt frameworks into app bundle"
#         VERBATIM
#     )
# elseif(WIN32 AND QT_DEPLOY_BIN_DIR)
#     set(QT_DEPLOY_TOOL "${QT_DEPLOY_BIN_DIR}/windeployqt")
#     add_custom_command(TARGET romfs-gui POST_BUILD
#         COMMAND "${QT_DEPLOY_TOOL}" --no-translations --no-opengl-sw "$<TARGET_FILE:romfs-gui>"
#         COMMENT "Deploying Qt runtime dependencies"
#         VERBATIM
#     )
# endif()
