cmake_minimum_required(VERSION 3.20)
project(opendoor VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
add_compile_definitions(_POSIX_C_SOURCE=200809L)

include(CTest)
find_package(Threads REQUIRED)
find_library(NCURSESW_LIBRARY NAMES ncursesw)
if(NOT NCURSESW_LIBRARY)
  find_file(NCURSESW_LIBRARY
    NAMES libncursesw.so.6
    PATHS /lib /usr/lib
    PATH_SUFFIXES x86_64-linux-gnu aarch64-linux-gnu
  )
endif()
if(NOT NCURSESW_LIBRARY)
  message(FATAL_ERROR "OpenDoor requires the ncursesw runtime and development library")
endif()
find_library(TINFO_LIBRARY NAMES tinfo)
if(NOT TINFO_LIBRARY)
  find_file(TINFO_LIBRARY
    NAMES libtinfo.so.6
    PATHS /lib /usr/lib
    PATH_SUFFIXES x86_64-linux-gnu aarch64-linux-gnu
  )
endif()

add_library(tomlc17 STATIC vendor/tomlc17/tomlc17.c)
target_include_directories(tomlc17 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/vendor/tomlc17)
add_library(jsmn STATIC vendor/jsmn/jsmn.c)
target_include_directories(jsmn PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/vendor/jsmn)

add_library(opendoor_core
  src/app.c
  src/common.c
  src/model.c
  src/config.c
  src/dotenv.c
  src/allocation.c
  src/scan.c
  src/docker.c
  src/discovery.c
  src/theme.c
  src/ui.c
  src/screens.c
  src/tui.c
  src/onboarding.c
  src/dashboard.c
)
target_include_directories(opendoor_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(opendoor_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor/jsmn)
target_compile_definitions(opendoor_core PUBLIC OPENDOOR_VERSION="${PROJECT_VERSION}")
target_compile_options(opendoor_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

add_executable(opendoor src/main.c)
target_link_libraries(opendoor_core PRIVATE tomlc17 jsmn Threads::Threads ${NCURSESW_LIBRARY})
if(TINFO_LIBRARY)
  target_link_libraries(opendoor_core PRIVATE ${TINFO_LIBRARY})
endif()
target_link_libraries(opendoor PRIVATE opendoor_core)
target_compile_options(opendoor PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

if(BUILD_TESTING)
  add_test(
    NAME cli_contract
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.sh
            ${CMAKE_BINARY_DIR}/opendoor
  )
  add_executable(test_core tests/test_core.c)
  target_link_libraries(test_core PRIVATE opendoor_core)
  target_compile_options(test_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  add_test(NAME core_contracts COMMAND test_core)
  add_executable(test_discovery tests/test_discovery.c)
  target_link_libraries(test_discovery PRIVATE opendoor_core)
  target_compile_options(test_discovery PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  add_test(NAME discovery_contracts COMMAND test_discovery)
  add_executable(test_ui tests/test_ui.c)
  target_link_libraries(test_ui PRIVATE opendoor_core)
  target_compile_options(test_ui PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  add_test(NAME ui_contracts COMMAND test_ui)
  add_executable(test_onboarding tests/test_onboarding.c)
  target_link_libraries(test_onboarding PRIVATE opendoor_core)
  target_compile_options(test_onboarding PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  add_test(NAME onboarding_contracts COMMAND test_onboarding)
  add_executable(test_dashboard tests/test_dashboard.c)
  target_link_libraries(test_dashboard PRIVATE opendoor_core)
  target_compile_options(test_dashboard PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  add_test(NAME dashboard_contracts COMMAND test_dashboard)
endif()

install(TARGETS opendoor RUNTIME DESTINATION bin)
install(FILES LICENSE DESTINATION share/licenses/opendoor)
install(FILES THIRD_PARTY_LICENSES.md DESTINATION share/licenses/opendoor)
