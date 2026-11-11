# SPDX-License-Identifier: GPL-2.0-or-later
find_package(Threads REQUIRED)
pkg_check_modules(AI_WORKFLOW_GLIB REQUIRED IMPORTED_TARGET glib-2.0)

add_library(inkscape_ai_workflow STATIC
    layout-search.cpp layout-search.h
    design-agents.cpp design-agents.h
    workflow-types.cpp workflow-types.h
    agent.cpp agent.h
    context-manager.cpp context-manager.h
    message-bus.cpp message-bus.h
    output-registry.cpp output-registry.h
    orchestrator.cpp orchestrator.h
    svg-validation.cpp svg-validation.h
)
set_target_properties(inkscape_ai_workflow PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(inkscape_ai_workflow PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(inkscape_ai_workflow PUBLIC nlohmann_json::nlohmann_json
    PRIVATE PkgConfig::AI_WORKFLOW_GLIB LibXml2::LibXml2 Threads::Threads)
target_link_libraries(inkscape_base PRIVATE inkscape_ai_workflow)

# Headless regression/demo target remains available when GoogleTest is absent.
add_executable(test_ai_workflow EXCLUDE_FROM_ALL ../../testfiles/src/ai-workflow-test.cpp)
target_link_libraries(test_ai_workflow PRIVATE inkscape_ai_workflow Threads::Threads PkgConfig::AI_WORKFLOW_GLIB)

add_executable(test_ai_design_agents EXCLUDE_FROM_ALL ../../testfiles/src/ai-design-agents-test.cpp)
target_link_libraries(test_ai_design_agents PRIVATE inkscape_ai_workflow Threads::Threads)

add_executable(test_ai_layout_search EXCLUDE_FROM_ALL ../../testfiles/src/ai-layout-search-test.cpp)
target_link_libraries(test_ai_layout_search PRIVATE inkscape_ai_workflow)

add_executable(test_ai_connection EXCLUDE_FROM_ALL ../../testfiles/src/ai-connection-test.cpp
    ../ui/dialog/ai-connection.cpp ../ui/dialog/ai-codex.cpp)
target_include_directories(test_ai_connection PRIVATE ..)
target_link_libraries(test_ai_connection PRIVATE nlohmann_json::nlohmann_json PkgConfig::AI_SOUP Threads::Threads)

add_executable(test_ai_assistant_ui EXCLUDE_FROM_ALL ../../testfiles/src/ai-assistant-ui-test.cpp)
target_link_libraries(test_ai_assistant_ui PRIVATE inkscape_base 2Geom::2geom)

add_executable(test_ai_design_services EXCLUDE_FROM_ALL ../../testfiles/src/ai-design-services-test.cpp)
target_link_libraries(test_ai_design_services PRIVATE inkscape_base PkgConfig::AI_WORKFLOW_GLIB)
add_executable(test_ai_design_assets EXCLUDE_FROM_ALL ../../testfiles/src/ai-design-assets-test.cpp)
target_link_libraries(test_ai_design_assets PRIVATE inkscape_base 2Geom::2geom PkgConfig::AI_WORKFLOW_GLIB)
