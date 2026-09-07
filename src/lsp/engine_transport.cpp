#include "lsp/engine_transport.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace gdscript_lsp {
namespace {
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
void close_socket(Socket s) { if (s != invalid_socket) closesocket(s); }
void sockets_init() {
	static const bool initialized = [] { WSADATA data; return WSAStartup(MAKEWORD(2, 2), &data) == 0; }();
	if (!initialized) throw std::runtime_error("Winsock initialization failed");
}
void nonblocking(Socket s) { u_long mode = 1; if (ioctlsocket(s, FIONBIO, &mode)) throw std::runtime_error("Socket configuration failed"); }
bool would_block() { auto e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
void close_socket(Socket s) { if (s != invalid_socket) ::close(s); }
void sockets_init() {}
void nonblocking(Socket s) {
	if (fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK) < 0 || fcntl(s, F_SETFD, FD_CLOEXEC) < 0)
		throw std::runtime_error("Socket configuration failed");
#ifdef SO_NOSIGPIPE
	int yes = 1; setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
}
bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EINTR; }
#endif
void checkpoint(EngineDeadline deadline, const EngineCancelled &cancelled) {
	if (cancelled()) throw std::runtime_error("Engine operation cancelled");
	if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Engine response timed out");
}
bool ready(Socket s, bool write, long microseconds) {
	fd_set set; FD_ZERO(&set); FD_SET(s, &set);
	timeval timeout{}; timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>(microseconds);
	return select(static_cast<int>(s + 1), write ? nullptr : &set, write ? &set : nullptr, nullptr, &timeout) > 0;
}
void append_log(std::string &log, const char *data, size_t size) {
	log.append(data, size);
	constexpr size_t limit = 65536;
	if (log.size() > limit) log.erase(0, log.size() - limit);
}
}

