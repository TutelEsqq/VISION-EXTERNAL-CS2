#include "entity.hpp"
#include "../render/settings.hpp"
#include "chams.hpp"

#include <cmath>
#include <cstring>
#include <utility>

Vector GetHeadPosition(const C_CSPlayerPawn& player)
{
	// bone transform: 0x20 bytes, position at +0x0; head bone index 6
	if (player.boneArray)
	{
		const Vector head = mem.Read<Vector>(player.boneArray + offset::headBone * offset::boneStride);

		// garbage scene node / stale cache -> bone array reads as nulls or nonsense
		if (!head.IsZero() && head.IsFinite() &&
			std::fabs(head.x) < 16384.f && std::fabs(head.y) < 16384.f && std::fabs(head.z) < 16384.f)
		{
			return head;
		}
	}

	// fallback: origin sits at the feet, eyes are ~65 units up
	const Vector origin = player.Position;
	return { origin.x, origin.y, origin.z + offset::headFallbackHeight };
}

namespace
{
	// chams: force the engine's own glow system on an enemy pawn. The fields live in
	// the networked CGlowProperty - the server only resends them when IT changes glow
	// state, so our local writes stick for enemies that are not glowing server-side.
	void ApplyChams(uintptr_t pawn)
	{
		const uintptr_t glow = pawn + offset::m_Glow;

		mem.Write<int>(glow + offset::glow_iGlowType, 1);        // entity glow
		mem.Write<int>(glow + offset::glow_nGlowRange, 1 << 20); // visible through anything
		mem.Write<int>(glow + offset::glow_nGlowRangeMin, 0);

		const ImVec4& c = g_settings.chamColor;

		mem.Write<float>(glow + offset::glow_fGlowColor + 0x0, c.x);
		mem.Write<float>(glow + offset::glow_fGlowColor + 0x4, c.y);
		mem.Write<float>(glow + offset::glow_fGlowColor + 0x8, c.z);

		// Color stores rgba bytes in memory order
		const uint32_t overrideColor =
			static_cast<uint32_t>(c.x * 255.f) |
			(static_cast<uint32_t>(c.y * 255.f) << 8) |
			(static_cast<uint32_t>(c.z * 255.f) << 16) |
			(static_cast<uint32_t>(c.w * 255.f) << 24);

		mem.Write<uint32_t>(glow + offset::glow_colorOverride, overrideColor);

		mem.Write<float>(glow + offset::glow_flGlowStartTime, 0.f);      // started long ago
		mem.Write<float>(glow + offset::glow_flGlowTime, 1 << 14);       // no fade-out
		mem.Write<bool>(glow + offset::glow_bEligible, true);
		mem.Write<bool>(glow + offset::glow_bGlowing, true);             // the actual switch
	}

