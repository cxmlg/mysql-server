/*
  Copyright (c) 2017, 2020, Oracle and/or its affiliates.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <chrono>
#include <cstring>
#include <initializer_list>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#else
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "config_builder.h"
#include "mysql/harness/net_ts/impl/resolver.h"
#include "mysql/harness/net_ts/impl/socket.h"
#include "mysql/harness/stdx/expected.h"
#include "mysql_session.h"
#include "router_component_test.h"
#include "router_test_helpers.h"
#include "socket_operations.h"
#include "tcp_port_pool.h"

using namespace std::chrono_literals;

using mysqlrouter::MySQLSession;

class RouterRoutingTest : public RouterComponentTest {
 protected:
  TcpPortPool port_pool_;
};

TEST_F(RouterRoutingTest, RoutingOk) {
  const auto server_port = port_pool_.get_next_available();
  const auto router_port = port_pool_.get_next_available();

  // use the json file that adds additional rows to the metadata to increase the
  // packet size to +10MB to verify routing of the big packets
  const std::string json_stmts = get_data_dir().join("bootstrap_gr.js").str();
  TempDirectory bootstrap_dir;

  // launch the server mock for bootstrapping
  launch_mysql_server_mock(
      json_stmts, server_port,
      false /*expecting huge data, can't print on the console*/);

  const std::string routing_section =
      "[routing:basic]\n"
      "bind_port = " +
      std::to_string(router_port) +
      "\n"
      "mode = read-write\n"
      "destinations = 127.0.0.1:" +
      std::to_string(server_port) + "\n";

  TempDirectory conf_dir("conf");
  std::string conf_file = create_config_file(conf_dir.name(), routing_section);

  // launch the router with simple static routing configuration
  /*auto &router_static =*/launch_router({"-c", conf_file});

  // launch another router to do the bootstrap connecting to the mock server
  // via first router instance
  auto &router_bootstrapping = launch_router(
      {
          "--bootstrap=localhost:" + std::to_string(router_port),
          "--report-host",
          "dont.query.dns",
          "-d",
          bootstrap_dir.name(),
      },
      EXIT_SUCCESS, true, false, -1s);

  router_bootstrapping.register_response(
      "Please enter MySQL password for root: ", "fake-pass\n");

  ASSERT_NO_FATAL_FAILURE(check_exit_code(router_bootstrapping, EXIT_SUCCESS));

  ASSERT_TRUE(router_bootstrapping.expect_output(
      "MySQL Router configured for the InnoDB Cluster 'mycluster'"));
}

TEST_F(RouterRoutingTest, RoutingTooManyConnections) {
  const auto server_port = port_pool_.get_next_available();
  const auto router_port = port_pool_.get_next_available();

  // doesn't really matter which file we use here, we are not going to do any
  // queries
  const std::string json_stmts = get_data_dir().join("bootstrap_gr.js").str();

  // launch the server mock
  launch_mysql_server_mock(json_stmts, server_port, false);

  // create a config with routing that has max_connections == 2
  const std::string routing_section =
      "[routing:basic]\n"
      "bind_port = " +
      std::to_string(router_port) +
      "\n"
      "mode = read-write\n"
      "max_connections = 2\n"
      "destinations = 127.0.0.1:" +
      std::to_string(server_port) + "\n";

  TempDirectory conf_dir("conf");
  std::string conf_file = create_config_file(conf_dir.name(), routing_section);

  // launch the router with the created configuration
  launch_router({"-c", conf_file});

  // try to create 3 connections, the third should fail
  // because of the max_connections limit being exceeded
  mysqlrouter::MySQLSession client1, client2, client3;
  EXPECT_NO_THROW(
      client1.connect("127.0.0.1", router_port, "root", "fake-pass", "", ""));
  EXPECT_NO_THROW(
      client2.connect("127.0.0.1", router_port, "root", "fake-pass", "", ""));
  ASSERT_THROW_LIKE(
      client3.connect("127.0.0.1", router_port, "root", "fake-pass", "", ""),
      std::runtime_error, "Too many connections to MySQL Router (1040)");
}

