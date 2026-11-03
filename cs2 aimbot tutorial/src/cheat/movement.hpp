#pragma once

#include "../render/settings.hpp"

#include <Windows.h>
#include <string>
#include <chrono>

// Auto-Stop: when the player releases all movement keys after moving, the
// opposite keys are briefly pressed (W+D -> S+A) so the game counter-strafes
// and the player stops almost instantly.
namespace movement
{
	namespace
	{
		enum class State
		{
			Idle,
			Stopping
		};

		State g_state = State::Idle;
		int g_injected[2] = { 0, 0 };   // the two vks we are holding (0 = none)
		int g_injectedCount = 0;
		std::chrono::steady_clock::time_point g_deadline;
		bool g_prevMoving = false;

		bool KeyDown(int vk)
		{
			return (GetAsyncKeyState(vk) & 0x8000) != 0;
		}

		bool GameFocused()
		{
			char title[256]{};
			GetWindowTextA(GetForegroundWindow(), title, sizeof(title));
			return std::strstr(title, "Counter-Strike 2") != nullptr;
		}

		// VK + scancode together - the most compatible form for games
		void SendKey(int vk, bool down)
		{
			INPUT input{};
			input.type = INPUT_KEYBOARD;
			input.ki.wVk = static_cast<WORD>(vk);
			input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC));
			input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
			input.ki.time = 0;
			SendInput(1, &input, sizeof(INPUT));
		}

		void ReleaseInjected()
		{
			for (int i = 0; i < g_injectedCount; ++i)
				SendKey(g_injected[i], false);

			g_injectedCount = 0;
		}
	}

	inline void Tick()
	{
		if (!g_settings.autoStopEnabled)
		{
			if (g_state == State::Stopping)
			{
				ReleaseInjected();
				g_state = State::Idle;
				g_prevMoving = false;
			}

			return;
		}

		if (!GameFocused())
			return;

		const bool w = KeyDown('W');
		const bool a = KeyDown('A');
		const bool s = KeyDown('S');
		const bool d = KeyDown('D');
		const bool moving = w || a || s || d;

		if (g_state == State::Idle)
		{
			// transition: was moving, now fully released -> counter-strafe
			if (g_prevMoving && !moving)
			{
				g_injectedCount = 0;

				// opposite of whatever was held
				if (w) g_injected[g_injectedCount++] = 'S';
				if (s) g_injected[g_injectedCount++] = 'W';
				if (a) g_injected[g_injectedCount++] = 'D';
				if (d) g_injected[g_injectedCount++] = 'A';

				if (g_injectedCount > 0)
				{
					for (int i = 0; i < g_injectedCount; ++i)
						SendKey(g_injected[i], true);

					g_deadline = std::chrono::steady_clock::now() +
						std::chrono::milliseconds(g_settings.stopDurationMs);
					g_state = State::Stopping;

					char keys[16] = "??";
					size_t pos = 0;

					for (int i = 0; i < g_injectedCount && pos + 2 < sizeof(keys); ++i)
					{
						keys[pos++] = static_cast<char>(g_injected[i]);
						if (i + 1 < g_injectedCount) keys[pos++] = '+';
					}

					keys[pos] = 0;
					printf("[movement] counter-strafe: %s (%d ms)\n", keys, g_settings.stopDurationMs);
				}
			}

			g_prevMoving = moving;
		}
		else // Stopping
		{
			// player pressed a key we did not inject - get out of the way instantly
			bool userPressed = false;

			for (int vk : {'W', 'A', 'S', 'D'})
			{
				bool ours = false;

				for (int i = 0; i < g_injectedCount; ++i)
				{
					if (g_injected[i] == vk)
					{
						ours = true;
						break;
					}
				}

				if (!ours && KeyDown(vk))
				{
					userPressed = true;
					break;
				}
			}

			const bool timedOut =
				std::chrono::steady_clock::now() >= g_deadline;

			if (userPressed || timedOut)
			{
				ReleaseInjected();
				g_state = State::Idle;
				// seed prev state so the next release event triggers cleanly
				g_prevMoving = userPressed;
			}
		}
	}
}
