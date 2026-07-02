#pragma once

#include "LSP/Client.hpp"

#include <vector>

class TestClient : public LSPClient
{
public:
    TestClient();

    std::vector<std::pair<std::string, std::optional<json>>> requestQueue;
    mutable std::vector<std::pair<std::string, std::optional<json>>> notificationQueue;
    std::vector<std::pair<std::optional<id_type>, JsonRpcException>> errorQueue;
    /// Response handlers for sent requests, indexed in step with requestQueue (nullopt if none)
    std::vector<std::optional<ResponseHandler>> requestHandlers;

    void sendRequest(
        const id_type& id, const std::string& method, const std::optional<json>& params, const std::optional<ResponseHandler>& handler) override;
    void sendNotification(const std::string& method, const std::optional<json>& params) const override;
    void sendError(const std::optional<id_type>& id, const JsonRpcException& e) override;

    /// Simulates the client responding to the most recent request with the given result
    void respondToLastRequest(const std::optional<json>& result);
};
