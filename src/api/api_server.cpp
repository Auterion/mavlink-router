#include "api_server.h"

#include <cerrno>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "api_methods.h"
#include "common/log.h"
#include "mainloop.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

static json make_error(ApiServerErrorCode code, const std::string &message)
{
    return json{{"code", static_cast<int>(code)}, {"message", message}};
}

static const ApiMethod *find_method(const std::string &name)
{
    for (const ApiMethod *m = api::methods; m->name; m++) {
        if (name == m->name) {
            return m;
        }
    }

    return nullptr;
}

ApiServer::ApiServer(const std::string &socket_path)
    : _unix_socket_path(socket_path)
{
}

ApiServer::~ApiServer()
{
    if (fd >= 0) {
        close(fd);
    }
    unlink(_unix_socket_path.c_str());
}

std::string ApiServer::handle_request(const std::string &raw_request)
{
    log_trace("API server: Got request: %s", raw_request.c_str());

    // The id stays null unless we get far enough to read it back from the request
    json response{{"jsonrpc", JSONRPC_VERSION}, {"id", nullptr}};
    std::string method;

    try {
        json request = parse_request(raw_request);

        response["id"] = request.value("id", json(nullptr));

        method = request.value("method", std::string{});
        const ApiMethod *api_method = find_method(method);
        if (api_method == nullptr) {
            throw ApiError{ApiServerErrorCode::MethodNotFound, "Method '" + method + "' not found"};
        }

        // Dispatch the request to the handler
        response["result"] = api_method->handler(request.value("params", json::object()));
    } catch (const ApiError &e) {
        // Client-caused: malformed request, unknown method or bad params
        log_debug("API server: Request failed: %s", e.what());
        response["error"] = make_error(e.code, e.what());
    } catch (const std::exception &e) {
        // Handler-caused: an unexpected throw is a server-side bug
        log_error("API server: Method '%s' threw: %s", method.c_str(), e.what());
        response["error"] = make_error(ApiServerErrorCode::InternalError, e.what());
    }

    return response.dump(-1, ' ', true, json::error_handler_t::replace) + "\n";
}

json ApiServer::parse_request(const std::string &raw_request)
{
    json request;

    try {
        request = json::parse(raw_request);
    } catch (const std::exception &e) {
        throw ApiError{ApiServerErrorCode::ParseError, e.what()};
    }

    // Validate the request is a valid JSON-RPC 2.0 request (https://www.jsonrpc.org/specification)
    if (!request.is_object() || request.value("jsonrpc", std::string{}) != JSONRPC_VERSION
        || !request.contains("method") || !request["method"].is_string()
        || (request.contains("params") && !request["params"].is_object()
            && !request["params"].is_array())
        || (request.contains("id") && !request["id"].is_string() && !request["id"].is_number()
            && !request["id"].is_null())) {
        throw ApiError{ApiServerErrorCode::InvalidRequest, "Invalid JSON-RPC request"};
    }

    return request;
}

bool ApiServer::setup()
{
    // Check that the socket path is valid and not too long for sockaddr_un
    constexpr size_t max_path_len = sizeof(sockaddr_un::sun_path) - 1;
    if (_unix_socket_path.empty() || _unix_socket_path.size() > max_path_len) {
        log_error("API Server: Invalid socket path '%s' (max %zu characters)",
                  _unix_socket_path.c_str(),
                  max_path_len);
        return false;
    }

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        log_error("API Server: Failed to create unix socket (%m)");
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    _unix_socket_path.copy(addr.sun_path, sizeof(addr.sun_path) - 1);

    // Remove socket file (if already existing)
    unlink(_unix_socket_path.c_str());

    // Bind socket
    if (bind(fd, (sockaddr *)&addr, sizeof(addr)) < 0) {
        log_error("API Server: Failed to bind to unix socket to %s (%m)",
                  _unix_socket_path.c_str());
        close(fd);
        return false;
    }

    // Listen on socket
    if (listen(fd, SOMAXCONN) < 0) {
        log_error("API Server: Failed to listen on unix socket (%m)");
        close(fd);
        return false;
    }
    log_info("API server listening on %s", _unix_socket_path.c_str());

    return true;
}

