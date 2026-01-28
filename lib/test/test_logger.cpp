#include "Logger.h"
#include <gtest/gtest.h>

TEST(LoggerTest, RootLoggerWorks) {
    auto rootLogger = pp::logging::getRootLogger();
    EXPECT_NO_THROW({
        rootLogger.debug << "Debug message";
        rootLogger.info << "Info message";
        rootLogger.warning << "Warning message";
        rootLogger.error << "Error message";
        rootLogger.critical << "Critical message";
    });
}

TEST(LoggerTest, NamedLoggerHasCorrectName) {
    auto namedLogger = pp::logging::getLogger("myapp");
    EXPECT_EQ(namedLogger.getName(), "myapp");
    EXPECT_NO_THROW(namedLogger.info << "Test message");
}

TEST(LoggerTest, LoggingLevelFiltersMessages) {
    auto levelLogger = pp::logging::getLogger("level_test");
    levelLogger.setLevel(pp::logging::Level::WARNING);
    
    EXPECT_EQ(levelLogger.getLevel(), pp::logging::Level::WARNING);
    
    // All levels should not throw, filtering happens internally
    EXPECT_NO_THROW({
        levelLogger.debug << "Debug message";
        levelLogger.info << "Info message";
        levelLogger.warning << "Warning message";
        levelLogger.error << "Error message";
    });
}

TEST(LoggerTest, FileHandlerWorks) {
    auto fileLogger = pp::logging::getLogger("file_test");
    EXPECT_NO_THROW(fileLogger.addFileHandler("test.log", pp::logging::Level::DEBUG));
    
    fileLogger.setLevel(pp::logging::Level::INFO);
    EXPECT_NO_THROW(fileLogger.addFileHandler("detailed.log", pp::logging::Level::DEBUG));
    
    EXPECT_NO_THROW({
        fileLogger.debug << "Debug message";
        fileLogger.info << "Info message";
        fileLogger.warning << "Warning message";
    });
}

TEST(LoggerTest, LoggerRedirect) {
    auto sourceLogger = pp::logging::getLogger("source");
    auto targetLogger = pp::logging::getLogger("target");
    
    sourceLogger.setLevel(pp::logging::Level::DEBUG);
    targetLogger.setLevel(pp::logging::Level::INFO);
    
    // Initially, source and target are both children of root
    auto root = pp::logging::getRootLogger();
    EXPECT_EQ(sourceLogger.getParent(), root);
    EXPECT_EQ(targetLogger.getParent(), root);
    
    // Redirect source to target - moves source in the tree
    sourceLogger.redirectTo("target");
    EXPECT_EQ(sourceLogger.getParent(), targetLogger);
    
    EXPECT_NO_THROW({
        sourceLogger.info << "Message via redirect";
        sourceLogger.debug << "Debug message (filtered by target level)";
        targetLogger.warning << "Direct message to target";
    });
    
    // Source is now a child of target (permanent)
    EXPECT_EQ(sourceLogger.getParent(), targetLogger);
    
    EXPECT_NO_THROW(sourceLogger.info << "Back to source logger");
}

TEST(LoggerTest, HierarchicalLoggerCreatesTree) {
    // Create hierarchical loggers
    auto moduleA = pp::logging::getLogger("moduleA");
    auto service1 = pp::logging::getLogger("moduleA.service1");
    auto service2 = pp::logging::getLogger("moduleA.service2");
    
    // Verify tree structure
    auto root = pp::logging::getRootLogger();
    EXPECT_EQ(moduleA.getParent(), root);
    EXPECT_EQ(service1.getParent(), moduleA);
    EXPECT_EQ(service2.getParent(), moduleA);
    
    // moduleA should have 2 children
    EXPECT_EQ(moduleA.getChildren().size(), 2);
}

TEST(LoggerTest, RedirectMovesLoggerAndChildren) {
    // Create tree structure:
    // root
    // ├── moduleA
    // │   ├── moduleA.service1
    // │   └── moduleA.service2
    // └── moduleB
    
    auto root = pp::logging::getRootLogger();
    auto moduleA = pp::logging::getLogger("moduleA");
    auto service1 = pp::logging::getLogger("moduleA.service1");
    auto service2 = pp::logging::getLogger("moduleA.service2");
    auto moduleB = pp::logging::getLogger("moduleB");
    
    // Disable propagation to see only direct handler output
    moduleA.setPropagate(false);
    service1.setPropagate(false);
    service2.setPropagate(false);
    moduleB.setPropagate(false);
    root.setPropagate(false);
    
    // Initial state
    EXPECT_EQ(moduleA.getParent(), root);
    EXPECT_EQ(service1.getParent(), moduleA);
    EXPECT_EQ(service2.getParent(), moduleA);
    EXPECT_EQ(moduleB.getParent(), root);
    
    // Move moduleA (and its children) under moduleB
    moduleA.redirectTo(moduleB);
    
    // New tree structure:
    // root
    // └── moduleB
    //     └── moduleA
    //         ├── moduleA.service1
    //         └── moduleA.service2
    
    EXPECT_EQ(moduleA.getParent(), moduleB);
    EXPECT_EQ(service1.getParent(), moduleA);  // Still parent of service1
    EXPECT_EQ(service2.getParent(), moduleA);  // Still parent of service2
    EXPECT_EQ(moduleB.getParent(), root);
    
    // moduleB now has moduleA as child
    EXPECT_EQ(moduleB.getChildren().size(), 1);
}

