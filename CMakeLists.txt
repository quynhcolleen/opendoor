cmake_minimum_required(VERSION 3.20)
project(opendoor VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
add_compile_definitions(_POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700)

include(CTest)
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

add_library(opendoor_core
  src/app.c
  src/common.c
  src/model.c
  src/config.c
  src/dotenv.c
  src/allocation.c
  src/scan.c
  src/discovery.c
  src/ui.c
  src/screens.c
  src/tui.c
  src/dashboard.c
  src/persistence.c
  src/resolution.c
)
target_include_directories(opendoor_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(opendoor_core PUBLIC OPENDOOR_VERSION="${PROJECT_VERSION}")
target_compile_options(opendoor_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

add_executable(opendoor src/main.c)
target_link_libraries(opendoor PRIVATE opendoor_core ${NCURSESW_LIBRARY})
if(TINFO_LIBRARY)
  target_link_libraries(opendoor PRIVATE ${TINFO_LIBRARY})
endif()
target_compile_options(opendoor PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

if(BUILD_TESTING)
  add_executable(test_core tests/test_core.c)
  target_link_libraries(test_core PRIVATE opendoor_core)
  target_compile_options(test_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  add_test(NAME resolution_allocation_contracts COMMAND test_core)

  add_executable(test_discovery tests/test_discovery.c)
  target_link_libraries(test_discovery PRIVATE opendoor_core)
  target_compile_options(test_discovery PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  add_test(NAME scan_contracts COMMAND test_discovery)

  add_executable(test_persistence tests/test_persistence.c)
  target_link_libraries(test_persistence PRIVATE opendoor_core)
  target_compile_options(test_persistence PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  add_test(NAME config_contracts COMMAND test_persistence)

  add_executable(test_ui tests/test_ui.c)
  target_link_libraries(test_ui PRIVATE opendoor_core ${NCURSESW_LIBRARY})
  if(TINFO_LIBRARY)
    target_link_libraries(test_ui PRIVATE ${TINFO_LIBRARY})
  endif()
  target_compile_options(test_ui PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  add_test(NAME menu_contracts COMMAND test_ui)

  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME tui_mouse_contracts
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tui_pty.py
              $<TARGET_FILE:opendoor>)
    set_tests_properties(tui_mouse_contracts PROPERTIES TIMEOUT 30)
  endif()
endif()

install(TARGETS opendoor RUNTIME DESTINATION bin)
install(FILES README.md DESTINATION share/doc/opendoor)
install(FILES LICENSE DESTINATION share/licenses/opendoor)
install(FILES THIRD_PARTY_LICENSES.md DESTINATION share/licenses/opendoor)
