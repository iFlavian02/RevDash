cmake_minimum_required(VERSION 3.28)

# Automatically locate and use the vcpkg toolchain if not explicitly passed by the IDE
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "vcpkg toolchain")
    elseif(EXISTS "C:/Program Files/Microsoft Visual Studio/18/Community/VC/vcpkg/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "C:/Program Files/Microsoft Visual Studio/18/Community/VC/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "vcpkg toolchain")
    endif()
endif()

project(RevDash VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CompilerOptions)
include(Dependencies)

# -----------------------------------------------------------------------------
# Core Engine Library (Qt-Independent)
# -----------------------------------------------------------------------------
add_library(revdash_core STATIC
    src/core/types.cpp
)

target_include_directories(revdash_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/headers
)

target_link_libraries(revdash_core
    PUBLIC
        Boost::system
        tl::expected
        nlohmann_json::nlohmann_json
        unofficial::sqlite3::sqlite3
        spdlog::spdlog
)

revdash_apply_compiler_options(revdash_core)

# -----------------------------------------------------------------------------
# Headless Diagnostic CLI Target
# -----------------------------------------------------------------------------
add_executable(revdash_cli
    src/cli/main.cpp
)

target_link_libraries(revdash_cli
    PRIVATE
        revdash_core
        CLI11::CLI11
        spdlog::spdlog
)

revdash_apply_compiler_options(revdash_cli)

# -----------------------------------------------------------------------------
# Offline DTC Importer Tool Target
# -----------------------------------------------------------------------------
add_executable(revdash_dtc_importer
    tools/dtc_importer/main.cpp
)

target_link_libraries(revdash_dtc_importer
    PRIVATE
        revdash_core
        unofficial::sqlite3::sqlite3
        CLI11::CLI11
        spdlog::spdlog
)

revdash_apply_compiler_options(revdash_dtc_importer)

# -----------------------------------------------------------------------------
# Desktop Qt GUI Application Target
# -----------------------------------------------------------------------------
add_executable(revdash_app
    src/app/main.cpp
)

if(Qt6_FOUND)
    target_link_libraries(revdash_app
        PRIVATE
            revdash_core
            Qt6::Core
            Qt6::Quick
            spdlog::spdlog
    )
    if(TARGET Qt6::Graphs)
        target_link_libraries(revdash_app PRIVATE Qt6::Graphs)
    endif()
    target_compile_definitions(revdash_app PRIVATE REVDASH_HAS_QT=1)
else()
    target_link_libraries(revdash_app
        PRIVATE
            revdash_core
            spdlog::spdlog
    )
endif()

revdash_apply_compiler_options(revdash_app)

# -----------------------------------------------------------------------------
# Unit & Integration Tests Target
# -----------------------------------------------------------------------------
enable_testing()

add_executable(revdash_unit_tests
    tests/unit/test_main.cpp
    tests/unit/test_data_source.cpp
)

target_link_libraries(revdash_unit_tests
    PRIVATE
        revdash_core
        Catch2::Catch2WithMain
)

revdash_apply_compiler_options(revdash_unit_tests)

add_test(NAME core_types COMMAND revdash_unit_tests "[core_types]")
add_test(NAME data_source_contract COMMAND revdash_unit_tests "[data_source_contract]")
