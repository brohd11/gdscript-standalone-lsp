#pragma once

#include <nlohmann/json.hpp>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace gdscript_lsp {
struct EngineConfiguration {
	std::string mode = "off";
	std::string executable;
	uint16_t port = 0;
	bool operator==(const EngineConfiguration &) const = default;
	static EngineConfiguration parse(const nlohmann::json &value);
};

class EngineBridge {
public:
	using Result = std::function<void(const std::string &, uint64_t, nlohmann::json)>;
	using State = std::function<void(nlohmann::json)>;
	EngineBridge(Result result, State state);
	~EngineBridge();
	void configure(const std::filesystem::path &root, const EngineConfiguration &config, bool force = false);
	void submit(const std::string &uri, std::string source, int64_t version, uint64_t revision, bool open, bool immediate);
	void saved(const std::string &uri);
	void project_changed();
	void forget(const std::string &uri);
	nlohmann::json status() const;
	bool ready() const;
	void stop();
private:
	struct Document {
		std::string source;
		int64_t version = -1;
		uint64_t revision = 0;
		bool open = false, queued = true, saved = false, priority = false;
		std::chrono::steady_clock::time_point due;
	};
	void run(std::stop_token stop);
	void set_state(uint64_t epoch, std::string state, std::string reason = {}, std::string version = {});
	Result result_;
	State state_;
	mutable std::mutex mutex_;
	std::condition_variable condition_;
	EngineConfiguration config_;
	std::filesystem::path root_;
	std::unordered_map<std::string, Document> documents_;
	std::unordered_set<std::string> pending_saves_;
	nlohmann::json status_ = {{"mode", "off"}, {"state", "off"}, {"reason", ""}, {"engineVersion", nullptr}};
	uint64_t epoch_ = 0;
	bool stopping_ = false, reload_required_ = false;
	std::jthread worker_;
};
} // namespace gdscript_lsp
