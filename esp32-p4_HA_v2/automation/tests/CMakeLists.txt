cmake_minimum_required(VERSION 3.16)
project(automation_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

# Тестируется только чистая логика правила: сопоставление и сборка команды.
set(AUTOMATION_CORE ../src/automation_rule.c)

function(automation_add_test name)
    add_executable(${name} ${name}.c ${AUTOMATION_CORE})
    target_include_directories(${name} PRIVATE
        ../include
        ../../sys/include
        ../../ha_model/include)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

automation_add_test(test_rule)
