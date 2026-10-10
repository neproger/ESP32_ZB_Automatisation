cmake_minimum_required(VERSION 3.16)
project(device_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

set(DEVICE_CORE ../src/device_service.c)
set(HA_MODEL_CORE
    ../../ha_model/src/ha_properties.c
    ../../ha_model/src/ha_entity.c
    ../../ha_model/src/ha_logical_device.c)

set(DOMAIN_CORE
    ../../domain/src/domain_registry.c
    ../../domain/src/domain_entity.c
    ../../domain/src/domain_journal.c
    ../../domain/src/domain_dispatch.c
    ../../domain/src/domain_fact.c
    ../../domain/src/domain_payload.c
    ../../domain/src/domain_command.c)
set(DOMAIN_HOST_PORT ../../domain/port/host/domain_platform_host.c)

set(MSTORE_CORE
    ../../mstore/src/mstore_table.c
    ../../mstore/src/mstore_runtime.c
    ../../mstore/src/mstore_invariants.c
    ../../mstore/src/mstore_ring.c
    ../../mstore/src/storage/mstore_storage.c
    ../../mstore/src/storage/mstore_bytes.c
    ../../mstore/src/storage/mstore_region.c
    ../../mstore/src/storage/mstore_storage_ram.c
    ../../mstore/src/storage/mstore_storage_flash.c
    ../../mstore/src/storage/mstore_storage_ram_flash.c)
set(MSTORE_HOST_PORT ../../mstore/port/host/mstore_platform_host.c)
set(NOR_SIM_SUPPORT
    ../../mstore/tests/support/nor_sim.c
    ../../mstore/tests/support/nor_sim_device.c)

function(device_add_test name)
    add_executable(${name} ${name}.c ${DEVICE_CORE} ${HA_MODEL_CORE} ${DOMAIN_CORE}
        ${DOMAIN_HOST_PORT} ${MSTORE_CORE} ${MSTORE_HOST_PORT} ${NOR_SIM_SUPPORT})
    target_include_directories(${name} PRIVATE
        ../include
        ../../domain/include
        ../../domain/src
        ../../mstore/include
        ../../mstore/src
        ../../mstore/tests/support
        ../../sys/include
        ../../ha_model/include)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

device_add_test(test_device_service)
