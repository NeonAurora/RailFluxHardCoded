cmake_minimum_required(VERSION 3.16)

project(RailFlux VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)

# Compiler warnings for safety-critical code
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(Qt6 REQUIRED COMPONENTS Quick Sql)

qt_standard_project_setup(REQUIRES 6.8)

qt_add_executable(appRailFlux
    main.cpp
)

qt_add_qml_module(appRailFlux
    URI RailFlux
    VERSION 1.0

    # All QML files consolidated in one section
    QML_FILES
        Main.qml
        layouts/StationLayout.qml
        components/TrackSegment.qml
        components/LevelCrossingGate.qml
        components/GridCanvas.qml
        components/OuterSignal.qml
        components/HomeSignal.qml
        components/StarterSignal.qml
        components/AdvanceStarterSignal.qml
        components/PointMachine.qml
        components/ToastNotification.qml
        components/SignalContextMenu.qml






        components/SignalIndicator.qml

    # All C++ source files properly organized
    SOURCES
        database/DatabaseManager.h
        database/DatabaseManager.cpp
        database/DatabaseInitializer.h
        database/DatabaseInitializer.cpp
        interlocking/InterlockingService.h
        interlocking/InterlockingService.cpp
        interlocking/SignalBranch.h
        interlocking/SignalBranch.cpp
        interlocking/TrackCircuitBranch.h
        interlocking/TrackCircuitBranch.cpp
        interlocking/PointMachineBranch.h
        interlocking/PointMachineBranch.cpp
        interlocking/SignalRule.h
        interlocking/SignalRule.cpp
        interlocking/InterlockingRuleEngine.h
        interlocking/InterlockingRuleEngine.cpp












        route/RouteAssignmentService.h
        route/RouteAssignmentService.cpp



    # Only non-code resources (SQL files, data files)
    RESOURCES
    sql/sql_commands_railflux.sql
        QML_FILES components/RouteAssignmentDialog.qml

)

# Separate resource bundle for application assets
qt_add_resources(appRailFlux "app_resources"
    PREFIX "/"
    FILES
        resources/icons/railway-icon.ico
        resources/data/signal_interlocking_rules.json
)

set_target_properties(appRailFlux PROPERTIES
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

target_link_libraries(appRailFlux
    PRIVATE Qt6::Quick Qt6::Sql
)
