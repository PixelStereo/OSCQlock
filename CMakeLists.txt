cmake_minimum_required(VERSION 3.21)
include(cmake/Version.cmake)
project(OSCQlock VERSION ${OSCQLOCK_VERSION_NUMERIC} LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Widgets Network)
find_package(Threads REQUIRED)

# pxs-qt (OSC, OSCQuery, the parameter tree, the look) is a submodule, and it carries pxs-core itself: nothing else to pin
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pxs-qt/CMakeLists.txt")
    message(FATAL_ERROR "third_party/pxs-qt is empty: run\n  git submodule update --init --recursive")
endif()
set(PXS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/pxs-qt EXCLUDE_FROM_ALL)

# The icon: in the macOS bundle (.icns) and in the Windows executable (.ico)
set(OSCQLOCK_ICONS "")
if(APPLE)
    list(APPEND OSCQLOCK_ICONS packaging/oscqlock.icns)
    set_source_files_properties(packaging/oscqlock.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
elseif(WIN32)
    list(APPEND OSCQLOCK_ICONS packaging/oscqlock.rc)
endif()
add_executable(OSCQlock WIN32 MACOSX_BUNDLE src/main.cpp src/Device.cpp src/Device.h src/GeneratorEditor.cpp src/GeneratorEditor.h src/Panel.cpp src/Panel.h src/Destinations.cpp src/Destinations.h src/DevicesWindow.cpp src/DevicesWindow.h src/SequencePage.cpp src/SequencePage.h src/Show.cpp src/Show.h src/Commands.h ${OSCQLOCK_ICONS})
target_include_directories(OSCQlock PRIVATE src)
target_compile_definitions(OSCQlock PRIVATE OSCQLOCK_VERSION="${OSCQLOCK_VERSION}")
target_link_libraries(OSCQlock PRIVATE pxs::params pxs::tree pxs::oscquery pxs::theme pxs::widgets pxs::filterview pxs::interaction pxs::timelineview pxs::oscclient pxs::osc pxs::sequence Qt6::Widgets Qt6::Network)
set_target_properties(OSCQlock PROPERTIES OUTPUT_NAME "${OSCQLOCK_APP_NAME}"
    MACOSX_BUNDLE_ICON_FILE "oscqlock.icns" MACOSX_BUNDLE_GUI_IDENTIFIER "fr.pixelstereo.oscqlock" MACOSX_BUNDLE_BUNDLE_NAME "OSCQlock"
    # Our own Info.plist: macOS only lets an app use the local network (Bonjour, multicast) when it says why
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_VERSION "${OSCQLOCK_VERSION}" MACOSX_BUNDLE_SHORT_VERSION_STRING "${OSCQLOCK_VERSION_NUMERIC}")

# What the packaging scripts read: the name, the version, the executable
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/application.json" CONTENT
  "{\"name\":\"${OSCQLOCK_APP_NAME}\",\"version\":\"${OSCQLOCK_VERSION}\",\"executable\":\"$<TARGET_FILE_NAME:OSCQlock>\"}\n")