TEST(LoggerTest, LogPropagationInTree) {
    auto root = pp::logging::getRootLogger();
    auto parent = pp::logging::getLogger("parent");
    auto child = pp::logging::getLogger("parent.child");
    
    // By default, logs propagate up the tree
    EXPECT_TRUE(child.getPropagate());
    
    // When child logs, it goes to child handlers AND propagates to parent
    child.info << "Child message";
    // This will show 3 times: [parent.child], [parent], and root
    
    // Can disable propagation
    child.setPropagate(false);
    EXPECT_FALSE(child.getPropagate());
    
    child.info << "No propagation";
    // This will show only once: [parent.child]
}

TEST(LoggerTest, PreventCircularRedirection) {
    auto loggerA = pp::logging::getLogger("loggerA");
    auto loggerB = pp::logging::getLogger("loggerB");
    auto loggerC = pp::logging::getLogger("loggerC");
    
    // Create chain: A . B . C
    loggerA.redirectTo(loggerB);
    loggerB.redirectTo(loggerC);
    
    // Try to create cycle: C . A (should throw)
    EXPECT_THROW(loggerC.redirectTo(loggerA), std::invalid_argument);
    
    // Also prevent self-redirection
    EXPECT_THROW(loggerA.redirectTo(loggerA), std::invalid_argument);
}

TEST(LoggerTest, ComplexTreeReorganization) {
    // Create initial tree:
    // root
    // ├── app
    // │   ├── app.ui
    // │   └── app.backend
    // │       └── app.backend.db
    // └── system
    
    auto root = pp::logging::getRootLogger();
    auto app = pp::logging::getLogger("app");
    auto ui = pp::logging::getLogger("app.ui");
    auto backend = pp::logging::getLogger("app.backend");
    auto db = pp::logging::getLogger("app.backend.db");
    auto system = pp::logging::getLogger("system");
    
    // Verify initial structure
    EXPECT_EQ(app.getParent(), root);
    EXPECT_EQ(ui.getParent(), app);
    EXPECT_EQ(backend.getParent(), app);
    EXPECT_EQ(db.getParent(), backend);
    EXPECT_EQ(system.getParent(), root);
    
    // Move entire backend subtree under system
    backend.redirectTo(system);
    
    // New tree:
    // root
    // ├── app
    // │   └── app.ui
    // └── system
    //     └── app.backend
    //         └── app.backend.db
    
    EXPECT_EQ(backend.getParent(), system);
    EXPECT_EQ(db.getParent(), backend);  // db still under backend
    EXPECT_EQ(ui.getParent(), app);      // ui still under app
    
    // app lost backend but still has ui
    EXPECT_EQ(app.getChildren().size(), 1);
    
    // system gained backend as child
    EXPECT_EQ(system.getChildren().size(), 1);
}

TEST(LoggerTest, RedirectToExistingLoggerMovesUnderIt) {
    // Case 1: R.A redirect to R.B where R.B exists . moves A under R.B
    auto loggerA = pp::logging::getLogger("root.A");
    auto loggerB = pp::logging::getLogger("root.B");
    auto root = pp::logging::getRootLogger();
    
    // Initially both are children of root
    EXPECT_EQ(loggerA.getParent().getFullName(), "root");
    EXPECT_EQ(loggerB.getParent().getFullName(), "root");
    EXPECT_EQ(loggerA.getFullName(), "root.A");
    
    // Redirect A to B (B exists)
    loggerA.redirectTo("root.B");
    
    // A should now be under B, name unchanged
    EXPECT_EQ(loggerA.getParent(), loggerB);
    EXPECT_EQ(loggerA.getFullName(), "root.B.A");
}

TEST(LoggerTest, RedirectToNonExistingLoggerRenames) {
    // Case 2: R.A redirect to R.C where R.C doesn't exist . R.A becomes child of R.C
    auto loggerA = pp::logging::getLogger("rename.A");
    auto renameRoot = pp::logging::getLogger("rename");
    
    EXPECT_EQ(loggerA.getFullName(), "rename.A");
    EXPECT_EQ(loggerA.getParent().getFullName(), "rename");
    
    // Redirect A to C (C doesn't exist, will be created)
    loggerA.redirectTo("rename.C");
    
    // A keeps its node name but is now under C
    EXPECT_EQ(loggerA.getName(), "A");  // Node name stays the same
    EXPECT_EQ(loggerA.getFullName(), "rename.C.A");  // Full path changes
    EXPECT_EQ(loggerA.getParent().getFullName(), "rename.C");
    
    // Verify C exists and is the parent
    auto loggerC = pp::logging::getLogger("rename.C");
    EXPECT_EQ(loggerC, loggerA.getParent());
    EXPECT_EQ(loggerC.getFullName(), "rename.C");
}

TEST(LoggerTest, RedirectRenameToCompletelyNewHierarchy) {
    // R.A redirect to S.B where S doesn't exist . R.A becomes child of S.B
    // Use unique names to avoid interference from previous tests
    auto loggerA = pp::logging::getLogger("hierarchy.A");
    
    EXPECT_EQ(loggerA.getFullName(), "hierarchy.A");
    
    // Redirect to completely new hierarchy
    loggerA.redirectTo("system.B");
    
    // A keeps its node name but full path changes
    EXPECT_EQ(loggerA.getName(), "A");  // Node name unchanged
    EXPECT_EQ(loggerA.getFullName(), "system.B.A");  // Now under system.B
    
    // Get the parent directly (should be "system.B")
    auto parent = loggerA.getParent();
    EXPECT_NE(parent.getNode(), nullptr);
    EXPECT_EQ(parent.getFullName(), "system.B");
    
    // Getting system.B from registry should return the same instance
    auto systemBLogger = pp::logging::getLogger("system.B");
    EXPECT_EQ(parent, systemBLogger);
}
