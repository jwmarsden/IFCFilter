cmake_minimum_required(VERSION 3.21)
project(BIMPass VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Boost CONFIG REQUIRED)
find_package(IfcOpenShell CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

add_executable(bimpass src/main.cpp src/report.cpp src/split.cpp src/spf.cpp src/compact.cpp src/zip.cpp)
target_compile_definitions(bimpass PRIVATE BOOST_OPTIONAL_USE_OLD_DEFINITION_OF_NONE)
if(MSVC)
    target_compile_options(bimpass PRIVATE /W4 /bigobj /utf-8)
else()
    target_compile_options(bimpass PRIVATE -Wall -Wextra)
endif()
target_link_libraries(bimpass PRIVATE IfcOpenShell::IfcParse Boost::headers ZLIB::ZLIB)
if(WIN32)
    target_compile_definitions(bimpass PRIVATE NOMINMAX)
    # IfcOpenShell only builds as a shared library, so its DLL goes beside the program
    add_custom_command(TARGET bimpass POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:bimpass> $<TARGET_FILE_DIR:bimpass>
        COMMAND_EXPAND_LISTS)
endif()

include(GNUInstallDirs)
install(TARGETS bimpass RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
