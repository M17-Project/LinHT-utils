cmake_minimum_required(VERSION 3.3...3.10)
project(SoapyLinHTSupport CXX)

# Default to Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
    message(STATUS "Build type not specified: defaulting to Release.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ZMQ
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZMQ REQUIRED libzmq)

# SoapySDR
find_package(SoapySDR REQUIRED)

include_directories(${SoapySDR_INCLUDE_DIRS})
include_directories(${ZMQ_INCLUDE_DIRS})

find_library(SX1255_LIB sx1255 REQUIRED)

message(STATUS "Using SX1255_LIB = ${SX1255_LIB}")
message(STATUS "SoapySDR_LIBRARIES = ${SoapySDR_LIBRARIES}")

add_library(LinHTSupport MODULE
    main.cpp
    fir.cpp
)

target_compile_options(LinHTSupport PRIVATE ${ZMQ_CFLAGS_OTHER})

target_link_libraries(LinHTSupport
    PRIVATE
        ${SoapySDR_LIBRARIES}
        ${ZMQ_LIBRARIES}
        ${SX1255_LIB}
        ${GPIOD_LIB}
        ${M_LIB}
)

set_target_properties(LinHTSupport PROPERTIES
    PREFIX "lib"
)