template <class T>
::testing::AssertionResult ThrowsExceptionWith(std::function<void()> callable,
                                               const char *expected_text) {
  try {
    callable();
    return ::testing::AssertionFailure()
           << "Expected exception to throw, but it didn't";
  } catch (const T &e) {
    if (nullptr == ::strstr(e.what(), expected_text)) {
      return ::testing::AssertionFailure()
             << "Expected exception-text to contain: " << expected_text
             << ". Actual: " << e.what();
    }

    return ::testing::AssertionSuccess();
  } catch (...) {
    // as T may be std::exception we can't use it as default case and need to do
    // this extra round
    try {
      throw;
    } catch (const std::exception &e) {
      return ::testing::AssertionFailure()
             << "Expected exception of type " << typeid(T).name()
             << ". Actual: " << typeid(e).name();
    } catch (...) {
      return ::testing::AssertionFailure()
             << "Expected exception of type " << typeid(T).name()
             << ". Actual: non-std exception";
    }
  }
}

#ifndef _WIN32  // named sockets are not supported on Windows;
                // on Unix, they're implemented using Unix sockets
TEST_F(RouterRoutingTest, named_socket_has_right_permissions) {
  /**
   * @test Verify that unix socket has the required file permissions so that it
   *       can be connected to by all users. According to man 7 unix, only r+w
   *       permissions are required, but Server sets x as well, so we do the
   * same.
   */

  // get config dir (we will also stuff our unix socket file there)
  TempDirectory bootstrap_dir;

  // launch Router with unix socket
  const std::string socket_file = bootstrap_dir.name() + "/sockfile";
  const std::string routing_section =
      "[routing:basic]\n"
      "socket = " +
      socket_file +
      "\n"
      "mode = read-write\n"
      "destinations = 127.0.0.1:1234\n";  // port can be bogus
  TempDirectory conf_dir("conf");
  const std::string conf_file =
      create_config_file(conf_dir.name(), routing_section);
  launch_router({"-c", conf_file});

  // loop until socket file appears and has correct permissions
  auto wait_for_correct_perms = [&socket_file](int timeout_ms) {
    const mode_t expected_mode = S_IFSOCK | S_IRUSR | S_IWUSR | S_IXUSR |
                                 S_IRGRP | S_IWGRP | S_IXGRP | S_IROTH |
                                 S_IWOTH | S_IXOTH;
    while (timeout_ms > 0) {
      struct stat info;

      memset(&info, 0, sizeof(info));
      stat(socket_file.c_str(),
           &info);  // silently ignore error when file doesn't exist yet

      if (info.st_mode == expected_mode) return true;

      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      timeout_ms -= 10;
    }

    return false;
  };

  EXPECT_THAT(wait_for_correct_perms(5000), testing::Eq(true));
}
#endif

TEST_F(RouterRoutingTest, RoutingMaxConnectErrors) {
  const auto server_port = port_pool_.get_next_available();
  const auto router_port = port_pool_.get_next_available();

  // json file does not actually matter in this test as we are not going to
  const std::string json_stmts = get_data_dir().join("bootstrap_gr.js").str();
  TempDirectory bootstrap_dir;

  // launch the server mock for bootstrapping
  launch_mysql_server_mock(
      json_stmts, server_port,
      false /*expecting huge data, can't print on the console*/);

  const std::string routing_section =
      "[routing:basic]\n"
      "bind_port = " +
      std::to_string(router_port) +
      "\n"
      "mode = read-write\n"
      "destinations = 127.0.0.1:" +
      std::to_string(server_port) +
      "\n"
      "max_connect_errors = 1\n";

  TempDirectory conf_dir("conf");
  std::string conf_file = create_config_file(conf_dir.name(), routing_section);

  // launch the router
  auto &router = launch_router({"-c", conf_file});

  // wait for router to begin accepting the connections
  // NOTE: this should cause connection/disconnection which
  //       should be treated as connection error and increment
  //       connection errors counter.  This test relies on that.
  ASSERT_NO_FATAL_FAILURE(check_port_ready(router, router_port));

  // wait until blocking client host info appears in the log
  bool res =
      find_in_file(get_logging_dir().str() + "/mysqlrouter.log",
                   [](const std::string &line) -> bool {
                     return line.find("blocking client host") != line.npos;
                   });

  ASSERT_TRUE(res) << "Did not found expected entry in log file";

  // for the next connection attempt we should get an error as the
  // max_connect_errors was exceeded
  MySQLSession client;
  EXPECT_THROW_LIKE(
      client.connect("127.0.0.1", router_port, "root", "fake-pass", "", ""),
      std::exception, "Too many connection errors");
}

