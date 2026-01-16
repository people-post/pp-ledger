#include "Logger.h"
#include <gtest/gtest.h>

TEST(LoggerTest, RootLoggerWorks) {
    auto rootLogger = pp::logging::getRootLogger();
    EXPECT_NO_THROW({
        rootLogger->debug << "Debug message";
        rootLogger->info << "Info message";
        rootLogger->warning << "Warning message";
        rootLogger->error << "Error message";
        rootLogger->critical << "Critical message";
    });
}

TEST(LoggerTest, NamedLoggerHasCorrectName) {
    auto namedLogger = pp::logging::getLogger("myapp");
    EXPECT_EQ(namedLogger->getName(), "myapp");
    EXPECT_NO_THROW(namedLogger->info << "Test message");
}

TEST(LoggerTest, LoggingLevelFiltersMessages) {
    auto levelLogger = pp::logging::getLogger("level_test");
    levelLogger->setLevel(pp::logging::Level::WARNING);
    
    EXPECT_EQ(levelLogger->getLevel(), pp::logging::Level::WARNING);
    
    // All levels should not throw, filtering happens internally
    EXPECT_NO_THROW({
        levelLogger->debug << "Debug message";
        levelLogger->info << "Info message";
        levelLogger->warning << "Warning message";
        levelLogger->error << "Error message";
    });
}

TEST(LoggerTest, FileHandlerWorks) {
    auto fileLogger = pp::logging::getLogger("file_test");
    EXPECT_NO_THROW(fileLogger->addFileHandler("test.log", pp::logging::Level::DEBUG));
    
    fileLogger->setLevel(pp::logging::Level::INFO);
    EXPECT_NO_THROW(fileLogger->addFileHandler("detailed.log", pp::logging::Level::DEBUG));
    
    EXPECT_NO_THROW({
        fileLogger->debug << "Debug message";
        fileLogger->info << "Info message";
        fileLogger->warning << "Warning message";
    });
}

TEST(LoggerTest, LoggerRedirect) {
    auto sourceLogger = pp::logging::getLogger("source");
    auto targetLogger = pp::logging::getLogger("target");
    
    sourceLogger->setLevel(pp::logging::Level::DEBUG);
    targetLogger->setLevel(pp::logging::Level::INFO);
    
    EXPECT_FALSE(sourceLogger->hasRedirect());
    
    sourceLogger->redirectTo("target");
    EXPECT_TRUE(sourceLogger->hasRedirect());
    EXPECT_EQ(sourceLogger->getRedirectTarget(), "target");
    
    EXPECT_NO_THROW({
        sourceLogger->info << "Message via redirect";
        sourceLogger->debug << "Debug message (filtered by target level)";
        targetLogger->warning << "Direct message to target";
    });
    
    sourceLogger->clearRedirect();
    EXPECT_FALSE(sourceLogger->hasRedirect());
    
    EXPECT_NO_THROW(sourceLogger->info << "Back to source logger");
}
