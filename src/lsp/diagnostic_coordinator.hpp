#pragma once
#include "core/workspace.hpp"
#include "lsp/engine_bridge.hpp"
#include <nlohmann/json.hpp>
#include <functional>
#include <memory>

namespace gdscript_lsp {
class DiagnosticCoordinatorImpl;
class DiagnosticCoordinator {
public:
    using Send = std::function<void(const nlohmann::json &)>;
    using Serialize = std::function<nlohmann::json(const Diagnostic &)>;
    DiagnosticCoordinator(Workspace &workspace, Send send, Serialize serialize);
    ~DiagnosticCoordinator();
    uint64_t begin_update();
    void finish_update(uint64_t generation, const std::vector<std::string> &immediate, const std::vector<std::string> &affected);
    void schedule_full();
    void set_open(const std::string &uri, bool open, const std::string &client_uri = {});
    void set_poll_interval(std::chrono::milliseconds interval);
    void request_stop();
    void configure_engine(const EngineConfiguration &config);
    void start_engine();
    void reconnect_engine();
    void external_changes(const std::vector<std::string> &uris);
    void saved(const std::string &uri);
    nlohmann::json engine_status() const;
    void pull(const std::string &uri, const nlohmann::json &id);
    void cancel_pull(const nlohmann::json &id);
private:
    std::unique_ptr<DiagnosticCoordinatorImpl> impl_;
};
}
