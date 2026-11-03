#pragma once
#include "../mem/memify.h"
#include "../math/vector.hpp"

#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

inline memify mem("cs2.exe");

namespace offset
{
	// client.dll build 14193 / 2026-10-30 (a2x cs2-dumper)
	// dw* verified live in-game 2026-11-03 (game rebuilt Nov 3): entityList +0x80, localPawn +0x80 vs Nov 2
	constexpr std::ptrdiff_t dwEntityList = 0x271EBA8;
	constexpr std::ptrdiff_t dwViewMatrix = 0x256F250;
	constexpr std::ptrdiff_t dwLocalPlayerPawn = 0x2569948;

	constexpr std::ptrdiff_t m_hPlayerPawn = 0x92C;  // CCSPlayerController

	constexpr std::ptrdiff_t m_iHealth = 0x34C;      // C_BaseEntity
	constexpr std::ptrdiff_t m_iTeamNum = 0x3E7;     // C_BaseEntity (uint8)
	constexpr std::ptrdiff_t m_vOldOrigin = 0x14A4;  // C_BasePlayerPawn
	constexpr std::ptrdiff_t m_entitySpottedState = 0x1E88; // C_CSPlayerPawn, 0x8 m_bSpotted

	// skeleton: pawn -> CGameSceneNode (CSkeletonInstance) -> CModelState -> bone array
	constexpr std::ptrdiff_t m_pGameSceneNode = 0x330;
	constexpr std::ptrdiff_t m_modelState = 0x140;   // CSkeletonInstance
	constexpr std::ptrdiff_t m_boneArray = 0x80;     // CModelState::m_pBones

	// CGlowProperty (C_BasePlayerPawn::m_Glow) - chams via engine glow tampering
	constexpr std::ptrdiff_t m_Glow = 0xDE8;
	constexpr std::ptrdiff_t glow_fGlowColor = 0x8;   // Vector rgb
	constexpr std::ptrdiff_t glow_iGlowType = 0x30;   // 1 = entity glow
	constexpr std::ptrdiff_t glow_nGlowRange = 0x38;  // through-wall distance
	constexpr std::ptrdiff_t glow_nGlowRangeMin = 0x3C;
	constexpr std::ptrdiff_t glow_colorOverride = 0x40; // Color rgba bytes
	constexpr std::ptrdiff_t glow_flGlowTime = 0x48;
	constexpr std::ptrdiff_t glow_flGlowStartTime = 0x4C;
	constexpr std::ptrdiff_t glow_bEligible = 0x50;
	constexpr std::ptrdiff_t glow_bGlowing = 0x51;

	// entity list layout
	constexpr std::ptrdiff_t entityListStride = 0x8;
	constexpr std::ptrdiff_t entityListOffset = 0x10;
	constexpr std::ptrdiff_t entityStride = 0x70;

	// bone data: 0x20-byte entries, position at +0x0 (BoneJointData)
	constexpr std::ptrdiff_t boneStride = 0x20;
	// verified live 2026-11-03: bone 6 = neck (~60u), bone 7 = head/eyes (~64u, eye height)
	constexpr int headBone = 7;

	// skeleton esp: bones we cache per player
	constexpr int maxBones = 28;

	// world-space offset from the eye bone to the top of the skull, for the esp box top
	constexpr float headTopOffset = 9.f;

	constexpr float headFallbackHeight = 65.f;
}

// create an entity class for our vector, since we need to be able to push_back().
class C_CSPlayerPawn
{
public:
	int health = 0;
	int team = 0;

	Vector Position;   // feet / origin
	Vector headPos;    // head bone, cached on the reader thread
	Vector bones[offset::maxBones] = {}; // full skeleton, cached on the reader thread
	bool bonesValid = false;

	uintptr_t pCSPlayerPawn = 0;
	uintptr_t boneArray = 0;
};

// bone read + sanity fallback, shared by aimbot and esp
Vector GetHeadPosition(const C_CSPlayerPawn& player);

inline C_CSPlayerPawn CCSPlayerPawn;

// create a class for filtering players, and our new thread.
class Reader
{
public:
	uintptr_t client = 0;

	void ThreadLoop();

	// snapshot under lock - the filter thread mutates the list while the aim thread reads it
	std::vector<C_CSPlayerPawn> Snapshot();

private:
	void FilterPlayers();

	std::mutex mtx;
	std::vector<C_CSPlayerPawn> playerList;
};

inline Reader reader;

// aimbot functions
class Aimbot
{
public:
	bool aimActive = false; // toggle mode state

	void doAimbot();

private:
	Vector GetAimPoint(const C_CSPlayerPawn& player);
	bool findClosest(const std::vector<Vector>& playerPositions, Vector& out, float fov);
	void MoveMouseToPlayer(const Vector& position);
};

inline Aimbot aimbot;
