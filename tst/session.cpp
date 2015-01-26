#include <type_traits>

#include <boost/thread.hpp>
#include <boost/thread/barrier.hpp>

#include <asio/read.hpp>
#include <asio/write.hpp>

#include <gtest/gtest.h>

#include <cocaine/dynamic.hpp>
#include <cocaine/idl/locator.hpp>
#include <cocaine/idl/node.hpp>
#include <cocaine/idl/streaming.hpp>

#include <cocaine/framework/connection.hpp>

#include "util/net.hpp"

using namespace cocaine::framework;
using namespace testing::util;

TEST(Connection, Constructor) {
    loop_t loop;
    auto conn = std::make_shared<connection_t>(loop);

    EXPECT_FALSE(conn->connected());

    static_assert(std::is_nothrow_constructible<connection_t, loop_t&>::value, "must be noexcept");
}

TEST(Connection, Connect) {
    // ===== Set Up Stage =====
    // We create a TCP server in separate thread and wait for incoming connection. After accepting
    // just close the socket.
    // Note, that some rendezvous point is required to be sure, that the server has been started
    // when the client is trying to connect.

    // An OS should select available port for us.
    const std::uint16_t port = testing::util::port();
    const io::ip::tcp::endpoint endpoint(io::ip::tcp::v4(), port);

    server_t server(port, [](io::ip::tcp::acceptor& acceptor, loop_t& loop){
        io::deadline_timer timer(loop);
        timer.expires_from_now(boost::posix_time::milliseconds(testing::util::TIMEOUT));
        timer.async_wait([&acceptor](const std::error_code& ec){
            EXPECT_EQ(io::error::operation_aborted, ec);
            acceptor.cancel();
        });

        io::ip::tcp::socket socket(loop);
        acceptor.async_accept(socket, [&timer](const std::error_code&){
            timer.cancel();
        });

        EXPECT_NO_THROW(loop.run());
    });

    client_t client;

    // ===== Test Stage =====
    auto conn = std::make_shared<connection_t>(client.loop());
    EXPECT_NO_THROW(conn->connect(endpoint).get());
    EXPECT_TRUE(conn->connected());
}

TEST(Connection, ConnectOnInvalidPort) {
    const io::ip::tcp::endpoint endpoint(io::ip::tcp::v4(), 0);

    client_t client;

    // ===== Test Stage =====
    auto conn = std::make_shared<connection_t>(client.loop());
    EXPECT_THROW(conn->connect(endpoint).get(), std::system_error);
    EXPECT_FALSE(conn->connected());
}

TEST(Connection, ConnectMultipleTimesOnDisconnectedService) {
    // ===== Set Up Stage =====
    const std::uint16_t port = testing::util::port();
    const io::ip::tcp::endpoint endpoint(io::ip::tcp::v4(), port);

    server_t server(port, [](io::ip::tcp::acceptor& acceptor, loop_t& loop){
        io::deadline_timer timer(loop);
        timer.expires_from_now(boost::posix_time::milliseconds(testing::util::TIMEOUT));
        timer.async_wait([&acceptor](const std::error_code& ec){
            EXPECT_EQ(io::error::operation_aborted, ec);
            acceptor.cancel();
        });

        io::ip::tcp::socket socket(loop);
        acceptor.async_accept(socket, [&timer](const std::error_code&){
            timer.cancel();
        });

        EXPECT_NO_THROW(loop.run());
    });

    client_t client;

    // ===== Test Stage =====
    auto conn = std::make_shared<connection_t>(client.loop());
    auto f1 = conn->connect(endpoint).then([&conn](future_t<void>& f){
        EXPECT_NO_THROW(f.get());
        EXPECT_TRUE(conn->connected());
    });

    auto f2 = conn->connect(endpoint).then([&conn](future_t<void>& f){
        EXPECT_NO_THROW(f.get());
        EXPECT_TRUE(conn->connected());
    });

    f1.get();
    f2.get();
}

