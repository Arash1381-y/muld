cmake_minimum_required(VERSION 3.16)
project(beast_client)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to Release if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Debug flags
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")

# Release flags
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

find_package(Boost REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(muld
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
    PRIVATE
    Boost::system
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
)

add_executable(muld_cli
    src/muld_cli.cpp
)
target_include_directories(muld_cli PUBLIC include)

target_link_libraries(muld_cli 
    PRIVATE
    muld
)