struct EngineConnection::Impl {
	Socket socket = invalid_socket;
	std::string buffer;
	~Impl() { close_socket(socket); }
};
EngineConnection::EngineConnection() : impl_(std::make_unique<Impl>()) { sockets_init(); }
EngineConnection::~EngineConnection() = default;
void EngineConnection::connect(uint16_t port, EngineDeadline deadline, const EngineCancelled &cancelled) {
	while (true) {
		checkpoint(deadline, cancelled);
		close_socket(impl_->socket);
		impl_->socket = ::socket(AF_INET, SOCK_STREAM, 0);
		if (impl_->socket == invalid_socket) throw std::runtime_error("Cannot create engine socket");
		nonblocking(impl_->socket);
		sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
		int result = ::connect(impl_->socket, reinterpret_cast<sockaddr *>(&address), sizeof(address));
		if (result == 0) return;
		if (would_block() && ready(impl_->socket, true, 100000)) {
			int error = 0;
#ifdef _WIN32
			int size = sizeof(error);
#else
			socklen_t size = sizeof(error);
#endif
			if (getsockopt(impl_->socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &size) == 0 && error == 0) return;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}
void EngineConnection::send(const nlohmann::json &message, EngineDeadline deadline, const EngineCancelled &cancelled) {
	auto body = message.dump();
	if (body.size() > 16 * 1024 * 1024) throw std::runtime_error("Engine request exceeds 16 MiB");
	auto packet = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
	size_t offset = 0;
	while (offset < packet.size()) {
		checkpoint(deadline, cancelled);
		if (!ready(impl_->socket, true, 100000)) continue;
#ifdef MSG_NOSIGNAL
		constexpr int flags = MSG_NOSIGNAL;
#else
		constexpr int flags = 0;
#endif
		auto count = ::send(impl_->socket, packet.data() + offset, static_cast<int>(packet.size() - offset), flags);
		if (count > 0) offset += count;
		else if (count == 0 || !would_block()) throw std::runtime_error("Engine connection closed while sending");
	}
}
nlohmann::json EngineConnection::receive(EngineDeadline deadline, const EngineCancelled &cancelled) {
	constexpr size_t maximum = 16 * 1024 * 1024;
	size_t body_start = 0, length = 0;
	while (true) {
		checkpoint(deadline, cancelled);
		if (!body_start) {
			auto end = impl_->buffer.find("\r\n\r\n");
			if (end != std::string::npos) {
				if (end > 8192) throw std::runtime_error("Engine message header exceeds limit");
				auto header = impl_->buffer.substr(0, end);
				std::transform(header.begin(), header.end(), header.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				auto at = header.find("content-length:");
				if (at == std::string::npos || (at && (at < 2 || header.substr(at - 2, 2) != "\r\n"))) throw std::runtime_error("Invalid engine message header");
				at += 15; while (at < header.size() && header[at] == ' ') ++at;
				auto finish = header.find("\r\n", at); if (finish == std::string::npos) finish = header.size();
				auto parsed = std::from_chars(header.data() + at, header.data() + finish, length);
				if (parsed.ec != std::errc() || parsed.ptr != header.data() + finish || !length || length > maximum)
					throw std::runtime_error("Invalid engine message length");
				body_start = end + 4;
			} else if (impl_->buffer.size() > 8192) throw std::runtime_error("Engine message header exceeds limit");
		}
		if (body_start && impl_->buffer.size() >= body_start + length) {
			auto result = nlohmann::json::parse(impl_->buffer.substr(body_start, length));
			impl_->buffer.erase(0, body_start + length);
			if (!result.is_object()) throw std::runtime_error("Invalid engine JSON-RPC message");
			return result;
		}
		if (!ready(impl_->socket, false, 100000)) continue;
		std::array<char, 8192> buffer;
		auto count = recv(impl_->socket, buffer.data(), static_cast<int>(buffer.size()), 0);
		if (count > 0) impl_->buffer.append(buffer.data(), count);
		else if (count == 0 || !would_block()) throw std::runtime_error("Engine connection closed");
	}
}
bool EngineConnection::readable() { return !impl_->buffer.empty() || ready(impl_->socket, false, 0); }

std::vector<uint16_t> engine_unused_ports(size_t count) {
	sockets_init();
	std::vector<Socket> sockets;
	std::vector<uint16_t> ports;
	try {
		while (ports.size() < count) {
			auto s = ::socket(AF_INET, SOCK_STREAM, 0); sockets.push_back(s);
			if (s == invalid_socket) throw std::runtime_error("Cannot allocate engine port");
			sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			if (bind(s, reinterpret_cast<sockaddr *>(&address), sizeof(address))) throw std::runtime_error("Cannot reserve engine port");
#ifdef _WIN32
			int size = sizeof(address);
#else
			socklen_t size = sizeof(address);
#endif
			if (getsockname(s, reinterpret_cast<sockaddr *>(&address), &size)) throw std::runtime_error("Cannot read engine port");
			ports.push_back(ntohs(address.sin_port));
		}
	} catch (...) { for (auto s : sockets) close_socket(s); throw; }
	for (auto s : sockets) close_socket(s);
	return ports;
}

#ifdef _WIN32
namespace {
std::wstring wide(const std::string &value) {
	int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
	if (!count && !value.empty()) throw std::runtime_error("Invalid UTF-8 process argument");
	std::wstring result(count, L'\0'); MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count); return result;
}
std::wstring quote(const std::string &value) {
	std::wstring result = L"\""; size_t slashes = 0;
	for (auto c : wide(value)) {
		if (c == L'\\') { ++slashes; continue; }
		result.append(slashes * (c == L'"' ? 2 : 1) + (c == L'"' ? 1 : 0), L'\\'); slashes = 0; result += c;
	}
	result.append(slashes * 2, L'\\'); return result + L'"';
}
}
struct EngineProcess::Impl {
	HANDLE process = nullptr, job = nullptr, output = nullptr;
	std::string log;
	void drain() {
		DWORD available = 0, count = 0; char buffer[4096];
		for (int i = 0; i < 64 && output && PeekNamedPipe(output, nullptr, 0, nullptr, &available, nullptr) && available; ++i) {
			if (!ReadFile(output, buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr)) break;
			append_log(log, buffer, count);
		}
	}
};
void EngineProcess::start(const std::string &executable, const std::vector<std::string> &arguments) {
	stop(); impl_->log.clear();
	SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE}; HANDLE output_write = nullptr;
	if (!CreatePipe(&impl_->output, &output_write, &security, 0)) throw std::runtime_error("Cannot create engine log pipe");
	SetHandleInformation(impl_->output, HANDLE_FLAG_INHERIT, 0);
	HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
	impl_->job = CreateJobObjectW(nullptr, nullptr);
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (!impl_->job || !SetInformationJobObject(impl_->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
		CloseHandle(output_write); CloseHandle(input); throw std::runtime_error("Cannot create engine process job");
	}
	auto command = quote(executable); for (const auto &argument : arguments) command += L" " + quote(argument);
	STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	startup.StartupInfo.hStdInput = input; startup.StartupInfo.hStdOutput = output_write; startup.StartupInfo.hStdError = output_write;
	SIZE_T attribute_size = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
	std::vector<unsigned char> attributes(attribute_size);
	startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
	HANDLE inherited[] = {input, output_write};
	if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_size)) {
		CloseHandle(input); CloseHandle(output_write); throw std::runtime_error("Cannot initialize engine handle list");
	}
	if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) {
		DeleteProcThreadAttributeList(startup.lpAttributeList); CloseHandle(input); CloseHandle(output_write); throw std::runtime_error("Cannot restrict engine handle inheritance");
	}
	PROCESS_INFORMATION info{};
	bool created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &info);
	DeleteProcThreadAttributeList(startup.lpAttributeList);
	CloseHandle(output_write); CloseHandle(input);
	if (!created) throw std::runtime_error("Cannot launch Godot executable");
	impl_->process = info.hProcess;
	if (!AssignProcessToJobObject(impl_->job, impl_->process)) { TerminateProcess(impl_->process, 1); CloseHandle(info.hThread); throw std::runtime_error("Cannot own engine process job"); }
	ResumeThread(info.hThread); CloseHandle(info.hThread);
}
bool EngineProcess::running() { impl_->drain(); return impl_->process && WaitForSingleObject(impl_->process, 0) == WAIT_TIMEOUT; }
void EngineProcess::stop() {
	if (impl_->job) { CloseHandle(impl_->job); impl_->job = nullptr; }
	if (impl_->process) { WaitForSingleObject(impl_->process, 2000); CloseHandle(impl_->process); impl_->process = nullptr; }
	impl_->drain(); if (impl_->output) { CloseHandle(impl_->output); impl_->output = nullptr; }
}
void install_engine_termination_handlers() {} // Owned jobs also close on abrupt process exit.
#else
struct EngineProcess::Impl {
	pid_t pid = -1;
	pid_t group = -1;
	int output = -1;
	std::string log;
	void drain() {
		char buffer[4096];
		for (int i = 0; i < 64 && output >= 0; ++i) {
			auto count = ::read(output, buffer, sizeof(buffer)); if (count <= 0) break;
			append_log(log, buffer, count);
		}
	}
};
void EngineProcess::start(const std::string &executable, const std::vector<std::string> &arguments) {
	stop(); impl_->log.clear();
	std::vector<char *> argv{const_cast<char *>(executable.c_str())};
	for (const auto &argument : arguments) argv.push_back(const_cast<char *>(argument.c_str())); argv.push_back(nullptr);
	int pipe_fds[2]; if (pipe(pipe_fds)) throw std::runtime_error("Cannot create engine log pipe");
	fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC); fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC);
	auto pid = fork();
	if (pid == 0) {
		setpgid(0, 0);
		int input = open("/dev/null", O_RDONLY);
		if (input < 0 || dup2(input, STDIN_FILENO) < 0 || dup2(pipe_fds[1], STDOUT_FILENO) < 0 || dup2(pipe_fds[1], STDERR_FILENO) < 0) _exit(126);
		close(input); close(pipe_fds[0]); close(pipe_fds[1]);
		execvp(argv[0], argv.data()); _exit(127);
	}
	close(pipe_fds[1]);
	if (pid < 0) { close(pipe_fds[0]); throw std::runtime_error("Cannot launch Godot executable"); }
	setpgid(pid, pid); impl_->pid = pid; impl_->group = pid; impl_->output = pipe_fds[0];
	fcntl(impl_->output, F_SETFL, fcntl(impl_->output, F_GETFL) | O_NONBLOCK);
}
bool EngineProcess::running() {
	impl_->drain(); if (impl_->pid < 0) return false;
	int status; auto result = waitpid(impl_->pid, &status, WNOHANG);
	if (result == 0 || (result < 0 && errno == EINTR)) return true;
	impl_->pid = -1; return false;
}
void EngineProcess::stop() {
	// Retain the owned group after reaping a crashed parent: import helpers or
	// plugin subprocesses may still be alive in that group.
	if (impl_->group > 0) kill(-impl_->group, SIGTERM);
	if (impl_->pid > 0) {
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (running() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(20));
		if (impl_->pid > 0) { kill(-impl_->pid, SIGKILL); int status; while (waitpid(impl_->pid, &status, 0) < 0 && errno == EINTR) {} impl_->pid = -1; }
	}
	if (impl_->group > 0) { kill(-impl_->group, SIGKILL); impl_->group = -1; }
	impl_->drain(); if (impl_->output >= 0) { close(impl_->output); impl_->output = -1; }
}
namespace { void terminate_frontend(int) { ::close(STDIN_FILENO); } }
void install_engine_termination_handlers() {
	struct sigaction action{}; action.sa_handler = terminate_frontend; sigemptyset(&action.sa_mask);
	sigaction(SIGTERM, &action, nullptr); sigaction(SIGINT, &action, nullptr);
	struct sigaction ignored{}; ignored.sa_handler = SIG_IGN; sigemptyset(&ignored.sa_mask); sigaction(SIGPIPE, &ignored, nullptr);
}
#endif
EngineProcess::EngineProcess() : impl_(std::make_unique<Impl>()) {}
EngineProcess::~EngineProcess() { stop(); }
std::string EngineProcess::log() { impl_->drain(); return impl_->log; }
} // namespace gdscript_lsp
