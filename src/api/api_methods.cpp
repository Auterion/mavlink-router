#include "api_methods.h"

#include "git_version.h"
#include "mainloop.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace {

/**
 * Get the endpoints state.
 *
 * @param params JSON object containing the following request parameters:
 *      - endpoint_names: optional array of endpoint names to filter the results. If omitted, all endpoints are returned.
 * @return JSON object containing the state of all endpoints.
 */
json get_endpoints(const json &params)
{
    // Param validation and handling
    if (params.contains("endpoint_names") && !params["endpoint_names"].is_array()) {
        throw ApiError{ApiServerErrorCode::InvalidParams,
                       "Invalid 'endpoint_names' parameter: must be an array"};
    }

    json endpoints = json::array();
    std::vector<std::string> filter_endpoints;

    if (params.contains("endpoint_names") && params["endpoint_names"].is_array()) {
        for (const auto &endpoint_name : params["endpoint_names"]) {
            if (endpoint_name.is_string()) {
                filter_endpoints.push_back(endpoint_name.get<std::string>());
            }
        }
    }

    for (const auto &e : Mainloop::get_instance().get_endpoints()) {
        if (!filter_endpoints.empty()
            && std::find(filter_endpoints.begin(), filter_endpoints.end(), e->get_name())
                == filter_endpoints.end()) {
            continue;
        }

        json components = json::array();
        for (const auto &component : e->get_known_mav_components()) {
            components.push_back({{"sysid", component.first}, {"compid", component.second}});
        }

        endpoints.push_back({{"name", e->get_name()},
                             {"type", e->get_type()},
                             {"group", e->get_group_name()},
                             {"fd", e->fd},
                             {"known_components", std::move(components)}});
    }

    return endpoints;
}

json get_version(const json & /* params */)
{
    return json(GIT_VERSION);
}

} // namespace

// clang-format off
namespace api {

const ApiMethod methods[] = {
    {"get_endpoints",       get_endpoints},
    {"get_version",         get_version},
    {}
};

} // namespace api
// clang-format on