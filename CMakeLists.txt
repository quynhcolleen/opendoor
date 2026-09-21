cmake_minimum_required(VERSION 3.20)
project(opendoor VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(CTest)

add_library(opendoor_core
  src/app.c
)
target_include_directories(opendoor_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(opendoor_core PUBLIC OPENDOOR_VERSION="${PROJECT_VERSION}")
target_compile_options(opendoor_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

add_executable(opendoor src/main.c)
target_link_libraries(opendoor PRIVATE opendoor_core)
target_compile_options(opendoor PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

if(BUILD_TESTING)
  add_test(
    NAME cli_contract
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.sh
            ${CMAKE_BINARY_DIR}/opendoor
  )
endif()

install(TARGETS opendoor RUNTIME DESTINATION bin)
install(FILES LICENSE DESTINATION share/licenses/opendoor)

