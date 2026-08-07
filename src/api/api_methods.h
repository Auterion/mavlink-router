#pragma once

#include "api_server.h"

/**
 * @brief Implementations of the methods exposed by the API server
 *
 * To add a method, implement its handler in api_methods.cpp and add one line to the
 * table. ApiServer looks methods up directly in it.
 */
namespace api {

extern const ApiMethod methods[];

} // namespace api