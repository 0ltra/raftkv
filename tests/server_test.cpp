// Socket-level tests: a real Server runs on its own thread, and the tests talk to it
// over real loopback TCP connections. Every client socket has a 5 second timeout, so
// a bug makes a test fail instead of hanging forever.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "commands/dispatcher.hpp"
#include "net/server.hpp"
#include "store/clock.hpp"
#include "store/store.hpp"
#include "test_support.hpp"

namespace {

using raftkv::Server;
using raftkv::ServerConfig;

using raftkv_test::TestClient;
using raftkv_test::encodeCommand;

// Everything a server needs, owned in one place.
struct Engine {
    raftkv::SystemClock clock;
    raftkv::Store store{clock};
    raftkv::CommandDispatcher dispatcher{store, clock};
};

class ServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        ServerConfig config;
        config.port = 0;  // any free port
        server = std::make_unique<Server>(config, engine.dispatcher);
        std::string error;
        ASSERT_TRUE(server->start(error)) << error;
        thread = std::thread([this] { server->run(); });
    }

    void TearDown() override {
        if (server) {
            server->stop();
        }
        if (thread.joinable()) {
            thread.join();
        }
    }

    std::uint16_t port() const { return server->port(); }

    Engine engine;
    std::unique_ptr<Server> server;
    std::thread thread;
};

std::string makeValue(std::size_t size) {
    std::string value(size, 'x');
    for (std::size_t i = 0; i < size; ++i) {
        value[i] = static_cast<char>('a' + (i % 26));
    }
    return value;
}

std::string bulkReply(const std::string& value) {
    return "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
}

}  // namespace

TEST_F(ServerTest, PingReturnsPong) {
    TestClient client(port());
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(client.send(encodeCommand({"PING"})));
    EXPECT_EQ(client.readExactly(7), "+PONG\r\n");
}

TEST_F(ServerTest, SetThenGetOverTheWire) {
    TestClient client(port());
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(client.send(encodeCommand({"SET", "greeting", "hello"})));
    EXPECT_EQ(client.readExactly(5), "+OK\r\n");
    ASSERT_TRUE(client.send(encodeCommand({"GET", "greeting"})));
    EXPECT_EQ(client.readExactly(11), "$5\r\nhello\r\n");
}

TEST_F(ServerTest, PipelinedCommandsInOneWriteAreAllAnswered) {
    TestClient client(port());
    ASSERT_TRUE(client.connected());
    const std::string batch = encodeCommand({"SET", "k", "v"}) + encodeCommand({"GET", "k"}) +
                              encodeCommand({"INCR", "n"}) + encodeCommand({"PING"});
    ASSERT_TRUE(client.send(batch));
    const std::string expected = "+OK\r\n$1\r\nv\r\n:1\r\n+PONG\r\n";
    EXPECT_EQ(client.readExactly(expected.size()), expected);
}

TEST_F(ServerTest, CommandSplitIntoSingleBytesIsReassembled) {
    TestClient client(port());
    ASSERT_TRUE(client.connected());
    const std::string request = encodeCommand({"SET", "foo", "bar"});
    for (const char byte : request) {
        ASSERT_TRUE(client.send(std::string_view(&byte, 1)));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));  // encourage separate TCP reads
    }
    EXPECT_EQ(client.readExactly(5), "+OK\r\n");
    ASSERT_TRUE(client.send(encodeCommand({"GET", "foo"})));
    EXPECT_EQ(client.readExactly(9), "$3\r\nbar\r\n");
}

TEST_F(ServerTest, TwoClientsShareOneStore) {
    TestClient writer(port());
    TestClient reader(port());
    ASSERT_TRUE(writer.connected());
    ASSERT_TRUE(reader.connected());
    ASSERT_TRUE(writer.send(encodeCommand({"SET", "shared", "yes"})));
    EXPECT_EQ(writer.readExactly(5), "+OK\r\n");
    ASSERT_TRUE(reader.send(encodeCommand({"GET", "shared"})));
    EXPECT_EQ(reader.readExactly(9), "$3\r\nyes\r\n");
}

