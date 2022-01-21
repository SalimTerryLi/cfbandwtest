cmake_minimum_required(VERSION 3.12)
project(cfbandwtest C)

find_package(CURL REQUIRED)
include_directories(${CURL_INCLUDE_DIR})

set(CMAKE_C_STANDARD 11)

add_executable(cfbandwtest main.c)
target_link_libraries(cfbandwtest ${CURL_LIBRARIES})