static stdx::expected<mysql_harness::socket_t, std::error_code> connect_to_host(
    uint16_t port) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  const auto addrinfo_res = net::impl::resolver::getaddrinfo(
      "127.0.0.1", std::to_string(port).c_str(), &hints);
  if (!addrinfo_res)
    throw std::system_error(addrinfo_res.error(), "getaddrinfo() failed: ");

  const auto *ainfo = addrinfo_res.value().get();

  const auto socket_res = net::impl::socket::socket(
      ainfo->ai_family, ainfo->ai_socktype, ainfo->ai_protocol);
  if (!socket_res) return socket_res;

  const auto connect_res = net::impl::socket::connect(
      socket_res.value(), ainfo->ai_addr, ainfo->ai_addrlen);
  if (!connect_res) {
    return stdx::make_unexpected(connect_res.error());
  }

  // return the fd
  return socket_res.value();
}

static void read_until_error(int sock) {
  std::array<char, 1024> buf;
  while (true) {
    const auto read_res = net::impl::socket::read(sock, buf.data(), buf.size());
    if (!read_res || read_res.value() == 0) return;
  }
}

static void make_bad_connection(uint16_t port) {
  // TCP-level connection phase
  auto connection_res = connect_to_host(port);
  ASSERT_TRUE(connection_res);

  auto sock = connection_res.value();

  // MySQL protocol handshake phase
  // To simplify code, instead of alternating between reading and writing
  // protocol packets, we write a lot of garbage upfront, and then read whatever
  // Router sends back. Router will read what we wrote in chunks, inbetween its
  // writes, thinking they're replies to its handshake packets. Eventually it
  // will finish the handshake with error and disconnect.
  std::vector<char> bogus_data(1024, 0);
  const auto write_res =
      net::impl::socket::write(sock, bogus_data.data(), bogus_data.size());
  if (!write_res) throw std::system_error(write_res.error(), "write() failed");
  read_until_error(sock);  // error triggered by Router disconnecting

  net::impl::socket::close(sock);
}

/**
 * @test
 * This test Verifies that:
 *   1. Router will block a misbehaving client after consecutive
 *      <max_connect_errors> connection errors
 *   2. Router will reset its connection error counter if client establishes a
 *      successful connection before <max_connect_errors> threshold is hit
 */
TEST_F(RouterRoutingTest, error_counters) {
  const uint16_t server_port = port_pool_.get_next_available();
  const uint16_t router_port = port_pool_.get_next_available();

  // doesn't really matter which file we use here, we are not going to do any
  // queries
  const std::string json_stmts = get_data_dir().join("bootstrap_gr.js").str();

  // launch the server mock
  launch_mysql_server_mock(json_stmts, server_port, false);

  // create a config with max_connect_errors == 3
  const std::string routing_section =
      "[routing:basic]\n"
      "bind_port = " +
      std::to_string(router_port) +
      "\n"
      "mode = read-write\n"
      "max_connect_errors = 3\n"
      "destinations = 127.0.0.1:" +
      std::to_string(server_port) + "\n";
  TempDirectory conf_dir("conf");
  std::string conf_file = create_config_file(conf_dir.name(), routing_section);

  // launch the router with the created configuration
  launch_router({"-c", conf_file});

  SCOPED_TRACE(
      "// make good and bad connections (connect() + 1024 0-bytes) to check "
      "blocked client gets reset");
  // we loop just for good measure, to additionally test that this behaviour is
  // repeatable
  for (int i = 0; i < 5; i++) {
    // good connection, followed by 2 bad ones. Good one should reset the error
    // counter
    mysqlrouter::MySQLSession client;
    try {
      client.connect("127.0.0.1", router_port, "root", "fake-pass", "", "");
    } catch (const std::exception &e) {
      FAIL() << e.what();
    }
    make_bad_connection(router_port);
    make_bad_connection(router_port);
  }

  SCOPED_TRACE("// make bad connection to trigger blocked client");
  // make a 3rd consecutive bad connection - it should cause Router to start
  // blocking us
  make_bad_connection(router_port);

  // we loop just for good measure, to additionally test that this behaviour is
  // repeatable
  for (int i = 0; i < 5; i++) {
    // now trying to make a good connection should fail due to blockage
    mysqlrouter::MySQLSession client;
    SCOPED_TRACE("// make connection to check if we are really blocked");
    try {
      client.connect("127.0.0.1", router_port, "root", "fake-pass", "", "");

      FAIL() << "connect should be blocked, but isn't";
    } catch (const std::exception &e) {
      EXPECT_THAT(e.what(), ::testing::HasSubstr("Too many connection errors"));
    }
  }
}

