#include "sysconfig.h"
#include <Ws2tcpip.h>
#include "sysdeps.h"

#include <thread>
#include <vector>

#include "options.h"
#include "memory.h"
#include "newcpu.h"
#include "debug.h"
#include "inputdevice.h"
#include "uae.h"
#include "debugmem.h"
#include "dxwrap.h" // AmigaMonitor
#include "custom.h"
#include "win32.h"
#include "savestate.h"

extern BITMAPINFO* screenshot_get_bi();
extern void* screenshot_get_bits();

// from main.cpp
extern struct uae_prefs currprefs;

// from newcpu.cpp
/*static*/ extern int baseclock;

// from debug.cpp
extern uae_u8 *get_real_address_debug(uaecptr addr);
extern void initialize_memwatch(int mode);
extern void memwatch_setup(void);
#define TRACE_SKIP_INS 1
#define TRACE_MATCH_PC 2
#define TRACE_MATCH_INS 3
#define TRACE_RANGE_PC 4
#define TRACE_SKIP_LINE 5
#define TRACE_RAM_PC 6
#define TRACE_NRANGE_PC 7
#define TRACE_CHECKONLY 10
/*static*/ extern int trace_mode;
/*static*/ extern uae_u32 trace_param1;
/*static*/ extern uae_u32 trace_param2;
/*static*/ extern uaecptr processptr;
/*static*/ extern uae_char *processname;
/*static*/ extern int memwatch_triggered;
/*static*/ extern struct memwatch_node mwhit;
extern int debug_illegal;
extern uae_u64 debug_illegal_mask;

#include "barto_gdbserver.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// -s input.config=1 -s input.1.keyboard.0.button.41.GRAVE=SPC_SINGLESTEP.0    -s use_gui=no -s quickstart=a500,1 -s debugging_features=gdbserver -s filesystem=rw,dh0:c:\Users\bwodok\Documents\Visual_Studio_Code\amiga-debug\bin\dh0
// c:\Users\bwodok\Documents\Visual_Studio_Code\amiga-debug\bin\opt\bin> m68k-amiga-elf-gdb.exe -ex "set debug remote 1" -ex "target remote :2345" -ex "monitor profile xxx" ..\..\..\template\a.mingw.elf

namespace barto_gdbserver {
	bool is_connected();
	bool data_available();
	void disconnect();

	static bool in_handle_packet = false;
	struct tracker {
		tracker() { backup = in_handle_packet; in_handle_packet = true; }
		~tracker() { in_handle_packet = backup; }
	private: 
		bool backup;
	};

	void barto_log(const char* format, ...);
	void barto_log(const wchar_t* format, ...);

	static std::string string_replace_all(const std::string& str, const std::string& search, const std::string& replace) {
		std::string copy(str);
		size_t start = 0;
		for(;;) {
			auto p = copy.find(search, start);
			if(p == std::string::npos)
				break;

			copy.replace(p, search.size(), replace);
			start = p + replace.size();
		}
		return copy;
	}

	static std::string string_to_utf8(LPCWSTR string) {
		int len = WideCharToMultiByte(CP_UTF8, 0, string, -1, nullptr, 0, nullptr, nullptr);
		std::unique_ptr<char[]> buffer(new char[len]);
		WideCharToMultiByte(CP_UTF8, 0, string, -1, buffer.get(), len, nullptr, nullptr);
		return std::string(buffer.get());
	}

	static constexpr char hex[]{ "0123456789abcdef" };
	static std::string hex8(uint8_t v) {
		std::string ret;
		ret += hex[v >> 4];
		ret += hex[v & 0xf];
		return ret;
	}
	static std::string hex32(uint32_t v) {
		std::string ret;
		for(int i = 28; i >= 0; i -= 4)
			ret += hex[(v >> i) & 0xf];
		return ret;
	}

	static std::string from_hex(const std::string& s) {
		std::string ret;
		for(size_t i = 0, len = s.length() & ~1; i < len; i += 2) {
			uint8_t v{};
			if(s[i] >= '0' && s[i] <= '9')
				v |= (s[i] - '0') << 4;
			else if(s[i] >= 'a' && s[i] <= 'f')
				v |= (s[i] - 'a' + 10) << 4;
			else if(s[i] >= 'A' && s[i] <= 'F')
				v |= (s[i] - 'A' + 10) << 4;
			if(s[i + 1] >= '0' && s[i + 1] <= '9')
				v |= (s[i + 1] - '0');
			else if(s[i + 1] >= 'a' && s[i + 1] <= 'f')
				v |= (s[i + 1] - 'a' + 10);
			else if(s[i + 1] >= 'A' && s[i + 1] <= 'F')
				v |= (s[i + 1] - 'A' + 10);
			ret += (char)v;
		}
		return ret;
	}

	static std::string to_hex(const std::string& s) {
		std::string ret;
		for(size_t i = 0, len = s.length(); i < len; i++) {
			uint8_t v = s[i];
			ret += hex[v >> 4];
			ret += hex[v & 0xf];
		}
		return ret;
	}

	std::thread connect_thread;
	PADDRINFOW socketinfo;
	SOCKET gdbsocket{ INVALID_SOCKET };
	SOCKET gdbconn{ INVALID_SOCKET };
	char socketaddr[sizeof SOCKADDR_INET];
	bool useAck{ true };
	uint32_t baseText{};
	uint32_t sizeText{};
	uint32_t systemStackLower{}, systemStackUpper{};
	uint32_t stackLower{}, stackUpper{};
	std::vector<uint32_t> sections; // base for every section
	std::string profile_outname;
	int profile_num_frames{};
	int profile_frame_count{};
	std::unique_ptr<cpu_profiler_unwind[]> profile_unwind{};

	enum class state {
		inited,
		connected,
		debugging,
		profile,
		profiling,
	};

	state debugger_state{ state::inited };

