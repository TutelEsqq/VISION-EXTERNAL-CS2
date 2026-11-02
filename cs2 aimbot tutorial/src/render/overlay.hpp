#pragma once

namespace esp
{
	// spawns the overlay thread; it owns its window, its message pump and the drawing
	void Start();
	void Stop();
	bool Running();
}
