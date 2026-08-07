#include "api_server.h"
#include "mainloop.h"
#include "nlohmann/json.hpp"
#include <gtest/gtest.h>
#include <regex>
#include <string>
#include <sys/socket.h>

using json = nlohmann::json;

TEST(ApiTest, no_param_request)
{
    // Simple request without parameters, should return a valid JSON-RPC response with a version number in the result field
    ApiServer api_server("/tmp/mavlink-router.sock");

    json request = {{"jsonrpc", "2.0"}, {"method", "get_version"}, {"id", 1}};

    const std::string response = api_server.handle_request(request.dump() + "\n");

    // Response should be a valid JSON-RPC payload with a string satisfying semantic versioning in the result field
    EXPECT_FALSE(response.empty());

    json response_json = json::parse(response, nullptr, false);
    EXPECT_FALSE(response_json.is_discarded());
    EXPECT_EQ(response_json["jsonrpc"], "2.0");
    EXPECT_EQ(response_json["id"], 1);
    EXPECT_TRUE(response_json.contains("result"));

    const char *semver_regex
        = R"(^v(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)(?:-(?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*)
                                 (?:\.(?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*))*)?(?:\+[0-9a-zA-Z-]+(?:\.[0-9a-zA-Z-]+)*)?(?:-\d+-g[0-9a-f]+)?\+?$)";

    const std::regex semver(semver_regex);
    EXPECT_TRUE(std::regex_match(response_json["result"].get<std::string>(), semver));
}

TEST(ApiTest, invalid_method_request)
{
    ApiServer api_server("/tmp/mavlink-router.sock");

    json request = {{"jsonrpc", "2.0"}, {"method", "invalid_method"}, {"id", 1}};

    const std::string response = api_server.handle_request(request.dump() + "\n");

    EXPECT_FALSE(response.empty());

    json response_json = json::parse(response, nullptr, false);
    EXPECT_FALSE(response_json.is_discarded());
    EXPECT_EQ(response_json["jsonrpc"], "2.0");
    EXPECT_EQ(response_json["id"], 1);
    EXPECT_TRUE(response_json.contains("error"));
    EXPECT_EQ(response_json["error"]["code"], -32601);
}

TEST(ApiTest, invalid_json_request)
{
    ApiServer api_server("/tmp/mavlink-router.sock");

    const std::string invalid_json = "{invalid_json}";

    const std::string response = api_server.handle_request(invalid_json + "\n");

    EXPECT_FALSE(response.empty());

    json response_json = json::parse(response, nullptr, false);
    EXPECT_FALSE(response_json.is_discarded());
    EXPECT_EQ(response_json["jsonrpc"], "2.0");
    EXPECT_TRUE(response_json.contains("error"));
    EXPECT_EQ(response_json["error"]["code"], -32700);
}

TEST(ApiTest, invalid_request_object)
{
    ApiServer api_server("/tmp/mavlink-router.sock");

    json request = {{"jsonrpc", "2.0"}, {"id", 1}};

    const std::string response = api_server.handle_request(request.dump() + "\n");

    EXPECT_FALSE(response.empty());

    json response_json = json::parse(response, nullptr, false);
    EXPECT_FALSE(response_json.is_discarded());
    EXPECT_EQ(response_json["jsonrpc"], "2.0");
    // Even if there was an "id" in the request, the response should have a null id because the request was invalid
    EXPECT_TRUE(response_json["id"].is_null());
    EXPECT_TRUE(response_json.contains("error"));
    EXPECT_EQ(response_json["error"]["code"], -32600);
}

TEST(ApiTest, pipelined_requests)
{
    ApiServer api_server("/tmp/mavlink-router.sock");
    ApiClient api_client(api_server);

    int fd[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fd)) {
        FAIL() << "Could not create socket pair";
    }
    api_client.fd = fd[0];

    json request_a = {{"jsonrpc", "2.0"}, {"method", "get_version"}, {"id", 1}};
    json request_b = {{"jsonrpc", "2.0"}, {"method", "get_version"}, {"id", 2}};
    const std::string payload = request_a.dump() + "\n" + request_b.dump() + "\n";

    // Send RPC request
    int bytes_written = write(fd[1], payload.c_str(), payload.size());
    if (bytes_written == -1 || static_cast<size_t>(bytes_written) != payload.size()) {
        FAIL() << "Could not write to socket";
    }

    api_client.handle_read();

    // Read RPC reply
    char buffer[1024];
    int bytes_read = read(fd[1], buffer, sizeof(buffer));
    if (bytes_read == -1) {
        FAIL() << "Could not read from socket";
    }

    // We expect two JSON-RPC responses, one for each request, separated by newlines
    std::string response_str(buffer, bytes_read);
    std::vector<std::string> responses;
    size_t pos = 0;
    while ((pos = response_str.find('\n')) != std::string::npos) {
        responses.push_back(response_str.substr(0, pos));
        response_str.erase(0, pos + 1);
    }
    EXPECT_EQ(responses.size(), 2);

    json response_json_a = json::parse(responses[0], nullptr, false);
    EXPECT_FALSE(response_json_a.is_discarded());
    EXPECT_EQ(response_json_a["jsonrpc"], "2.0");
    EXPECT_EQ(response_json_a["id"], 1);
    EXPECT_TRUE(response_json_a.contains("result"));
    EXPECT_TRUE(response_json_a["result"].is_string());

    json response_json_b = json::parse(responses[1], nullptr, false);
    EXPECT_FALSE(response_json_b.is_discarded());
    EXPECT_EQ(response_json_b["jsonrpc"], "2.0");
    EXPECT_EQ(response_json_b["id"], 2);
    EXPECT_TRUE(response_json_b.contains("result"));
    EXPECT_TRUE(response_json_b["result"].is_string());
}

