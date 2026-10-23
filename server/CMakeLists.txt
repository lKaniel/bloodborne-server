cmake_minimum_required(VERSION 3.16)
project(shadnet VERSION 1.0.0 LANGUAGES CXX)

include(CTest)

# C++20: the co-op code shared with the game (coop/) needs it.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# === Runtime library: must match Qt6 which always uses DLL runtime ===
if(MSVC)
    cmake_policy(SET CMP0091 NEW)
    # /MD (release) or /MDd (debug) — DLL runtime, same as Qt6.
    # protobuf_MSVC_STATIC_RUNTIME=OFF ensures protobuf follows the same setting.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    set(protobuf_MSVC_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
    add_compile_options(/EHsc /utf-8)
    # Remove /VERBOSE:LIB from link options (noisy, only needed for debugging)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Network Sql Concurrent HttpServer)

add_subdirectory(externals)

set(PROTO_OUT "${CMAKE_CURRENT_BINARY_DIR}/proto_gen")
file(MAKE_DIRECTORY "${PROTO_OUT}")

add_custom_command(
    OUTPUT
        "${PROTO_OUT}/shadnet.pb.cc"
        "${PROTO_OUT}/shadnet.pb.h"
    COMMAND $<TARGET_FILE:protoc>
        "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}/src"
        "--cpp_out=${PROTO_OUT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/shadnet.proto"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/shadnet.proto"
        protoc
    COMMENT "Generating shadnet protobuf sources"
    VERBATIM
)

# Our co-op messages (coop/coop.proto), shared with the game; with the world log (coop/), which the
# game compiles too.
add_custom_command(
    OUTPUT
        "${PROTO_OUT}/coop.pb.cc"
        "${PROTO_OUT}/coop.pb.h"
    COMMAND $<TARGET_FILE:protoc>
        "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}/coop"
        "--cpp_out=${PROTO_OUT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/coop/coop.proto"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/coop/coop.proto"
        protoc
    COMMENT "Generating the co-op protobuf sources"
    VERBATIM
)
set(COOP_SOURCES
    coop/coop_types.h
    coop/world_log.h
    coop/world_log.cpp
    coop/game_link.h
    coop/packet_routes.h
    coop/coop_stream.h
    coop/coop_messages.h
    coop/coop_messages.cpp
    coop/team_lobby.h
    coop/team_lobby.cpp
    coop/trade.h
    coop/trade.cpp
    coop/invasions.h
    coop/invasions.cpp
    "${PROTO_OUT}/coop.pb.cc"
)

set(SOURCES
    src/main.cpp
    src/watchdog.cpp
    src/bloodborne_reports.cpp
    src/webapi_routes_reports.cpp
    src/config.cpp
    src/config.h
    src/server.cpp
    src/server.h
    src/client_session.cpp
    src/client_session.h
    src/protocol.h
    src/stream_extractor.h
    src/database.cpp
    src/database.h
    src/cmd_account.cpp
    src/cmd_friend.cpp
    src/score_types.h
    src/score_db.h
    src/score_db.cpp
    src/score_cache.cpp
    src/score_cache.h
    src/score_files.h
    src/cmd_score.cpp
    src/proto_utils.h
    "${PROTO_OUT}/shadnet.pb.cc"
    # Matchmaking
    src/matching.h
    src/cmd_matching.cpp
    src/link_server.h
    src/link_server.cpp
    src/admission.h
    src/admission.cpp
    src/http_gate.h
    src/http_gate.cpp
    # The games' one link to the server (co-op phase 2)
    src/server_link.h
    src/server_link.cpp
    # The parties' worlds (co-op phase 3)
    src/bloodborne_world.h
    src/bloodborne_world.cpp
    src/bloodborne_party_boards.h
    src/bloodborne_party_boards.cpp
    ${COOP_SOURCES}
    # WebAPI HTTP/JSON listener
    src/webapi_server.h
    src/webapi_server.cpp
    src/webapi_auth.h
    src/webapi_auth.cpp
    src/webapi_routes_users.h
    src/webapi_routes_users.cpp
    src/webapi_routes_profile.h
    src/webapi_routes_profile.cpp
    src/webapi_routes_presence.h
    src/webapi_routes_presence.cpp
    src/webapi_routes_session.h
    src/webapi_routes_session.cpp
    src/bloodborne_summon_broker.h
    src/bloodborne_summon_broker.cpp
    src/webapi_routes_bloodborne.h
    src/webapi_routes_bloodborne.cpp
    src/webapi_routes_common.h
    # The game's own online services (backend/bb_dummy_server.py's job)
    src/bloodborne_backend_json.h
    src/bloodborne_backend_json.cpp
    src/bloodborne_backend.h
    src/bloodborne_backend.cpp
    src/bloodborne_backend_server.h
    src/bloodborne_backend_server.cpp
    # Read-only stats HTTP/JSON listener
    src/stats_server.h
    src/stats_server.cpp
)

