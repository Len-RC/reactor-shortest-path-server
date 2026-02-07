cmake_minimum_required(VERSION 3.16)
project(reactor_mysql_project LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)

# ---- MySQL / MariaDB client detection ----
# Prefer mysql_config (works well with mysql-community-devel and many installs)
find_program(MYSQL_CONFIG mysql_config)

if(MYSQL_CONFIG)
  execute_process(COMMAND ${MYSQL_CONFIG} --cflags
    OUTPUT_VARIABLE MYSQL_CFLAGS
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND ${MYSQL_CONFIG} --libs
    OUTPUT_VARIABLE MYSQL_LDFLAGS
    OUTPUT_STRIP_TRAILING_WHITESPACE)

  separate_arguments(MYSQL_CFLAGS_LIST NATIVE_COMMAND "${MYSQL_CFLAGS}")
  separate_arguments(MYSQL_LDFLAGS_LIST NATIVE_COMMAND "${MYSQL_LDFLAGS}")

else()
  # Fallback: pkg-config (common on many distros, esp. MariaDB)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(MYSQLCLIENT QUIET mysqlclient mariadb)
  endif()

  if(NOT MYSQLCLIENT_FOUND)
    message(FATAL_ERROR
      "Cannot find MySQL/MariaDB client dev files.\n"
      "Install one of:\n"
      "  - Ubuntu/Debian: libmysqlclient-dev OR libmariadb-dev\n"
      "  - RHEL/CentOS: mysql-community-devel OR mariadb-devel\n"
    )
  endif()
endif()

# ===== server =====
add_executable(server src/server.cpp)
if(MYSQL_CONFIG)
  target_compile_options(server PRIVATE ${MYSQL_CFLAGS_LIST})
  target_link_libraries(server PRIVATE ${MYSQL_LDFLAGS_LIST} Threads::Threads)
else()
  target_include_directories(server PRIVATE ${MYSQLCLIENT_INCLUDE_DIRS})
  target_link_libraries(server PRIVATE ${MYSQLCLIENT_LIBRARIES} Threads::Threads)
endif()

# ===== benchmark tool =====
add_executable(benchmark src/benchmark.cpp)
target_link_libraries(benchmark PRIVATE Threads::Threads)

# ===== offline tool =====
add_executable(offline_build src/offline_build.cpp)
if(MYSQL_CONFIG)
  target_compile_options(offline_build PRIVATE ${MYSQL_CFLAGS_LIST})
  target_link_libraries(offline_build PRIVATE ${MYSQL_LDFLAGS_LIST} Threads::Threads)
else()
  target_include_directories(offline_build PRIVATE ${MYSQLCLIENT_INCLUDE_DIRS})
  target_link_libraries(offline_build PRIVATE ${MYSQLCLIENT_LIBRARIES} Threads::Threads)
endif()
