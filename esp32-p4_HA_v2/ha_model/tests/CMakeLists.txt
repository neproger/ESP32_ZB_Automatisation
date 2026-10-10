cmake_minimum_required(VERSION 3.16)
project(ha_model_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

# Пассивный словарь: тестируется только чистый lookup и содержимое дескрипторов.
set(HA_PROPERTIES_SRC ../src/ha_properties.c)

function(ha_model_add_test name)
    add_executable(${name} ${name}.c ${HA_PROPERTIES_SRC})
    target_include_directories(${name} PRIVATE ../include)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

ha_model_add_test(test_properties)