# The HTTP services (WebAPI, the game's backend, the stats page) need Qt 6.8's QHttpServer; with an
# older Qt (Ubuntu 24.04's 6.4) shadNet builds without them: the link, the worlds and the lobby
# only, enough for a load test (docs/HOSTED_SERVER.md).
set(SHADNET_HTTP_SOURCES
    src/webapi_server.cpp
    src/webapi_routes_bloodborne.cpp
    src/webapi_routes_session.cpp
    src/bloodborne_backend_server.cpp
    src/stats_server.cpp
    src/webapi_server.h
    src/bloodborne_backend_server.h
    src/stats_server.h
)
if(Qt6_VERSION VERSION_LESS 6.8)
    message(WARNING "Qt ${Qt6_VERSION} < 6.8: shadnet builds without its HTTP services")
    list(REMOVE_ITEM SOURCES ${SHADNET_HTTP_SOURCES})
endif()

add_executable(shadnet ${SOURCES})
if(Qt6_VERSION VERSION_LESS 6.8)
    target_compile_definitions(shadnet PRIVATE SHADNET_NO_HTTP)
endif()

# The ss.info template the game's online check reads; the Python backend reads the same file.
set(BLOODBORNE_SS_INFO_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/../backend/ss_info_template.xml"
    CACHE FILEPATH "backend/ss_info_template.xml, built into shadnet")
if(NOT EXISTS "${BLOODBORNE_SS_INFO_TEMPLATE}")
    message(FATAL_ERROR "No ss.info template at ${BLOODBORNE_SS_INFO_TEMPLATE}")
endif()
get_filename_component(BLOODBORNE_SS_INFO_DIR "${BLOODBORNE_SS_INFO_TEMPLATE}" DIRECTORY)
qt_add_resources(shadnet bloodborne_backend
    PREFIX "/backend"
    BASE "${BLOODBORNE_SS_INFO_DIR}"
    FILES "${BLOODBORNE_SS_INFO_TEMPLATE}"
)

target_include_directories(shadnet PRIVATE
    src
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROTO_OUT}"                                    
    "${CMAKE_CURRENT_SOURCE_DIR}/externals/protobuf/src"
)

target_link_libraries(shadnet PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Sql
    Qt6::Concurrent
    Qt6::HttpServer
    libprotobuf
)

