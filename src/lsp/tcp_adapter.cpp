#include "lsp/tcp_adapter.hpp"

#include "lsp/tcp_adapter_internal.hpp"

#include <iostream>

namespace gdscript_lsp {

int run_tcp_adapter(uint16_t port, int argc, char **argv) {
#ifdef _WIN32
	return run_tcp_adapter_windows(port, argc, argv);
#elif defined(__linux__) || defined(__APPLE__)
	return run_tcp_adapter_posix(port, argc, argv);
#else
	(void)port;
	(void)argc;
	(void)argv;
	std::cerr << "gdscript-lsp: --tcp is not supported on this platform\n";
	return 2;
#endif
}

} // namespace gdscript_lsp
