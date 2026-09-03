#pragma once

#include "core/workspace.hpp"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <atomic>
#include <memory>
#include <thread>

namespace gdscript_lsp {

class GDScriptLanguageService : public godot::RefCounted {
	GDCLASS(GDScriptLanguageService, godot::RefCounted)

public:
	GDScriptLanguageService();
	~GDScriptLanguageService() override;

	godot::Error open_workspace(const godot::String &project_root, const godot::Dictionary &options = {});
	bool is_ready() const;
	void update_document(const godot::String &uri, const godot::String &text, int64_t version);
	void close_document(const godot::String &uri);
	void refresh_files(const godot::PackedStringArray &paths);
	godot::Dictionary completion(const godot::String &uri, int line, int utf16_column) const;
	godot::Dictionary completion_ex(const godot::String &uri, int line, int utf16_column,
		const godot::Dictionary &options = {}) const;
	void set_configuration(const godot::Dictionary &configuration);
	godot::Dictionary hover(const godot::String &uri, int line, int utf16_column) const;
	godot::Array definition(const godot::String &uri, int line, int utf16_column) const;
	godot::Array document_symbols(const godot::String &uri) const;
	godot::Array diagnostics(const godot::String &uri) const;
	godot::Dictionary resolve_type(const godot::String &uri, int line, int utf16_column,
		const godot::String &expression = {}) const;

	void _finish_open(const godot::String &error);

protected:
	static void _bind_methods();

private:
	std::unique_ptr<Workspace> workspace_;
	std::jthread index_thread_;
	std::atomic_bool ready_ = false;
};

} // namespace gdscript_lsp