if(BUILD_TESTING)
    add_executable(test_bloodborne_summon_broker EXCLUDE_FROM_ALL
        tests/test_bloodborne_summon_broker.cpp
        src/bloodborne_summon_broker.cpp
    )
    target_include_directories(test_bloodborne_summon_broker PRIVATE src)
    target_link_libraries(test_bloodborne_summon_broker PRIVATE Qt6::Core)
    add_test(NAME bloodborne_summon_broker COMMAND test_bloodborne_summon_broker)

    add_executable(test_bloodborne_summon_party EXCLUDE_FROM_ALL
        tests/test_bloodborne_summon_party.cpp
        src/bloodborne_summon_broker.cpp
    )
    target_include_directories(test_bloodborne_summon_party PRIVATE src)
    target_link_libraries(test_bloodborne_summon_party PRIVATE Qt6::Core)
    add_test(NAME bloodborne_summon_party COMMAND test_bloodborne_summon_party)

    add_executable(test_bloodborne_backend_json EXCLUDE_FROM_ALL
        tests/test_bloodborne_backend_json.cpp
        src/bloodborne_backend_json.cpp
    )
    target_include_directories(test_bloodborne_backend_json PRIVATE src)
    target_link_libraries(test_bloodborne_backend_json PRIVATE Qt6::Core)
    add_test(NAME bloodborne_backend_json COMMAND test_bloodborne_backend_json)

    add_executable(test_request_address EXCLUDE_FROM_ALL
        tests/test_request_address.cpp
    )
    target_include_directories(test_request_address PRIVATE src)
    add_test(NAME request_address COMMAND test_request_address)

    add_executable(test_ss_info_path EXCLUDE_FROM_ALL
        tests/test_ss_info_path.cpp
    )
    target_include_directories(test_ss_info_path PRIVATE src)
    add_test(NAME ss_info_path COMMAND test_ss_info_path)

    add_executable(test_server_link EXCLUDE_FROM_ALL
        tests/test_server_link.cpp
        src/server_link.cpp
    )
    target_include_directories(test_server_link PRIVATE src "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(test_server_link PRIVATE Qt6::Core)
    target_compile_definitions(test_server_link PRIVATE
        BLOODBORNE_SERVER_LINK_CONTRACTS="${CMAKE_CURRENT_SOURCE_DIR}/tests/contracts/server_link_frames.txt")
    add_test(NAME server_link COMMAND test_server_link)

    add_executable(test_packet_routes EXCLUDE_FROM_ALL tests/test_packet_routes.cpp)
    target_include_directories(test_packet_routes PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME packet_routes COMMAND test_packet_routes)

    add_executable(test_rpc_tunnel EXCLUDE_FROM_ALL tests/test_rpc_tunnel.cpp)
    target_include_directories(test_rpc_tunnel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME rpc_tunnel COMMAND test_rpc_tunnel)

    add_executable(test_watchdog EXCLUDE_FROM_ALL tests/test_watchdog.cpp src/watchdog.cpp)
    target_include_directories(test_watchdog PRIVATE src)
    target_link_libraries(test_watchdog PRIVATE Qt6::Core)
    add_test(NAME watchdog COMMAND test_watchdog)

    add_executable(test_reports EXCLUDE_FROM_ALL tests/test_reports.cpp src/bloodborne_reports.cpp)
    target_include_directories(test_reports PRIVATE src)
    target_link_libraries(test_reports PRIVATE Qt6::Core)
    add_test(NAME reports COMMAND test_reports)

    add_executable(test_bloodborne_world EXCLUDE_FROM_ALL
        tests/test_bloodborne_world.cpp
        src/bloodborne_world.cpp
        src/bloodborne_party_boards.cpp
        src/server_link.cpp
        ${COOP_SOURCES}
    )
    target_include_directories(test_bloodborne_world PRIVATE
        src
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROTO_OUT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/externals/protobuf/src"
    )
    target_link_libraries(test_bloodborne_world PRIVATE Qt6::Core libprotobuf)
    add_test(NAME bloodborne_world COMMAND test_bloodborne_world)

    add_executable(test_world_writer EXCLUDE_FROM_ALL
        tests/test_world_writer.cpp
        src/bloodborne_world.cpp
        src/bloodborne_party_boards.cpp
        src/server_link.cpp
        ${COOP_SOURCES}
    )
    target_include_directories(test_world_writer PRIVATE
        src
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROTO_OUT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/externals/protobuf/src"
    )
    target_link_libraries(test_world_writer PRIVATE Qt6::Core libprotobuf)
    add_test(NAME world_writer COMMAND test_world_writer)

    add_executable(test_admission EXCLUDE_FROM_ALL tests/test_admission.cpp src/admission.cpp)
    target_include_directories(test_admission PRIVATE src)
    target_link_libraries(test_admission PRIVATE Qt6::Core Qt6::Network)
    add_test(NAME admission COMMAND test_admission)

    add_executable(test_http_gate EXCLUDE_FROM_ALL tests/test_http_gate.cpp src/http_gate.cpp)
    target_include_directories(test_http_gate PRIVATE src)
    target_link_libraries(test_http_gate PRIVATE Qt6::Core Qt6::Network)
    add_test(NAME http_gate COMMAND test_http_gate)

    add_executable(test_team_lobby EXCLUDE_FROM_ALL
        tests/test_team_lobby.cpp
        ${COOP_SOURCES}
    )
    target_include_directories(test_team_lobby PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROTO_OUT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/externals/protobuf/src"
    )
    target_link_libraries(test_team_lobby PRIVATE libprotobuf)
    add_test(NAME team_lobby COMMAND test_team_lobby)

    add_executable(test_invasions EXCLUDE_FROM_ALL tests/test_invasions.cpp coop/invasions.cpp)
    target_include_directories(test_invasions PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME invasions COMMAND test_invasions)

    add_executable(test_room_owner EXCLUDE_FROM_ALL tests/test_room_owner.cpp)
    target_include_directories(test_room_owner PRIVATE src)
    target_link_libraries(test_room_owner PRIVATE Qt6::Core)
    add_test(NAME room_owner COMMAND test_room_owner)

    add_executable(test_bloodborne_trade EXCLUDE_FROM_ALL
        tests/test_bloodborne_trade.cpp
        ${COOP_SOURCES}
    )
    target_include_directories(test_bloodborne_trade PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROTO_OUT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/externals/protobuf/src"
    )
    target_link_libraries(test_bloodborne_trade PRIVATE libprotobuf)
    add_test(NAME bloodborne_trade COMMAND test_bloodborne_trade)

    add_executable(test_accounts EXCLUDE_FROM_ALL tests/test_accounts.cpp src/database.cpp)
    target_include_directories(test_accounts PRIVATE src)
    target_link_libraries(test_accounts PRIVATE Qt6::Core Qt6::Sql)
    add_test(NAME accounts COMMAND test_accounts)

    # The backend in shadnet against backend/bb_dummy_server.py: the same requests, the same
    # answers (needs Python 3).
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME bloodborne_backend_parity
            COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_parity
            WORKING_DIRECTORY "${BLOODBORNE_SS_INFO_DIR}/tests")
        set_tests_properties(bloodborne_backend_parity PROPERTIES
            ENVIRONMENT "SHADNET_EXE=$<TARGET_FILE:shadnet>")
        add_test(NAME http_gate_routes
            COMMAND "${Python3_EXECUTABLE}" -m unittest -v test_http_gate_routes
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        set_tests_properties(http_gate_routes PROPERTIES
            ENVIRONMENT "SHADNET_EXE=$<TARGET_FILE:shadnet>")
    endif()
endif()

# Simulated players for a load test (loadtest/bbload.cpp, docs/HOSTED_SERVER.md).
add_executable(bbload EXCLUDE_FROM_ALL
    loadtest/bbload.cpp
    src/server_link.cpp
    ${COOP_SOURCES}
)
target_include_directories(bbload PRIVATE
    src
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROTO_OUT}"
    "${CMAKE_CURRENT_SOURCE_DIR}/externals/protobuf/src"
)
target_link_libraries(bbload PRIVATE Qt6::Core Qt6::Network libprotobuf)

add_custom_command(TARGET shadnet POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/worlds.cfg"
        "$<TARGET_FILE_DIR:shadnet>/worlds.cfg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/scoreboards.cfg"
        "$<TARGET_FILE_DIR:shadnet>/scoreboards.cfg"
)

install(TARGETS shadnet RUNTIME DESTINATION bin)
install(FILES worlds.cfg scoreboards.cfg DESTINATION bin)