#pragma once

#include <cstdint>

namespace gdscript_lsp {

#if defined(__linux__) || defined(__APPLE__)
int run_tcp_adapter_posix(uint16_t port, int argc, char **argv);
#endif

#ifdef _WIN32
int run_tcp_adapter_windows(uint16_t port, int argc, char **argv);
#endif

} // namespace gdscript_lsp
