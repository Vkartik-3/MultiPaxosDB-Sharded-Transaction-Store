# Gather all .cpp files in the utils directory
file(GLOB_RECURSE CLIENT_SOURCES "*.cc")

message(STATUS "CLIENT_SOURCES: ${CLIENT_SOURCES}")

# Create a library from the source files
add_library(client STATIC ${CLIENT_SOURCES})

# Specify the include directories for this library
target_include_directories(client PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    "${CMAKE_CURRENT_SOURCE_DIR}/../types"
    "${CMAKE_CURRENT_SOURCE_DIR}/../utils")