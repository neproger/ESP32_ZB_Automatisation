cmake_minimum_required(VERSION 3.16)
project(semantics_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

# Мост + словарь ha_model (чистый lookup). Без FreeRTOS, Domain и Zigbee.
set(SEMANTICS_SRC ../src/semantics.c)
set(HA_PROPERTIES_SRC ../../ha_model/src/ha_properties.c)

function(semantics_add_test name)
    add_executable(${name} ${name}.c ${SEMANTICS_SRC} ${HA_PROPERTIES_SRC})
    target_include_directories(${name} PRIVATE ../include ../../ha_model/include)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

semantics_add_test(test_semantics)
