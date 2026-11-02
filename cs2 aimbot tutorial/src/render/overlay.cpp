#include "overlay.hpp"

#include "../cheat/entity.hpp"
#include "../math/vector.hpp"

#include <atomic>
#include <chrono>
#include <thread>

namespace esp
{
	namespace
	{
		std::atomic<bool> g_running{ false };
		std::thread g_thread;

		constexpr wchar_t kWindowClass[] = L"cs2_external_overlay";
		constexpr DWORD kColorKey = RGB(0, 0, 0); // never draw in pure black

		struct OverlayState
		{
			HWND game = nullptr;
			HWND window = nullptr;

			HDC memoryDc = nullptr;
			HBITMAP bitmap = nullptr;
			HBITMAP oldBitmap = nullptr;
			int width = 0;
			int height = 0;
		};

		BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM param)
		{
			if (!IsWindowVisible(hwnd))
				return TRUE;

			char title[256]{};
			GetWindowTextA(hwnd, title, sizeof(title));

			char className[256]{};
			GetClassNameA(hwnd, className, sizeof(className));

			// title can be empty while the game is loading, so accept the SDL window class too
			if (strstr(title, "Counter-Strike 2") || strcmp(className, "SDL_app") == 0)
			{
				*reinterpret_cast<HWND*>(param) = hwnd;
				return FALSE;
			}

			return TRUE;
		}

		HWND FindGameWindow()
		{
			HWND found = nullptr;
			EnumWindows(FindGameWindowProc, reinterpret_cast<LPARAM>(&found));
			return found;
		}