	bool is_connected() {
		socklen_t sa_len = sizeof SOCKADDR_INET;
		if(gdbsocket == INVALID_SOCKET)
			return false;
		if(gdbconn == INVALID_SOCKET) {
			struct timeval tv;
			fd_set fd;
			tv.tv_sec = 0;
			tv.tv_usec = 0;
			fd.fd_array[0] = gdbsocket;
			fd.fd_count = 1;
			if(select(1, &fd, nullptr, nullptr, &tv)) {
				gdbconn = accept(gdbsocket, (struct sockaddr*)socketaddr, &sa_len);
				if(gdbconn != INVALID_SOCKET)
					barto_log("GDBSERVER: connection accepted\n");
			}
		}
		return gdbconn != INVALID_SOCKET;
	}

	bool data_available() {
		if(is_connected()) {
			struct timeval tv;
			fd_set fd;
			tv.tv_sec = 0;
			tv.tv_usec = 0;
			fd.fd_array[0] = gdbconn;
			fd.fd_count = 1;
			int err = select(1, &fd, nullptr, nullptr, &tv);
			if(err == SOCKET_ERROR) {
				disconnect();
				return false;
			}
			if(err > 0)
				return true;
		}
		return false;
	}

	bool listen() {
		barto_log("GDBSERVER: listen()\n");

		assert(debugger_state == state::inited);

		WSADATA wsaData = { 0 };
		if(WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
			DWORD lasterror = WSAGetLastError();
			barto_log(_T("GDBSERVER: can't open winsock, error %d\n"), lasterror);
			return false;
		}
		int err;
		const int one = 1;
		const struct linger linger_1s = { 1, 1 };
		constexpr auto name = _T("127.0.0.1");
		constexpr auto port = _T("2345");

		err = GetAddrInfoW(name, port, nullptr, &socketinfo);
		if(err < 0) {
			barto_log(_T("GDBSERVER: GetAddrInfoW() failed, %s:%s: %d\n"), name, port, WSAGetLastError());
			return false;
		}
		gdbsocket = socket(socketinfo->ai_family, socketinfo->ai_socktype, socketinfo->ai_protocol);
		if(gdbsocket == INVALID_SOCKET) {
			barto_log(_T("GDBSERVER: socket() failed, %s:%s: %d\n"), name, port, WSAGetLastError());
			return false;
		}
		err = ::bind(gdbsocket, socketinfo->ai_addr, (int)socketinfo->ai_addrlen);
		if(err < 0) {
			barto_log(_T("GDBSERVER: bind() failed, %s:%s: %d\n"), name, port, WSAGetLastError());
			return false;
		}
		err = ::listen(gdbsocket, 1);
		if(err < 0) {
			barto_log(_T("GDBSERVER: listen() failed, %s:%s: %d\n"), name, port, WSAGetLastError());
			return false;
		}
		err = setsockopt(gdbsocket, SOL_SOCKET, SO_LINGER, (char*)&linger_1s, sizeof linger_1s);
		if(err < 0) {
			barto_log(_T("GDBSERVER: setsockopt(SO_LINGER) failed, %s:%s: %d\n"), name, port, WSAGetLastError());
			return false;
		}
		err = setsockopt(gdbsocket, SOL_SOCKET, SO_REUSEADDR, (char*)&one, sizeof one);
		if(err < 0) {
			barto_log(_T("GDBSERVER: setsockopt(SO_REUSEADDR) failed, %s:%s: %d\n"), name, port, WSAGetLastError());
			return false;
		}

		return true;
	}

	bool init() {
		if(currprefs.debugging_features & (1 << 2)) { // "gdbserver"
			warpmode(1);

			// disable console
			static TCHAR empty[2] = { 0 };
			setconsolemode(empty, 1);

			activate_debugger();
			initialize_memwatch(0);

			// from debug.cpp@process_breakpoint()
			processptr = 0;
			xfree(processname);
			processname = nullptr;
			processname = ua(currprefs.debugging_trigger);
			trace_mode = TRACE_CHECKONLY;

			// call as early as possible to avoid delays with GDB having to retry to connect...
			listen();
		}

		return true;
	}

	void close() {
		if(gdbconn != INVALID_SOCKET)
			closesocket(gdbconn);
		gdbconn = INVALID_SOCKET;
		if(gdbsocket != INVALID_SOCKET)
			closesocket(gdbsocket);
		gdbsocket = INVALID_SOCKET;
		if(socketinfo)
			FreeAddrInfoW(socketinfo);
		socketinfo = nullptr;
		WSACleanup();
	}

	void disconnect() {
		if(gdbconn == INVALID_SOCKET)
			return;
		closesocket(gdbconn);
		gdbconn = INVALID_SOCKET;
		barto_log(_T("GDBSERVER: disconnect\n"));
	}

	// from binutils-gdb/gdb/m68k-tdep.c
/*	static const char* m68k_register_names[] = {
		"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
		"a0", "a1", "a2", "a3", "a4", "a5", "a6", "sp", //BARTO
		"sr", "pc", //BARTO
		"fp0", "fp1", "fp2", "fp3", "fp4", "fp5", "fp6", "fp7",
		"fpcontrol", "fpstatus", "fpiaddr"
	}*/
	enum regnames {
		D0, D1, D2, D3, D4, D5, D6, D7,
		A0, A1, A2, A3, A4, A5, A6, A7,
		SR, PC
	};

	static std::string get_register(int reg) {
		uint32_t regvalue{};
		// need to byteswap because GDB expects 68k big-endian
		switch(reg) {
		case SR: 
			regvalue = regs.sr; 
			break;
		case PC: 
			regvalue = M68K_GETPC; 
			break;
		case D0: case D1: case D2: case D3: case D4: case D5: case D6: case D7:
			regvalue = m68k_dreg(regs, reg - D0);
			break;
		case A0: case A1: case A2: case A3: case A4: case A5: case A6: case A7:
			regvalue = m68k_areg(regs, reg - A0);
			break;
		default:
			return "xxxxxxxx";
		}
		return hex32(regvalue);
	}

	static std::string get_registers() {
		barto_log("GDBSERVER: PC=%x\n", M68K_GETPC);
		std::string ret;
		for(int reg = 0; reg < 18; reg++)
			ret += get_register(reg);
		return ret;
	}

