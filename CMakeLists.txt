cmake_minimum_required(VERSION 3.8)
project(lockfree_queues)

option(LOCKFREE_QUEUES_BUILD_TESTS "Build the tests" OFF)
option(LOCKFREE_QUEUES_BUILD_BENCHMARKS "Build the benchmarks" OFF)
option(LOCKFREE_QUEUES_SANITIZE_ADDRESS "Enable address sanitizer in tests" OFF)
option(LOCKFREE_QUEUES_SANITIZE_THREAD "Enable thread sanitizer in tests" OFF)

if (NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif ()

# address sanitizer flags
if (LOCKFREE_QUEUES)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address,undefined -fno-omit-frame-pointer -g")
endif ()

# thread sanitizer flags
if (LOCKFREE_QUEUES)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread")
endif ()

# library name
set(TARGET_NAME lockfree_queues)

# header files
set(HEADER_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lockfree_queues/sp_broadcast_queue.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/lockfree_queues/utilities.h)

# Add this as a library
add_library(${TARGET_NAME} INTERFACE)

# Add target sources
target_sources(${TARGET_NAME} INTERFACE ${HEADER_FILES})

# Add include directories for this library
target_include_directories(${TARGET_NAME} INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

if (LOCKFREE_QUEUES_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif ()

if (LOCKFREE_QUEUES_BUILD_BENCHMARKS)
    add_subdirectory(benchmark)
endif ()