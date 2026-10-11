cmake_minimum_required(VERSION 3.22)
project(routing_e2e_compiler LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Qt6 REQUIRED COMPONENTS Core Network)
set(ROUTING_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../client/core/models/routing")
add_executable(routing_config_export
    main.cpp
    ${ROUTING_DIR}/routingProfile.cpp
    ${ROUTING_DIR}/routingCompiler.cpp
    ${ROUTING_DIR}/geoData.cpp
    ${ROUTING_DIR}/ipRanges.cpp
)
target_include_directories(routing_config_export PRIVATE ${ROUTING_DIR})
target_link_libraries(routing_config_export PRIVATE Qt6::Core Qt6::Network)
