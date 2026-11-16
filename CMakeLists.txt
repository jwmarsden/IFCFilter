cmake_minimum_required(VERSION 3.21)
project(IFCFilter VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Build the vendored Open CASCADE independently: OCCT's CMake assumes it is the
# top-level project. Its installed package can then be consumed by IfcOpenShell.
option(IFCFILTER_BUILD_OCCT "Build the Open CASCADE submodule" OFF)
option(IFCFILTER_WITH_GEOMETRY "Use IfcOpenShell geometry for opening cuts" OFF)
set(IFCFILTER_MAX_OPENINGS 32 CACHE STRING "Maximum openings per element for cache geometry; 0 disables the limit")
set(IFCFILTER_MAX_CUT_WORK 192 CACHE STRING "Maximum opening and shape-item combinations per element; 0 disables the limit")
set(IFCFILTER_MAX_OPENING_FACES 512 CACHE STRING "Maximum mesh faces in one opening; 0 disables the limit")
if(NOT IFCFILTER_MAX_OPENINGS MATCHES "^[0-9]+$")
    message(FATAL_ERROR "IFCFILTER_MAX_OPENINGS must be a non-negative integer")
endif()
if(NOT IFCFILTER_MAX_CUT_WORK MATCHES "^[0-9]+$")
    message(FATAL_ERROR "IFCFILTER_MAX_CUT_WORK must be a non-negative integer")
endif()
if(NOT IFCFILTER_MAX_OPENING_FACES MATCHES "^[0-9]+$")
    message(FATAL_ERROR "IFCFILTER_MAX_OPENING_FACES must be a non-negative integer")
endif()
if(IFCFILTER_BUILD_OCCT)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/OCCT/CMakeLists.txt")
        message(FATAL_ERROR "Open CASCADE is not checked out: run git submodule update --init")
    endif()
    include(ExternalProject)
    ExternalProject_Add(ifcfilter_occt
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/OCCT"
        BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/occt-build"
        INSTALL_DIR "${CMAKE_CURRENT_BINARY_DIR}/occt-install"
        CMAKE_ARGS
            -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
            -DBUILD_USE_VCPKG=OFF
            -DBUILD_LIBRARY_TYPE=Shared
            -DBUILD_MODULE_Draw=OFF
            -DBUILD_MODULE_Visualization=OFF
            -DBUILD_MODULE_ApplicationFramework=OFF
            -DBUILD_MODULE_DataExchange=OFF
            -DUSE_FREETYPE=OFF
            -DUSE_TCL=OFF
        # Each library finds the others beside it once installed, as the geometry kernel loads them as a
        # plugin: otherwise Linux can't find libTKFillet and the rest, and every cut falls back to uncut
        CMAKE_CACHE_ARGS
            "-DCMAKE_INSTALL_RPATH:STRING=$ORIGIN")
endif()

find_package(Boost CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
if(IFCFILTER_WITH_GEOMETRY)
    find_package(Eigen3 3.3.9 CONFIG REQUIRED)
endif()
# Decimating meshes (simplify.cpp)
find_package(OpenMesh CONFIG REQUIRED)

# For the viewing cache
find_package(SQLite3 REQUIRED)
if(TARGET SQLite3::SQLite3)
    set(SQLITE_LIBRARY SQLite3::SQLite3)
else()
    set(SQLITE_LIBRARY SQLite::SQLite3)
endif()
find_package(zstd CONFIG REQUIRED)
if(TARGET zstd::libzstd)
    set(ZSTD_LIBRARY zstd::libzstd)
elseif(TARGET zstd::libzstd_shared)
    set(ZSTD_LIBRARY zstd::libzstd_shared)
else()
    set(ZSTD_LIBRARY zstd::libzstd_static)
endif()

# An installed IfcOpenShell if there is one, as the flake provides. Otherwise the submodule, built
# here with the same options: the parser only, and just the schemas the models use.
if(NOT IFCFILTER_WITH_GEOMETRY)
    find_package(IfcOpenShell CONFIG QUIET)
endif()
if(IfcOpenShell_FOUND)
    message(STATUS "IfcOpenShell: installed, at ${IfcOpenShell_DIR}")
else()
    set(IFCOPENSHELL_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/IfcOpenShell)
    if(NOT EXISTS ${IFCOPENSHELL_SOURCE}/cmake/CMakeLists.txt)
        message(FATAL_ERROR "IfcOpenShell isn't installed or checked out: run git submodule update --init")
    endif()
    message(STATUS "IfcOpenShell: building the submodule")
    set(SCHEMA_VERSIONS 2x3 4 4x3_add2)
    foreach(flag BUILD_IFCPYTHON BUILD_CONVERT BUILD_GEOMSERVER BUILD_EXAMPLES WITH_CGAL COLLADA_SUPPORT)
        set(${flag} OFF)
    endforeach()
    set(BUILD_IFCGEOM ${IFCFILTER_WITH_GEOMETRY})
    set(WITH_OPENCASCADE ${IFCFILTER_WITH_GEOMETRY})
    add_subdirectory(${IFCOPENSHELL_SOURCE}/cmake ifcopenshell EXCLUDE_FROM_ALL)
    if(IFCFILTER_WITH_GEOMETRY)
        include(cmake/ifcopenshell-opening-retry.cmake)
    endif()
    add_library(IfcOpenShell::IfcParse ALIAS IfcParse)
    add_library(IfcOpenShell::parse_schema_ifc2x3 ALIAS parse_schema_ifc2x3)
    add_library(IfcOpenShell::parse_schema_ifc4 ALIAS parse_schema_ifc4)
    add_library(IfcOpenShell::parse_schema_ifc4x3_add2 ALIAS parse_schema_ifc4x3_add2)
endif()

add_executable(ifcfilter src/main.cpp src/report.cpp src/split.cpp src/merge.cpp src/progress.cpp src/combine.cpp src/bake.cpp src/extent.cpp src/mapping.cpp src/instance.cpp src/rigid.cpp src/register.cpp src/system.cpp src/upgrade.cpp src/spf.cpp src/breps.cpp src/compact.cpp src/simplify.cpp src/tidy.cpp src/zip.cpp src/cache.cpp src/edges.cpp src/trace.cpp src/property_schema.cpp src/site_lookup.cpp src/location_reference.cpp src/site_inventory.cpp src/xlsx.cpp src/uniclass.cpp src/signs.cpp)
# The schemas are plugins, loaded when a file is opened, so nothing links to them
add_dependencies(ifcfilter IfcOpenShell::parse_schema_ifc2x3 IfcOpenShell::parse_schema_ifc4 IfcOpenShell::parse_schema_ifc4x3_add2)
target_compile_definitions(ifcfilter PRIVATE BOOST_OPTIONAL_USE_OLD_DEFINITION_OF_NONE IFCFILTER_VERSION="${PROJECT_VERSION}")
if(MSVC)
    target_compile_options(ifcfilter PRIVATE /W4 /bigobj /utf-8)
else()
    target_compile_options(ifcfilter PRIVATE -Wall -Wextra)
endif()
target_link_libraries(ifcfilter PRIVATE IfcOpenShell::IfcParse Boost::headers ZLIB::ZLIB OpenMeshCore OpenMeshTools ${SQLITE_LIBRARY} ${ZSTD_LIBRARY})
if(IFCFILTER_WITH_GEOMETRY)
    target_compile_definitions(ifcfilter PRIVATE IFCFILTER_WITH_GEOMETRY IFCFILTER_MAX_OPENINGS=${IFCFILTER_MAX_OPENINGS} IFCFILTER_MAX_CUT_WORK=${IFCFILTER_MAX_CUT_WORK} IFCFILTER_MAX_OPENING_FACES=${IFCFILTER_MAX_OPENING_FACES})
    target_link_libraries(ifcfilter PRIVATE IfcGeom Eigen3::Eigen)
    add_dependencies(ifcfilter geometry_kernel_opencascade geometry_mapping_ifc2x3 geometry_mapping_ifc4 geometry_mapping_ifc4x3_add2)
    if(WIN32)
        if(MSVC)
            foreach(geom_target geometry_kernel_opencascade geometry_mapping_ifc2x3 geometry_mapping_ifc4 geometry_mapping_ifc4x3_add2)
                target_compile_options(${geom_target} PRIVATE /MP2)
            endforeach()
        endif()
        add_custom_command(TARGET ifcfilter POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:geometry_kernel_opencascade>
            $<TARGET_FILE:geometry_mapping_ifc2x3>
            $<TARGET_FILE:geometry_mapping_ifc4>
            $<TARGET_FILE:geometry_mapping_ifc4x3_add2>
            $<TARGET_RUNTIME_DLLS:geometry_kernel_opencascade>
            $<TARGET_FILE_DIR:ifcfilter> COMMAND_EXPAND_LISTS)
        # A kernel-only change need not relink the executable, so POST_BUILD
        # alone would leave the runtime copy stale on the next build.
        if(CMAKE_CONFIGURATION_TYPES)
            set(IFCFILTER_RUNTIME_DIR "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
        else()
            set(IFCFILTER_RUNTIME_DIR "${CMAKE_CURRENT_BINARY_DIR}")
        endif()
        add_custom_target(ifcfilter_geometry_runtime
            COMMAND ${CMAKE_COMMAND} -E make_directory "${IFCFILTER_RUNTIME_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:geometry_kernel_opencascade>
                $<TARGET_FILE:geometry_mapping_ifc2x3>
                $<TARGET_FILE:geometry_mapping_ifc4>
                $<TARGET_FILE:geometry_mapping_ifc4x3_add2>
                $<TARGET_RUNTIME_DLLS:geometry_kernel_opencascade>
                "${IFCFILTER_RUNTIME_DIR}"
            DEPENDS geometry_kernel_opencascade geometry_mapping_ifc2x3 geometry_mapping_ifc4 geometry_mapping_ifc4x3_add2
            COMMAND_EXPAND_LISTS)
        add_dependencies(ifcfilter ifcfilter_geometry_runtime)
    endif()
endif()
if(WIN32)
    target_compile_definitions(ifcfilter PRIVATE NOMINMAX _USE_MATH_DEFINES)
    # IfcOpenShell only builds as shared libraries, so its DLLs go beside the program. Schema plugins
    # are looked for beside the parser's DLL, and aren't linked, so they have to be named.
    add_custom_command(TARGET ifcfilter POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:ifcfilter>
            $<TARGET_FILE:IfcOpenShell::parse_schema_ifc2x3>
            $<TARGET_FILE:IfcOpenShell::parse_schema_ifc4>
            $<TARGET_FILE:IfcOpenShell::parse_schema_ifc4x3_add2>
            $<TARGET_FILE_DIR:ifcfilter>
        COMMAND_EXPAND_LISTS)
endif()

include(GNUInstallDirs)
install(TARGETS ifcfilter RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Keep reference tables beside the executable.
add_custom_command(TARGET ifcfilter POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/data/uniclass" "$<TARGET_FILE_DIR:ifcfilter>/uniclass")
install(DIRECTORY data/uniclass DESTINATION ${CMAKE_INSTALL_BINDIR})

# The edge rule's own checks: ctest --test-dir build
enable_testing()
add_executable(edges_test tests/edges_test.cpp src/edges.cpp)
target_include_directories(edges_test PRIVATE src)
add_test(NAME edges COMMAND edges_test)