	void print_breakpoints() {
		barto_log("GDBSERVER: Breakpoints:\n");
		for(auto& bpn : bpnodes) {
			if(bpn.enabled) {
				barto_log("GDBSERVER: - %d, 0x%x, 0x%x\n", bpn.type, bpn.value1, bpn.value2);
			}
		}
	}

	void print_watchpoints() {
		barto_log("GDBSERVER: Watchpoints:\n");
		for(auto& mwn : mwnodes) {
			if(mwn.size) {
				barto_log("GDBSERVER: - 0x%x, 0x%x\n", mwn.addr, mwn.size);
			}
		}
	}

	void send_ack(const std::string& ack) {
		if(useAck && !ack.empty()) {
			barto_log("GDBSERVER: <- %s\n", ack.c_str());
			int result = send(gdbconn, ack.data(), (int)ack.length(), 0);
			if(result == SOCKET_ERROR)
				barto_log(_T("GDBSERVER: error sending ack: %d\n"), WSAGetLastError());
		}
	}

	void send_response(std::string response) {
		tracker _;
		if(!response.empty()) {
			barto_log("GDBSERVER: <- %s\n", response.substr(1).c_str());
			uint8_t cksum{};
			for(size_t i = 1; i < response.length(); i++)
				cksum += response[i];
			response += '#';
			response += hex[cksum >> 4];
			response += hex[cksum & 0xf];
			int result = send(gdbconn, response.data(), (int)response.length(), 0);
			if(result == SOCKET_ERROR)
				barto_log(_T("GDBSERVER: error sending data: %d\n"), WSAGetLastError());
		}
	}