	// Candidate layouts for the entity list. Runs once when the normal walk finds nothing,
	// so we learn which offset/stride this build uses instead of guessing patches later.
	void ProbeLayout(uintptr_t entityList)
	{
		constexpr std::ptrdiff_t listOffsets[] = { 0x10, 0x8, 0x0, 0x18 };
		constexpr std::ptrdiff_t strides[] = { 0x70, 0x78, 0x60, 0x80 };

		printf("[probe] entityList=%p\n", reinterpret_cast<void*>(entityList));

		for (const std::ptrdiff_t listOffset : listOffsets)
		{
			const uintptr_t chunk = mem.Read<uintptr_t>(entityList + listOffset);
			printf("[probe] listOffset 0x%lX -> %p\n", static_cast<long>(listOffset),
				reinterpret_cast<void*>(chunk));

			if (!chunk)
				continue;

			for (const std::ptrdiff_t stride : strides)
			{
				int controllers = 0;
				uintptr_t first = 0;

				for (int i = 1; i <= 64; ++i)
				{
					const uintptr_t controller = mem.Read<uintptr_t>(chunk + stride * (i & 0x1FF));

					if (controller)
					{
						++controllers;

						if (!first)
							first = controller;
					}
				}

				printf("[probe]   stride 0x%lX controllers=%d first=%p\n", static_cast<long>(stride),
					controllers, reinterpret_cast<void*>(first));

				if (!first)
					continue;

				// shape check: real entity pointers look like 0x00007FF... , garbage like small ints
				printf("[probe]     sample %p %p %p\n",
					reinterpret_cast<void*>(first),
					reinterpret_cast<void*>(mem.Read<uintptr_t>(chunk + stride * 2)),
					reinterpret_cast<void*>(mem.Read<uintptr_t>(chunk + stride * 3)));

				// raw neighbourhood around the handle field of the first controller
				printf("[probe]     qwords @0x900:");
				for (std::ptrdiff_t dumpOffset = 0x900; dumpOffset < 0x950; dumpOffset += 8)
					printf(" %llX", static_cast<unsigned long long>(mem.Read<uint64_t>(first + dumpOffset)));
				printf("\n");

				// hunt for the pawn handle inside the first controller that resolved
				for (std::ptrdiff_t handleOffset = 0x400; handleOffset < 0xA00; handleOffset += 4)
				{
					const uint32_t handle = mem.Read<uint32_t>(first + handleOffset);

					if (!handle || handle == 0xFFFFFFFF)
						continue;

					const uintptr_t chunk2 = mem.Read<uintptr_t>(
						entityList + listOffset + 8 * ((handle & 0x7FFF) >> 9));

					if (!chunk2)
						continue;

					const uintptr_t pawn = mem.Read<uintptr_t>(chunk2 + stride * (handle & 0x1FF));

					if (!pawn)
						continue;

					const int hp = mem.Read<int32_t>(pawn + offset::m_iHealth);

					if (hp < 1 || hp > 100)
						continue;

					const uint8_t team = mem.Read<uint8_t>(pawn + offset::m_iTeamNum);

					if (team != 2 && team != 3)
						continue;

					printf("[probe]   handle 0x%lX -> pawn %p hp=%d team=%u\n",
						static_cast<long>(handleOffset), reinterpret_cast<void*>(pawn), hp, team);
				}
			}
		}
	}
}

void Reader::ThreadLoop()
{
	while (true)
	{
		// process may not be up yet - keep retrying instead of dying on a null handle
		while (!mem.Attached())
		{
			if (mem.TryAttach())
				std::cout << "attached to cs2.exe\n";
			else
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
		}

		while (!client)
		{
			client = mem.GetBase("client.dll");

			if (!client)
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}

		std::cout << "client.dll " << std::hex << client << std::dec << std::endl;

		while (mem.Alive())
		{
			// poll interval is configurable from the Properties tab - lower = fresher
			// entity list, higher = less CPU spent on RPM reads
			std::this_thread::sleep_for(std::chrono::milliseconds(
				g_settings.pollMs > 0 ? g_settings.pollMs : 1));

			FilterPlayers();
		}

		// game closed - drop every handle so the next launch re-attaches cleanly
		std::cout << "lost cs2.exe, waiting for restart\n";
		mem.Detach();
		client = 0;

		std::lock_guard<std::mutex> lock(mtx);
		playerList.clear();
	}
}

std::vector<C_CSPlayerPawn> Reader::Snapshot()
{
	std::lock_guard<std::mutex> lock(mtx);
	return playerList;
}

