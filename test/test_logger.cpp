#include "Logger.h"
#include <gtest/gtest.h>

TEST(LoggerTest, RootLoggerExists) {
    auto& rootLogger = pp::logging::getRootLogger();
    // Root logger may have empty name or "root" depending on implementation
    EXPECT_NO_THROW(rootLogger.info << "Root logger works");
}

TEST(LoggerTest, RootLoggerHandlesAllLevels) {
    auto& rootLogger = pp::logging::getRootLogger();
    EXPECT_NO_THROW({
        rootLogger.debug << "This is a debug message from root";
        rootLogger.info << "This is an info message from root";
        rootLogger.warning << "This is a warning message from root";
        rootLogger.error << "This is an error message from root";
        rootLogger.critical << "This is a critical message from root";
    });
}

TEST(LoggerTest, NamedLoggerHasCorrectName) {
    auto& namedLogger = pp::logging::getLogger("myapp");
    EXPECT_EQ(namedLogger.getName(), "myapp");
    EXPECT_NO_THROW(namedLogger.info << "This is from a named logger");
}

TEST(LoggerTest, HierarchicalLoggers) {
    auto& parentLogger = pp::logging::getLogger("app");
    auto& childLogger = pp::logging::getLogger("app.module");
    auto& grandchildLogger = pp::logging::getLogger("app.module.component");
    
    EXPECT_EQ(parentLogger.getName(), "app");
    EXPECT_EQ(childLogger.getName(), "app.module");
    EXPECT_EQ(grandchildLogger.getName(), "app.module.component");
    
    EXPECT_NO_THROW({
        parentLogger.info << "Message from parent logger";
        childLogger.info << "Message from child logger";
        grandchildLogger.info << "Message from grandchild logger";
    });
}

TEST(LoggerTest, LoggingLevelFiltersMessages) {
    auto& levelLogger = pp::logging::getLogger("level_test");
    levelLogger.setLevel(pp::logging::Level::WARNING);
    
    EXPECT_EQ(levelLogger.getLevel(), pp::logging::Level::WARNING);
    
    EXPECT_NO_THROW({
        levelLogger.debug << "This debug should NOT appear";
        levelLogger.info << "This info should NOT appear";
        levelLogger.warning << "This warning SHOULD appear";
        levelLogger.error << "This error SHOULD appear";
    });
}

TEST(LoggerTest, FileHandlerAddsSuccessfully) {
    auto& fileLogger = pp::logging::getLogger("file_test");
    EXPECT_NO_THROW(fileLogger.addFileHandler("test.log", pp::logging::Level::DEBUG));
    EXPECT_NO_THROW({
        fileLogger.info << "This message goes to both console and file";
        fileLogger.debug << "This debug message also goes to file";
    });
}

TEST(LoggerTest, FileHandlerWithDifferentLevel) {
    auto& multiLogger = pp::logging::getLogger("multi_handler");
    multiLogger.setLevel(pp::logging::Level::INFO);
    EXPECT_NO_THROW(multiLogger.addFileHandler("detailed.log", pp::logging::Level::DEBUG));
    
    EXPECT_NO_THROW({
        multiLogger.debug << "Debug: only in file (if file level allows)";
        multiLogger.info << "Info: in both console and file";
        multiLogger.warning << "Warning: in both console and file";
    });
}

TEST(LoggerTest, LoggerRedirect) {
    auto& sourceLogger = pp::logging::getLogger("source");
    auto& targetLogger = pp::logging::getLogger("target");
    
    sourceLogger.setLevel(pp::logging::Level::DEBUG);
    targetLogger.setLevel(pp::logging::Level::INFO);
    
    EXPECT_FALSE(sourceLogger.hasRedirect());
    
    sourceLogger.redirectTo("target");
    EXPECT_TRUE(sourceLogger.hasRedirect());
    EXPECT_EQ(sourceLogger.getRedirectTarget(), "target");
    
    EXPECT_NO_THROW({
        sourceLogger.info << "This should appear as target logger";
        sourceLogger.debug << "This debug should NOT appear (target level is INFO)";
        targetLogger.warning << "Direct message to target";
    });
    
    sourceLogger.clearRedirect();
    EXPECT_FALSE(sourceLogger.hasRedirect());
    
    EXPECT_NO_THROW(sourceLogger.info << "Back to source logger");
}

TEST(LoggerTest, NestedLoggerRedirect) {
    auto& loggerAB = pp::logging::getLogger("a.b");
    auto& loggerCD = pp::logging::getLogger("c.d");
    
    loggerCD.setLevel(pp::logging::Level::WARNING);
    
    loggerAB.redirectTo("c.d");
    EXPECT_TRUE(loggerAB.hasRedirect());
    
    EXPECT_NO_THROW({
        loggerAB.info << "This info should NOT appear (c.d level is WARNING)";
        loggerAB.warning << "This warning SHOULD appear via c.d";
        loggerAB.error << "This error SHOULD appear via c.d";
    });
}