	void handle_packet() {
		tracker _;
		if(data_available()) {
			char buf[512];
			auto result = recv(gdbconn, buf, sizeof(buf) - 1, 0);
			if(result > 0) {
				buf[result] = '\0';
				barto_log("GDBSERVER: received %d bytes: >>%s<<\n", result, buf);
				std::string request{ buf }, ack{}, response;
				if(request[0] == '+') {
					request = request.substr(1);
				} else if(request[0] == '-') {
					barto_log("GDBSERVER: client non-ack'd our last packet\n");
					request = request.substr(1);
				}
				if(!request.empty() && request[0] == 0x03) {
					// Ctrl+C
					ack = "+";
					response = "$";
					response += "S05"; // SIGTRAP
					debugger_state = state::debugging;
					activate_debugger();
				} else if(!request.empty() && request[0] == '$') {
					ack = "-";
					auto end = request.find('#');
					if(end != std::string::npos) {
						uint8_t cksum{};
						for(size_t i = 1; i < end; i++)
							cksum += request[i];
						if(request.length() >= end + 2) {
							if(tolower(request[end + 1]) == hex[cksum >> 4] && tolower(request[end + 2]) == hex[cksum & 0xf]) {
								request = request.substr(1, end - 1);
								barto_log("GDBSERVER: -> %s\n", request.c_str());
								ack = "+";
								response = "$";
								if(request.substr(0, strlen("qSupported")) == "qSupported") {
									response += "PacketSize=512;BreakpointCommands+;swbreak+;hwbreak+;QStartNoAckMode+;vContSupported+;";
								} else if(request.substr(0, strlen("qAttached")) == "qAttached") {
									response += "1";
								} else if(request.substr(0, strlen("qTStatus")) == "qTStatus") {
									response += "T0";
								} else if(request.substr(0, strlen("QStartNoAckMode")) == "QStartNoAckMode") {
									send_ack(ack);
									useAck = false;
									response += "OK";
								} else if(request.substr(0, strlen("qfThreadInfo")) == "qfThreadInfo") {
									response += "m1";
								} else if(request.substr(0, strlen("qsThreadInfo")) == "qsThreadInfo") {
									response += "l";
								} else if(request.substr(0, strlen("qC")) == "qC") {
									response += "QC1";
								} else if(request.substr(0, strlen("qOffsets")) == "qOffsets") {
									auto BADDR = [](auto bptr) { return bptr << 2; };
									auto BSTR = [](auto bstr) { return std::string(reinterpret_cast<char*>(bstr) + 1, bstr[0]); };
									// from debug.cpp@show_exec_tasks
									auto execbase = get_long_debug(4);
									auto ThisTask = get_long_debug(execbase + 276);
									response += "E01";
									if(ThisTask) {
										auto ln_Name = reinterpret_cast<char*>(get_real_address_debug(get_long_debug(ThisTask + 10)));
										barto_log("GDBSERVER: ln_Name = %s\n", ln_Name);
										auto ln_Type = get_byte_debug(ThisTask + 8);
										bool process = ln_Type == 13; // NT_PROCESS
										sections.clear();
										if(process) {
											constexpr auto sizeofLN = 14;
											// not correct when started from CLI
											auto tc_SPLower = get_long_debug(ThisTask + sizeofLN + 44);
											auto tc_SPUpper = get_long_debug(ThisTask + sizeofLN + 48) - 2;
											stackLower = tc_SPLower;
											stackUpper = tc_SPUpper;
											//auto pr_StackBase = BADDR(get_long_debug(ThisTask + 144));
											//stackUpper = pr_StackBase;

											systemStackLower = get_long_debug(execbase + 58);
											systemStackUpper = get_long_debug(execbase + 54);
											auto pr_SegList = BADDR(get_long_debug(ThisTask + 128));
											// not correct when started from CLI
											auto numSegLists = get_long_debug(pr_SegList + 0);
											auto segList = BADDR(get_long_debug(pr_SegList + 12)); // from debug.cpp@debug()
											auto pr_CLI = BADDR(get_long_debug(ThisTask + 172));
											int pr_TaskNum = get_long_debug(ThisTask + 140);
											if(pr_CLI && pr_TaskNum) {
												auto cli_CommandName = BSTR(get_real_address_debug(BADDR(get_long_debug(pr_CLI + 16))));
												barto_log("GDBSERVER: cli_CommandName = %s\n", cli_CommandName.c_str());
												segList = BADDR(get_long_debug(pr_CLI + 60));
												// don't know how to get the real stack except reading current stack pointer
												auto pr_StackSize = get_long_debug(ThisTask + 132);
												stackUpper = m68k_areg(regs, A7 - A0);
												stackLower = stackUpper - pr_StackSize;
											}
											baseText = 0;
											for(int i = 0; segList; i++) {
												auto size = get_long_debug(segList - 4) - 4;
												auto base = segList + 4;
												if(i == 0) {
													baseText = base;
													sizeText = size;
												}
												if(i == 0)
													response = "$";
												else
													response += ";";
												// this is non-standard (we report addresses of all segments), works only with modified gdb
												response += hex32(base);
												sections.push_back(base);
												barto_log("GDBSERVER:   base=%x; size=%x\n", base, size);
												segList = BADDR(get_long_debug(segList));
											}
										}
									}
								} else if(request.substr(0, strlen("qRcmd,")) == "qRcmd,") {
									// "monitor" command. used for profiling
									auto cmd = from_hex(request.substr(strlen("qRcmd,")));
									barto_log("GDBSERVER:   monitor %s\n", cmd.c_str());
									// syntax: monitor profile <num_frames> <unwind_file> <out_file>
									if(cmd.substr(0, strlen("profile")) == "profile") {
										auto s = cmd.substr(strlen("profile "));
										std::string profile_unwindname;
										profile_num_frames = 0;
										profile_outname.clear();

										// get num_frames
										while(s[0] >= '0' && s[0] <= '9') {
											profile_num_frames = profile_num_frames * 10 + s[0] - '0';
											s = s.substr(1);
										}
										profile_num_frames = max(1, min(100, profile_num_frames));
										s = s.substr(1); // skip space

										// get profile_unwindname
										if(s.substr(0, 1) == "\"") {
											auto last = s.find('\"', 1);
											if(last != std::string::npos) {
												profile_unwindname = s.substr(1, last - 1);
												s = s.substr(last + 1);
											} else {
												s.clear();
											}
										} else {
											auto last = s.find(' ', 1);
											if(last != std::string::npos) {
												profile_unwindname = s.substr(0, last);
												s = s.substr(last + 1);
											} else {
												s.clear();
											}
										}

										s = s.substr(1); // skip space

										// get profile_outname
										if(s.substr(0, 1) == "\"") {
											auto last = s.find('\"', 1);
											if(last != std::string::npos) {
												profile_outname = s.substr(1, last - 1);
												s = s.substr(last + 1);
											} else {
												s.clear();
											}
										} else {
											profile_unwindname = s.substr(1);
										}

										if(!profile_unwindname.empty() && !profile_outname.empty()) {
											if(auto f = fopen(profile_unwindname.c_str(), "rb")) {
												profile_unwind = std::make_unique<cpu_profiler_unwind[]>(sizeText >> 1);
												fread(profile_unwind.get(), sizeof(cpu_profiler_unwind), sizeText >> 1, f);
												fclose(f);
												send_ack(ack);
												profile_frame_count = 0;
												debugger_state = state::profile;
												deactivate_debugger();
												return; // response is sent when profile is finished (vsync)
											}
										}
									} else if(cmd == "reset") {
										savestate_quick(0, 0); // restore state saved at process entry
										response += "OK";
									} else {
										// unknown monitor command
										response += "E01";
									}
								} else if(request.substr(0, strlen("vCont?")) == "vCont?") {
									response += "vCont;c;C;s;S;t;r";
								} else if(request.substr(0, strlen("vCont;")) == "vCont;") {
									auto actions = request.substr(strlen("vCont;"));
									while(!actions.empty()) {
										std::string action;
										// split actions by ';'
										auto semi = actions.find(';');
										if(semi != std::string::npos) {
											action = actions.substr(0, semi);
											actions = actions.substr(semi + 1);
										} else {
											action = actions;
											actions.clear();
										}
										// thread specified by ':'
										auto colon = action.find(':');
										if(colon != std::string::npos) {
											// ignore thread ID
											action = action.substr(0, colon);
										}

										// hmm.. what to do with multiple actions?!

										if(action == "s") { // single-step
											// step over - GDB does this in a different way
											//auto pc = M68K_GETPC;
											//decltype(pc) nextpc;
											//m68k_disasm(pc, &nextpc, pc, 1);
											//trace_mode = TRACE_MATCH_PC;
											//trace_param1 = nextpc;

											// step in
											trace_param1 = 1;
											trace_mode = TRACE_SKIP_INS;

											exception_debugging = 1;
											debugger_state = state::connected;
											send_ack(ack);
											return;
										} else if(action == "c") { // continue
											debugger_state = state::connected;
											deactivate_debugger();
											// none work...
											//SetWindowPos(AMonitors[0].hAmigaWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE); // bring window to top
											//BringWindowToTop(AMonitors[0].hAmigaWnd);
											//SetForegroundWindow(AMonitors[0].hAmigaWnd);
											//setmouseactive(0, 2);
											send_ack(ack);
											return;
										} else if(action[0] == 'r') { // keep stepping in range
											auto comma = action.find(',', 3);
											if(comma != std::string::npos) {
												uaecptr start = strtoul(action.data() + 1, nullptr, 16);
												uaecptr end = strtoul(action.data() + comma + 1, nullptr, 16);
												trace_mode = TRACE_NRANGE_PC;
												trace_param1 = start;
												trace_param2 = end;
												debugger_state = state::connected;
												send_ack(ack);
												return;
											}
										} else {
											barto_log("GDBSERVER: unknown vCont action: %s\n", action.c_str());
										}
									}
								} else if(request[0] == 'H') {
									response += "OK";
								} else if(request[0] == 'T') {
									response += "OK";
/*								} else if(request.substr(0, strlen("vRun")) == "vRun") {
									debugger_state = state::wait_for_process;
									activate_debugger();
									send_ack(ack);
									return;
*/								} else if(request[0] == 'D') { // detach
									response += "OK";
/*								} else if(request[0] == '!') { // enable extended mode
									response += "OK";
*/								} else if(request[0] == '?') { // reason for stopping
									response += "S05"; // SIGTRAP
								} else if(request[0] == 's') { // single-step
									assert(!"should have used vCont;s");
								} else if(request[0] == 'c') { // continue
									assert(!"should have used vCont;c");
								} else if(request[0] == 'k') { // kill
									uae_quit();
									deactivate_debugger();
									return;
								} else if(request.substr(0, 2) == "Z0") { // set software breakpoint
									auto comma = request.find(',', strlen("Z0"));
									if(comma != std::string::npos) {
										uaecptr adr = strtoul(request.data() + strlen("Z0,"), nullptr, 16);
										if(adr == 0xffffffff) {
											// step out of kickstart
											trace_mode = TRACE_RANGE_PC;
											trace_param1 = 0;
											trace_param2 = 0xF80000;
											response += "OK";
										} else {
											for(auto& bpn : bpnodes) {
												if(bpn.enabled)
													continue;
												bpn.value1 = adr;
												bpn.type = BREAKPOINT_REG_PC;
												bpn.oper = BREAKPOINT_CMP_EQUAL;
												bpn.enabled = 1;
												trace_mode = 0;
												print_breakpoints();
												response += "OK";
												break;
											}
											// TODO: error when too many breakpoints!
										}
									} else
										response += "E01";
								} else if(request.substr(0, 2) == "z0") { // clear software breakpoint
									auto comma = request.find(',', strlen("z0"));
									if(comma != std::string::npos) {
										uaecptr adr = strtoul(request.data() + strlen("z0,"), nullptr, 16);
										if(adr == 0xffffffff) {
											response += "OK";
										} else {
											for(auto& bpn : bpnodes) {
												if(bpn.enabled && bpn.value1 == adr) {
													bpn.enabled = 0;
													trace_mode = 0;
													print_breakpoints();
													response += "OK";
													break;
												}
											}
											// TODO: error when breakpoint not found
										}
									} else
										response += "E01";
								} else if(request.substr(0, 2) == "Z2" || request.substr(0, 2) == "Z3" || request.substr(0, 2) == "Z4") { // Z2: write watchpoint, Z3: read watchpoint, Z4: access watchpoint
									int rwi = 0;
									if(request[1] == '2')
										rwi = 2; // write
									else if(request[1] == '3')
										rwi = 1; // read
									else
										rwi = 1 | 2; // read + write
									auto comma = request.find(',', strlen("Z2"));
									auto comma2 = request.find(',', strlen("Z2,"));
									if(comma != std::string::npos && comma2 != std::string::npos) {
										uaecptr adr = strtoul(request.data() + strlen("Z2,"), nullptr, 16);
										int size = strtoul(request.data() + comma2 + 1, nullptr, 16);
										barto_log("GDBSERVER: write watchpoint at 0x%x, size 0x%x\n", adr, size);
										for(auto& mwn : mwnodes) {
											if(mwn.size)
												continue;
											mwn.addr = adr;
											mwn.size = size;
											mwn.rwi = rwi;
											// defaults from debug.cpp@memwatch()
											mwn.val_enabled = 0;
											mwn.val_mask = 0xffffffff;
											mwn.val = 0;
											mwn.access_mask = MW_MASK_ALL;
											mwn.reg = 0xffffffff;
											mwn.frozen = 0;
											mwn.modval_written = 0;
											mwn.mustchange = 0;
											mwn.bus_error = 0;
											mwn.reportonly = false;
											mwn.nobreak = false;
											print_watchpoints();
											response += "OK";
											break;
										}
										memwatch_setup();
										// TODO: error when too many watchpoints!
									} else
										response += "E01";
								} else if(request.substr(0, 2) == "z2" || request.substr(0, 2) == "z3" || request.substr(0, 2) == "z4") { // Z2: clear write watchpoint, Z3: clear read watchpoint, Z4: clear access watchpoint
									auto comma = request.find(',', strlen("z2"));
									if(comma != std::string::npos) {
										uaecptr adr = strtoul(request.data() + strlen("z2,"), nullptr, 16);
										for(auto& mwn : mwnodes) {
											if(mwn.size && mwn.addr == adr) {
												mwn.size = 0;
												trace_mode = 0;
												print_watchpoints();
												response += "OK";
												break;
											}
											// TODO: error when watchpoint not found
										}
										memwatch_setup();
									} else
										response += "E01";
								} else if(request[0] == 'g') { // get registers
									response += get_registers();
								} else if(request[0] == 'p') { // get register
									response += get_register(strtoul(request.data() + 1, nullptr, 16));
								} else if(request[0] == 'm') { // read memory
									auto comma = request.find(',');
									if(comma != std::string::npos) {
										std::string mem;
										uaecptr adr = strtoul(request.data() + strlen("m"), nullptr, 16);
										int len = strtoul(request.data() + comma + 1, nullptr, 16);
										barto_log("GDBSERVER: want 0x%x bytes at 0x%x\n", len, adr);
										while(len-- > 0) {
											auto debug_read_memory_8_no_custom = [](uaecptr addr) -> int {
												addrbank* ad;
												ad = &get_mem_bank(addr);
												if(ad && ad != &custom_bank)
													return ad->bget(addr);
												return -1;
											};

											auto data = debug_read_memory_8_no_custom(adr);
											if(data == -1) {
												barto_log("GDBSERVER: error reading memory at 0x%x\n", len, adr);
												response += "E01";
												mem.clear();
												break;
											}
											data &= 0xff; // custom_bget seems to have a problem?
											mem += hex[data >> 4];
											mem += hex[data & 0xf];
											adr++;
										}
										response += mem;
									} else
										response += "E01";
								}
							} else
								barto_log("GDBSERVER: packet checksum mismatch: got %c%c, want %c%c\n", tolower(request[end + 1]), tolower(request[end + 2]), hex[cksum >> 4], hex[cksum & 0xf]);
						} else
							barto_log("GDBSERVER: packet checksum missing\n");
					} else
						barto_log("GDBSERVER: packet end marker '#' not found\n");
				}

				send_ack(ack);
				send_response(response);
			} else if(result == 0) {
				disconnect();
			} else {
				barto_log(_T("GDBSERVER: error receiving data: %d\n"), WSAGetLastError());
				disconnect();
			}
		}
		if(!is_connected()) {
			debugger_state = state::inited;
			close();
			deactivate_debugger();
		}
	}