TEST(Connection, ConnectOnConnectedService) {
    // ===== Set Up Stage =====
    const std::uint16_t port = testing::util::port();
    const io::ip::tcp::endpoint endpoint(io::ip::tcp::v4(), port);

    server_t server(port, [](io::ip::tcp::acceptor& acceptor, loop_t& loop){
        io::deadline_timer timer(loop);
        timer.expires_from_now(boost::posix_time::milliseconds(testing::util::TIMEOUT));
        timer.async_wait([&acceptor](const std::error_code& ec){
            EXPECT_EQ(io::error::operation_aborted, ec);
            acceptor.cancel();
        });

        io::ip::tcp::socket socket(loop);
        acceptor.async_accept(socket, [&timer](const std::error_code&){
            timer.cancel();
        });

        EXPECT_NO_THROW(loop.run());
    });

    client_t client;

    // ===== Test Stage =====
    auto conn = std::make_shared<connection_t>(client.loop());
    conn->connect(endpoint).get();

    EXPECT_NO_THROW(conn->connect(endpoint).get());
    EXPECT_TRUE(conn->connected());
}

TEST(Connection, RAIIOnConnect) {
    // ===== Set Up Stage =====
    const std::uint16_t port = testing::util::port();
    const io::ip::tcp::endpoint endpoint(io::ip::tcp::v4(), port);

    server_t server(port, [](io::ip::tcp::acceptor& acceptor, loop_t& loop){
        io::deadline_timer timer(loop);
        timer.expires_from_now(boost::posix_time::milliseconds(testing::util::TIMEOUT));
        timer.async_wait([&acceptor](const std::error_code& ec){
            EXPECT_EQ(io::error::operation_aborted, ec);
            acceptor.cancel();
        });

        io::ip::tcp::socket socket(loop);
        acceptor.async_accept(socket, [&timer](const std::error_code&){
            timer.cancel();
        });

        EXPECT_NO_THROW(loop.run());
    });

    client_t client;

    // ===== Test Stage =====
    future_t<void> future;
    {
        auto conn = std::make_shared<connection_t>(client.loop());
        future = std::move(conn->connect(endpoint));
    }
    EXPECT_NO_THROW(future.get());
}

TEST(Encoder, InvokeEvent) {
    cocaine::io::encoded<cocaine::io::locator::resolve> message(1, std::string("node"));

    const std::vector<std::uint8_t> expected = {{ 147, 1, 0, 145, 164, 110, 111, 100, 101 }};

    EXPECT_EQ(expected, std::vector<std::uint8_t>(message.data(), message.data() + 9));
}

TEST(Connection, InvokeSendsProperMessage) {
    // ===== Set Up Stage =====
    const std::uint16_t port = testing::util::port();
    const io::ip::tcp::endpoint endpoint(io::ip::tcp::v4(), port);

    server_t server(port, [](io::ip::tcp::acceptor& acceptor, loop_t& loop){
        io::deadline_timer timer(loop);
        timer.expires_from_now(boost::posix_time::milliseconds(testing::util::TIMEOUT));
        timer.async_wait([&acceptor](const std::error_code& ec){
            EXPECT_EQ(io::error::operation_aborted, ec);
            acceptor.cancel();
        });

        std::array<std::uint8_t, 32> actual;
        io::ip::tcp::socket socket(loop);
        acceptor.async_accept(socket, [&timer, &socket, &actual](const std::error_code&){
            timer.cancel();

            socket.async_read_some(io::buffer(actual), [&actual](const std::error_code& ec, size_t size){
                EXPECT_EQ(9, size);
                EXPECT_EQ(0, ec.value());

                std::array<std::uint8_t, 9> expected = {{ 147, 1, 0, 145, 164, 110, 111, 100, 101 }};
                for (int i = 0; i < 9; ++i) {
                    EXPECT_EQ(expected[i], actual[i]);
                }
            });
        });

        EXPECT_NO_THROW(loop.run());
    });

    client_t client;

    // ===== Test Stage =====
    auto conn = std::make_shared<connection_t>(client.loop());
    conn->connect(endpoint).get();
    EXPECT_TRUE(conn->connected());
    conn->invoke<cocaine::io::locator::resolve>(std::string("node"));
}