TEST(ApiTest, request_over_multiple_writes)
{
    ApiServer api_server("/tmp/mavlink-router.sock");
    ApiClient api_client(api_server);

    int fd[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fd)) {
        FAIL() << "Could not create socket pair";
    }
    api_client.fd = fd[0];

    json request = {{"jsonrpc", "2.0"}, {"method", "get_version"}, {"id", 1}};
    const std::string payload = request.dump() + "\n";

    // Send RPC request in two parts
    size_t mid = payload.size() / 2;
    int bytes_written = write(fd[1], payload.c_str(), mid);
    if (bytes_written == -1 || static_cast<size_t>(bytes_written) != mid) {
        FAIL() << "Could not write first part to socket";
    }

    // Handle read for the first part
    api_client.handle_read();

    // No response should be generated yet since the request is incomplete
    char buffer[1024];
    int bytes_read = 0;

    bytes_read = read(fd[1], buffer, sizeof(buffer));
    // We expect EAGAIN since the socket is set as non-blocking
    EXPECT_EQ(bytes_read, -1);
    EXPECT_EQ(errno, EAGAIN);

    // Send the second part
    bytes_written = write(fd[1], payload.c_str() + mid, payload.size() - mid);
    if (bytes_written == -1 || static_cast<size_t>(bytes_written) != payload.size() - mid) {
        FAIL() << "Could not write second part to socket";
    }

    // Handle read for the second part
    api_client.handle_read();

    // Read and validate the response
    bytes_read = read(fd[1], buffer, sizeof(buffer));
    if (bytes_read == -1) {
        FAIL() << "Could not read from socket";
    }

    json response_json = json::parse(std::string(buffer, bytes_read), nullptr, false);
    EXPECT_FALSE(response_json.is_discarded());
    EXPECT_EQ(response_json["jsonrpc"], "2.0");
    EXPECT_EQ(response_json["id"], 1);
    EXPECT_TRUE(response_json.contains("result"));
    EXPECT_TRUE(response_json["result"].is_string());
}

TEST(ApiTest, byzantine_too_long_request)
{
    // To protect against Byzantine clients that never send a newline, the API server
    // should drop the connection if the receive buffer exceeds exceeds 256 KB

    ApiServer api_server("/tmp/mavlink-router.sock");
    ApiClient api_client(api_server);

    int fd[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fd)) {
        FAIL() << "Could not create socket pair";
    }
    api_client.fd = fd[0];

    // Send a request that exceeds 256 KB without a newline
    std::string long_request(1024 * 256 + 1, 'a');
    int result = 0;

    while (long_request.size() > 0 && result == 0) {
        ssize_t bytes_written = write(fd[1], long_request.c_str(), long_request.size());
        if (bytes_written == -1) {
            if (errno != EAGAIN) {
                FAIL() << "Could not write to socket: " << strerror(errno);
            }
        } else {
            long_request.erase(0, bytes_written);
        }
        result = api_client.handle_read();
    }

    // Drain whatever is left until the overflow is detected
    while (result == 0) {
        result = api_client.handle_read();
    }

    // Handle read for the long request should return -1
    EXPECT_EQ(result, -1);

    // The client should now be marked as invalid
    EXPECT_FALSE(api_client.is_valid());
}

TEST(ApiTest, byzantine_no_readback)
{
    // To protect against Byzantine clients that flood the server with requests
    // but never read responses; the API server should drop the connection if
    // the transmit buffer exceeds 256 KB

    Mainloop::init();
    ApiServer api_server("/tmp/mavlink-router.sock");
    ApiClient api_client(api_server);

    int fd[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fd)) {
        FAIL() << "Could not create socket pair";
    }
    api_client.fd = fd[0];

    // The client sends valid requests but never reads the responses
    // so the transmit buffer will grow until it exceeds 256 KB

    json request = {{"jsonrpc", "2.0"}, {"method", "get_version"}, {"id", 2}};
    const std::string payload = request.dump() + "\n";

    // Flood server with RPC requests
    const size_t requests = 1000 * 8;
    for (size_t i = 0; i < requests; i++) {
        int bytes_written = write(fd[1], payload.c_str(), payload.size());
        if (bytes_written == -1 || static_cast<size_t>(bytes_written) != payload.size()) {
            FAIL() << "Could not write to socket";
        }
        api_client.handle_read();
    }

    // The client should now be marked as invalid
    EXPECT_FALSE(api_client.is_valid());
    Mainloop::teardown();
}