	void vsync_pre() {
		if(!(currprefs.debugging_features & (1 << 2))) // "gdbserver"
			return;

		static uae_u32 profile_start_cycles{};
		static uae_u16 profile_dmacon{};
		static uae_u16 profile_custom_regs[256]{}; // at start of profile 
		static FILE* profile_outfile{};

		if(debugger_state == state::profile) {
start_profile:
			// start profiling
			barto_log("PRF: %d/%d\n", profile_frame_count + 1, profile_num_frames);
			if(profile_frame_count == 0) {
				profile_outfile = fopen(profile_outname.c_str(), "wb");
				if(!profile_outfile) {
					send_response("$E01");
					debugger_state = state::debugging;
					activate_debugger();
					return;
				}
				int section_count = (int)sections.size();
				fwrite(&profile_num_frames, sizeof(int), 1, profile_outfile);
				fwrite(&section_count, sizeof(int), 1, profile_outfile);
				fwrite(sections.data(), sizeof(uint32_t), section_count, profile_outfile);
				fwrite(&systemStackLower, sizeof(uint32_t), 1, profile_outfile);
				fwrite(&systemStackUpper, sizeof(uint32_t), 1, profile_outfile);
				fwrite(&stackLower, sizeof(uint32_t), 1, profile_outfile);
				fwrite(&stackUpper, sizeof(uint32_t), 1, profile_outfile);

				// store chipmem
				auto profile_chipmem_size = chipmem_bank.allocated_size;
				auto profile_chipmem = std::make_unique<uint8_t[]>(profile_chipmem_size);
				memcpy(profile_chipmem.get(), chipmem_bank.baseaddr, profile_chipmem_size);

				// store bogomem
				auto profile_bogomem_size = bogomem_bank.allocated_size;
				auto profile_bogomem = std::make_unique<uint8_t[]>(profile_bogomem_size);
				memcpy(profile_bogomem.get(), bogomem_bank.baseaddr, profile_bogomem_size);

				// memory
				fwrite(&profile_chipmem_size, sizeof(profile_chipmem_size), 1, profile_outfile);
				fwrite(profile_chipmem.get(), 1, profile_chipmem_size, profile_outfile);
				fwrite(&profile_bogomem_size, sizeof(profile_bogomem_size), 1, profile_outfile);
				fwrite(profile_bogomem.get(), 1, profile_bogomem_size, profile_outfile);

				// CPU information
				fwrite(&baseclock, sizeof(int), 1, profile_outfile);
				fwrite(&cpucycleunit, sizeof(int), 1, profile_outfile);
			}

			// store DMACON
			profile_dmacon = dmacon;

			// store custom registers
			for(int i = 0; i < _countof(custom_storage); i++)
				profile_custom_regs[i] = custom_storage[i].value;

			// reset idle
			if(barto_debug_idle_count > 0) {
				barto_debug_idle[0] = barto_debug_idle[barto_debug_idle_count - 1] & 0x80000000;
				barto_debug_idle_count = 1;
			}

			// start profiler
			start_cpu_profiler(baseText, baseText + sizeText, profile_unwind.get());
			debug_dma = 1;
			profile_start_cycles = get_cycles() / cpucycleunit;
			//barto_log("GDBSERVER: Start CPU Profiler @ %u cycles\n", get_cycles() / cpucycleunit);
			debugger_state = state::profiling;
		} else if(debugger_state == state::profiling) {
			profile_frame_count++;
			// end profiling
			stop_cpu_profiler();
			debug_dma = 0;
			uae_u32 profile_end_cycles = get_cycles() / cpucycleunit;
			//barto_log("GDBSERVER: Stop CPU Profiler @ %u cycles => %u cycles\n", profile_end_cycles, profile_end_cycles - profile_start_cycles);

			// process dma records
			static constexpr int NR_DMA_REC_HPOS_IN = 256, NR_DMA_REC_VPOS_IN = 1000;
			static constexpr int NR_DMA_REC_HPOS_OUT = 227, NR_DMA_REC_VPOS_OUT = 313;
			auto dma_in = get_dma_records();
			auto dma_out = std::make_unique<dma_rec[]>(NR_DMA_REC_HPOS_OUT * NR_DMA_REC_VPOS_OUT);
			for(size_t y = 0; y < NR_DMA_REC_VPOS_OUT; y++) {
				for(size_t x = 0; x < NR_DMA_REC_HPOS_OUT; x++) {
					dma_out[y * NR_DMA_REC_HPOS_OUT + x] = dma_in[y * NR_DMA_REC_HPOS_IN + x];
				}
			}

			int profile_cycles = profile_end_cycles - profile_start_cycles;

			// calculate idle cycles
			int idle_cycles = 0;
			int last_idle = 0;
			for(int i = 0; i < barto_debug_idle_count; i++) {
				auto this_idle = barto_debug_idle[i];
				if((last_idle & 0x80000000) && !(this_idle & 0x80000000)) { // idle->busy
					idle_cycles += (this_idle & 0x7fffffff) - max(profile_start_cycles, (last_idle & 0x7fffffff));
				}

				if((this_idle ^ last_idle) & 0x80000000)
					last_idle = this_idle;
			}
			if(last_idle & 0x80000000)
				idle_cycles += profile_end_cycles - max(profile_start_cycles, (last_idle & 0x7fffffff));
			//barto_log("idle_cycles: %d\n", idle_cycles);

			fwrite(&profile_dmacon, sizeof(profile_dmacon), 1, profile_outfile);
			fwrite(&profile_custom_regs, sizeof(uae_u16), _countof(profile_custom_regs), profile_outfile);

			// DMA
			int dmarec_size = sizeof(dma_rec);
			int dmarec_count = NR_DMA_REC_HPOS_OUT * NR_DMA_REC_VPOS_OUT;
			fwrite(&dmarec_size, sizeof(int), 1, profile_outfile);
			fwrite(&dmarec_count, sizeof(int), 1, profile_outfile);
			fwrite(dma_out.get(), sizeof(dma_rec), NR_DMA_REC_HPOS_OUT * NR_DMA_REC_VPOS_OUT, profile_outfile);

			// resources
			int resource_size = sizeof(barto_debug_resource);
			int resource_count = barto_debug_resources_count;
			fwrite(&resource_size, sizeof(int), 1, profile_outfile);
			fwrite(&resource_count, sizeof(int), 1, profile_outfile);
			fwrite(barto_debug_resources, resource_size, resource_count, profile_outfile);

			fwrite(&profile_cycles, sizeof(int), 1, profile_outfile);
			fwrite(&idle_cycles, sizeof(int), 1, profile_outfile);

			// profiles
			int profile_count = get_cpu_profiler_output_count();
			fwrite(&profile_count, sizeof(int), 1, profile_outfile);
			fwrite(get_cpu_profiler_output(), sizeof(uae_u32), profile_count, profile_outfile);
			// write JPEG screenshot
			if(profile_num_frames > 1) {
				int monid = getfocusedmonitor();
				int imagemode = 1;
				if(screenshot_prepare(monid, imagemode) == 1) {
					auto bi = screenshot_get_bi();
					auto bi_bits = (const uint8_t*)screenshot_get_bits();
					if(bi->bmiHeader.biBitCount == 24) {
						// need to flip bits and swap rgb channels
						const auto w = bi->bmiHeader.biWidth;
						const auto h = bi->bmiHeader.biHeight;
						auto bits = std::make_unique<uint8_t[]>(w * 3 * h);
						for(int y = 0; y < bi->bmiHeader.biHeight; y++) {
							for(int x = 0; x < bi->bmiHeader.biWidth; x++) {
								bits[y * w * 3 + x * 3 + 0] = bi_bits[(h - 1 - y) * w * 3 + x * 3 + 2];
								bits[y * w * 3 + x * 3 + 1] = bi_bits[(h - 1 - y) * w * 3 + x * 3 + 1];
								bits[y * w * 3 + x * 3 + 2] = bi_bits[(h - 1 - y) * w * 3 + x * 3 + 0];
							}
						}
						struct write_context_t {
							uint8_t data[1'000'000]{};
							int size = 0;
						};
						auto write_context = std::make_unique<write_context_t>();
						auto write_func = [](void* _context, void* data, int size) {
							auto context = (write_context_t*)_context;
							memcpy(&context->data[context->size], data, size);
							context->size += size;
						};
						stbi_write_jpg_to_func(write_func, write_context.get(), w, h, 3, bits.get(), 50);
						write_context->size = (write_context->size + 3) & ~3; // pad to 32bit
						fwrite(&write_context->size, sizeof(int), 1, profile_outfile);
						fwrite(write_context->data, 1, write_context->size, profile_outfile);
					}
				}
			} else {
				int screenshot_size = 0;
				fwrite(&screenshot_size, sizeof(int), 1, profile_outfile);
			}

			if(profile_frame_count == profile_num_frames) {
				fclose(profile_outfile);
				send_response("$OK");

				debugger_state = state::debugging;
				activate_debugger();
			} else {
				debugger_state = state::profile;
				goto start_profile;
			}
		}

		if(debugger_state == state::connected && data_available()) {
			handle_packet();
		}
	}