TEST(Connection, InvokeMultipleTimesSendsProperMessages) {
    // ===== Set Up Stage =====
    const std::uint16_t port = testing::util::port();
    const io::ip::tcp::endpoint endpoint(io::ip::tcp::v4(), port);

    server_t server(port, [](io::ip::tcp::acceptor& acceptor, loop_t& loop){
        io::deadline_timer timer(loop);
        timer.expires_from_now(boost::posix_time::milliseconds(testing::util::TIMEOUT));
        timer.async_wait([&acceptor](const std::error_code& ec){
            EXPECT_EQ(io::error::operation_aborted, ec);
            acceptor.cancel();
        });

        std::array<std::uint8_t, 18> actual;
        io::ip::tcp::socket socket(loop);
        acceptor.async_accept(socket, [&timer, &socket, &actual](const std::error_code&){
            timer.cancel();

            io::async_read(socket, io::buffer(actual), [&actual](const std::error_code& ec, size_t size){
                EXPECT_EQ(18, size);
                EXPECT_EQ(0, ec.value());

                std::array<std::uint8_t, 9> expected1 = {{ 147, 1, 0, 145, 164, 110, 111, 100, 101 }};
                std::array<std::uint8_t, 9> expected2 = {{ 147, 2, 0, 145, 164, 101,  99, 104, 111 }};
                for (int i = 0; i < 9; ++i) {
                    EXPECT_EQ(expected1[i], actual[i]);
                }
                for (int i = 0; i < 9; ++i) {
                    EXPECT_EQ(expected2[i], actual[9 + i]);
                }
            });
        });

        EXPECT_NO_THROW(loop.run());
    });

    client_t client;

    // ===== Test Stage =====
    auto conn = std::make_shared<connection_t>(client.loop());
    conn->connect(endpoint).get();
    conn->invoke<cocaine::io::locator::resolve>(std::string("node"));
    conn->invoke<cocaine::io::locator::resolve>(std::string("echo"));
}

TEST(Connection, DecodeIncomingMessage) {
    // ===== Set Up Stage =====
    const std::uint16_t port = testing::util::port();
    const io::ip::tcp::endpoint endpoint(io::ip::tcp::v4(), port);

    server_t server(port, [](io::ip::tcp::acceptor& acceptor, loop_t& loop){
        io::deadline_timer timer(loop);
        timer.expires_from_now(boost::posix_time::milliseconds(testing::util::TIMEOUT));
        timer.async_wait([&acceptor](const std::error_code& ec){
            EXPECT_EQ(io::error::operation_aborted, ec);
            acceptor.cancel();
        });

        // The following sequence is an encoded [1, 0, [['echo', 'http']]] struct, which is the
        // real response from Node service's 'list' request.
        // The caller service must properly unpack it and return to the user.
        const std::array<std::uint8_t, 15> buf = {
            { 147, 1, 0, 145, 146, 164, 101, 99, 104, 111, 164, 104, 116, 116, 112 }
        };
        io::ip::tcp::socket socket(loop);
        acceptor.async_accept(socket, [&timer, &socket, &buf](const std::error_code&){
            timer.cancel();

            // \note sometimes a race condition can occur - the socket should be read first.
            io::async_write(socket, io::buffer(buf), [](const std::error_code& ec, size_t size){
                EXPECT_EQ(0, ec.value());
                EXPECT_EQ(15, size);
            });
        });

        EXPECT_NO_THROW(loop.run());
    });

    client_t client;

    // ===== Test Stage =====
    auto conn = std::make_shared<connection_t>(client.loop());
    conn->connect(endpoint).get();
    std::shared_ptr<basic_receiver_t<cocaine::io::node::list>> rx;
    std::tie(std::ignore, rx) = conn->invoke<cocaine::io::node::list>();
    auto res = rx->recv().get();
    auto apps = boost::get<cocaine::dynamic_t>(res);
    EXPECT_EQ(cocaine::dynamic_t(std::vector<cocaine::dynamic_t>({ "echo", "http" })), apps);
}