		HWND CreateOverlayWindow()
		{
			WNDCLASSEXW wc{};
			wc.cbSize = sizeof(wc);
			wc.lpfnWndProc = DefWindowProcW; // click-through window never sees input anyway
			wc.hInstance = GetModuleHandleW(nullptr);
			wc.lpszClassName = kWindowClass;

			static bool registered = false;

			if (!registered)
			{
				registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;

				if (!registered)
					return nullptr;
			}

			const HWND window = CreateWindowExW(
				WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
				kWindowClass, L"", WS_POPUP,
				0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

			if (!window)
				return nullptr;

			// black = transparent, everything drawn on top stays opaque
			SetLayeredWindowAttributes(window, kColorKey, 0, LWA_COLORKEY);

			return window;
		}

		void ReleaseSurface(OverlayState& state)
		{
			if (state.memoryDc)
			{
				if (state.oldBitmap)
					SelectObject(state.memoryDc, state.oldBitmap);

				DeleteDC(state.memoryDc);
				state.memoryDc = nullptr;
				state.oldBitmap = nullptr;
			}

			if (state.bitmap)
			{
				DeleteObject(state.bitmap);
				state.bitmap = nullptr;
			}

			state.width = 0;
			state.height = 0;
		}

		bool EnsureSurface(OverlayState& state, int width, int height)
		{
			if (width <= 0 || height <= 0)
				return false;

			if (state.memoryDc && state.width == width && state.height == height)
				return true;

			ReleaseSurface(state);

			const HDC screen = GetDC(nullptr);
			state.memoryDc = CreateCompatibleDC(screen);
			state.bitmap = CreateCompatibleBitmap(screen, width, height);
			ReleaseDC(nullptr, screen);

			if (!state.memoryDc || !state.bitmap)
			{
				ReleaseSurface(state);
				return false;
			}

			state.oldBitmap = static_cast<HBITMAP>(SelectObject(state.memoryDc, state.bitmap));
			state.width = width;
			state.height = height;

			return true;
		}

		void DrawBox(HDC dc, const C_CSPlayerPawn& player, const view_matrix_t& viewMatrix, int width, int height)
		{
			Vector feet;
			Vector head;

			// cached world positions, projected against the overlay surface, not the desktop
			if (!Vector::world_to_screen(viewMatrix, player.Position, feet, width, height))
				return;

			if (!Vector::world_to_screen(viewMatrix, player.headPos, head, width, height))
				return;

			const float boxHeight = feet.y - head.y;

			// head below feet (behind camera edge cases) or absurd scale (right in the face)
			if (boxHeight < 8.f || boxHeight > static_cast<float>(height) * 2.5f)
				return;

			const float boxWidth = boxHeight * 0.45f; // ~player shoulder width

			const int left = static_cast<int>(head.x - boxWidth * 0.5f);
			const int top = static_cast<int>(head.y);
			const int right = static_cast<int>(head.x + boxWidth * 0.5f);
			const int bottom = static_cast<int>(feet.y);

			// fully off screen - nothing to draw
			if (right < 0 || left > width || bottom < 0 || top > height)
				return;

			// low hp turns the box red, otherwise green
			const COLORREF color = player.health <= 35 ? RGB(255, 70, 70) : RGB(0, 255, 120);

			const HPEN pen = CreatePen(PS_SOLID, 1, color);
			const HGDIOBJ oldPen = SelectObject(dc, pen);

			Rectangle(dc, left, top, right, bottom);

			SelectObject(dc, oldPen);
			DeleteObject(pen);
		}

		void DrawFrame(OverlayState& state)
		{
			const HBRUSH background = CreateSolidBrush(kColorKey);
			const RECT full{ 0, 0, state.width, state.height };
			FillRect(state.memoryDc, &full, background);
			DeleteObject(background);

			SelectObject(state.memoryDc, GetStockObject(NULL_BRUSH));

			const std::vector<C_CSPlayerPawn> players = reader.Snapshot();

			if (!players.empty() && reader.client)
			{
				const view_matrix_t viewMatrix = mem.Read<view_matrix_t>(reader.client + offset::dwViewMatrix);

				for (const auto& player : players)
				{
					if (!player.pCSPlayerPawn || player.health <= 0)
						continue;

					DrawBox(state.memoryDc, player, viewMatrix, state.width, state.height);
				}
			}

			const HDC windowDc = GetDC(state.window);
			BitBlt(windowDc, 0, 0, state.width, state.height, state.memoryDc, 0, 0, SRCCOPY);
			ReleaseDC(state.window, windowDc);
		}

		void ThreadMain()
		{
			OverlayState state;

			int frame = 0;

			while (g_running)
			{
				if (!state.game || !IsWindow(state.game))
				{
					state.game = FindGameWindow();

					if (!state.game)
					{
						if (frame++ % 30 == 0)
							printf("[esp] game window not found\n");

						std::this_thread::sleep_for(std::chrono::milliseconds(500));
						continue;
					}

					printf("[esp] game window %p\n", state.game);
				}

				if (!state.window)
				{
					state.window = CreateOverlayWindow();

					if (!state.window)
					{
						printf("[esp] overlay window creation failed, error %lu\n", GetLastError());
						std::this_thread::sleep_for(std::chrono::milliseconds(500));
						continue;
					}

					printf("[esp] overlay window %p\n", state.window);
				}

				MSG msg;

				while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
				{
					TranslateMessage(&msg);
					DispatchMessageW(&msg);
				}

				RECT client{};

				// compare against the game window itself - title matching for "foreground"
				// broke whenever the game was launched with a different caption
				const HWND foreground = GetForegroundWindow();
				const bool gameFocused = foreground == state.game;

				if (IsIconic(state.game) || !GetClientRect(state.game, &client) || !gameFocused)
				{
					ShowWindow(state.window, SW_HIDE);
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
					continue;
				}

				POINT origin{ client.left, client.top };
				ClientToScreen(state.game, &origin);

				const int width = client.right - client.left;
				const int height = client.bottom - client.top;

				SetWindowPos(state.window, HWND_TOPMOST, origin.x, origin.y, width, height,
					SWP_NOACTIVATE | SWP_SHOWWINDOW);
				ShowWindow(state.window, SW_SHOWNOACTIVATE);

				if (EnsureSurface(state, width, height))
					DrawFrame(state);

				if (++frame % 120 == 0)
				{
					const std::vector<C_CSPlayerPawn> players = reader.Snapshot();
					const view_matrix_t viewMatrix = mem.Read<view_matrix_t>(reader.client + offset::dwViewMatrix);

					printf("[esp] client %dx%d at %d,%d players=%zu\n", width, height, origin.x, origin.y, players.size());

					if (!players.empty())
					{
						Vector head;

						if (Vector::world_to_screen(viewMatrix, players[0].headPos, head, width, height))
							printf("[esp] first head world %.0f,%.0f,%.0f -> screen %.0f,%.0f\n",
								players[0].headPos.x, players[0].headPos.y, players[0].headPos.z, head.x, head.y);
						else
							printf("[esp] first head behind camera, world %.0f,%.0f,%.0f\n",
								players[0].headPos.x, players[0].headPos.y, players[0].headPos.z);
					}
				}

				std::this_thread::sleep_for(std::chrono::milliseconds(16));
			}

			ReleaseSurface(state);

			if (state.window)
				DestroyWindow(state.window);

			UnregisterClassW(kWindowClass, GetModuleHandleW(nullptr));
		}
	}

	void Start()
	{
		if (g_running.exchange(true))
			return;

		g_thread = std::thread(ThreadMain);
	}

	void Stop()
	{
		if (!g_running.exchange(false))
			return;

		if (g_thread.joinable())
			g_thread.join();
	}

	bool Running()
	{
		return g_running;
	}
}
