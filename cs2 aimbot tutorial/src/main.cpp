#include "cheat/entity.hpp"
#include "render/overlay.hpp"

int main()
{
	// must happen before any GetSystemMetrics / window call: if the process is DPI
	// virtualized later, w2s works in virtual pixels and the overlay in real ones
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	printf("Starting cheat\n");

	std::thread ReadThread(&Reader::ThreadLoop, &reader);
	ReadThread.detach();

	// overlay owns its own window, message pump and frame loop
	esp::Start();

	while (true)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));

		// run aimbot code
		aimbot.doAimbot();
	}
}