TEST(Connection, InvokeWhileServerClosesConnection) {
    // ===== Set Up Stage =====
    const std::uint16_t port = testing::util::port();
    const io::ip::tcp::endpoint endpoint(io::ip::tcp::v4(), port);

    server_t server(port, [](io::ip::tcp::acceptor& acceptor, loop_t& loop){
        io::deadline_timer timer(loop);
        timer.expires_from_now(boost::posix_time::milliseconds(testing::util::TIMEOUT));
        timer.async_wait([&acceptor](const std::error_code& ec){
            EXPECT_EQ(io::error::operation_aborted, ec);
            acceptor.cancel();
        });

        std::array<std::uint8_t, 32> actual;
        io::ip::tcp::socket socket(loop);
        acceptor.async_accept(socket, [&timer, &socket, &actual](const std::error_code&){
            timer.cancel();

            socket.shutdown(asio::ip::tcp::socket::shutdown_both);
            socket.close();
        });

        EXPECT_NO_THROW(loop.run());
    });

    client_t client;

    // ===== Test Stage =====
    auto conn = std::make_shared<connection_t>(client.loop());
    conn->connect(endpoint).get();

    std::shared_ptr<basic_receiver_t<cocaine::io::locator::resolve>> rx;
    std::tie(std::ignore, rx) = conn->invoke<cocaine::io::locator::resolve>(std::string("node"));
    EXPECT_THROW(rx->recv().get(), std::system_error);
}

TEST(Connection, InvokeWhenServerClosesConnectionBeforeMessageNotWrittenYet) {
    // ===== Set Up Stage =====
    const std::uint16_t port = testing::util::port();
    const io::ip::tcp::endpoint endpoint(io::ip::tcp::v4(), port);

    boost::barrier barrier(2);
    server_t server(port, [&barrier](io::ip::tcp::acceptor& acceptor, loop_t& loop){
        io::deadline_timer timer(loop);
        timer.expires_from_now(boost::posix_time::milliseconds(testing::util::TIMEOUT));
        timer.async_wait([&acceptor](const std::error_code& ec){
            EXPECT_EQ(io::error::operation_aborted, ec);
            acceptor.cancel();
        });

        std::array<std::uint8_t, 32> actual;
        io::ip::tcp::socket socket(loop);
        acceptor.async_accept(socket, [&timer, &socket, &actual, &barrier](const std::error_code&){
            timer.cancel();

            socket.shutdown(asio::ip::tcp::socket::shutdown_both);
            socket.close();

            barrier.wait();
        });

        EXPECT_NO_THROW(loop.run());
    });

    client_t client;

    // ===== Test Stage =====
    auto conn = std::make_shared<connection_t>(client.loop());
    conn->connect(endpoint).get();

    // After the following statement the session's socket should be disconnected, which results in
    // write error.
    barrier.wait();

    std::shared_ptr<basic_receiver_t<cocaine::io::locator::resolve>> rx;
    std::tie(std::ignore, rx) = conn->invoke<cocaine::io::locator::resolve>(std::string("node"));
    EXPECT_THROW(rx->recv().get(), std::system_error);
}

TEST(Connection, InvokeWhileConnectionResetByPeer) {
    // ===== Set Up Stage =====
    const std::uint16_t port = testing::util::port();
    const io::ip::tcp::endpoint endpoint(io::ip::tcp::v4(), port);

    server_t server(port, [](io::ip::tcp::acceptor& acceptor, loop_t& loop){
        io::deadline_timer timer(loop);
        timer.expires_from_now(boost::posix_time::milliseconds(testing::util::TIMEOUT));
        timer.async_wait([&acceptor](const std::error_code& ec){
            EXPECT_EQ(io::error::operation_aborted, ec);
            acceptor.cancel();
        });

        std::array<std::uint8_t, 32> actual;
        io::ip::tcp::socket socket(loop);
        acceptor.async_accept(socket, [&timer, &socket, &actual](const std::error_code&){
            timer.cancel();

            socket.set_option(io::socket_base::linger(true, 0));
            socket.close();
        });

        EXPECT_NO_THROW(loop.run());
    });

    client_t client;

    // ===== Test Stage =====
    auto conn = std::make_shared<connection_t>(client.loop());
    conn->connect(endpoint).get();

    std::shared_ptr<basic_receiver_t<cocaine::io::locator::resolve>> rx;
    std::tie(std::ignore, rx) = conn->invoke<cocaine::io::locator::resolve>(std::string("node"));
    EXPECT_THROW(rx->recv().get(), std::system_error);
}