TEST_F(ServerTest, FiftyClientsConnectedAtOnceAllGetDistinctCounterValues) {
    std::vector<std::unique_ptr<TestClient>> clients;
    for (int i = 0; i < 50; ++i) {
        clients.push_back(std::make_unique<TestClient>(port()));
        ASSERT_TRUE(clients.back()->connected());
    }
    for (const auto& client : clients) {
        ASSERT_TRUE(client->send(encodeCommand({"INCR", "counter"})));
    }

    std::vector<int> values;
    for (const auto& client : clients) {
        // Replies look like ":7\r\n" (4 bytes) or ":42\r\n" (5 bytes). Read up to the CRLF.
        std::string reply = client->readExactly(4);
        while (reply.size() < 2 || reply.substr(reply.size() - 2) != "\r\n") {
            const std::string more = client->readExactly(1);
            ASSERT_FALSE(more.empty());
            reply += more;
        }
        ASSERT_EQ(reply.front(), ':');
        values.push_back(std::stoi(reply.substr(1, reply.size() - 3)));
    }
    std::sort(values.begin(), values.end());
    for (int i = 0; i < 50; ++i) {
        EXPECT_EQ(values[static_cast<std::size_t>(i)], i + 1);
    }
}

TEST_F(ServerTest, OneMegabyteValueRoundTrips) {
    TestClient client(port());
    ASSERT_TRUE(client.connected());
    const std::string value = makeValue(1024 * 1024);
    ASSERT_TRUE(client.send(encodeCommand({"SET", "big", value})));
    EXPECT_EQ(client.readExactly(5), "+OK\r\n");
    ASSERT_TRUE(client.send(encodeCommand({"GET", "big"})));
    const std::string expected = bulkReply(value);
    EXPECT_TRUE(client.readExactly(expected.size()) == expected);
}

TEST_F(ServerTest, SlowReaderStillGetsEveryByteOfLargePipelinedReplies) {
    TestClient client(port());
    ASSERT_TRUE(client.connected());
    const std::string value = makeValue(1024 * 1024);
    ASSERT_TRUE(client.send(encodeCommand({"SET", "big", value})));
    ASSERT_EQ(client.readExactly(5), "+OK\r\n");

    // 20 replies of ~1 MiB each are queued before the client reads anything, so the
    // server's socket buffer fills up and it has to finish the writes later.
    constexpr int kRequests = 20;
    std::string requests;
    for (int i = 0; i < kRequests; ++i) {
        requests += encodeCommand({"GET", "big"});
    }
    ASSERT_TRUE(client.send(requests));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const std::string one = bulkReply(value);
    const std::string received = client.readExactly(one.size() * kRequests);
    ASSERT_EQ(received.size(), one.size() * kRequests);
    for (int i = 0; i < kRequests; ++i) {
        const std::size_t offset = one.size() * static_cast<std::size_t>(i);
        ASSERT_TRUE(received.compare(offset, one.size(), one) == 0) << "reply " << i << " differs";
    }
}

TEST_F(ServerTest, ProtocolErrorGetsAnErrorReplyThenTheConnectionCloses) {
    TestClient client(port());
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(client.send("PING\r\n"));  // inline commands are not supported
    EXPECT_EQ(client.readUntilEof(), "-ERR Protocol error: expected '*'\r\n");
    EXPECT_TRUE(client.sawEof());
}

TEST_F(ServerTest, OversizedBulkLengthIsRejectedImmediately) {
    TestClient client(port());
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(client.send("*1\r\n$67108865\r\n"));  // 64 MiB + 1, and no body follows
    EXPECT_EQ(client.readUntilEof(), "-ERR Protocol error: invalid bulk length\r\n");
    EXPECT_TRUE(client.sawEof());
}

TEST_F(ServerTest, CommandsBeforeAProtocolErrorStillRunAndLaterBytesAreIgnored) {
    TestClient client(port());
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(client.send(encodeCommand({"PING"}) + "garbage\r\n" + encodeCommand({"PING"})));
    EXPECT_EQ(client.readUntilEof(), "+PONG\r\n-ERR Protocol error: expected '*'\r\n");
}

TEST_F(ServerTest, ClientDisconnectingMidCommandDoesNotAffectOtherClients) {
    {
        TestClient quitter(port());
        ASSERT_TRUE(quitter.connected());
        ASSERT_TRUE(quitter.send("*2\r\n$3\r\nGE"));  // half a command, then the socket closes
    }
    TestClient client(port());
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(client.send(encodeCommand({"PING"})));
    EXPECT_EQ(client.readExactly(7), "+PONG\r\n");
}

