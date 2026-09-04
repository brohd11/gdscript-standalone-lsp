#include "lsp/tcp_adapter_internal.hpp"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace gdscript_lsp {
namespace {

std::atomic_bool stopping = false;
std::atomic<SOCKET> listener_socket = INVALID_SOCKET;

std::wstring utf8_to_wide(const char *text) {
	if (text == nullptr || *text == '\0') return {};
	int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
	UINT code_page = CP_UTF8;
	DWORD flags = MB_ERR_INVALID_CHARS;
	if (length == 0) {
		code_page = CP_ACP;
		flags = 0;
		length = MultiByteToWideChar(code_page, flags, text, -1, nullptr, 0);
	}
	if (length == 0) return {};
	std::wstring result(static_cast<size_t>(length), L'\0');
	MultiByteToWideChar(code_page, flags, text, -1, result.data(), length);
	result.pop_back();
	return result;
}

std::wstring executable_path() {
	std::vector<wchar_t> buffer(512);
	while (true) {
		const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (length == 0) return {};
		if (length < buffer.size() - 1) return std::wstring(buffer.data(), length);
		buffer.resize(buffer.size() * 2);
	}
}

std::wstring quote_argument(const std::wstring &argument) {
	if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) return argument;
	std::wstring result = L"\"";
	size_t backslashes = 0;
	for (const auto character : argument) {
		if (character == L'\\') {
			++backslashes;
			continue;
		}
		if (character == L'\"') {
			result.append(backslashes * 2 + 1, L'\\');
			result.push_back(character);
			backslashes = 0;
			continue;
		}
		result.append(backslashes, L'\\');
		backslashes = 0;
		result.push_back(character);
	}
	result.append(backslashes * 2, L'\\');
	result.push_back(L'\"');
	return result;
}

std::wstring session_command_line(const std::wstring &image, int argc, char **argv) {
	std::wstring result = quote_argument(image);
	for (int index = 1; index < argc; ++index) {
		if (std::strcmp(argv[index], "--tcp") == 0) {
			++index;
			continue;
		}
		result.push_back(L' ');
		result += quote_argument(utf8_to_wide(argv[index]));
	}
	return result;
}

void report_windows_error(const char *operation, DWORD error = GetLastError()) {
	std::cerr << "gdscript-lsp: " << operation << " (Windows error " << error << ")\n";
}

void report_socket_error(const char *operation, int error = WSAGetLastError()) {
	std::cerr << "gdscript-lsp: " << operation << " (Winsock error " << error << ")\n";
}

BOOL WINAPI handle_console_event(DWORD event) {
	if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT && event != CTRL_CLOSE_EVENT &&
			event != CTRL_LOGOFF_EVENT && event != CTRL_SHUTDOWN_EVENT) {
		return FALSE;
	}
	stopping = true;
	const auto listener = listener_socket.exchange(INVALID_SOCKET);
	if (listener != INVALID_SOCKET) closesocket(listener);
	return TRUE;
}