TEST(Connection, InvokeMultipleTimesWhileServerClosesConnection) {
    // ===== Set Up Stage =====
    const std::uint16_t port = testing::util::port();
    const io::ip::tcp::endpoint endpoint(io::ip::tcp::v4(), port);

    server_t server(port, [](io::ip::tcp::acceptor& acceptor, loop_t& loop){
        io::deadline_timer timer(loop);
        timer.expires_from_now(boost::posix_time::milliseconds(testing::util::TIMEOUT));
        timer.async_wait([&acceptor](const std::error_code& ec){
            EXPECT_EQ(io::error::operation_aborted, ec);
            acceptor.cancel();
        });

        std::array<std::uint8_t, 32> actual;
        io::ip::tcp::socket socket(loop);
        acceptor.async_accept(socket, [&timer, &socket, &actual](const std::error_code&){
            timer.cancel();

            socket.shutdown(asio::ip::tcp::socket::shutdown_both);
            socket.close();
        });

        EXPECT_NO_THROW(loop.run());
    });

    client_t client;

    // ===== Test Stage =====
    auto conn = std::make_shared<connection_t>(client.loop());
    conn->connect(endpoint).get();

    std::shared_ptr<basic_receiver_t<cocaine::io::locator::resolve>> rx1, rx2;
    std::tie(std::ignore, rx1) = conn->invoke<cocaine::io::locator::resolve>(std::string("node"));
    std::tie(std::ignore, rx2) = conn->invoke<cocaine::io::locator::resolve>(std::string("node"));

    EXPECT_THROW(rx1->recv().get(), std::system_error);
    EXPECT_THROW(rx2->recv().get(), std::system_error);
}

TEST(Connection, SendSendsProperMessage) {
    // ===== Set Up Stage =====
    const std::uint16_t port = testing::util::port();
    const io::ip::tcp::endpoint endpoint(io::ip::tcp::v4(), port);

    server_t server(port, [](io::ip::tcp::acceptor& acceptor, loop_t& loop){
        io::deadline_timer timer(loop);
        timer.expires_from_now(boost::posix_time::milliseconds(testing::util::TIMEOUT));
        timer.async_wait([&acceptor](const std::error_code& ec){
            EXPECT_EQ(io::error::operation_aborted, ec);
            acceptor.cancel();
        });

        std::array<std::uint8_t, 24> actual;
        io::ip::tcp::socket socket(loop);
        acceptor.async_accept(socket, [&timer, &socket, &actual](const std::error_code&){
            timer.cancel();
            timer.expires_from_now(boost::posix_time::milliseconds(testing::util::TIMEOUT));
            timer.async_wait([&socket](const std::error_code& ec){
                EXPECT_EQ(io::error::operation_aborted, ec);
                socket.cancel();
            });

            io::async_read(socket, io::buffer(actual), [&timer, &actual](const std::error_code& ec, size_t size){
                timer.cancel();
                EXPECT_EQ(24, size);
                EXPECT_EQ(0, ec.value());

                std::array<std::uint8_t, 9> invoke = {{ 147, 1, 0, 145, 164, 112, 105, 110, 103 }};
                std::array<std::uint8_t, 15> send =  {{ 147, 1, 0, 145, 170, 108, 101,  32, 109, 101, 115, 115, 97, 103, 101 }};
                for (int i = 0; i < 9; ++i) {
                    EXPECT_EQ(invoke[i], actual[i]);
                }

                for (int i = 0; i < 9; ++i) {
                    EXPECT_EQ(send[i], actual[9 + i]);
                }
            });
        });

        EXPECT_NO_THROW(loop.run());
    });

    client_t client;

    // ===== Test Stage =====
    auto conn = std::make_shared<connection_t>(client.loop());
    conn->connect(endpoint).get();
    std::shared_ptr<basic_sender_t> tx;
    std::tie(tx, std::ignore) = conn->invoke<cocaine::io::app::enqueue>(std::string("ping"));
    typedef cocaine::io::protocol<
        cocaine::io::event_traits<
            cocaine::io::app::enqueue
        >::dispatch_type
    >::scope protocol;
    tx->send<protocol::chunk>(std::string("le message"));
}

