#pragma once

namespace barto_gdbserver {
	bool init();
	void close();
	void vsync_pre();
	void vsync_post();
	bool debug();
}