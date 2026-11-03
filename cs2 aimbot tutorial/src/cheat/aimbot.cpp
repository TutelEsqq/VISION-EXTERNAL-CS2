#include "entity.hpp"
#include "../render/settings.hpp"

Vector Aimbot::GetAimPoint(const C_CSPlayerPawn& player)
{
	// head bone is cached on the reader thread; validate before trusting it
	if (!player.headPos.IsZero() && player.headPos.IsFinite())
		return player.headPos;

	// fallback: origin sits at the feet, eyes are ~65 units up
	const Vector origin = player.Position;
	return { origin.x, origin.y, origin.z + offset::headFallbackHeight };
}

void Aimbot::doAimbot()
{
	if (!g_settings.aimEnabled)
		return;

	// hold mode: aim while the key is down; toggle mode: flip on each press
	const bool keyDown = (GetAsyncKeyState(g_settings.aimKey) & 0x8000) != 0;
	static bool prevKeyDown = false;

	if (g_settings.aimToggle)
	{
		if (keyDown && !prevKeyDown)
		{
			aimActive = !aimActive;
			printf("[esp] aim toggle %s\n", aimActive ? "on" : "off");
		}

		prevKeyDown = keyDown;

		if (!aimActive)
			return;
	}
	else
	{
		if (!keyDown)
			return;
	}

	const std::vector<C_CSPlayerPawn> players = reader.Snapshot();

	if (players.empty() || !reader.client)
		return;

	const view_matrix_t viewMatrix = mem.Read<view_matrix_t>(reader.client + offset::dwViewMatrix);

	std::vector<Vector> targets;
	targets.reserve(players.size());

	for (const auto& player : players)
	{
		if (!player.pCSPlayerPawn || player.health <= 0)
			continue;

		const Vector aimPoint = GetAimPoint(player);
		Vector screenPosition;

		if (Vector::world_to_screen(viewMatrix, aimPoint, screenPosition))
			targets.push_back(screenPosition);
	}

	if (targets.empty())
		return;

	Vector closest;

	if (!findClosest(targets, closest, g_settings.aimFov))
		return;

	MoveMouseToPlayer(closest);
}

bool Aimbot::findClosest(const std::vector<Vector>& playerPositions, Vector& out, float fov)
{
	if (playerPositions.empty())
		return false;

	const Vector center_of_screen{ (float)GetSystemMetrics(SM_CXSCREEN) * 0.5f,
									(float)GetSystemMetrics(SM_CYSCREEN) * 0.5f, 0.f };

	// fov is the aim radius in px, same circle the overlay draws around the crosshair
	float lowestDistance = fov * fov;
	int index = -1;

	for (int i = 0; i < static_cast<int>(playerPositions.size()); ++i)
	{
		const float dx = playerPositions[i].x - center_of_screen.x;
		const float dy = playerPositions[i].y - center_of_screen.y;
		const float distance = dx * dx + dy * dy;

		if (distance < lowestDistance)
		{
			lowestDistance = distance;
			index = i;
		}
	}

	if (index < 0)
		return false;

	out = playerPositions[index];
	return true;
}

void Aimbot::MoveMouseToPlayer(const Vector& position)
{
	if (!position.IsFinite())
		return;

	POINT cursor{};

	if (!GetCursorPos(&cursor))
		return;

	// sub-pixel residue carried between ticks - mouse_event only accepts whole pixels,
	// truncating every call made the crosshair crawl short of the target
	static float carry_x = 0.f;
	static float carry_y = 0.f;

	// smoothness: 0/1 = full delta per tick (instant snap), higher = move a fraction
	// of the remaining distance each tick for a gradual approach
	const float step = g_settings.aimSmooth > 1.f ? 1.f / g_settings.aimSmooth : 1.f;

	carry_x += (position.x - static_cast<float>(cursor.x)) * step;
	carry_y += (position.y - static_cast<float>(cursor.y)) * step;

	const LONG dx = static_cast<LONG>(carry_x);
	const LONG dy = static_cast<LONG>(carry_y);

	carry_x -= static_cast<float>(dx);
	carry_y -= static_cast<float>(dy);

	if (!dx && !dy)
		return;

	mouse_event(MOUSEEVENTF_MOVE, dx, dy, 0, 0);
}
