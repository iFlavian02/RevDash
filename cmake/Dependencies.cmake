# RevDash External Dependencies Configuration

find_package(Boost REQUIRED COMPONENTS system)
find_package(tl-expected CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(Catch2 3 CONFIG REQUIRED)

# Qt is intentionally installed separately and located through Qt6_ROOT.
# Qt Graphs is excluded because its licensing does not fit the default project policy.
find_package(Qt6 6.11.2 COMPONENTS Core Gui Qml Quick QUIET)

if(Qt6_FOUND)
    message(STATUS "Qt6 found: ${Qt6_VERSION} at ${Qt6_DIR}")
else()
    message(STATUS "Qt6 not found. revdash_app UI target will be configured when Qt6 is available.")
endif()