TEST_F(ServerTest, HalfClosedClientStillReceivesItsReplies) {
    TestClient client(port());
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(client.send(encodeCommand({"SET", "k", "v"}) + encodeCommand({"GET", "k"})));
    client.shutdownWrite();  // like `printf ... | nc`: done sending, still wants the answers
    EXPECT_EQ(client.readUntilEof(), "+OK\r\n$1\r\nv\r\n");
    EXPECT_TRUE(client.sawEof());
}

TEST_F(ServerTest, HalfCloseWhileRepliesAreStillQueuedDoesNotLoseThem) {
    TestClient client(port());
    ASSERT_TRUE(client.connected());
    const std::string value = makeValue(1024 * 1024);
    ASSERT_TRUE(client.send(encodeCommand({"SET", "big", value})));
    ASSERT_EQ(client.readExactly(5), "+OK\r\n");

    // ~20 MiB of replies cannot fit in the socket buffers, so when the server sees the
    // client's end-of-input it still has most of them waiting to be sent.
    constexpr int kRequests = 20;
    std::string requests;
    for (int i = 0; i < kRequests; ++i) {
        requests += encodeCommand({"GET", "big"});
    }
    ASSERT_TRUE(client.send(requests));
    client.shutdownWrite();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const std::string one = bulkReply(value);
    const std::string received = client.readUntilEof();
    ASSERT_EQ(received.size(), one.size() * kRequests);
    for (int i = 0; i < kRequests; ++i) {
        const std::size_t offset = one.size() * static_cast<std::size_t>(i);
        ASSERT_TRUE(received.compare(offset, one.size(), one) == 0) << "reply " << i << " differs";
    }
    EXPECT_TRUE(client.sawEof());
}

TEST_F(ServerTest, StopWhileClientsAreConnectedEndsTheLoopAndClosesThem) {
    TestClient client(port());
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(client.send(encodeCommand({"PING"})));
    ASSERT_EQ(client.readExactly(7), "+PONG\r\n");

    server->stop();
    thread.join();  // would hang (and fail via the ctest timeout) if stop() did not wake the loop
    EXPECT_EQ(client.readUntilEof(), "");
    EXPECT_TRUE(client.sawEof());
}

TEST(ServerLifecycle, StartFailsWhenThePortIsAlreadyInUse) {
    Engine engine;
    ServerConfig config;
    config.port = 0;
    Server first(config, engine.dispatcher);
    std::string error;
    ASSERT_TRUE(first.start(error)) << error;

    ServerConfig sameport;
    sameport.port = first.port();
    Server second(sameport, engine.dispatcher);
    EXPECT_FALSE(second.start(error));
    EXPECT_NE(error.find("bind"), std::string::npos) << error;
}

TEST(ServerLifecycle, InvalidBindAddressIsRejected) {
    Engine engine;
    ServerConfig config;
    config.bindAddress = "not-an-address";
    Server server(config, engine.dispatcher);
    std::string error;
    EXPECT_FALSE(server.start(error));
    EXPECT_NE(error.find("invalid IPv4 bind address"), std::string::npos) << error;
}

TEST(ServerLifecycle, RunReturnsImmediatelyIfStopWasAlreadyCalled) {
    Engine engine;
    ServerConfig config;
    config.port = 0;
    Server server(config, engine.dispatcher);
    std::string error;
    ASSERT_TRUE(server.start(error)) << error;
    server.stop();
    server.run();  // must not block
    SUCCEED();
}

TEST(ServerLifecycle, TickCallbackRunsRepeatedly) {
    Engine engine;
    std::atomic<int> ticks{0};
    ServerConfig config;
    config.port = 0;
    config.tickIntervalMs = 10;
    config.onTick = [&ticks] { ++ticks; };
    Server server(config, engine.dispatcher);
    std::string error;
    ASSERT_TRUE(server.start(error)) << error;

    std::thread loop([&server] { server.run(); });
    for (int i = 0; i < 400 && ticks.load() < 3; ++i) {  // wait up to ~2 seconds
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    server.stop();
    loop.join();
    EXPECT_GE(ticks.load(), 3);
}
