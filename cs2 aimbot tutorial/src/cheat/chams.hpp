#pragma once

#include <cstdint>

// chams via material hook (external vtable hook + shellcode, adapted from the
// public approach by xsip). The hook runs INSIDE the game on the render thread
// and recolors every animatable scene object before the engine draws it.
namespace chams
{
	// one-time init: resolve scenesystem/materialsystem2, create the chams material,
	// install the render hook. Call repeatedly until it returns true.
	bool EnsureInit();

	// live controls (write into the remote hook data)
	void SetEnabled(bool enabled);
	void SetColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
}