	void vsync_post() {
		if(!(currprefs.debugging_features & (1 << 2))) // "gdbserver"
			return;
	}

	uaecptr KPutCharX{};
	uaecptr Trap7{};
	uaecptr AddressError{};
	uaecptr IllegalError{};
	std::string KPutCharOutput;

	void output(const char* string) {
		if(gdbconn != INVALID_SOCKET && !in_handle_packet) {
			std::string response = "$O";
			while(*string)
				response += hex8(*string++);
			send_response(response);
		}
	}

	void log_output(const TCHAR* tstring) {
		auto utf8 = string_to_utf8(tstring);
		if(utf8.substr(0, 5) == "DBG: ") {
			utf8 = utf8.substr(0, utf8.length() - 1); // get rid of extra newline from uaelib
			for(size_t start = 0;;) { // append "DBG: " to every newline, because GDB splits text by lines and vscode doesn't know that the extra lines are DBG output
				auto p = utf8.find('\n', start);
				if(p == std::string::npos || p == utf8.length() - 1)
					break;

				utf8.replace(p, 1, "\nDBG: ");
				start = p + 6;
			}

		}
		output(utf8.c_str());
	}

	void barto_log(const char* format, ...) {
		char buffer[1024];
		va_list parms;
		va_start(parms, format);
		vsprintf(buffer, format, parms);
		OutputDebugStringA(buffer);
		output(buffer);
		va_end(parms);
	}

