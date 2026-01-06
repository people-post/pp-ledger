#include "Logger.h"

#include <iostream>

int main() {
    std::cout << "=== Testing Logger Features ===\n\n";
    
    // Test 1: Root logger (default console output)
    std::cout << "1. Testing root logger:\n";
    auto& rootLogger = pp::logging::getRootLogger();
    rootLogger.debug << "This is a debug message from root";
    rootLogger.info << "This is an info message from root";
    rootLogger.warning << "This is a warning message from root";
    rootLogger.error << "This is an error message from root";
    rootLogger.critical << "This is a critical message from root";
    
    std::cout << "\n2. Testing named logger:\n";
    auto& namedLogger = pp::logging::getLogger("myapp");
    namedLogger.info << "This is from a named logger";
    
    std::cout << "\n3. Testing hierarchical loggers (dot notation):\n";
    auto& parentLogger = pp::logging::getLogger("app");
    auto& childLogger = pp::logging::getLogger("app.module");
    auto& grandchildLogger = pp::logging::getLogger("app.module.component");
    
    parentLogger.info << "Message from parent logger";
    childLogger.info << "Message from child logger";
    grandchildLogger.info << "Message from grandchild logger";
    
    std::cout << "\n4. Testing logging level:\n";
    auto& levelLogger = pp::logging::getLogger("level_test");
    levelLogger.setLevel(pp::logging::Level::WARNING);
    levelLogger.debug << "This debug should NOT appear";
    levelLogger.info << "This info should NOT appear";
    levelLogger.warning << "This warning SHOULD appear";
    levelLogger.error << "This error SHOULD appear";
    
    std::cout << "\n5. Testing file handler:\n";
    auto& fileLogger = pp::logging::getLogger("file_test");
    fileLogger.addFileHandler("test.log", pp::logging::Level::DEBUG);
    fileLogger.info << "This message goes to both console and file";
    fileLogger.debug << "This debug message also goes to file";
    
    std::cout << "\n6. Testing file handler with different level:\n";
    auto& multiLogger = pp::logging::getLogger("multi_handler");
    multiLogger.setLevel(pp::logging::Level::INFO);
    multiLogger.addFileHandler("detailed.log", pp::logging::Level::DEBUG);
    
    multiLogger.debug << "Debug: only in file (if file level allows)";
    multiLogger.info << "Info: in both console and file";
    multiLogger.warning << "Warning: in both console and file";
    
    std::cout << "\n=== Test Complete ===\n";
    std::cout << "Check test.log and detailed.log for file output\n";
    
    return 0;
}
