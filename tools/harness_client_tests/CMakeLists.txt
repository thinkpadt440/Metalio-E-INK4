cmake_minimum_required(VERSION 3.16)
project(harness_client_tests C CXX)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(CJSON_ROOT "${CJSON_ROOT}" CACHE PATH "ESP-IDF components/json/cJSON")
if(NOT EXISTS "${CJSON_ROOT}/cJSON.c")
 set(CJSON_ROOT "$ENV{IDF_PATH}/components/json/cJSON")
endif()
if(NOT EXISTS "${CJSON_ROOT}/cJSON.c")
 message(FATAL_ERROR "Supply CJSON_ROOT or source ESP-IDF export.sh")
endif()
enable_testing()
add_library(harness_host STATIC ${ROOT}/main/harness/client.cc ${ROOT}/main/harness/socket_io.cc
 ${ROOT}/main/harness/approval_tracker.cc ${ROOT}/main/harness/run_tracker.cc ${ROOT}/main/harness/upload.cc ${ROOT}/main/harness/history.cc ${ROOT}/main/harness/catalog.cc ${ROOT}/main/harness/json.cc ${ROOT}/main/harness/transport.cc ${ROOT}/components/esp-ml307/src/web_socket.cc ${CJSON_ROOT}/cJSON.c)
target_include_directories(harness_host PUBLIC ${ROOT}/main ${ROOT}/tools/ws_transport_tests/stubs
 ${ROOT}/components/esp-ml307/include ${CJSON_ROOT})
target_compile_options(harness_host PRIVATE -Wall -Wextra)
option(ENABLE_SANITIZERS "Address and undefined behavior sanitizers" ON)
if(ENABLE_SANITIZERS)
 target_compile_options(harness_host PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
 target_link_options(harness_host PUBLIC -fsanitize=address,undefined)
endif()
add_executable(client_tests client_test.cc)
target_link_libraries(client_tests PRIVATE harness_host)
target_compile_options(client_tests PRIVATE -UNDEBUG -Wall -Wextra)
add_test(NAME harness_client COMMAND client_tests)

add_executable(socket_io_tests socket_io_test.cc)
target_link_libraries(socket_io_tests PRIVATE harness_host)
target_compile_options(socket_io_tests PRIVATE -UNDEBUG -Wall -Wextra)
add_test(NAME harness_socket_io COMMAND socket_io_tests)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_executable(local_integration local_integration.cc)
target_link_libraries(local_integration PRIVATE harness_host pthread)
target_compile_options(local_integration PRIVATE -UNDEBUG -Wall -Wextra)
add_test(NAME harness_localhost COMMAND ${Python3_EXECUTABLE}
 ${CMAKE_CURRENT_SOURCE_DIR}/mock_integration.py $<TARGET_FILE:local_integration>)
set_tests_properties(harness_localhost PROPERTIES TIMEOUT 25)

add_test(NAME harness_catalog_localhost COMMAND ${Python3_EXECUTABLE}
 ${CMAKE_CURRENT_SOURCE_DIR}/mock_integration.py $<TARGET_FILE:local_integration> --catalog)
set_tests_properties(harness_catalog_localhost PROPERTIES TIMEOUT 25)

foreach(scenario ok max silent begin_ack body end_ack wrong_ack busy busy_retry conflict bad_digest short odd)
    add_test(NAME harness_audio_${scenario} COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/mock_integration.py $<TARGET_FILE:local_integration> --audio ${scenario})
    set_tests_properties(harness_audio_${scenario} PROPERTIES TIMEOUT 25)
endforeach()

foreach(scenario cancel unsupported race lost_ack unknown lost_terminal recover)
    add_test(NAME harness_run_${scenario} COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/mock_integration.py $<TARGET_FILE:local_integration> --run ${scenario})
    set_tests_properties(harness_run_${scenario} PROPERTIES TIMEOUT 25)
endforeach()

foreach(scenario approve reject long web_only expire web_wins version lost_ack cancel reconnect multi)
    add_test(NAME harness_approval_${scenario} COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/mock_integration.py $<TARGET_FILE:local_integration> --approval ${scenario})
    set_tests_properties(harness_approval_${scenario} PROPERTIES TIMEOUT 25)
endforeach()

add_test(NAME harness_standby_localhost COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/mock_integration.py $<TARGET_FILE:local_integration> --standby)
set_tests_properties(harness_standby_localhost PROPERTIES TIMEOUT 25)
