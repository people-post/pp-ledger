#include "Logger.h"
#include <gtest/gtest.h>

TEST(LoggerNodeTest, BasicNodeCreation) {
    auto node = std::make_shared<pp::logging::LoggerNode>("test");
    EXPECT_EQ(node->getName(), "test");
    EXPECT_EQ(node->getLevel(), pp::logging::Level::DEBUG);
    EXPECT_TRUE(node->getPropagate());
}

TEST(LoggerNodeTest, NodeLogging) {
    auto node = std::make_shared<pp::logging::LoggerNode>("test_logging");
    EXPECT_NO_THROW({
        node->log(pp::logging::Level::DEBUG, "Debug message");
        node->log(pp::logging::Level::INFO, "Info message");
        node->log(pp::logging::Level::WARNING, "Warning message");
        node->log(pp::logging::Level::ERROR, "Error message");
        node->log(pp::logging::Level::CRITICAL, "Critical message");
    });
}

TEST(LoggerNodeTest, LevelFiltering) {
    auto node = std::make_shared<pp::logging::LoggerNode>("level_test");
    node->setLevel(pp::logging::Level::WARNING);
    
    EXPECT_EQ(node->getLevel(), pp::logging::Level::WARNING);
    
    // All levels should not throw, filtering happens internally
    EXPECT_NO_THROW({
        node->log(pp::logging::Level::DEBUG, "Debug message");
        node->log(pp::logging::Level::INFO, "Info message");
        node->log(pp::logging::Level::WARNING, "Warning message");
        node->log(pp::logging::Level::ERROR, "Error message");
    });
}

TEST(LoggerNodeTest, FileHandler) {
    auto node = std::make_shared<pp::logging::LoggerNode>("file_test");
    EXPECT_NO_THROW(node->addFileHandler("test.log", pp::logging::Level::DEBUG));
    
    node->setLevel(pp::logging::Level::INFO);
    EXPECT_NO_THROW(node->addFileHandler("detailed.log", pp::logging::Level::DEBUG));
    
    EXPECT_NO_THROW({
        node->log(pp::logging::Level::DEBUG, "Debug message");
        node->log(pp::logging::Level::INFO, "Info message");
        node->log(pp::logging::Level::WARNING, "Warning message");
    });
}

TEST(LoggerNodeTest, ParentChildRelationships) {
    auto parent = std::make_shared<pp::logging::LoggerNode>("parent");
    auto child = std::make_shared<pp::logging::LoggerNode>("child");
    
    // Initially, child has no parent
    EXPECT_EQ(child->getParent(), nullptr);
    
    // Add child to parent
    child->setParent(parent);
    parent->addChild(child);
    
    EXPECT_EQ(child->getParent(), parent);
    EXPECT_EQ(parent->getChildren().size(), 1);
    EXPECT_EQ(parent->getChildren()[0], child);
}

TEST(LoggerNodeTest, HierarchicalLoggerCreatesTree) {
    // Create hierarchical structure manually
    auto root = std::make_shared<pp::logging::LoggerNode>("");
    auto moduleA = root->getOrInitChild("moduleA");
    auto service1 = moduleA->getOrInitChild("service1");
    auto service2 = moduleA->getOrInitChild("service2");
    
    // Verify tree structure
    EXPECT_EQ(moduleA->getParent(), root);
    EXPECT_EQ(service1->getParent(), moduleA);
    EXPECT_EQ(service2->getParent(), moduleA);
    
    // moduleA should have 2 children
    EXPECT_EQ(moduleA->getChildren().size(), 2);
}

TEST(LoggerNodeTest, GetOrInitChildCreatesIfNeeded) {
    auto root = std::make_shared<pp::logging::LoggerNode>("root");
    
    // First call creates the child
    auto child1 = root->getOrInitChild("child");
    EXPECT_NE(child1, nullptr);
    EXPECT_EQ(child1->getName(), "child");
    EXPECT_EQ(child1->getParent(), root);
    EXPECT_EQ(root->getChildren().size(), 1);
    
    // Second call returns the same child
    auto child2 = root->getOrInitChild("child");
    EXPECT_EQ(child1, child2);
    EXPECT_EQ(root->getChildren().size(), 1);
}

TEST(LoggerNodeTest, GetOrInitChildHandlesHierarchicalNames) {
    auto root = std::make_shared<pp::logging::LoggerNode>("root");
    
    // Create hierarchical path
    auto deepNode = root->getOrInitChild("level1.level2.level3");
    EXPECT_NE(deepNode, nullptr);
    EXPECT_EQ(deepNode->getName(), "level3");
    
    // Verify intermediate nodes were created
    auto level1 = root->getOrInitChild("level1");
    EXPECT_NE(level1, nullptr);
    EXPECT_EQ(level1->getName(), "level1");
    
    auto level2 = level1->getOrInitChild("level2");
    EXPECT_NE(level2, nullptr);
    EXPECT_EQ(level2->getName(), "level2");
    EXPECT_EQ(level2->getParent(), level1);
    
    EXPECT_EQ(deepNode->getParent(), level2);
}

TEST(LoggerNodeTest, FullNameReflectsHierarchy) {
    auto root = std::make_shared<pp::logging::LoggerNode>("");
    auto parent = root->getOrInitChild("parent");
    auto child = parent->getOrInitChild("child");
    
    EXPECT_EQ(root->getFullName(), "");
    EXPECT_EQ(parent->getFullName(), "parent");
    EXPECT_EQ(child->getFullName(), "parent.child");
}

