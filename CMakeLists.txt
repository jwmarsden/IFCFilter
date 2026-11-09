cmake_minimum_required(VERSION 3.21)
project(IFCFilter VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Boost CONFIG REQUIRED)
find_package(ZLIB REQUIRED)


# An installed IfcOpenShell if there is one, as the flake provides. Otherwise the submodule, built
# here with the same options: the parser only, and just the schemas the models use.
find_package(IfcOpenShell CONFIG QUIET)
if(IfcOpenShell_FOUND)
    message(STATUS "IfcOpenShell: installed, at ${IfcOpenShell_DIR}")
else()
    set(IFCOPENSHELL_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/IfcOpenShell)
    if(NOT EXISTS ${IFCOPENSHELL_SOURCE}/cmake/CMakeLists.txt)
        message(FATAL_ERROR "IfcOpenShell isn't installed or checked out: run git submodule update --init")
    endif()
    message(STATUS "IfcOpenShell: building the submodule")
    set(SCHEMA_VERSIONS 2x3 4)
    foreach(flag BUILD_IFCGEOM BUILD_IFCPYTHON BUILD_CONVERT BUILD_GEOMSERVER BUILD_EXAMPLES WITH_OPENCASCADE WITH_CGAL COLLADA_SUPPORT)
        set(${flag} OFF)
    endforeach()
    add_subdirectory(${IFCOPENSHELL_SOURCE}/cmake ifcopenshell EXCLUDE_FROM_ALL)
    add_library(IfcOpenShell::IfcParse ALIAS IfcParse)
    add_library(IfcOpenShell::parse_schema_ifc2x3 ALIAS parse_schema_ifc2x3)
    add_library(IfcOpenShell::parse_schema_ifc4 ALIAS parse_schema_ifc4)
endif()

add_executable(ifcfilter src/main.cpp src/report.cpp src/split.cpp src/merge.cpp src/bake.cpp src/extent.cpp src/mapping.cpp src/instance.cpp src/rigid.cpp src/register.cpp src/system.cpp src/upgrade.cpp src/spf.cpp src/breps.cpp src/compact.cpp src/tidy.cpp src/zip.cpp)
# The schemas are plugins, loaded when a file is opened, so nothing links to them
add_dependencies(ifcfilter IfcOpenShell::parse_schema_ifc2x3 IfcOpenShell::parse_schema_ifc4)
target_compile_definitions(ifcfilter PRIVATE BOOST_OPTIONAL_USE_OLD_DEFINITION_OF_NONE IFCFILTER_VERSION="${PROJECT_VERSION}")
if(MSVC)
    target_compile_options(ifcfilter PRIVATE /W4 /bigobj /utf-8)
else()
    target_compile_options(ifcfilter PRIVATE -Wall -Wextra)
endif()
target_link_libraries(ifcfilter PRIVATE IfcOpenShell::IfcParse Boost::headers ZLIB::ZLIB)
if(WIN32)
    target_compile_definitions(ifcfilter PRIVATE NOMINMAX)
    # IfcOpenShell only builds as shared libraries, so its DLLs go beside the program. Schema plugins
    # are looked for beside the parser's DLL, and aren't linked, so they have to be named.
    add_custom_command(TARGET ifcfilter POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:ifcfilter>
            $<TARGET_FILE:IfcOpenShell::parse_schema_ifc2x3>
            $<TARGET_FILE:IfcOpenShell::parse_schema_ifc4>
            $<TARGET_FILE_DIR:ifcfilter>
        COMMAND_EXPAND_LISTS)
endif()

include(GNUInstallDirs)
install(TARGETS ifcfilter RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
