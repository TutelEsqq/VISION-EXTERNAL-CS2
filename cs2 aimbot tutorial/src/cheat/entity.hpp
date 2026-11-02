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
	// dwEntityList verified live in-game 2026-11-02: +0x20 vs dump (game rebuilt Oct 31)
	constexpr std::ptrdiff_t dwEntityList = 0x271EB28;
	constexpr std::ptrdiff_t dwViewMatrix = 0x256F1D0;
	constexpr std::ptrdiff_t dwLocalPlayerPawn = 0x25698C8;

	constexpr std::ptrdiff_t m_hPlayerPawn = 0x92C;  // CCSPlayerController

	constexpr std::ptrdiff_t m_iHealth = 0x34C;      // C_BaseEntity
	constexpr std::ptrdiff_t m_iTeamNum = 0x3E7;     // C_BaseEntity (uint8)
	constexpr std::ptrdiff_t m_vOldOrigin = 0x14A4;  // C_BasePlayerPawn
	constexpr std::ptrdiff_t m_entitySpottedState = 0x1E88; // C_CSPlayerPawn, 0x8 m_bSpotted

	// skeleton: pawn -> CGameSceneNode (CSkeletonInstance) -> CModelState -> bone array
	constexpr std::ptrdiff_t m_pGameSceneNode = 0x330;
	constexpr std::ptrdiff_t m_modelState = 0x140;   // CSkeletonInstance
	constexpr std::ptrdiff_t m_boneArray = 0x80;     // CModelState::m_pBones

	// entity list layout
	constexpr std::ptrdiff_t entityListStride = 0x8;
	constexpr std::ptrdiff_t entityListOffset = 0x10;
	constexpr std::ptrdiff_t entityStride = 0x70;

	// bone data: 0x20-byte entries, position at +0x0 (BoneJointData)
	constexpr std::ptrdiff_t boneStride = 0x20;
	constexpr int headBone = 6;

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
	void doAimbot();

private:
	Vector GetAimPoint(const C_CSPlayerPawn& player);
	bool findClosest(const std::vector<Vector>& playerPositions, Vector& out);
	void MoveMouseToPlayer(const Vector& position);
};

inline Aimbot aimbot;
