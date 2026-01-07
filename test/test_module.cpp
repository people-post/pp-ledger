#include "Module.h"
#include "Server.h"
#include "Client.h"
#include "BlockChain.h"
#include "BlockDir.h"
#include "BlockFile.h"
#include "Ledger.h"
#include <gtest/gtest.h>

TEST(ModuleTest, ServerHasCorrectLoggerName) {
    pp::Server server;
    EXPECT_EQ(server.getLoggerName(), "server");
}

TEST(ModuleTest, ClientHasCorrectLoggerName) {
    pp::Client client;
    EXPECT_EQ(client.getLoggerName(), "client");
}

TEST(ModuleTest, BlockChainHasCorrectLoggerName) {
    pp::BlockChain blockchain(2);
    EXPECT_EQ(blockchain.getLoggerName(), "blockchain");
}

TEST(ModuleTest, LedgerHasCorrectLoggerName) {
    pp::Ledger ledger(2);
    EXPECT_EQ(ledger.getLoggerName(), "ledger");
}

TEST(ModuleTest, BlockFileHasCorrectLoggerName) {
    pp::BlockFile blockFile;
    EXPECT_EQ(blockFile.getLoggerName(), "blockfile");
}

TEST(ModuleTest, BlockDirHasCorrectLoggerName) {
    pp::BlockDir blockDir;
    EXPECT_EQ(blockDir.getLoggerName(), "blockdir");
}

TEST(ModuleTest, LoggerWorksForModules) {
    pp::Server server;
    pp::Client client;
    pp::BlockChain blockchain(2);
    pp::Ledger ledger(2);
    
    EXPECT_NO_THROW({
        server.log().info << "Message from Server module";
        client.log().info << "Message from Client module";
        blockchain.log().info << "Message from BlockChain module";
        ledger.log().info << "Message from Ledger module";
    });
}

TEST(ModuleTest, LoggerRedirect) {
    pp::Server server;
    
    EXPECT_NO_THROW(server.log().info << "Server message before redirect");
    
    server.redirectLogger("main");
    EXPECT_NO_THROW(server.log().info << "Server message after redirect");
    
    server.clearLoggerRedirect();
    EXPECT_NO_THROW(server.log().info << "Server message after clearing redirect");
}

TEST(ModuleTest, MultipleModulesWithDifferentRedirects) {
    pp::Client client;
    pp::BlockChain blockchain(2);
    
    client.redirectLogger("system");
    blockchain.redirectLogger("system");
    
    EXPECT_NO_THROW({
        client.log().info << "Client message going to system logger";
        blockchain.log().info << "BlockChain message going to system logger";
    });
}
