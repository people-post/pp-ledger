# Lib library
add_library(ppledger_lib STATIC
    Lib.cpp
    Lib.h
    Logger.cpp
    Logger.h
    Module.cpp
    Module.h
    ResultOrError.hpp
)

target_include_directories(ppledger_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Add tests subdirectory if testing is enabled
if(BUILD_TESTING)
    add_subdirectory(test)
endif()
