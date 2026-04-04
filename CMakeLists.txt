cmake_minimum_required(VERSION 3.16)
project(muld 
    VERSION 0.1.0 
    DESCRIPTION "MULti-thread Downloader (MULD)"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # Standard practice for portability

include(GNUInstallDirs)

# Default to Release if not specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to 'Release' as none was specified.")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# Find dependencies
find_package(Boost REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# 2. Library Target
# Explicitly allow users to build as SHARED or STATIC using BUILD_SHARED_LIBS
add_library(muld STATIC
    src/chunk_info.h
    src/download_handler.h
    src/download_job.h
    src/downloader.h
    src/error.h
    src/file_info.h
    src/logger.h
    src/muld_download_manager.h
    src/task.h
    src/threadpool.h
    src/url.h
    src/writer.h
    src/download_handler.cpp
    src/downloader.cpp
    src/muld_download_manager.cpp
    src/url.cpp
)

target_include_directories(muld PUBLIC src)


target_link_libraries(muld
    PUBLIC
        Boost::system
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(muld PRIVATE -Wall -Wextra -pedantic)
    target_compile_options(muld PRIVATE $<$<CONFIG:Debug>:-O0 -g> $<$<CONFIG:Release>:-O3>)
endif()

add_executable(muld_cli src/muld_cli.cpp)
set_target_properties(muld_cli PROPERTIES OUTPUT_NAME muld)
target_include_directories(muld_cli PRIVATE include)
target_link_libraries(muld_cli PRIVATE muld)

# Install the library, executable, and export the targets
install(TARGETS muld_cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}       # Executables (.exe, etc)
)

include(CPack)