TEST(Connection, SendOnClosedSocket) {
    // ===== Set Up Stage =====
    boost::barrier barrier(2);
    const std::uint16_t port = testing::util::port();
    const io::ip::tcp::endpoint endpoint(io::ip::tcp::v4(), port);

    server_t server(port, [&barrier](io::ip::tcp::acceptor& acceptor, loop_t& loop){
        io::deadline_timer timer(loop);
        timer.expires_from_now(boost::posix_time::milliseconds(testing::util::TIMEOUT));
        timer.async_wait([&acceptor](const std::error_code& ec){
            EXPECT_EQ(io::error::operation_aborted, ec);
            acceptor.cancel();
        });

        std::array<std::uint8_t, 9> actual;
        io::ip::tcp::socket socket(loop);
        acceptor.async_accept(socket, [&timer, &socket, &actual, &barrier](const std::error_code&){
            timer.cancel();
            timer.expires_from_now(boost::posix_time::milliseconds(testing::util::TIMEOUT));
            timer.async_wait([&socket](const std::error_code& ec){
                EXPECT_EQ(io::error::operation_aborted, ec);
                socket.cancel();
            });

            io::async_read(socket, io::buffer(actual), [&timer, &socket, &actual, &barrier](const std::error_code& ec, size_t size){
                timer.cancel();
                EXPECT_EQ(9, size);
                EXPECT_EQ(0, ec.value());

                std::array<std::uint8_t, 9> invoke = {{ 147, 1, 0, 145, 164, 112, 105, 110, 103 }};
                for (int i = 0; i < 9; ++i) {
                    EXPECT_EQ(invoke[i], actual[i]);
                }

                socket.shutdown(asio::ip::tcp::socket::shutdown_both);

                barrier.wait();
            });
        });

        EXPECT_NO_THROW(loop.run());
    });

    client_t client;

    // ===== Test Stage =====
    auto conn = std::make_shared<connection_t>(client.loop());
    conn->connect(endpoint).get();
    std::shared_ptr<basic_sender_t> tx;
    std::shared_ptr<basic_receiver_t<cocaine::io::app::enqueue>> rx;
    std::tie(tx, rx) = conn->invoke<cocaine::io::app::enqueue>(std::string("ping"));

    barrier.wait();

    typedef cocaine::io::protocol<
        cocaine::io::event_traits<
            cocaine::io::app::enqueue
        >::dispatch_type
    >::scope protocol;
    tx->send<protocol::chunk>(std::string("le message"));

    EXPECT_THROW(rx->recv().get(), std::system_error);
}

TEST(Connection, SilentlyDropOrphanMessageButContinueToListen) {
    // ===== Set Up Stage =====
    const std::uint16_t port = testing::util::port();
    const io::ip::tcp::endpoint endpoint(io::ip::tcp::v4(), port);

    server_t server(port, [](io::ip::tcp::acceptor& acceptor, loop_t& loop){
        io::deadline_timer timer(loop);
        timer.expires_from_now(boost::posix_time::milliseconds(testing::util::TIMEOUT));
        timer.async_wait([&acceptor](const std::error_code& ec){
            EXPECT_EQ(io::error::operation_aborted, ec);
            acceptor.cancel();
        });

        std::array<io::const_buffer, 2> bufs = {
            io::buffer(std::array<std::uint8_t, 15> {
                { 147, 2, 0, 145, 146, 164, 101, 99, 104, 111, 164, 104, 116, 116, 112 }
            }),
            io::buffer(std::array<std::uint8_t, 15> {
                { 147, 1, 0, 145, 146, 164, 101, 99, 104, 111, 164, 104, 116, 116, 112 }
            })
        };
        io::ip::tcp::socket socket(loop);
        acceptor.async_accept(socket, [&timer, &socket, &bufs](const std::error_code&){
            timer.cancel();

            // \note sometimes a race condition can occur - the socket should be read first.
            io::async_write(socket, bufs, [](const std::error_code& ec, size_t size){
                EXPECT_EQ(0, ec.value());
                EXPECT_EQ(30, size);
            });
        });

        EXPECT_NO_THROW(loop.run());
    });

    client_t client;

    // ===== Test Stage =====
    auto conn = std::make_shared<connection_t>(client.loop());
    conn->connect(endpoint).get();
    std::shared_ptr<basic_receiver_t<cocaine::io::node::list>> rx;
    std::tie(std::ignore, rx) = conn->invoke<cocaine::io::node::list>();
    auto res = rx->recv().get();
    auto apps = boost::get<cocaine::dynamic_t>(res);
    EXPECT_EQ(cocaine::dynamic_t(std::vector<cocaine::dynamic_t>({ "echo", "http" })), apps);
}