void Reader::FilterPlayers()
{
	std::vector<C_CSPlayerPawn> players;

	const uintptr_t entityList = mem.Read<uintptr_t>(client + offset::dwEntityList);

	if (!entityList)
		return;

	const uintptr_t localPawn = mem.Read<uintptr_t>(client + offset::dwLocalPlayerPawn);
	const uint8_t localTeam = localPawn ? mem.Read<uint8_t>(localPawn + offset::m_iTeamNum) : 0;

	// funnel counters - tells us exactly which dereference dies
	int listEntries = 0;
	int controllers = 0;
	int handles = 0;
	int secondEntries = 0;
	int pawns = 0;
	int alive = 0;
	int zeroHandles = 0;
	int deadHandles = 0;

	// index 0 is the world entity
	for (int i = 1; i <= 64; ++i)
	{
		const uintptr_t listEntry1 = mem.Read<uintptr_t>(
			entityList + offset::entityListStride * ((i & 0x7FFF) >> 9) + offset::entityListOffset);

		if (!listEntry1)
			continue;

		++listEntries;

		const uintptr_t controller = mem.Read<uintptr_t>(
			listEntry1 + offset::entityStride * (i & 0x1FF));

		if (!controller)
			continue;

		++controllers;

		const uint32_t pawnHandle = mem.Read<uint32_t>(controller + offset::m_hPlayerPawn);

		if (!pawnHandle)
		{
			++zeroHandles;
			continue;
		}

		if (pawnHandle == 0xFFFFFFFF)
		{
			++deadHandles;
			continue;
		}

		++handles;

		const uintptr_t listEntry2 = mem.Read<uintptr_t>(
			entityList + offset::entityListStride * ((pawnHandle & 0x7FFF) >> 9) + offset::entityListOffset);

		if (!listEntry2)
			continue;

		++secondEntries;

		const uintptr_t pawn = mem.Read<uintptr_t>(
			listEntry2 + offset::entityStride * (pawnHandle & 0x1FF));

		if (!pawn)
			continue;

		++pawns;

		if (pawn == localPawn)
			continue;

		const int health = mem.Read<int32_t>(pawn + offset::m_iHealth);

		if (health <= 0 || health > 100)
			continue;

		const uint8_t team = mem.Read<uint8_t>(pawn + offset::m_iTeamNum);

		// localTeam is 0 while local player is not spawned yet - skip the filter then
		if (localTeam && team == localTeam)
			continue;

		++alive;

		const uintptr_t sceneNode = mem.Read<uintptr_t>(pawn + offset::m_pGameSceneNode);
		const uintptr_t bones = sceneNode
			? mem.Read<uintptr_t>(sceneNode + offset::m_modelState + offset::m_boneArray)
			: 0;

		C_CSPlayerPawn entity{};
		entity.pCSPlayerPawn = pawn;
		entity.boneArray = bones;
		entity.health = health;
		entity.team = team;
		entity.Position = mem.Read<Vector>(pawn + offset::m_vOldOrigin);
		entity.headPos = GetHeadPosition(entity);

		// skeleton: one read for all bones, position at +0x0 of each 0x20 entry
		if (entity.boneArray)
		{
			uint8_t boneBlob[offset::maxBones * offset::boneStride]{};

			if (mem.ReadRaw(entity.boneArray, boneBlob, sizeof(boneBlob)))
			{
				bool anyValid = false;

				for (int b = 0; b < offset::maxBones; ++b)
				{
					Vector bone;
					std::memcpy(&bone, boneBlob + b * offset::boneStride, sizeof(Vector));

					// garbage scene node / stale cache guard, same as GetHeadPosition
					if (bone.IsFinite() &&
						std::fabs(bone.x) < 16384.f && std::fabs(bone.y) < 16384.f && std::fabs(bone.z) < 16384.f)
					{
						entity.bones[b] = bone;

						if (bone.x != 0.f || bone.y != 0.f || bone.z != 0.f)
							anyValid = true;
					}
				}

				entity.bonesValid = anyValid;
			}
		}

		if (g_settings.chamsEnabled)
			ApplyChams(pawn);

		players.push_back(entity);
	}

	// heartbeat: without this an empty list and a broken layout look identical in the console
	static int tick = 0;
	static int emptyTicks = 0;
	static bool probed = false;

	if (++tick % 60 == 0)
	{
		printf("[reader] players=%zu | list=%d controllers=%d handles=%d zero=%d ff=%d second=%d pawns=%d alive=%d | localTeam=%u\n",
			players.size(), listEntries, controllers, handles, zeroHandles, deadHandles,
			secondEntries, pawns, alive, static_cast<unsigned>(localTeam));
	}

	if (players.empty())
		++emptyTicks;

	if (players.empty() && !probed && emptyTicks >= 2)
	{
		probed = true;
		ProbeLayout(entityList);
	}

	std::lock_guard<std::mutex> lock(mtx);
	playerList = std::move(players);
}