bool create_child_pipe(HANDLE &read_handle, HANDLE &write_handle) {
	SECURITY_ATTRIBUTES security {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
	return CreatePipe(&read_handle, &write_handle, &security, 0) != FALSE;
}

void close_handle(HANDLE &handle) {
	if (handle != nullptr && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
	handle = nullptr;
}

HANDLE inheritable_stderr() {
	HANDLE duplicate = nullptr;
	const auto source = GetStdHandle(STD_ERROR_HANDLE);
	if (source != nullptr && source != INVALID_HANDLE_VALUE &&
			DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &duplicate, 0, TRUE,
					DUPLICATE_SAME_ACCESS)) {
		return duplicate;
	}
	SECURITY_ATTRIBUTES security {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
	return CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

bool write_pipe(HANDLE pipe, const char *data, DWORD size) {
	DWORD offset = 0;
	while (offset < size) {
		DWORD written = 0;
		if (!WriteFile(pipe, data + offset, size - offset, &written, nullptr) || written == 0) return false;
		offset += written;
	}
	return true;
}

bool send_socket(SOCKET socket, std::mutex &socket_mutex, const char *data, int size) {
	int offset = 0;
	while (offset < size) {
		std::lock_guard lock(socket_mutex);
		const auto sent = send(socket, data + offset, size - offset, 0);
		if (sent == SOCKET_ERROR || sent == 0) return false;
		offset += sent;
	}
	return true;
}

struct SessionRegistry {
	std::mutex mutex;
	std::unordered_set<SOCKET> sockets;
};

void run_session(SOCKET client, HANDLE job, const std::wstring &image, const std::wstring &command_line,
		SessionRegistry &registry, const std::shared_ptr<std::atomic_bool> &done) {
	HANDLE child_stdin_read = nullptr;
	HANDLE parent_stdin_write = nullptr;
	HANDLE parent_stdout_read = nullptr;
	HANDLE child_stdout_write = nullptr;
	HANDLE child_stderr = nullptr;
	PROCESS_INFORMATION process {};

	auto finish = [&] {
		close_handle(child_stdin_read);
		close_handle(parent_stdin_write);
		close_handle(parent_stdout_read);
		close_handle(child_stdout_write);
		close_handle(child_stderr);
		close_handle(process.hThread);
		close_handle(process.hProcess);
		{
			std::lock_guard lock(registry.mutex);
			registry.sockets.erase(client);
		}
		closesocket(client);
		done->store(true, std::memory_order_release);
	};

	if (!create_child_pipe(child_stdin_read, parent_stdin_write) ||
			!create_child_pipe(parent_stdout_read, child_stdout_write)) {
		report_windows_error("could not create TCP session pipes");
		finish();
		return;
	}
	if (!SetHandleInformation(parent_stdin_write, HANDLE_FLAG_INHERIT, 0) ||
			!SetHandleInformation(parent_stdout_read, HANDLE_FLAG_INHERIT, 0)) {
		report_windows_error("could not configure TCP session pipes");
		finish();
		return;
	}
	child_stderr = inheritable_stderr();
	if (child_stderr == nullptr || child_stderr == INVALID_HANDLE_VALUE) {
		report_windows_error("could not configure TCP session stderr");
		finish();
		return;
	}

	STARTUPINFOW startup {};
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdInput = child_stdin_read;
	startup.hStdOutput = child_stdout_write;
	startup.hStdError = child_stderr;
	auto mutable_command_line = command_line;
	if (!CreateProcessW(image.c_str(), mutable_command_line.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED,
			nullptr, nullptr, &startup, &process)) {
		report_windows_error("could not start TCP session");
		finish();
		return;
	}
	if (!AssignProcessToJobObject(job, process.hProcess)) {
		report_windows_error("could not supervise TCP session");
		TerminateProcess(process.hProcess, 127);
		WaitForSingleObject(process.hProcess, INFINITE);
		finish();
		return;
	}
	if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
		report_windows_error("could not resume TCP session");
		TerminateProcess(process.hProcess, 127);
		WaitForSingleObject(process.hProcess, INFINITE);
		finish();
		return;
	}
	close_handle(process.hThread);
	close_handle(child_stdin_read);
	close_handle(child_stdout_write);
	close_handle(child_stderr);

	std::mutex socket_mutex;
	std::thread output([client, pipe = parent_stdout_read, &socket_mutex] {
		std::array<char, 16 * 1024> buffer {};
		while (true) {
			DWORD read = 0;
			if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || read == 0) break;
			if (!send_socket(client, socket_mutex, buffer.data(), static_cast<int>(read))) break;
		}
		CloseHandle(pipe);
		shutdown(client, SD_BOTH);
	});
	parent_stdout_read = nullptr;

	std::thread input([client, pipe = parent_stdin_write, &socket_mutex] {
		std::array<char, 16 * 1024> buffer {};
		while (true) {
			fd_set read_set;
			FD_ZERO(&read_set);
			FD_SET(client, &read_set);
			timeval timeout {0, 100000};
			const auto ready = select(0, &read_set, nullptr, nullptr, &timeout);
			if (ready == SOCKET_ERROR) break;
			if (ready == 0) continue;
			int received = 0;
			{
				std::lock_guard lock(socket_mutex);
				received = recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
			}
			if (received <= 0 || !write_pipe(pipe, buffer.data(), static_cast<DWORD>(received))) break;
		}
		CloseHandle(pipe);
		shutdown(client, SD_RECEIVE);
	});
	parent_stdin_write = nullptr;

	WaitForSingleObject(process.hProcess, INFINITE);
	shutdown(client, SD_RECEIVE);
	output.join();
	input.join();
	finish();
}

struct SessionThread {
	std::thread worker;
	std::shared_ptr<std::atomic_bool> done;
};

void reap_sessions(std::vector<SessionThread> &sessions, bool all = false) {
	for (auto iterator = sessions.begin(); iterator != sessions.end();) {
		if (all || iterator->done->load(std::memory_order_acquire)) {
			iterator->worker.join();
			iterator = sessions.erase(iterator);
		} else {
			++iterator;
		}
	}
}

} // namespace

