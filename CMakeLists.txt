cmake_minimum_required(VERSION 3.20)

project(AgentOS
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

option(AGENTOS_ENABLE_POSTGRES "Enable the libpqxx PostgreSQL event store" OFF)
option(AGENTOS_ENABLE_GRPC "Build the gRPC runtime service" OFF)

if(AGENTOS_ENABLE_POSTGRES)
    find_package(libpqxx CONFIG QUIET)
    if(TARGET libpqxx::pqxx)
        set(AGENTOS_POSTGRES_TARGET libpqxx::pqxx)
    else()
        find_path(PQXX_INCLUDE_DIR NAMES pqxx/pqxx)
        find_library(PQXX_LIBRARY NAMES pqxx libpqxx)
        find_library(PQ_LIBRARY NAMES pq libpq)

        if(NOT PQXX_INCLUDE_DIR OR NOT PQXX_LIBRARY OR NOT PQ_LIBRARY)
            message(FATAL_ERROR
                "PostgreSQL support requires libpqxx and libpq. On Windows, install "
                "them with vcpkg: vcpkg install libpqxx:x64-windows, then configure "
                "with -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake")
        endif()

        add_library(agentos_pqxx UNKNOWN IMPORTED)
        set_target_properties(agentos_pqxx PROPERTIES
            IMPORTED_LOCATION "${PQXX_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${PQXX_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES "${PQ_LIBRARY}"
        )
        set(AGENTOS_POSTGRES_TARGET agentos_pqxx)
    endif()
endif()

if(AGENTOS_ENABLE_GRPC)
    if(NOT AGENTOS_ENABLE_POSTGRES)
        message(FATAL_ERROR "AGENTOS_ENABLE_GRPC requires AGENTOS_ENABLE_POSTGRES")
    endif()
    find_package(Protobuf CONFIG REQUIRED)
    find_package(gRPC CONFIG REQUIRED)

    set(AGENTOS_PROTO ${CMAKE_CURRENT_SOURCE_DIR}/runtime/proto/agentos.proto)
    set(AGENTOS_PROTO_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
    file(MAKE_DIRECTORY ${AGENTOS_PROTO_DIR})
    add_custom_command(
        OUTPUT ${AGENTOS_PROTO_DIR}/agentos.pb.cc
               ${AGENTOS_PROTO_DIR}/agentos.pb.h
               ${AGENTOS_PROTO_DIR}/agentos.grpc.pb.cc
               ${AGENTOS_PROTO_DIR}/agentos.grpc.pb.h
        COMMAND protobuf::protoc
        ARGS --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/runtime/proto
             --cpp_out=${AGENTOS_PROTO_DIR}
             --grpc_out=${AGENTOS_PROTO_DIR}
             --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
             ${AGENTOS_PROTO}
        DEPENDS ${AGENTOS_PROTO}
        VERBATIM
    )
    add_executable(agentos_server
        runtime/grpc_runtime_server.cpp
        ${AGENTOS_PROTO_DIR}/agentos.pb.cc
        ${AGENTOS_PROTO_DIR}/agentos.grpc.pb.cc
    )
    target_include_directories(agentos_server PRIVATE runtime ${AGENTOS_PROTO_DIR})
    target_link_libraries(agentos_server PRIVATE gRPC::grpc++)
    target_compile_options(agentos_server PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
    )
    if(MSVC)
        # vcpkg builds grpc as a static library on x64-windows while protobuf is a DLL.
        # MSVC then reports LNK2005 for std::string_view members defined in both.
        target_link_options(agentos_server PRIVATE /FORCE:MULTIPLE)
    endif()
    if(AGENTOS_ENABLE_POSTGRES)
        target_compile_definitions(agentos_server PRIVATE AGENTOS_ENABLE_POSTGRES)
        target_link_libraries(agentos_server PRIVATE ${AGENTOS_POSTGRES_TARGET})
    endif()

    add_executable(agentos_grpc_tests
        tests/grpc_postgres_tests.cpp
        ${AGENTOS_PROTO_DIR}/agentos.pb.cc
        ${AGENTOS_PROTO_DIR}/agentos.grpc.pb.cc
    )
    target_include_directories(agentos_grpc_tests PRIVATE runtime ${AGENTOS_PROTO_DIR})
    target_link_libraries(agentos_grpc_tests PRIVATE gRPC::grpc++)
    target_compile_definitions(agentos_grpc_tests PRIVATE AGENTOS_ENABLE_POSTGRES)
    target_link_libraries(agentos_grpc_tests PRIVATE ${AGENTOS_POSTGRES_TARGET})
    target_compile_options(agentos_grpc_tests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
    )
    if(MSVC)
        target_link_options(agentos_grpc_tests PRIVATE /FORCE:MULTIPLE)
    endif()
    add_test(NAME grpc_submit COMMAND agentos_grpc_tests grpc_submit)
    add_test(NAME grpc_cancel COMMAND agentos_grpc_tests grpc_cancel)
    add_test(NAME grpc_recovery COMMAND agentos_grpc_tests grpc_recovery)
    add_test(NAME grpc_lease_steal COMMAND agentos_grpc_tests grpc_lease_steal)
    set_tests_properties(grpc_submit grpc_cancel grpc_recovery grpc_lease_steal
        PROPERTIES RESOURCE_LOCK AgentOSPostgres)
    set_tests_properties(grpc_lease_steal PROPERTIES TIMEOUT 30)
endif()

add_executable(agentos
    runtime/main.cpp
)

target_compile_options(agentos PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

add_executable(agentos_tests
    tests/scheduler_tests.cpp
)

add_executable(agentos_benchmark
    benchmarks/scheduler_benchmark.cpp
)

target_include_directories(agentos_tests PRIVATE runtime)
target_include_directories(agentos_benchmark PRIVATE runtime)

target_compile_options(agentos_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

target_compile_options(agentos_benchmark PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

if(AGENTOS_ENABLE_POSTGRES)
    target_compile_definitions(agentos PRIVATE AGENTOS_ENABLE_POSTGRES)
    target_compile_definitions(agentos_tests PRIVATE AGENTOS_ENABLE_POSTGRES)
    target_compile_definitions(agentos_benchmark PRIVATE AGENTOS_ENABLE_POSTGRES)
    target_link_libraries(agentos PRIVATE ${AGENTOS_POSTGRES_TARGET})
    target_link_libraries(agentos_tests PRIVATE ${AGENTOS_POSTGRES_TARGET})
    target_link_libraries(agentos_benchmark PRIVATE ${AGENTOS_POSTGRES_TARGET})
endif()

add_test(NAME dependency_order COMMAND agentos_tests dependency_order)
add_test(NAME concurrent_tasks COMMAND agentos_tests concurrent_tasks)
add_test(NAME dynamic_submission COMMAND agentos_tests dynamic_submission)
add_test(NAME mixed_submission COMMAND agentos_tests mixed_submission)
add_test(NAME completed_dependency COMMAND agentos_tests completed_dependency)
add_test(NAME validation COMMAND agentos_tests validation)
add_test(NAME atomic_rejection COMMAND agentos_tests atomic_rejection)
add_test(NAME incremental_validation COMMAND agentos_tests incremental_validation)
add_test(NAME failure_propagation COMMAND agentos_tests failure_propagation)
add_test(NAME retry_success COMMAND agentos_tests retry_success)
add_test(NAME retry_exhausted COMMAND agentos_tests retry_exhausted)
add_test(NAME events COMMAND agentos_tests events)
add_test(NAME deadline COMMAND agentos_tests deadline)
add_test(NAME cancellation COMMAND agentos_tests cancellation)
add_test(NAME lease_expiry COMMAND agentos_tests lease_expiry)
set_tests_properties(lease_expiry PROPERTIES TIMEOUT 30)
add_test(NAME lease_renewal COMMAND agentos_tests lease_renewal)
set_tests_properties(lease_renewal PROPERTIES TIMEOUT 30)
add_test(NAME cancel_state COMMAND agentos_tests cancel_state)
add_test(NAME idempotency COMMAND agentos_tests idempotency)
add_test(NAME task_context COMMAND agentos_tests task_context)
add_test(NAME durable_events COMMAND agentos_tests durable_events)
if(AGENTOS_ENABLE_POSTGRES)
    add_test(NAME postgres_recovery COMMAND agentos_tests postgres_recovery)
    set_tests_properties(postgres_recovery PROPERTIES RESOURCE_LOCK AgentOSPostgres)
endif()

# Single CTest entry point for this build tree. Configure with
# AGENTOS_ENABLE_POSTGRES=ON and AGENTOS_ENABLE_GRPC=ON (AgentOS/build-grpc)
# to register in-process, Postgres, and gRPC tests together.
set(AGENTOS_TEST_ALL_DEPS agentos_tests)
if(AGENTOS_ENABLE_GRPC)
    list(APPEND AGENTOS_TEST_ALL_DEPS agentos_grpc_tests)
endif()
add_custom_target(test-all
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} -C $<CONFIG> --output-on-failure
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    DEPENDS ${AGENTOS_TEST_ALL_DEPS}
    COMMENT "Run every CTest registered in this build directory"
    USES_TERMINAL
    VERBATIM
)
