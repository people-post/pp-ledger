cmake_minimum_required(VERSION 3.15)
project(pp-ledger VERSION 1.0.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add subdirectories for each component
add_subdirectory(lib)
add_subdirectory(client)
add_subdirectory(server)
add_subdirectory(app)

# Enable testing
enable_testing()
add_subdirectory(test)
