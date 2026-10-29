// The rule being tested: a reply is never sent before the commands behind it were committed.

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "commands/handler.hpp"
#include "net/server.hpp"
#include "test_support.hpp"

namespace {

using raftkv_test::encodeCommand;
using raftkv_test::TestClient;

// Replies +OK to everything and lets the test control what commit() does.
class FakeHandler final : public raftkv::CommandHandler {
public:
    void execute(const raftkv::resp::Command&, std::string& out) override {
        ++executed;
        out += "+OK\r\n";
    }

    bool commit() override {
        std::unique_lock<std::mutex> lock(mutex);
        ++commits;
        cv.wait(lock, [this] { return !blockCommits; });
        return commitResult;
    }

    void blockUntilReleased() {
        std::lock_guard<std::mutex> lock(mutex);
        blockCommits = true;
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            blockCommits = false;
        }
        cv.notify_all();
    }

    void setCommitResult(bool value) {
        std::lock_guard<std::mutex> lock(mutex);
        commitResult = value;
    }

    std::atomic<int> executed{0};
    std::atomic<int> commits{0};

private:
    std::mutex mutex;
    std::condition_variable cv;
    bool blockCommits = false;
    bool commitResult = true;
};

class ServerCommitTest : public ::testing::Test {
protected:
    void SetUp() override {
        raftkv::ServerConfig config;
        config.port = 0;
        server = std::make_unique<raftkv::Server>(config, handler);
        std::string error;
        ASSERT_TRUE(server->start(error)) << error;
        thread = std::thread([this] { server->run(); });
    }

    void TearDown() override {
        handler.release();  // never leave the loop stuck inside commit()
        server->stop();
        if (thread.joinable()) {
            thread.join();
        }
    }

    FakeHandler handler;
    std::unique_ptr<raftkv::Server> server;
    std::thread thread;
};

}  // namespace

TEST_F(ServerCommitTest, NoReplyIsSentWhileTheCommitIsStillInProgress) {
    handler.blockUntilReleased();
    TestClient client(server->port());
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(client.send(encodeCommand({"SET", "k", "v"})));

    // The command has run, and the server is waiting inside commit(). No bytes may arrive yet.
    EXPECT_FALSE(client.waitReadable(300));
    EXPECT_EQ(handler.executed.load(), 1);

    handler.release();
    EXPECT_EQ(client.readExactly(5), "+OK\r\n");
}

TEST_F(ServerCommitTest, AReadBatchOfPipelinedCommandsIsCommittedOnce) {
    TestClient client(server->port());
    ASSERT_TRUE(client.connected());
    std::string batch;
    for (int i = 0; i < 10; ++i) {
        batch += encodeCommand({"SET", "k", "v"});
    }
    ASSERT_TRUE(client.send(batch));
    EXPECT_EQ(client.readExactly(50), std::string("+OK\r\n").append("+OK\r\n+OK\r\n+OK\r\n+OK\r\n+OK\r\n+OK\r\n+OK\r\n+OK\r\n+OK\r\n"));
    EXPECT_EQ(handler.executed.load(), 10);
    EXPECT_EQ(handler.commits.load(), 1);
}

TEST_F(ServerCommitTest, AFailedCommitDropsTheClientWithoutAnyReply) {
    handler.setCommitResult(false);
    TestClient client(server->port());
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(client.send(encodeCommand({"SET", "k", "v"})));
    EXPECT_EQ(client.readUntilEof(), "");
    EXPECT_TRUE(client.sawEof());
}

TEST_F(ServerCommitTest, TheServerKeepsServingOthersAfterAFailedCommit) {
    handler.setCommitResult(false);
    {
        TestClient doomed(server->port());
        ASSERT_TRUE(doomed.connected());
        ASSERT_TRUE(doomed.send(encodeCommand({"SET", "k", "v"})));
        EXPECT_EQ(doomed.readUntilEof(), "");
    }
    handler.setCommitResult(true);
    TestClient healthy(server->port());
    ASSERT_TRUE(healthy.connected());
    ASSERT_TRUE(healthy.send(encodeCommand({"SET", "k", "v"})));
    EXPECT_EQ(healthy.readExactly(5), "+OK\r\n");
}