void ApiServer::process_client_hangups()
{
    if (!_should_process_client_hangups) {
        return;
    }

    // Remove invalid API clients
    for (auto it = _api_clients.begin(); it != _api_clients.end();) {
        if (!(*it)->is_valid()) {
            it = _api_clients.erase(it);
        } else {
            ++it;
        }
    }

    _should_process_client_hangups = false;
}

bool ApiServer::accept_client()
{
    int client_fd = ::accept4(fd, nullptr, nullptr, SOCK_NONBLOCK);
    if (client_fd < 0) {
        log_error("API server: Could not accept unix socket connection (%m)");
        return false;
    }

    auto client = std::make_shared<ApiClient>(*this);
    client->fd = client_fd;

    // Register the client with the epoll
    if (Mainloop::get_instance().add_fd(client->fd, client.get(), EPOLLIN) < 0) {
        return false;
    }

    log_info("API server: Accepted new client [%d]", client_fd);
    _api_clients.push_back(std::move(client));

    return true;
}

int ApiClient::handle_read()
{
    // Read data from the client socket into stack buffer
    char buffer[1024];
    ssize_t bytes_read = recv(fd, buffer, sizeof(buffer), 0);
    if (bytes_read < 0) {
        if (errno == EAGAIN) {
            return 0;
        }
        log_error("API server: recv error (%m) from [%d]", fd);
        _valid = false;
        _server.schedule_client_hangups();
        return -1;
    } else if (bytes_read == 0) {
        // A read of zero on a stream socket means that other side shut down
        log_info("API server: Connection with [%d] closed by peer", fd);
        _valid = false;
        _server.schedule_client_hangups();
        return -1;
    }

    // Append the read data to the receive buffer
    _rx_buffer.append(buffer, bytes_read);

    // Process complete requests (newline delimited JSON-RPC)
    size_t newline_pos;
    while ((newline_pos = _rx_buffer.find('\n')) != std::string::npos) {
        std::string request = _rx_buffer.substr(0, newline_pos);
        _rx_buffer.erase(0, newline_pos + 1); // Remove the processed request
        queue_response(_server.handle_request(request));
    }

    // Protect against Byzantine clients never sending a newline
    if (_rx_buffer.size() > MAX_BUFFER_SIZE) {
        log_warning("API server: Receive buffer overflow from [%d]", fd);
        _valid = false;
        _server.schedule_client_hangups();
        return -1;
    }

    return 0;
}

void ApiClient::queue_response(const std::string &payload)
{
    _tx_buffer.append(payload);

    // Register the client for write events
    if (flush_tx_buffer() == -EAGAIN) {
        Mainloop::get_instance().mod_fd(fd, this, EPOLLIN | EPOLLOUT);
    }

    // Byzantine clients could keep sending requests without reading responses, so we need to protect against buffer overflow
    if (_tx_buffer.size() > MAX_BUFFER_SIZE) {
        log_warning("API server: Transmit buffer overflow for [%d]", fd);
        _valid = false;
        _server.schedule_client_hangups();
        return;
    }
}

int ApiClient::flush_tx_buffer()
{
    log_debug("API server: Flushing %zu bytes to [%d]", _tx_buffer.size(), fd);

    while (!_tx_buffer.empty()) {
        ssize_t n = ::send(fd, _tx_buffer.data(), _tx_buffer.size(), MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN) {
                return -EAGAIN;
            }
            log_error("API server: Could not send response to [%d] (%m)", fd);
            if (errno == EPIPE || errno == ECONNRESET) {
                _valid = false;
                _server.schedule_client_hangups();
            }
            return -errno;
        }
        _tx_buffer.erase(0, n);
    }

    return 0;
}

bool ApiClient::handle_canwrite()
{
    return flush_tx_buffer() == -EAGAIN;
}

bool ApiClient::is_valid()
{
    return _valid;
}