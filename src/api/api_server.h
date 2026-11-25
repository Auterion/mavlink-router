#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "nlohmann/json_fwd.hpp"

#include "pollable.h"

class ApiServer;

/**
 * @brief JSON-RPC 2.0 error codes (https://www.jsonrpc.org/specification#error_object)
 */
enum class ApiServerErrorCode {
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603
};

/**
 * @brief Exception carrying a JSON-RPC error code
 *
 * Thrown by API method handlers to report a specific error back to the client.
 */
class ApiError : public std::runtime_error {
public:
    ApiError(ApiServerErrorCode error_code, const std::string &message)
        : std::runtime_error(message)
        , code(error_code)
    {
    }

    ApiServerErrorCode code;
};

/**
 * @brief One entry of the API method table
 *
 * The handler gets the "params" value of the request (an object or an array, as per
 * JSON-RPC) and returns the value to be sent back as "result". It throws ApiError to
 * report a failure.
 *
 * The table is terminated by a zeroed sentinel entry, e.g. { ..., {} }.
 */
struct ApiMethod {
    const char *name;
    nlohmann::json (*handler)(const nlohmann::json &params);
};

class ApiClient : public Pollable {
public:
    ApiClient(ApiServer &server)
        : _server(server)
    {
    }

    int handle_read() override;

    /**
     * Flush any pending messages.
     * Return true if there still more messages to be flushed.
     */
    bool handle_canwrite() override;

    /**
     * If a pollabe isn't valid anymore, it should be removed
     * from poll.
     */
    bool is_valid() override;

private:
    ApiServer &_server;
    bool _valid = true;

    std::string _rx_buffer;
    std::string _tx_buffer;
    constexpr static size_t MAX_BUFFER_SIZE = 1024 * 256; // 256 KB

    /**
     * @brief Queue a response to be sent to the client. The response is appended to the transmit buffer.
     *
     * @param payload The response payload to be queued
     * @return void
     */
    void queue_response(const std::string &payload);

    int flush_tx_buffer();
};

class ApiServer {
public:
    int fd = -1;
    static constexpr const char *JSONRPC_VERSION = "2.0";

    ApiServer(const std::string &socket_path);
    ~ApiServer();

    /**
     * @brief Create and bind the unix socket
     *
     * @return true if successfull
     * @return false otherwise
     */
    bool setup();

    /**
     * @brief Ask for invalid clients to be reaped on the next mainloop iteration
     */
    void schedule_client_hangups() { _should_process_client_hangups = true; };

    /**
     * @brief Process hangups for API clients
     */
    void process_client_hangups();

    /**
     * @brief Accept a new API client connection and register it with the mainloop
     *
     * @return true if a client was accepted and registered
     * @return false otherwise
     */
    bool accept_client();

    /**
     * @brief Dispatch a raw client request to the handler of the requested method
     *
     * @return the newline terminated JSON response, ready to be sent back
     */
    std::string handle_request(const std::string &raw_request);

private:
    std::string _unix_socket_path;
    std::vector<std::shared_ptr<ApiClient>> _api_clients{};
    bool _should_process_client_hangups = false;

    /**
     * @brief Parse and validate a raw JSON-RPC 2.0 request
     *
     * @param raw_request The raw request string
     * @return the parsed request object
     * @throws ApiError with ParseError or InvalidRequest
     */
    static nlohmann::json parse_request(const std::string &raw_request);
};