TEST(LoggerNodeTest, LogPropagation) {
    auto parent = std::make_shared<pp::logging::LoggerNode>("parent");
    auto child = std::make_shared<pp::logging::LoggerNode>("child");
    
    child->setParent(parent);
    parent->addChild(child);
    
    // By default, logs propagate up the tree
    EXPECT_TRUE(child->getPropagate());
    
    // Logging should work
    EXPECT_NO_THROW(child->log(pp::logging::Level::INFO, "Child message"));
    
    // Can disable propagation
    child->setPropagate(false);
    EXPECT_FALSE(child->getPropagate());
    
    EXPECT_NO_THROW(child->log(pp::logging::Level::INFO, "No propagation"));
}

TEST(LoggerNodeTest, RemoveChild) {
    auto parent = std::make_shared<pp::logging::LoggerNode>("parent");
    auto child1 = std::make_shared<pp::logging::LoggerNode>("child1");
    auto child2 = std::make_shared<pp::logging::LoggerNode>("child2");
    
    parent->addChild(child1);
    parent->addChild(child2);
    
    EXPECT_EQ(parent->getChildren().size(), 2);
    
    // Remove child1
    parent->removeChild(child1.get());
    EXPECT_EQ(parent->getChildren().size(), 1);
    EXPECT_EQ(parent->getChildren()[0], child2);
    
    // Remove child2
    parent->removeChild(child2.get());
    EXPECT_EQ(parent->getChildren().size(), 0);
}

// Integration tests using the Logger wrapper
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

TEST(LoggerNodeTest, GetLoggerWithEmptyName) {
    // Getting a logger with empty name should return the root logger
    auto emptyLogger = pp::logging::getLogger("");
    auto rootLogger = pp::logging::getRootLogger();
    
    EXPECT_EQ(emptyLogger.getName(), "");
    EXPECT_EQ(emptyLogger, rootLogger);
    EXPECT_EQ(emptyLogger.getFullName(), "");
}

TEST(LoggerNodeTest, GetOrInitChildWithEmptyName) {
    auto root = std::make_shared<pp::logging::LoggerNode>("root");
    
    // Getting a child with empty name should return the node itself
    auto emptyChild = root->getOrInitChild("");
    
    // Should return the same node (root itself)
    EXPECT_EQ(emptyChild, root);
    EXPECT_EQ(emptyChild->getName(), "root");
    
    // Second call should return the same node
    auto emptyChild2 = root->getOrInitChild("");
    EXPECT_EQ(emptyChild, emptyChild2);
    EXPECT_EQ(emptyChild2, root);
}

TEST(LoggerNodeTest, EmptyNameInHierarchicalPath) {
    auto root = std::make_shared<pp::logging::LoggerNode>("root");
    
    // Test leading dot (empty first component)
    auto child1 = root->getOrInitChild(".child");
    EXPECT_EQ(child1->getName(), "child");
    
    // Should be the same as without the leading dot
    auto child2 = root->getOrInitChild("child");
    EXPECT_EQ(child1, child2);
}

TEST(LoggerTest, RootLoggerRedirectSwitchesInsteadOfMoving) {
    auto rootLogger = pp::logging::getRootLogger();
    auto targetLogger = pp::logging::getLogger("target");
    
    // Save original root for comparison
    auto originalRootNode = rootLogger.getFullName();
    EXPECT_EQ(originalRootNode, "");
    
    // When root logger redirects, it should switch to target, not move the root node
    rootLogger.redirectTo("target");
    
    // The wrapper should now point to target
    EXPECT_EQ(rootLogger.getFullName(), "target");
    EXPECT_EQ(rootLogger, targetLogger);
    
    // Get root logger again - should still be the actual root
    auto actualRoot = pp::logging::getRootLogger();
    EXPECT_EQ(actualRoot.getFullName(), "");
    EXPECT_NE(actualRoot, rootLogger); // rootLogger has switched to target
}

TEST(LoggerTest, RedirectMergesChildrenAndDissolvesNode) {
    // Create a tree structure using LoggerNode directly to verify internal behavior
    auto root = std::make_shared<pp::logging::LoggerNode>("");
    auto sourceNode = root->getOrInitChild("source");
    auto child1Node = sourceNode->getOrInitChild("child1");
    auto child2Node = sourceNode->getOrInitChild("child2");
    auto targetNode = root->getOrInitChild("target");
    
    // Verify initial structure
    EXPECT_EQ(sourceNode->getChildren().size(), 2);
    EXPECT_EQ(targetNode->getChildren().size(), 0);
    EXPECT_EQ(child1Node->getParent(), sourceNode);
    EXPECT_EQ(child2Node->getParent(), sourceNode);
    
    // Now test with Logger wrapper
    auto source = pp::logging::getLogger("source2");
    auto child1 = pp::logging::getLogger("source2.child1");
    auto child2 = pp::logging::getLogger("source2.child2");
    auto target = pp::logging::getLogger("target2");
    
    // Redirect source to target
    source.redirectTo("target2");
    
    // After redirect:
    // - source wrapper should point to target
    // - children should now report being under target
    
    EXPECT_EQ(source, target);
    EXPECT_EQ(source.getFullName(), "target2");
    
    // Children's full names should reflect new parent
    EXPECT_EQ(child1.getFullName(), "target2.child1");
    EXPECT_EQ(child2.getFullName(), "target2.child2");
}
