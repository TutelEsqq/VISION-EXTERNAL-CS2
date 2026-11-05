#include "overlay.hpp"

#include "../cheat/entity.hpp"
#include "../cheat/movement.hpp"
#include "../math/vector.hpp"
#include "../cheat/chams.hpp"
#include "settings.hpp"

#include <d3d11.h>
#include <dxgi.h>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include <atomic>
#include <chrono>
#include <thread>

// imgui's win32 backend exposes this for the message pump
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace esp
{
	namespace
	{
		std::atomic<bool> g_running{ false };
		std::thread g_thread;

		constexpr wchar_t kWindowClass[] = L"cs2_external_overlay";
		constexpr UINT kMenuToggleKey = VK_INSERT;

		struct OverlayState
		{
			HWND game = nullptr;
			HWND window = nullptr;

			bool menuOpen = false;

			ID3D11Device* device = nullptr;
			ID3D11DeviceContext* context = nullptr;
			IDXGISwapChain* swapChain = nullptr;
			ID3D11RenderTargetView* target = nullptr;
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

		// readable name for the key-binding menu; mouse4/5 get their gamer names
		const char* KeyName(int vk)
		{
			static char buf[32]{};

			switch (vk)
			{
			case VK_LBUTTON: return "LMB";
			case VK_RBUTTON: return "RMB";
			case VK_MBUTTON: return "MMB";
			case VK_XBUTTON1: return "MOUSE4";
			case VK_XBUTTON2: return "MOUSE5";
			}

			const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);

			if (GetKeyNameTextA(static_cast<LONG>(scan) << 16, buf, sizeof(buf)))
				return buf;

			return "?";
		}

		// click-through while the menu is closed; when it opens the style flag is dropped
		// so the imgui widgets can receive the mouse
		void SetClickThrough(HWND window, bool clickThrough)
		{
			const LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
			LONG_PTR updated = exStyle;

			if (clickThrough)
				updated |= WS_EX_TRANSPARENT;
			else
				updated &= ~WS_EX_TRANSPARENT;

			if (updated != exStyle)
				SetWindowLongPtrW(window, GWL_EXSTYLE, updated);
		}

		LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
		{
			if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
				return 1;

			switch (msg)
			{
			case WM_DESTROY:
				PostQuitMessage(0);
				return 0;
			}

			return DefWindowProcW(hwnd, msg, wParam, lParam);
		}

		HWND CreateOverlayWindow(HINSTANCE instance)
		{
			WNDCLASSEXW wc{};
			wc.cbSize = sizeof(wc);
			wc.lpfnWndProc = OverlayWndProc; // forwards input to imgui instead of swallowing it
			wc.hInstance = instance;
			wc.hCursor = LoadCursor(nullptr, IDC_ARROW); // visible arrow while the menu open (charset-agnostic)
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

			// colorkey: every pure black pixel the swapchain presents is transparent
			SetLayeredWindowAttributes(window, RGB(0, 0, 0), 0, LWA_COLORKEY);

			return window;
		}

		void ReleaseRenderTarget(OverlayState& state)
		{
			if (state.target)
			{
				state.target->Release();
				state.target = nullptr;
			}
		}

		void ReleaseD3D(OverlayState& state)
		{
			ReleaseRenderTarget(state);

			if (state.swapChain)
			{
				state.swapChain->Release();
				state.swapChain = nullptr;
			}

			if (state.context)
			{
				state.context->Release();
				state.context = nullptr;
			}

			if (state.device)
			{
				state.device->Release();
				state.device = nullptr;
			}

			state.width = 0;
			state.height = 0;
		}

		bool EnsureD3D(OverlayState& state, int width, int height)
		{
			if (width <= 0 || height <= 0)
				return false;

			if (state.device && state.width == width && state.height == height)
				return true;

			if (state.device)
			{
				// game window resized - rebuild the swapchain buffers, keep the device
				ReleaseRenderTarget(state);

				if (state.swapChain)
				{
					DXGI_SWAP_CHAIN_DESC desc{};
					state.swapChain->GetDesc(&desc);

					if (FAILED(state.swapChain->ResizeBuffers(0, width, height, desc.BufferDesc.Format, 0)))
					{
						printf("[esp] swapchain resize failed, restarting d3d\n");
						ReleaseD3D(state);
						ImGui_ImplDX11_Shutdown();
						ImGui_ImplWin32_Shutdown();
						ImGui::DestroyContext();
						return false; // retry on the next frame with a fresh device
					}
				}
			}

			if (!state.device)
			{
				DXGI_SWAP_CHAIN_DESC scd{};
				scd.BufferDesc.Width = width;
				scd.BufferDesc.Height = height;
				scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
				scd.SampleDesc.Count = 1;
				scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
				scd.BufferCount = 2;
				scd.OutputWindow = state.window;
				scd.Windowed = TRUE;
				scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
				scd.Flags = 0;

				const HRESULT hr = D3D11CreateDeviceAndSwapChain(
					nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
					nullptr, 0, D3D11_SDK_VERSION, &scd,
					&state.swapChain, &state.device, nullptr, &state.context);

				if (FAILED(hr) || !state.device || !state.swapChain || !state.context)
				{
					printf("[esp] d3d11 init failed, hr=0x%08lX\n", static_cast<unsigned long>(hr));
					ReleaseD3D(state);
					return false;
				}

				ImGui::CreateContext();
				ImGuiIO& io = ImGui::GetIO();
				io.IniFilename = nullptr; // do not litter the cheat folder with imgui.ini
				ImGui_ImplWin32_Init(state.window);
				ImGui_ImplDX11_Init(state.device, state.context);

				printf("[esp] d3d11 + imgui ready\n");
			}

			ID3D11Texture2D* backBuffer = nullptr;

			if (FAILED(state.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
				return false;

			const HRESULT hr = state.device->CreateRenderTargetView(backBuffer, nullptr, &state.target);
			backBuffer->Release();

			if (FAILED(hr))
				return false;

			state.width = width;
			state.height = height;
			return true;
		}

		// skeleton topology for the CS2 player rig (indices verified live:
		// 1 pelvis, 2-5 spine, 6 neck, 7 head, 9-11 left arm, 13-15 right arm,
		// 17-19 left leg, 20-22 right leg; bone 0 is the feet-level root -
		// linking it draws a stray tail between the legs, so it is not drawn)
		constexpr int kSkeletonLinks[][2] = {
			{1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 6}, {6, 7},
			{6, 9}, {9, 10}, {10, 11},
			{6, 13}, {13, 14}, {14, 15},
			{1, 17}, {17, 18}, {18, 19},
			{1, 20}, {20, 21}, {21, 22},
		};

		void DrawSkeleton(const C_CSPlayerPawn& player, const view_matrix_t& viewMatrix, ImU32 color, const ImVec2& display)
		{
			if (!player.bonesValid)
				return;

			ImDrawList* draw = ImGui::GetBackgroundDrawList();

			for (const auto& link : kSkeletonLinks)
			{
				const Vector& from = player.bones[link[0]];
				const Vector& to = player.bones[link[1]];

				// bones the model does not have read as zeroed - skip those chains
				if (from.IsZero() || to.IsZero() || !from.IsFinite() || !to.IsFinite())
					continue;

				Vector fromScreen;
				Vector toScreen;

				if (!Vector::world_to_screen(viewMatrix, from, fromScreen))
					continue;

				if (!Vector::world_to_screen(viewMatrix, to, toScreen))
					continue;

				// both endpoints far off screen - nothing visible to draw
				if ((fromScreen.x < -50.f && toScreen.x < -50.f) ||
					(fromScreen.x > display.x + 50.f && toScreen.x > display.x + 50.f) ||
					(fromScreen.y < -50.f && toScreen.y < -50.f) ||
					(fromScreen.y > display.y + 50.f && toScreen.y > display.y + 50.f))
					continue;

				draw->AddLine(ImVec2(fromScreen.x, fromScreen.y), ImVec2(toScreen.x, toScreen.y), color, 1.2f);
			}
		}

		// readable name for the common weapon ids (defindexes are stable since CS:GO)
		const char* WeaponDisplayName(uint16_t id)
		{
			switch (id)
			{
			case 1: return "Desert Eagle";
			case 2: return "Dual Berettas";
			case 3: return "Five-SeveN";
			case 4: return "Glock-18";
			case 7: return "AK-47";
			case 8: return "AUG";
			case 9: return "AWP";
			case 10: return "FAMAS";
			case 11: return "G3SG1";
			case 13: return "Galil AR";
			case 14: return "M249";
			case 16: return "M4A4";
			case 17: return "MAC-10";
			case 19: return "P90";
			case 23: return "MP5-SD";
			case 24: return "UMP-45";
			case 25: return "XM1014";
			case 26: return "PP-Bizon";
			case 27: return "MAG-7";
			case 28: return "Negev";
			case 29: return "Sawed-Off";
			case 30: return "Tec-9";
			case 32: return "P2000";
			case 33: return "MP7";
			case 34: return "MP9";
			case 35: return "Nova";
			case 36: return "P250";
			case 38: return "SCAR-20";
			case 39: return "SG 553";
			case 40: return "SSG 08";
			case 42: case 500: return "Knife";
			case 43: return "Flashbang";
			case 44: return "HE Grenade";
			case 45: return "Smoke";
			case 46: return "Molotov";
			case 47: return "Decoy";
			case 48: return "Inc. Grenade";
			case 49: return "C4";
			case 59: return "Knife";
			case 60: return "M4A1-S";
			case 61: return "USP-S";
			case 63: return "CZ75-Auto";
			case 64: return "Revolver";
			case 505: return "Flip Knife";
			case 506: return "Gut Knife";
			case 507: return "Karambit";
			case 508: return "M9 Bayonet";
			case 509: return "Huntsman";
			}

			return nullptr;
		}

		void DrawWorldOverlay(const std::vector<C_CSPlayerPawn>& players, const view_matrix_t& viewMatrix)
		{
			ImDrawList* draw = ImGui::GetBackgroundDrawList();
			const ImVec2 display = ImGui::GetIO().DisplaySize;

			if (g_settings.fovCircle)
			{
				const ImVec2 center(display.x * 0.5f, display.y * 0.5f);

				// plain thin ring, no fill
				draw->AddCircle(center, g_settings.aimFov,
					ImGui::ColorConvertFloat4ToU32(g_settings.fovColor), 64, 1.0f);
			}

			if (!g_settings.espBoxes)
				return;

			for (const auto& player : players)
			{
				if (!player.pCSPlayerPawn || player.health <= 0)
					continue;

				Vector feet;
				Vector headTop;

				// cached world positions, projected against the overlay surface, not the desktop
				if (!Vector::world_to_screen(viewMatrix, player.Position, feet))
					continue;

				// box top = top of the skull, not the eye bone: eye sits ~9u below the crown
				Vector headTopWorld = player.headPos;
				headTopWorld.z += offset::headTopOffset;

				if (!Vector::world_to_screen(viewMatrix, headTopWorld, headTop))
					continue;

				const float boxHeight = feet.y - headTop.y;

				// head below feet (behind camera edge cases) or absurd scale (right in the face)
				if (boxHeight < 8.f || boxHeight > display.y * 2.5f)
					continue;

				const float boxWidth = boxHeight * 0.45f; // ~player shoulder width

				const float left = headTop.x - boxWidth * 0.5f;
				const float right = headTop.x + boxWidth * 0.5f;
				const float top = headTop.y;
				const float bottom = feet.y;

				// fully off screen - nothing to draw
				if (right < 0.f || left > display.x || bottom < 0.f || top > display.y)
					continue;

				// low hp turns the box red, otherwise the configured enemy color
				const ImU32 color = player.health <= 35
					? ImGui::ColorConvertFloat4ToU32(g_settings.lowHpColor)
					: ImGui::ColorConvertFloat4ToU32(g_settings.boxColor);

				if (g_settings.cornerBoxes)
				{
					// corner-only style: four L-shaped strokes, length clamped to the box
					float len = g_settings.cornerLength;

					if (len > boxWidth * 0.5f)
						len = boxWidth * 0.5f;

					if (len > boxHeight * 0.5f)
						len = boxHeight * 0.5f;

					// top-left / top-right
					draw->AddLine(ImVec2(left, top), ImVec2(left + len, top), color, 1.5f);
					draw->AddLine(ImVec2(left, top), ImVec2(left, top + len), color, 1.5f);
					draw->AddLine(ImVec2(right, top), ImVec2(right - len, top), color, 1.5f);
					draw->AddLine(ImVec2(right, top), ImVec2(right, top + len), color, 1.5f);
					// bottom-left / bottom-right
					draw->AddLine(ImVec2(left, bottom), ImVec2(left + len, bottom), color, 1.5f);
					draw->AddLine(ImVec2(left, bottom), ImVec2(left, bottom - len), color, 1.5f);
					draw->AddLine(ImVec2(right, bottom), ImVec2(right - len, bottom), color, 1.5f);
					draw->AddLine(ImVec2(right, bottom), ImVec2(right, bottom - len), color, 1.5f);
				}
				else
				{
					draw->AddRect(ImVec2(left, top), ImVec2(right, bottom), color, 0.f, 0, 1.5f);
				}

				if (g_settings.skeletonEnabled)
					DrawSkeleton(player, viewMatrix,
						ImGui::ColorConvertFloat4ToU32(g_settings.skeletonColor), display);

				// -------------------------------------------------- text + head dot

				if (g_settings.headDotEnabled && g_settings.headDotRadius > 0.f)
				{
					Vector eyeScreen;

					if (Vector::world_to_screen(viewMatrix, player.headPos, eyeScreen))
					{
						draw->AddCircleFilled(ImVec2(eyeScreen.x, eyeScreen.y),
							g_settings.headDotRadius, IM_COL32(120, 220, 255, 90), 24);
						draw->AddCircle(ImVec2(eyeScreen.x, eyeScreen.y),
							g_settings.headDotRadius, IM_COL32(255, 255, 255, 200), 24, 1.f);
					}
				}

				if (g_settings.showNames && player.name[0])
				{
					ImVec2 textSize = ImGui::CalcTextSize(player.name);
					draw->AddText(ImVec2(headTop.x - textSize.x * 0.5f, headTop.y - textSize.y - 3.f),
						IM_COL32(255, 255, 255, 230), player.name);
				}

				if (g_settings.showWeapons && player.weaponId)
				{
					const char* weaponName = WeaponDisplayName(player.weaponId);

					if (weaponName)
					{
						// centered under the box, on the box's own horizontal axis
						const float centerX = (left + right) * 0.5f;
						const ImVec2 textSize = ImGui::CalcTextSize(weaponName);
						draw->AddText(ImVec2(centerX - textSize.x * 0.5f, bottom + 3.f),
							IM_COL32(255, 255, 255, 220), weaponName);
					}
				}

				if (g_settings.showHealthBar && boxHeight > 0.f)
				{
					// vertical hp bar left of the box, drained from the bottom
					const float barX = left - 6.f;
					const float barW = 3.f;

					draw->AddRectFilled(ImVec2(barX, top), ImVec2(barX + barW, bottom),
						IM_COL32(10, 10, 10, 160));

					const float hpFrac = player.health / 100.f;
					const float fill = (bottom - top) * hpFrac;

					if (fill > 0.f)
					{
						const ImU32 hpColor = player.health > 50
							? IM_COL32(80, 220, 80, 220)
							: (player.health > 25 ? IM_COL32(220, 200, 60, 220) : IM_COL32(220, 60, 60, 220));

						draw->AddRectFilled(ImVec2(barX, bottom - fill), ImVec2(barX + barW, bottom), hpColor);
					}
				}
			}
		}

		void DrawMenu()
		{
			ImGui::SetNextWindowPos(ImVec2(60.f, 60.f), ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSize(ImVec2(320.f, 0.f), ImGuiCond_FirstUseEver);

			ImGui::Begin("VISION", nullptr, ImGuiWindowFlags_NoCollapse);

			ImGui::Text("INSERT - open/close this menu");

			if (ImGui::BeginTabBar("##tabs"))
			{
				// ------------------------------------------------------------- aimbot
				if (ImGui::BeginTabItem("Aimbot"))
				{
					if (ImGui::Checkbox("Enabled", &g_settings.aimEnabled))
						printf("[esp] aimbot %s\n", g_settings.aimEnabled ? "on" : "off");

					ImGui::Separator();
					ImGui::Text("Activation");

					// shared int keeps the radios exclusive; re-synced from settings
					// every frame so an external change is reflected too
					int aimMode = g_settings.aimToggle ? 1 : 0;

					ImGui::RadioButton("Hold", &aimMode, 0);
					ImGui::SameLine();
					ImGui::RadioButton("Toggle", &aimMode, 1);

					const bool newToggle = (aimMode == 1);

					if (newToggle != g_settings.aimToggle)
					{
						g_settings.aimToggle = newToggle;
						aimbot.aimActive = false; // start toggling from an off state
						printf("[esp] aim mode %s\n", newToggle ? "toggle" : "hold");
					}

					// key rebinding: click the button, then press the wanted key
					static bool waitingForAimKey = false;

					char keyLabel[64];
					sprintf_s(keyLabel, "Aim key: %s", waitingForAimKey ? "press any key..." : KeyName(g_settings.aimKey));

					if (ImGui::Button(keyLabel))
						waitingForAimKey = !waitingForAimKey;

					if (waitingForAimKey)
					{
						// first pressed key wins; LMB is skipped - it drives this very button
						for (int vk = 1; vk < 255; ++vk)
						{
							if (vk == VK_LBUTTON)
								continue;

							if (GetAsyncKeyState(vk) & 0x8000)
							{
								g_settings.aimKey = vk;
								printf("[esp] aim key bound to %s (vk=0x%02X)\n", KeyName(vk), vk);
								waitingForAimKey = false;
								break;
							}
						}
					}

					ImGui::Separator();
					ImGui::Text("Targeting");

					if (ImGui::SliderFloat("Aim FOV", &g_settings.aimFov, 20.f, 800.f, "%.0f px"))
						g_settings.aimFov = static_cast<float>(static_cast<int>(g_settings.aimFov));

					if (ImGui::Checkbox("FOV circle", &g_settings.fovCircle))
						printf("[esp] fov circle %s\n", g_settings.fovCircle ? "on" : "off");

					// 0 = crosshair snaps to the target in a single tick
					if (ImGui::SliderFloat("Aim smoothness", &g_settings.aimSmooth, 0.f, 20.f, "%.1f"))
						printf("[esp] aim smoothness %.1f\n", g_settings.aimSmooth);

					ImGui::ColorEdit4("FOV circle", (float*)&g_settings.fovColor, ImGuiColorEditFlags_NoInputs);

					ImGui::EndTabItem();
				}

				// ------------------------------------------------------------ visuals
				if (ImGui::BeginTabItem("Visuals"))
				{
					if (ImGui::Checkbox("ESP boxes", &g_settings.espBoxes))
						printf("[esp] boxes %s\n", g_settings.espBoxes ? "on" : "off");

					if (ImGui::Checkbox("Skeleton", &g_settings.skeletonEnabled))
						printf("[esp] skeleton %s\n", g_settings.skeletonEnabled ? "on" : "off");

					if (ImGui::Checkbox("Nicknames", &g_settings.showNames))
						printf("[esp] nicknames %s\n", g_settings.showNames ? "on" : "off");

					if (ImGui::Checkbox("Weapons", &g_settings.showWeapons))
						printf("[esp] weapons %s\n", g_settings.showWeapons ? "on" : "off");

					if (ImGui::Checkbox("Health bar", &g_settings.showHealthBar))
						printf("[esp] health bar %s\n", g_settings.showHealthBar ? "on" : "off");

					if (ImGui::Checkbox("Head dot", &g_settings.headDotEnabled))
						printf("[esp] head dot %s\n", g_settings.headDotEnabled ? "on" : "off");

					if (g_settings.headDotEnabled && ImGui::SliderFloat("Head dot radius", &g_settings.headDotRadius, 2.f, 30.f, "%.0f px"))
						g_settings.headDotRadius = static_cast<float>(static_cast<int>(g_settings.headDotRadius));

					if (ImGui::Checkbox("Corner boxes", &g_settings.cornerBoxes))
						printf("[esp] corner boxes %s\n", g_settings.cornerBoxes ? "on" : "off");

					if (g_settings.cornerBoxes)
					{
						if (ImGui::SliderFloat("Corner length", &g_settings.cornerLength, 2.f, 60.f, "%.0f px"))
							g_settings.cornerLength = static_cast<float>(static_cast<int>(g_settings.cornerLength));
					}

					ImGui::Separator();
					ImGui::Text("Colors");

					ImGui::ColorEdit4("Box", (float*)&g_settings.boxColor, ImGuiColorEditFlags_NoInputs);
					ImGui::ColorEdit4("Low HP", (float*)&g_settings.lowHpColor, ImGuiColorEditFlags_NoInputs);
					ImGui::ColorEdit4("Skeleton", (float*)&g_settings.skeletonColor, ImGuiColorEditFlags_NoInputs);

					ImGui::Separator();
					ImGui::Text("Chams (material hook)");

					// material hook init is crash-prone (remote material creation),
					// so it only runs when the user explicitly asks for it
					if (ImGui::Checkbox("Chams enabled", &g_settings.chamsEnabled))
					{
						if (g_settings.chamsEnabled)
						{
							if (chams::EnsureInit())
							{
								chams::SetEnabled(true);
								chams::SetColor(
									static_cast<uint8_t>(g_settings.chamColor.x * 255.f),
									static_cast<uint8_t>(g_settings.chamColor.y * 255.f),
									static_cast<uint8_t>(g_settings.chamColor.z * 255.f),
									static_cast<uint8_t>(g_settings.chamColor.w * 255.f));
								printf("[esp] chams on\n");
							}
							else
							{
								g_settings.chamsEnabled = false; // init failed - revert
								printf("[esp] chams init failed, reverted\n");
							}
						}
						else
						{
							chams::SetEnabled(false);
							printf("[esp] chams off\n");
						}
					}

					if (ImGui::ColorEdit4("Chams color", (float*)&g_settings.chamColor, ImGuiColorEditFlags_NoInputs))
					{
						chams::SetColor(
							static_cast<uint8_t>(g_settings.chamColor.x * 255.f),
							static_cast<uint8_t>(g_settings.chamColor.y * 255.f),
							static_cast<uint8_t>(g_settings.chamColor.z * 255.f),
							static_cast<uint8_t>(g_settings.chamColor.w * 255.f));
					}

					ImGui::EndTabItem();
				}

				// ------------------------------------------------------------ movement
				if (ImGui::BeginTabItem("Movement"))
				{
					if (ImGui::Checkbox("Auto-Stop", &g_settings.autoStopEnabled))
					{
						printf("[esp] auto-stop %s (game focused: %s)\n",
							g_settings.autoStopEnabled ? "on" : "off",
							movement::GameFocused() ? "yes" : "no");
					}

					if (g_settings.autoStopEnabled)
					{
						if (ImGui::SliderInt("Stop duration (ms)", &g_settings.stopDurationMs, 50, 400))
							printf("[esp] stop duration %d ms\n", g_settings.stopDurationMs);

						ImGui::TextWrapped("Releases W/A/S/D -> briefly presses the opposite keys "
							"(W+D -> S+A) for a fast counter-strafe stop. "
							"Pressing any movement key cancels it.");
					}

					ImGui::EndTabItem();
				}

				// --------------------------------------------------------- properties
				if (ImGui::BeginTabItem("Properties"))
				{
					if (ImGui::SliderInt("Poll rate (ms)", &g_settings.pollMs, 1, 50))
						printf("[esp] poll rate %d ms\n", g_settings.pollMs);

					ImGui::TextWrapped("Lower = boxes and aim track moving players better, "
						"but spend more CPU on memory reads. Applies immediately.");

					ImGui::EndTabItem();
				}

				ImGui::EndTabBar();
			}

			ImGui::End();
		}

		void RenderFrame(OverlayState& state)
		{
			ImGui_ImplDX11_NewFrame();
			ImGui_ImplWin32_NewFrame();
			ImGui::NewFrame();

			const std::vector<C_CSPlayerPawn> players = reader.Snapshot();

			if (!players.empty() && reader.client)
			{
				const view_matrix_t viewMatrix = mem.Read<view_matrix_t>(reader.client + offset::dwViewMatrix);
				DrawWorldOverlay(players, viewMatrix);
			}

			if (state.menuOpen)
				DrawMenu();

			ImGui::Render();

			const float clearColor[4] = { 0.f, 0.f, 0.f, 1.f }; // black = colorkey = transparent

			state.context->OMSetRenderTargets(1, &state.target, nullptr);
			state.context->ClearRenderTargetView(state.target, clearColor);
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

			state.swapChain->Present(0, 0);
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
					state.window = CreateOverlayWindow(GetModuleHandleW(nullptr));

					if (!state.window)
					{
						printf("[esp] overlay window creation failed, error %lu\n", GetLastError());
						std::this_thread::sleep_for(std::chrono::milliseconds(500));
						continue;
					}

					printf("[esp] overlay window %p\n", state.window);
					SetClickThrough(state.window, !state.menuOpen);
				}

				MSG msg;

				while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
				{
					TranslateMessage(&msg);
					DispatchMessageW(&msg);
				}

				// menu toggle via polling - the overlay window is WS_EX_NOACTIVATE, so
				// keyboard messages land in the game and a WndProc toggle never fires
				static bool menuKeyDown = false;
				const bool insertDown = (GetAsyncKeyState(kMenuToggleKey) & 0x8000) != 0;

				if (insertDown && !menuKeyDown)
				{
					state.menuOpen = !state.menuOpen;
					SetClickThrough(state.window, !state.menuOpen);
					printf("[esp] menu %s\n", state.menuOpen ? "open" : "closed");
				}

				menuKeyDown = insertDown;

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

				if (EnsureD3D(state, width, height))
					RenderFrame(state);

				if (++frame % 120 == 0)
					printf("[esp] client %dx%d at %d,%d players=%zu\n", width, height, origin.x, origin.y, reader.Snapshot().size());

				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}

			ReleaseD3D(state);

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
