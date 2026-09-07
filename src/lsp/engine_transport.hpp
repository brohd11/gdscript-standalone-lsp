#pragma once

#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gdscript_lsp {
using EngineDeadline = std::chrono::steady_clock::time_point;
using EngineCancelled = std::function<bool()>;

// Owned by the bridge worker. All blocking operations observe cancellation.
class EngineConnection {
public:
	EngineConnection();
	~EngineConnection();
	void connect(uint16_t port, EngineDeadline deadline, const EngineCancelled &cancelled);
	void send(const nlohmann::json &message, EngineDeadline deadline, const EngineCancelled &cancelled);
	nlohmann::json receive(EngineDeadline deadline, const EngineCancelled &cancelled);
	bool readable();
private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

class EngineProcess {
public:
	EngineProcess();
	~EngineProcess();
	void start(const std::string &executable, const std::vector<std::string> &arguments);
	bool running();
	std::string log();
	void stop();
private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

std::vector<uint16_t> engine_unused_ports(size_t count);
// Ensure normal frontend termination also unwinds owned engine processes.
void install_engine_termination_handlers();
} // namespace gdscript_lsp