// Usage:
//  Connection - The base class, does almost all the job:
//    std::tie(tx, rx) = conn->invoke<E>();
//    std::tie(rx, res) = rx.recv();
//    tx = tx.send<C>(); // May chain: tx.send<C1>().send<C2>();
//
//  Service - Proxy with auto resolving and RAII style:
//    std::tie(tx, rx) = node.invoke<E>(); // Nonblock.
//    tx = tx.send<C>(...);                // Nonblock, returns the following Sender<...>. May throw.
//    std::tie(rx, res) = rx.recv();       // Returns tuple with the following Receiver<...> and Future<V>. No throw.
//
//    service.detach(); // Now the service's destrucor won't block.
//
//  Wrappers:
//    Primitive + void Dispatch [almost all services]:
//      conn->invoke<M>(args).get() -> value | error
//    Sequenced + void Dispatch [not implemented]:
//      conn->invoke<M>(args).get() -> Receiver<T, U, ...>
//    Streaming: conn->invoke<M>(args) -> (tx, rx).
//      rx.recv() -> T | E | C where T == dispatch_type, E - error type, C - choke.
//      rx.recv<T>() -> T | throw exception.
//      May throw error (network or protocol) or be exhaused (throw exception after E | C).

/// Test conn ctor.
/// Test conn connect.
/// Test conn connect failed.
/// Test conn async connect multiple times.
/// Test conn async connect multiple times when already connected.
/// Test conn invoke.
/// Test conn invoke multiple times - channel id must be increased.
/// Test conn invoke - network error on write - notify client.
/// Test conn invoke and recv - ok.
// Test conn invoke and recv invalid message, then valid.
/// Test conn invoke - network error on read - notify client.
/// Test conn invoke - network error on read - notify all invokers.
/// Test conn invoke and send - ok.
/// Test conn invoke and send - error - notify client.
// Test conn invoke and send multiple times - error - notify only once.

// Test service ctor.
// Test service move ctor.
// Test service dtor (waits).
// Test service dtor after detach.
// Test service invoke - server received proper message.
// Test service invoke - server responds and the client receives and decodes proper message.
// Test service invoke - server responds and the client receives improper message.
// Test service invoke - server responds and the client receives an orphan message.
// Test service send.
// Test service send traverse.
// Test service send failed.
// Test service recv.
// Test service recv traverse.
// Test service recv failed.
// Test service connect.
// Test service connect failed.
// Test service async connect multiple times.
// Test service async connect multiple times when already connected.
// Test service reconnect on invalid connect.
// Test service timeout on connect.
// Test service timeout on invoke.
// Test service timeout on send(?).
// Test service timeout on recv.
// \note Strands will possibly be required.
// \note On worker side serialize all callbacks through a single thread (may be configured).
// Primitive protocol wrapper (value/error).
// \note Exception type guarantee.
// Service manager with thread pool (io loop pool, actually).
// GetService from SM.
// GetService async from SM.
// SM dtor.
// \note Internal thread safety.
// Test return version number expected (through T).
// Test error version mismatch.

// Test locator
// Test node
// Test storage
// Test echo.

// Test sync usage (background with thread).
// Test async usage (with single thread, but using nonblocking methods).