int run_tcp_adapter_windows(uint16_t port, int argc, char **argv) {
	WSADATA winsock {};
	const auto startup_error = WSAStartup(MAKEWORD(2, 2), &winsock);
	if (startup_error != 0) {
		report_socket_error("could not initialize Winsock", startup_error);
		return 1;
	}

	const auto image = executable_path();
	if (image.empty()) {
		report_windows_error("could not find the gdscript-lsp executable");
		WSACleanup();
		return 1;
	}
	const auto command_line = session_command_line(image, argc, argv);

	HANDLE job = CreateJobObjectW(nullptr, nullptr);
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits {};
	job_limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (job == nullptr || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &job_limits,
			static_cast<DWORD>(sizeof(job_limits)))) {
		report_windows_error("could not create TCP session supervisor");
		close_handle(job);
		WSACleanup();
		return 1;
	}

	stopping = false;
	if (!SetConsoleCtrlHandler(handle_console_event, TRUE)) {
		report_windows_error("could not install TCP supervisor console handler");
		close_handle(job);
		WSACleanup();
		return 1;
	}

	// Accepted sockets inherit this attribute. Without it, Windows serializes
	// socket I/O and the input worker's wait can delay the output worker's send.
	const auto listener = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
			WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
	if (listener == INVALID_SOCKET) {
		report_socket_error("could not create TCP listener");
		SetConsoleCtrlHandler(handle_console_event, FALSE);
		close_handle(job);
		WSACleanup();
		return 1;
	}
	listener_socket = listener;

	BOOL exclusive_address = TRUE;
	if (setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
			reinterpret_cast<const char *>(&exclusive_address), sizeof(exclusive_address)) == SOCKET_ERROR) {
		report_socket_error("could not configure TCP listener");
		closesocket(listener_socket.exchange(INVALID_SOCKET));
		SetConsoleCtrlHandler(handle_console_event, FALSE);
		close_handle(job);
		WSACleanup();
		return 1;
	}

	sockaddr_in address {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons(port);
	if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR) {
		report_socket_error("could not bind TCP listener");
		closesocket(listener_socket.exchange(INVALID_SOCKET));
		SetConsoleCtrlHandler(handle_console_event, FALSE);
		close_handle(job);
		WSACleanup();
		return 1;
	}
	if (listen(listener, 8) == SOCKET_ERROR) {
		report_socket_error("could not listen for TCP clients");
		closesocket(listener_socket.exchange(INVALID_SOCKET));
		SetConsoleCtrlHandler(handle_console_event, FALSE);
		close_handle(job);
		WSACleanup();
		return 1;
	}

	std::cerr << "gdscript-lsp: listening on 127.0.0.1:" << port << '\n';
	SessionRegistry registry;
	std::vector<SessionThread> sessions;
	while (!stopping.load()) {
		reap_sessions(sessions);
		const auto client = accept(listener, nullptr, nullptr);
		if (client == INVALID_SOCKET) {
			const auto error = WSAGetLastError();
			if (stopping.load() || error == WSAENOTSOCK || error == WSAEINTR) break;
			report_socket_error("could not accept TCP client", error);
			continue;
		}
		if (!SetHandleInformation(reinterpret_cast<HANDLE>(client), HANDLE_FLAG_INHERIT, 0)) {
			report_windows_error("could not configure TCP client");
			closesocket(client);
			continue;
		}

		{
			std::lock_guard lock(registry.mutex);
			registry.sockets.insert(client);
		}
		auto done = std::make_shared<std::atomic_bool>(false);
		sessions.push_back({std::thread(run_session, client, job, std::cref(image), std::cref(command_line),
				std::ref(registry), done), done});
	}

	const auto active_listener = listener_socket.exchange(INVALID_SOCKET);
	if (active_listener != INVALID_SOCKET) closesocket(active_listener);
	{
		std::lock_guard lock(registry.mutex);
		for (const auto client : registry.sockets) shutdown(client, SD_BOTH);
	}
	close_handle(job);
	reap_sessions(sessions, true);
	SetConsoleCtrlHandler(handle_console_event, FALSE);
	WSACleanup();
	return 0;
}

} // namespace gdscript_lsp

#endif
