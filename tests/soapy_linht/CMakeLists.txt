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
find_package(SoapySDR CONFIG)
if(NOT SoapySDR_FOUND)
    message(WARNING "SoapySDR development files not found - skipping LinHTSupport module")
    return()
endif()

include_directories(${ZMQ_INCLUDE_DIRS})

SOAPY_SDR_MODULE_UTIL(
    TARGET LinHTSupport
    SOURCES main.cpp fir.cpp
    LIBRARIES ${ZMQ_LIBRARIES}
)

target_compile_options(LinHTSupport PRIVATE ${ZMQ_CFLAGS_OTHER})