struct RoutingConfigParam {
  const char *test_name;

  std::vector<std::pair<std::string, std::string>> routing_opts;

  std::function<void(const std::vector<std::string> &)> checker;
};

class RoutingConfigTest
    : public RouterComponentTest,
      public ::testing::WithParamInterface<RoutingConfigParam> {};

TEST_P(RoutingConfigTest, check) {
  mysql_harness::ConfigBuilder builder;

  const std::string routing_section =
      builder.build_section("routing", GetParam().routing_opts);

  TempDirectory conf_dir("conf");
  std::string conf_file = create_config_file(conf_dir.name(), routing_section);

  // launch the router with the created configuration
  auto &router =
      launch_router({"-c", conf_file}, EXIT_FAILURE, true, false, -1ms);
  router.wait_for_exit();

  std::vector<std::string> lines;
  {
    std::istringstream ss{router.get_full_logfile()};

    std::string line;
    while (std::getline(ss, line, '\n')) {
      lines.push_back(std::move(line));
    }
  }

  GetParam().checker(lines);
}

const RoutingConfigParam routing_config_param[] = {
    {"no_destination",
     {
         {"destinations", "127.0.0.1:3306"},
         {"routing_strategy", "first-available"},
     },
     [](const std::vector<std::string> &lines) {
       EXPECT_THAT(lines, ::testing::Contains(::testing::HasSubstr(
                              "either bind_address or socket option needs to "
                              "be supplied, or both")));
     }},
    {"missing_port_in_bind_address",
     {
         {"destinations", "127.0.0.1:3306"},
         {"routing_strategy", "first-available"},
         {"bind_address", "127.0.0.1"},
     },
     [](const std::vector<std::string> &lines) {
       EXPECT_THAT(lines, ::testing::Contains(::testing::HasSubstr(
                              "either bind_address or socket option needs to "
                              "be supplied, or both")));
     }},
    {"invalid_port_in_bind_address",
     {
         {"destinations", "127.0.0.1:3306"},
         {"routing_strategy", "first-available"},
         {"bind_address", "127.0.0.1:999292"},
     },
     [](const std::vector<std::string> &lines) {
       EXPECT_THAT(
           lines, ::testing::Contains(::testing::HasSubstr(
                      "option bind_address in [routing]: '127.0.0.1:999292' is "
                      "not a valid endpoint")));
     }},
    {"too_large_bind_port",
     {
         {"destinations", "127.0.0.1:3306"},
         {"routing_strategy", "first-available"},
         {"bind_port", "23123124123123"},
     },
     [](const std::vector<std::string> &lines) {
       EXPECT_THAT(
           lines, ::testing::Contains(::testing::HasSubstr(
                      "option bind_port in [routing] needs value between 1 and "
                      "65535 inclusive, was '23123124123123'")));
     }},
    {"invalid_mode",
     {
         {"destinations", "127.0.0.1:3306"},
         {"bind_address", "127.0.0.1"},
         {"bind_port", "6000"},
         {"mode", "invalid"},
     },
     [](const std::vector<std::string> &lines) {
       EXPECT_THAT(
           lines,
           ::testing::Contains(::testing::HasSubstr(
               "option mode in [routing] is invalid; valid are read-write "
               "and read-only (was 'invalid')")));
     }},
    {"invalid_routing_strategy",
     {
         {"destinations", "127.0.0.1:3306"},
         {"bind_address", "127.0.0.1"},
         {"bind_port", "6000"},
         {"routing_strategy", "invalid"},
     },
     [](const std::vector<std::string> &lines) {
       EXPECT_THAT(lines,
                   ::testing::Contains(::testing::HasSubstr(
                       "option routing_strategy in [routing] is invalid; valid "
                       "are first-available, "
                       "next-available, and round-robin (was 'invalid')")));
     }},
    {"empty_mode",
     {
         {"destinations", "127.0.0.1:3306"},
         {"bind_address", "127.0.0.1"},
         {"bind_port", "6000"},
         {"mode", ""},
     },
     [](const std::vector<std::string> &lines) {
       EXPECT_THAT(lines, ::testing::Contains(::testing::HasSubstr(
                              "option mode in [routing] needs a value")));
     }},
    {"empty_routing_strategy",
     {
         {"destinations", "127.0.0.1:3306"},
         {"bind_address", "127.0.0.1"},
         {"bind_port", "6000"},
         {"routing_strategy", ""},
     },
     [](const std::vector<std::string> &lines) {
       EXPECT_THAT(lines,
                   ::testing::Contains(::testing::HasSubstr(
                       "option routing_strategy in [routing] needs a value")));
     }},
    {"missing_routing_strategy",
     {
         {"destinations", "127.0.0.1:3306"},
         {"bind_address", "127.0.0.1"},
         {"bind_port", "6000"},
     },
     [](const std::vector<std::string> &lines) {
       EXPECT_THAT(lines,
                   ::testing::Contains(::testing::HasSubstr(
                       "option routing_strategy in [routing] is required")));
     }},
    {"thread_stack_size_negative",
     {
         {"destinations", "127.0.0.1:3306"},
         {"bind_address", "127.0.0.1"},
         {"bind_port", "6000"},
         {"routing_strategy", "first-available"},
         {"thread_stack_size", "-1"},
     },
     [](const std::vector<std::string> &lines) {
       EXPECT_THAT(lines,
                   ::testing::Contains(::testing::HasSubstr(
                       "option thread_stack_size in [routing] needs "
                       "value between 1 and 65535 inclusive, was '-1'")));
     }},
    {"thread_stack_size_float",
     {
         {"destinations", "127.0.0.1:3306"},
         {"bind_address", "127.0.0.1"},
         {"bind_port", "6000"},
         {"routing_strategy", "first-available"},
         {"thread_stack_size", "4.5"},
     },
     [](const std::vector<std::string> &lines) {
       EXPECT_THAT(lines,
                   ::testing::Contains(::testing::HasSubstr(
                       "option thread_stack_size in [routing] needs "
                       "value between 1 and 65535 inclusive, was '4.5'")));
     }},
    {"thread_stack_size_string",
     {
         {"destinations", "127.0.0.1:3306"},
         {"bind_address", "127.0.0.1"},
         {"bind_port", "6000"},
         {"routing_strategy", "first-available"},
         {"thread_stack_size", "dfs4"},
     },
     [](const std::vector<std::string> &lines) {
       EXPECT_THAT(lines,
                   ::testing::Contains(::testing::HasSubstr(
                       "option thread_stack_size in [routing] needs "
                       "value between 1 and 65535 inclusive, was 'dfs4'")));
     }},
    {"invalid_destination_host_start",
     {
         {"bind_address", "127.0.0.1"},
         {"bind_port", "6000"},
         {"routing_strategy", "first-available"},
         {"destinations", "{#mysqld1}"},
     },
     [](const std::vector<std::string> &lines) {
       EXPECT_THAT(lines, ::testing::Contains(::testing::HasSubstr(
                              "option destinations in [routing] has an "
                              "invalid destination address '{#mysqld1}'")));
     }},
    {"invalid_destination_host_mid",
     {
         {"bind_address", "127.0.0.1"},
         {"bind_port", "6000"},
         {"routing_strategy", "first-available"},
         {"destinations", "{mysqld1@1}"},
     },
     [](const std::vector<std::string> &lines) {
       EXPECT_THAT(lines, ::testing::Contains(::testing::HasSubstr(
                              "option destinations in [routing] has an "
                              "invalid destination address '{mysqld1@1}'")));
     }},
    {"invalid_destination_host_end",
     {
         {"bind_address", "127.0.0.1"},
         {"bind_port", "6000"},
         {"routing_strategy", "first-available"},
         {"destinations", "{mysqld1`}"},
     },
     [](const std::vector<std::string> &lines) {
       EXPECT_THAT(lines, ::testing::Contains(::testing::HasSubstr(
                              "option destinations in [routing] has an "
                              "invalid destination address '{mysqld1`}'")));
     }},
    {"invalid_destination_host_many",
     {
         {"bind_address", "127.0.0.1"},
         {"bind_port", "6000"},
         {"routing_strategy", "first-available"},
         {"destinations", "{mysql$d1%1}"},
     },
     [](const std::vector<std::string> &lines) {
       EXPECT_THAT(lines, ::testing::Contains(::testing::HasSubstr(
                              "option destinations in [routing] has an "
                              "invalid destination address '{mysql$d1%1}'")));
     }},
    {"invalid_destination_space_start",
     {
         {"bind_address", "127.0.0.1"},
         {"bind_port", "6000"},
         {"routing_strategy", "first-available"},
         {"destinations", "{ mysql1}"},
     },
     [](const std::vector<std::string> &lines) {
       EXPECT_THAT(lines, ::testing::Contains(::testing::HasSubstr(
                              "option destinations in [routing] has an "
                              "invalid destination address '{ mysql1}'")));
     }},
    {"invalid_destination_space_mid",
     {
         {"bind_address", "127.0.0.1"},
         {"bind_port", "6000"},
         {"routing_strategy", "first-available"},
         {"destinations", "{my sql1}"},
     },
     [](const std::vector<std::string> &lines) {
       EXPECT_THAT(lines, ::testing::Contains(::testing::HasSubstr(
                              "option destinations in [routing] has an "
                              "invalid destination address '{my sql1}'")));
     }},
    {"invalid_destination_space_end",
     {
         {"bind_address", "127.0.0.1"},
         {"bind_port", "6000"},
         {"routing_strategy", "first-available"},
         {"destinations", "{mysql1 }"},
     },
     [](const std::vector<std::string> &lines) {
       EXPECT_THAT(lines, ::testing::Contains(::testing::HasSubstr(
                              "option destinations in [routing] has an "
                              "invalid destination address '{mysql1 }'")));
     }},
    {"invalid_destination_space",
     {
         {"bind_address", "127.0.0.1"},
         {"bind_port", "6000"},
         {"routing_strategy", "first-available"},
         {"destinations", "{m@ysql d1}"},
     },
     [](const std::vector<std::string> &lines) {
       EXPECT_THAT(lines, ::testing::Contains(::testing::HasSubstr(
                              "option destinations in [routing] has an "
                              "invalid destination address '{m@ysql d1}'")));
     }},
    {"invalid_destination_multiple_space",
     {
         {"bind_address", "127.0.0.1"},
         {"bind_port", "6000"},
         {"routing_strategy", "first-available"},
         {"destinations", "{my sql d1}"},
     },
     [](const std::vector<std::string> &lines) {
       EXPECT_THAT(lines, ::testing::Contains(::testing::HasSubstr(
                              "option destinations in [routing] has an "
                              "invalid destination address '{my sql d1}'")));
     }},
    {"invalid_bind_port",
     {
         {"destinations", "127.0.0.1:3306"},
         {"bind_address", "127.0.0.1"},
         {"routing_strategy", "first-available"},

         {"bind_port", "{mysqld@1}"},
     },
     [](const std::vector<std::string> &lines) {
       EXPECT_THAT(lines,
                   ::testing::Contains(::testing::HasSubstr(
                       "option bind_port in [routing] needs value "
                       "between 1 and 65535 inclusive, was '{mysqld@1}'")));
     }},
};

INSTANTIATE_TEST_SUITE_P(Spec, RoutingConfigTest,
                         ::testing::ValuesIn(routing_config_param),
                         [](const auto &info) { return info.param.test_name; });

int main(int argc, char *argv[]) {
  init_windows_sockets();
  ProcessManager::set_origin(Path(argv[0]).dirname());
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