	void barto_log(const wchar_t* format, ...) {
		wchar_t buffer[1024];
		va_list parms;
		va_start(parms, format);
		vswprintf(buffer, format, parms);
		OutputDebugStringW(buffer);
		output(string_to_utf8(buffer).c_str());
		va_end(parms);
	}

	// returns true if gdbserver handles debugging
	bool debug() {
		if(!(currprefs.debugging_features & (1 << 2))) // "gdbserver"
			return false;

		// break at start of process
		if(debugger_state == state::inited) {
			//KPutCharX
			auto execbase = get_long_debug(4);
			KPutCharX = execbase - 0x204;
			for (auto& bpn : bpnodes) {
				if (bpn.enabled)
					continue;
				bpn.value1 = KPutCharX;
				bpn.type = BREAKPOINT_REG_PC;
				bpn.oper = BREAKPOINT_CMP_EQUAL;
				bpn.enabled = 1;
				barto_log("GDBSERVER: Breakpoint for KPutCharX at 0x%x installed\n", bpn.value1);
				break;
			}

			// TRAP#7 breakpoint (GCC generates this opcode when it encounters undefined behavior)
			Trap7 = get_long_debug(regs.vbr + 0x9c);
			for(auto& bpn : bpnodes) {
				if(bpn.enabled)
					continue;
				bpn.value1 = Trap7;
				bpn.type = BREAKPOINT_REG_PC;
				bpn.oper = BREAKPOINT_CMP_EQUAL;
				bpn.enabled = 1;
				barto_log("GDBSERVER: Breakpoint for TRAP#7 at 0x%x installed\n", bpn.value1);
				break;
			}

			AddressError = get_long_debug(regs.vbr + 3 * 4);
			for(auto& bpn : bpnodes) {
				if(bpn.enabled)
					continue;
				bpn.value1 = AddressError;
				bpn.type = BREAKPOINT_REG_PC;
				bpn.oper = BREAKPOINT_CMP_EQUAL;
				bpn.enabled = 1;
				barto_log("GDBSERVER: Breakpoint for AddressError at 0x%x installed\n", bpn.value1);
				break;
			}

			IllegalError = get_long_debug(regs.vbr + 4 * 4);
			for(auto& bpn : bpnodes) {
				if(bpn.enabled)
					continue;
				bpn.value1 = IllegalError;
				bpn.type = BREAKPOINT_REG_PC;
				bpn.oper = BREAKPOINT_CMP_EQUAL;
				bpn.enabled = 1;
				barto_log("GDBSERVER: Breakpoint for IllegalError at 0x%x installed\n", bpn.value1);
				break;
			}

			// watchpoint for NULL (GCC sees this as undefined behavior)
			// disabled for now, always triggered in OpenScreen()
			/*for(auto& mwn : mwnodes) {
				if(mwn.size)
					continue;
				mwn.addr = 0;
				mwn.size = 4;
				mwn.rwi = 1 | 2; // read + write
				// defaults from debug.cpp@memwatch()
				mwn.val_enabled = 0;
				mwn.val_mask = 0xffffffff;
				mwn.val = 0;
				mwn.access_mask = MW_MASK_CPU_D_R | MW_MASK_CPU_D_W; // CPU data read/write only
				mwn.reg = 0xffffffff;
				mwn.frozen = 0;
				mwn.modval_written = 0;
				mwn.mustchange = 0;
				mwn.bus_error = 0;
				mwn.reportonly = false;
				mwn.nobreak = false;
				memwatch_setup();
				barto_log("GDBSERVER: Watchpoint for NULL installed\n");
				break;
			}*/

			// enable break at exceptions - doesn't break when exceptions occur in Kickstart
			debug_illegal = 1;
			debug_illegal_mask = (1 << 3) || (1 << 4); // 3 = address error, 4 = illegal instruction

			warpmode(0);
			// from debug.cpp@process_breakpoint()
			processptr = 0;
			xfree(processname);
			processname = nullptr;
			savestate_quick(0, 1); // save state for "monitor reset"
			barto_log("GDBSERVER: Waiting for connection...\n");
			while(!is_connected()) {
				barto_log(".");
				Sleep(100);
			}
			barto_log("\n");
			useAck = true;
			debugger_state = state::debugging;
			debugmem_enable_stackframe(true);
			debugmem_trace = true;
		}

		// something stopped execution and entered debugger
		if(debugger_state == state::connected) {
//while(!IsDebuggerPresent()) Sleep(100); __debugbreak();
			auto pc = munge24(m68k_getpc());
			if (pc == KPutCharX) {
				// if this is too slow, hook uaelib trap#86
				auto ascii = static_cast<uint8_t>(m68k_dreg(regs, 0));
				KPutCharOutput += ascii;
				if(ascii == '\0') {
					std::string response = "$O";
					for(const auto& ch : KPutCharOutput)
						response += hex8(ch);
					send_response(response);
					KPutCharOutput.clear();
				}
				deactivate_debugger();
				return true;
			}

			std::string response{ "S05" };

			//if(memwatch_triggered) // can't use, debug() will reset it, so just check mwhit
			if(mwhit.size) {
				for(const auto& mwn : mwnodes) {
					if(mwn.size && mwhit.addr >= mwn.addr && mwhit.addr < mwn.addr + mwn.size) {
						if(mwn.addr == 0) {
							response = "S0B"; // undefined behavior -> SIGSEGV
						} else {
//while(!IsDebuggerPresent()) Sleep(100); __debugbreak();
//							auto data = get_long_debug(mwn.addr);
							response = "T05";
							if(mwhit.rwi == 2)
								response += "watch";
							else if(mwhit.rwi == 1)
								response += "rwatch";
							else
								response += "awatch";
							response += ":";
							response += hex32(mwhit.addr);
							response += ";";
						}
						// so we don't trigger again
						mwhit.size = 0;
						mwhit.addr = 0;
						goto send_response;
					}
				}
			}
			for(const auto& bpn : bpnodes) {
				if(bpn.enabled && bpn.type == BREAKPOINT_REG_PC && bpn.value1 == pc) {
					// see binutils-gdb/include/gdb/signals.def for number of signals
					if(pc == Trap7) {
						response = "S07"; // TRAP#7 -> SIGEMT
						// unwind PC & stack for better debugging experience (otherwise we're probably just somewhere in Kickstart)
						regs.pc = regs.instruction_pc_user_exception - 2;
						m68k_areg(regs, A7 - A0) = regs.usp;
					} else if(pc == AddressError) {
						response = "S0A"; // AddressError -> SIGBUS
						// unwind PC & stack for better debugging experience (otherwise we're probably just somewhere in Kickstart)
						regs.pc = regs.instruction_pc_user_exception; // don't know size of opcode that caused exception
						m68k_areg(regs, A7 - A0) = regs.usp;
					} else if(pc == IllegalError) {
						response = "S04"; // AddressError -> SIGILL
						// unwind PC & stack for better debugging experience (otherwise we're probably just somewhere in Kickstart)
						regs.pc = regs.instruction_pc_user_exception; // don't know size of opcode that caused exception
						m68k_areg(regs, A7 - A0) = regs.usp;
					} else {
						response = "T05swbreak:;";
					}
					goto send_response;
				}
			}
send_response:
			send_response("$" + response);
			trace_mode = 0;
			debugger_state = state::debugging;
		}

		// debugger active
		while(debugger_state == state::debugging) {
			handle_packet();

			MSG msg{};
			while(PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
			Sleep(1);
		}

		return true;
	}
} // namespace barto_gdbserver
