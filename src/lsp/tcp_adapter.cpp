#include "lsp/tcp_adapter.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>

#ifdef __linux__
#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <unordered_set>
#include <vector>
#endif

namespace gdscript_lsp {

#ifdef __linux__
namespace {

volatile std::sig_atomic_t stopping = 0;
volatile std::sig_atomic_t listener_fd = -1;

void handle_stop(int) {
	stopping = 1;
	if (listener_fd >= 0) {
		close(listener_fd);
		listener_fd = -1;
	}
}

void handle_child(int) {}

bool install_handler(int signal, void (*handler)(int)) {
	struct sigaction action {};
	action.sa_handler = handler;
	sigemptyset(&action.sa_mask);
	return sigaction(signal, &action, nullptr) == 0;
}

void reap_children(std::unordered_set<pid_t> &children) {
	while (true) {
		int status = 0;
		auto child = waitpid(-1, &status, WNOHANG);
		if (child > 0) {
			children.erase(child);
			continue;
		}
		if (child < 0 && errno == EINTR) continue;
		break;
	}
}

[[noreturn]] void run_session_child(int client, int listener, int argc, char **argv) {
	if (dup2(client, STDIN_FILENO) < 0 || dup2(client, STDOUT_FILENO) < 0) {
		std::cerr << "gdscript-lsp: could not attach TCP client: " << std::strerror(errno) << '\n';
		_exit(127);
	}
	close(client);
	close(listener);

	std::vector<char *> child_arguments;
	child_arguments.reserve(static_cast<size_t>(argc));
	for (int index = 0; index < argc; ++index) {
		if (std::strcmp(argv[index], "--tcp") == 0) {
			++index;
			continue;
		}
		child_arguments.push_back(argv[index]);
	}
	child_arguments.push_back(nullptr);

	execvp(child_arguments.front(), child_arguments.data());
	std::cerr << "gdscript-lsp: could not start TCP session: " << std::strerror(errno) << '\n';
	_exit(127);
}

} // namespace
#endif

int run_tcp_adapter(uint16_t port, int argc, char **argv) {
#ifndef __linux__
	(void)port;
	(void)argc;
	(void)argv;
	std::cerr << "gdscript-lsp: --tcp is currently supported on Linux only\n";
	return 2;
#else
	stopping = 0;
	auto listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (listener < 0) {
		std::cerr << "gdscript-lsp: could not create TCP listener: " << std::strerror(errno) << '\n';
		return 1;
	}

	int reuse_address = 1;
	if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address)) < 0) {
		std::cerr << "gdscript-lsp: could not configure TCP listener: " << std::strerror(errno) << '\n';
		close(listener);
		return 1;
	}

	sockaddr_in address {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons(port);
	if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
		std::cerr << "gdscript-lsp: could not bind 127.0.0.1:" << port << ": " << std::strerror(errno) << '\n';
		close(listener);
		return 1;
	}
	if (listen(listener, 8) < 0) {
		std::cerr << "gdscript-lsp: could not listen on 127.0.0.1:" << port << ": " << std::strerror(errno) << '\n';
		close(listener);
		return 1;
	}

	if (!install_handler(SIGINT, handle_stop) || !install_handler(SIGTERM, handle_stop) ||
			!install_handler(SIGCHLD, handle_child)) {
		std::cerr << "gdscript-lsp: could not install TCP supervisor signal handlers: " << std::strerror(errno) << '\n';
		close(listener);
		return 1;
	}

	listener_fd = listener;
	std::cerr << "gdscript-lsp: listening on 127.0.0.1:" << port << '\n';
	std::unordered_set<pid_t> children;
	while (!stopping) {
		reap_children(children);
		auto client = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
		if (client < 0) {
			if (errno == EINTR) continue;
			if (stopping || errno == EBADF || errno == EINVAL) break;
			std::cerr << "gdscript-lsp: could not accept TCP client: " << std::strerror(errno) << '\n';
			continue;
		}

		auto child = fork();
		if (child == 0) run_session_child(client, listener, argc, argv);
		close(client);
		if (child < 0) {
			std::cerr << "gdscript-lsp: could not start TCP session: " << std::strerror(errno) << '\n';
			continue;
		}
		children.insert(child);
	}

	if (listener_fd >= 0) close(listener);
	listener_fd = -1;
	for (auto child : children) kill(child, SIGTERM);
	for (auto child : children) {
		while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
	}
	return 0;
#endif
}

} // namespace gdscript_lsp
