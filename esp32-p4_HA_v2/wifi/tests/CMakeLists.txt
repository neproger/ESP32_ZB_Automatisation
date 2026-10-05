cmake_minimum_required(VERSION 3.16)
project(wifi_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

# Тестируется только чистая логика provisioning: выбор известной точки по скану.
set(WIFI_CORE ../src/wifi_select.c)

function(wifi_add_test name)
    add_executable(${name} ${name}.c ${WIFI_CORE})
    target_include_directories(${name} PRIVATE ../include ../../sys/include ../../ha_model/include)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

wifi_add_test(test_wifi_select)
