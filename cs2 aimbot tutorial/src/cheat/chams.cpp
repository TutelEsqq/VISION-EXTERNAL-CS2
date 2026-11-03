#include "chams.hpp"
#include "entity.hpp"

#include <Windows.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include <string>
#include <vector>
#include <cstring>

// Chams via material hook, adapted from the public approach by xsip
// (github.com/xsip/CS2-External-Chams, MIT-style credit kept in comments).
//
// How it works:
//   1. pattern-scan CAnimatableSceneObjectDesc::Render in scenesystem.dll
//   2. find the .rdata vtable entry that points at it
//   3. create a chams material by running a compiled thread function inside the
//      game (CreateRemoteThread) that calls the game's own LoadKV3/CreateMaterial
//   4. copy a compiled render hook into the game (RWX), patch its rip-relative
//      references to a remote data block, and overwrite the vtable entry
//   5. the hook runs on the game's render thread and recolors every animatable
//      scene object (r/g/b/a + material) before the engine draws it
//
// Controls: the remote data block fields are updated externally (SetEnabled /
// SetColor), so chams toggle and recolor instantly without touching the hook.

namespace chams
{
	namespace
	{
		// ------------------------- remote helpers -------------------------

		struct RemoteModule
		{
			uintptr_t base = 0;
			size_t size = 0;
			bool Valid() const { return base && size; }
		};

		RemoteModule GetRemoteModule(const char* name)
		{
			HANDLE handle = mem.RawHandle();

			if (!handle)
				return {};

			HMODULE modules[1024]{};
			DWORD needed = 0;

			if (!EnumProcessModules(handle, modules, sizeof(modules), &needed))
				return {};

			const DWORD count = needed / sizeof(HMODULE);

			for (DWORD i = 0; i < count; ++i)
			{
				char path[MAX_PATH]{};

				if (!GetModuleFileNameExA(handle, modules[i], path, sizeof(path)))
					continue;

				const char* slash = std::strrchr(path, '\\');
				const char* baseName = slash ? slash + 1 : path;

				if (_stricmp(baseName, name) == 0)
				{
					MODULEINFO info{};
					if (!GetModuleInformation(handle, modules[i], &info, sizeof(info)))
						return {};

					return { reinterpret_cast<uintptr_t>(info.lpBaseOfDll), info.SizeOfImage };
				}
			}

			return {};
		}

		bool ReadMem(uintptr_t addr, void* buffer, size_t size)
		{
			return mem.ReadRaw(addr, buffer, size);
		}

		bool WriteMem(uintptr_t addr, const void* data, size_t size)
		{
			return mem.WriteRaw(addr, data, size);
		}

		template <typename T>
		bool WriteValue(uintptr_t addr, const T& value)
		{
			return WriteMem(addr, &value, sizeof(T));
		}

		void* RemoteAlloc(size_t size)
		{
			return VirtualAllocEx(mem.RawHandle(), nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
		}

		void* RemoteAllocRW(size_t size)
		{
			return VirtualAllocEx(mem.RawHandle(), nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		}

		// "AA BB ?? CD" - ?? is a wildcard byte
		uintptr_t ScanModule(const RemoteModule& mod, const char* pattern)
		{
			if (!mod.Valid())
				return 0;

			std::vector<uint8_t> image(mod.size);

			if (!ReadMem(mod.base, image.data(), image.size()))
				return 0;

			// parse the pattern once
			std::vector<int> bytes;
			std::vector<int> masks; // -1 = wildcard

			const char* p = pattern;

			while (*p)
			{
				while (*p == ' ')
					++p;

				if (!*p)
					break;

				if (p[0] == '?')
				{
					// single or double '?' - wildcard
					bytes.push_back(0);
					masks.push_back(-1);
					p += 2;
				}
				else
				{
					bytes.push_back(static_cast<int>(std::strtoul(p, nullptr, 16)));
					masks.push_back(0);
					p += 2;
				}
			}

			const size_t plen = bytes.size();

			if (!plen || image.size() < plen)
				return 0;

			for (size_t i = 0; i + plen <= image.size(); ++i)
			{
				bool ok = true;

				for (size_t j = 0; j < plen; ++j)
				{
					if (masks[j] == 0 && image[i + j] != static_cast<uint8_t>(bytes[j]))
					{
						ok = false;
						break;
					}
				}

				if (ok)
					return mod.base + i;
			}

			return 0;
		}

		// find a PE section by name (remote)
		bool GetSection(const RemoteModule& mod, const char* name, uintptr_t& addr, size_t& size)
		{
			uint8_t headers[0x1000]{};

			if (!ReadMem(mod.base, headers, sizeof(headers)))
				return false;

			const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(headers);

			if (dos->e_magic != IMAGE_DOS_SIGNATURE)
				return false;

			const auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(headers + dos->e_lfanew);

			if (nt->Signature != IMAGE_NT_SIGNATURE)
				return false;

			const auto section = IMAGE_FIRST_SECTION(nt);
			const WORD count = nt->FileHeader.NumberOfSections;

			for (WORD i = 0; i < count; ++i)
			{
				if (std::strncmp(reinterpret_cast<const char*>(section[i].Name), name, 8) == 0)
				{
					addr = mod.base + section[i].VirtualAddress;
					size = section[i].Misc.VirtualSize;
					return true;
				}
			}

			return false;
		}

		// find the .rdata vtable entry pointing at fn; backscan to locate the table start
		bool FindVTableEntry(const RemoteModule& mod, uintptr_t fn, uintptr_t& vtableAddr, int& index)
		{
			uintptr_t rdata = 0;
			size_t rdataSize = 0;

			if (!GetSection(mod, ".rdata", rdata, rdataSize))
				return false;

			std::vector<uint8_t> data(rdataSize);

			if (!ReadMem(rdata, data.data(), data.size()))
				return false;

			for (size_t i = 0; i + 8 <= data.size(); i += 8)
			{
				uint64_t value = 0;
				std::memcpy(&value, &data[i], 8);

				if (value != fn)
					continue;

				// backscan: vtable entries all point into the module
				size_t back = i;
				int found = 0;
				uintptr_t start = 0;

				while (back >= 8)
				{
					back -= 8;
					uint64_t candidate = 0;
					std::memcpy(&candidate, &data[back], 8);

					if (candidate > mod.base && candidate < mod.base + mod.size)
						++found;
					else
					{
						start = rdata + back + 8;
						break;
					}
				}

				if (!start)
					start = rdata + (i - static_cast<size_t>(found) * 8);

				vtableAddr = start;
				index = found;
				return true;
			}

			return false;
		}

		// remote PE export lookup by name
		uintptr_t RemoteGetProcAddress(const RemoteModule& mod, const char* fnName)
		{
			uint8_t headers[0x1000]{};

			if (!ReadMem(mod.base, headers, sizeof(headers)))
				return 0;

			const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(headers);

			if (dos->e_magic != IMAGE_DOS_SIGNATURE)
				return 0;

			const auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(headers + dos->e_lfanew);
			const auto& exportDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];

			if (!exportDir.Size || !exportDir.VirtualAddress)
				return 0;

			std::vector<uint8_t> dir(exportDir.Size);

			if (!ReadMem(mod.base + exportDir.VirtualAddress, dir.data(), dir.size()))
				return 0;

			const auto names = reinterpret_cast<uint32_t*>(dir.data());
			const uintptr_t namesVA = exportDir.VirtualAddress;
			(void)names;

			// the export directory layout inside the copied buffer:
			const auto exp = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(dir.data());
			const auto funcs = reinterpret_cast<uint32_t*>(dir.data() + (exp->AddressOfFunctions - namesVA));
			const auto nameRvas = reinterpret_cast<uint32_t*>(dir.data() + (exp->AddressOfNames - namesVA));
			const auto ordinals = reinterpret_cast<uint16_t*>(dir.data() + (exp->AddressOfNameOrdinals - namesVA));

			for (uint32_t i = 0; i < exp->NumberOfNames; ++i)
			{
				const uintptr_t nameRva = nameRvas[i];
				const char* name = nullptr;

				// names live anywhere in the export section - read via RVA offset into dir if covered,
				// otherwise read separately
				if (nameRva >= exportDir.VirtualAddress && nameRva + 64 <= exportDir.VirtualAddress + exportDir.Size)
				{
					name = reinterpret_cast<const char*>(dir.data() + (nameRva - exportDir.VirtualAddress));
				}
				else
				{
					static char fallback[64];
					if (!ReadMem(mod.base + nameRva, fallback, sizeof(fallback) - 1))
						continue;

					fallback[sizeof(fallback) - 1] = 0;
					name = fallback;
				}

				if (std::strcmp(name, fnName) == 0)
					return mod.base + funcs[ordinals[i]];
			}

			return 0;
		}

		// ------------------------- our compiled code -------------------------

		// CBaseSceneData layout used by the hook (matches xsip's static_assert)
		struct SceneData
		{
			char pad0[0x20];
			void* material;
			void* material2;
			char pad1[0x18];
			uint8_t r;
			uint8_t g;
			uint8_t b;
			uint8_t a;
		};

		struct KV3ID
		{
			const char* name;
			uint64_t unk0;
			uint64_t unk1;
		};

		// sizeof == 0x118 per the source2 layout used by the reference project
		struct KV3Buffer
		{
			char data[0x118];
		};

		struct CreateMaterialCtx
		{
			KV3Buffer* kv;
			void* loadKv3;
			void* createMaterial;
			void* outMaterial;      // CMaterial2*
			const char* kv3Text;
			const char* kvIdName;
			const char* matName;
			bool hadErrors;
			bool finished;
		};

		// the hook's tunables, living in remote memory
		struct HookData
		{
			uint64_t originalFn;    // GeneratePrimitives
			uint64_t material;      // CMaterial2*
			uint32_t tint;          // packed rgba (bytes r,g,b,a in memory order)
			uint32_t stride;        // CMeshDrawPrimitive stride (0x68 on current builds)
			bool enabled;
			uint32_t maxBatch;      // sanity cap for the recolor loop
		};

		// referenced by the copied hook code via rip-relative loads; the remote
		// dataPtrStorage mirrors this pointer so the patched code reads the same value
		HookData* g_hookData = nullptr;

		// hook thread proc + render hook: both compiled into their own code segments
		// and copied into the game. they must not touch CRT, static locals, string
		// literals or anything that would emit non-rip-relative references.

#pragma code_seg(".chams_mat")
#pragma optimize("", off)
#pragma runtime_checks("", off)
#pragma check_stack(off)
		__declspec(safebuffers)
		DWORD WINAPI CreateMaterialThread(LPVOID lpParam)
		{
			CreateMaterialCtx* ctx = reinterpret_cast<CreateMaterialCtx*>(lpParam);
			KV3Buffer* kv = ctx->kv;

			for (size_t i = 0; i < sizeof(KV3Buffer); ++i)
				reinterpret_cast<uint8_t*>(kv)[i] = 0;

			KV3ID kvId;
			kvId.name = ctx->kvIdName;
			kvId.unk0 = 0x469806E97412167CULL;
			kvId.unk1 = 0xE73790B53EE6F2AFULL;

			typedef bool(__fastcall* LoadKV3Fn)(KV3Buffer*, void*, const char*, KV3ID*, void*);
			LoadKV3Fn loadKv3 = reinterpret_cast<LoadKV3Fn>(ctx->loadKv3);

			if (!loadKv3(kv, nullptr, ctx->kv3Text, &kvId, nullptr))
			{
				ctx->hadErrors = true;
				ctx->finished = true;
				return 0;
			}

			typedef void(__fastcall* CreateMaterialFn)(void*, void**, const char*, KV3Buffer*, int, int);
			CreateMaterialFn createMaterial = reinterpret_cast<CreateMaterialFn>(ctx->createMaterial);

			void* material = nullptr;
			createMaterial(nullptr, &material, ctx->matName, kv, 0, 1);

			ctx->outMaterial = material;
			ctx->hadErrors = material == nullptr;
			ctx->finished = true;
			return 1;
		}

		DWORD WINAPI CreateMaterialThreadEnd() { return 0; }
#pragma check_stack(on)
#pragma runtime_checks("", restore)
#pragma optimize("", on)
#pragma code_seg()

#pragma code_seg(".chams_hook")
#pragma optimize("", off)
#pragma runtime_checks("", off)
#pragma check_stack(off)
	__declspec(safebuffers)
	void* __fastcall RenderObjectsHook(
		uint64_t a1, uint64_t object, uint64_t a3, uint64_t renderBuffer,
		uint64_t a5, uint64_t a6, uint64_t a7)
	{
		HookData* data = g_hookData;

		if (!data)
			return nullptr;

		typedef void* (__fastcall* GeneratePrimitivesFn)(uint64_t, uint64_t, uint64_t, uint64_t);
		GeneratePrimitivesFn original = reinterpret_cast<GeneratePrimitivesFn>(data->originalFn);

		// CMeshPrimitiveOutputBuffer: +0x00 CMeshDrawPrimitive* out, +0x0C int startPrimitives
		const int oldCount = *reinterpret_cast<volatile int*>(renderBuffer + 0xC);

		original(a1, object, a3, renderBuffer);

		const int newCount = *reinterpret_cast<volatile int*>(renderBuffer + 0xC);

		if (data->enabled && newCount > oldCount)
		{
			uintptr_t out = *reinterpret_cast<uintptr_t*>(renderBuffer);

			if (out)
			{
				const uint32_t stride = data->stride;
				int32_t limit = newCount - oldCount;

				if (limit > static_cast<int32_t>(data->maxBatch))
					limit = static_cast<int32_t>(data->maxBatch);

				for (int32_t i = 0; i < limit; ++i)
				{
					uint8_t* mesh = reinterpret_cast<uint8_t*>(out)
						+ static_cast<uintptr_t>(oldCount + i) * stride;

					// +0x18 scene object, +0x20/+0x28 materials - only touch
					// entries that look like real meshes so a drifted stride
					// degrades to "no chams" instead of corruption
					const uint64_t sceneObject = *reinterpret_cast<volatile uint64_t*>(mesh + 0x18);
					const uint64_t currentMaterial = *reinterpret_cast<volatile uint64_t*>(mesh + 0x20);

					if (!sceneObject || !currentMaterial)
						continue;

					*reinterpret_cast<volatile uint64_t*>(mesh + 0x20) = data->material;
					*reinterpret_cast<volatile uint64_t*>(mesh + 0x28) = data->material;
					*reinterpret_cast<volatile uint32_t*>(mesh + 0x50) = data->tint;
				}
			}
		}

		return nullptr;
	}

	void RenderObjectsHookEnd() {}
#pragma check_stack(on)
#pragma runtime_checks("", restore)
#pragma optimize("", on)
#pragma code_seg()

		// copy a function's machine code (start..end markers) into the game
		void* CopyFunctionToGame(void* fnStart, void* fnEnd)
		{
			const uintptr_t start = reinterpret_cast<uintptr_t>(fnStart);
			const uintptr_t end = reinterpret_cast<uintptr_t>(fnEnd);
			const size_t size = end > start ? end - start : 0;

			if (!size)
				return nullptr;

			void* remote = RemoteAlloc(size);

			if (!remote)
				return nullptr;

			if (!WriteMem(reinterpret_cast<uintptr_t>(remote), fnStart, size))
				return nullptr;

			return remote;
		}

		// patch every rip-relative load/lea inside the copied code to point at `storage`
		// (48/4C 8B|8D modrm with mod=00 rm=101, followed by disp32)
		bool PatchRipReferences(uintptr_t remoteCode, const uint8_t* localCode, size_t size, uint64_t storage)
		{
			int patched = 0;

			for (size_t i = 0; i + 7 <= size; ++i)
			{
				const uint8_t rex = localCode[i];

				if (rex != 0x48 && rex != 0x4C)
					continue;

				const uint8_t op = localCode[i + 1];

				if (op != 0x8B && op != 0x8D)
					continue;

				const uint8_t modrm = localCode[i + 2];

				if ((modrm & 0xC7) != 0x05)
					continue;

				const uintptr_t instrAddr = remoteCode + i;
				const int32_t disp = static_cast<int32_t>(storage - (instrAddr + 7));

				if (!WriteValue<int32_t>(instrAddr + 3, disp))
					return false;

				++patched;
				i += 6; // skip past the disp32
			}

			return patched > 0;
		}

		// ------------------------- state -------------------------

		enum class State
		{
			NotTried,
			Ready,
			Failed
		};

		State g_state = State::NotTried;
		void* g_dataRemote = nullptr;     // HookData in the game
		void* g_hookRemote = nullptr;     // copied render hook
		uintptr_t g_vtableEntry = 0;      // address of the patched vtable slot
		uint64_t g_originalFn = 0;
		void* g_materialRemote = nullptr; // copied material thread (kept for reuse)
		bool g_hookInstalled = false;
	}

	// ------------------------- public API -------------------------

	bool EnsureInit()
	{
		if (g_state == State::Ready || g_state == State::Failed)
			return g_state == State::Ready;

		if (!mem.Attached() || !mem.RawHandle())
			return false;

		printf("[chams] initializing...\n");

		const RemoteModule scenesystem = GetRemoteModule("scenesystem.dll");
		const RemoteModule materialsystem = GetRemoteModule("materialsystem2.dll");
		const RemoteModule tier0 = GetRemoteModule("tier0.dll");

		if (!scenesystem.Valid() || !materialsystem.Valid() || !tier0.Valid())
		{
			printf("[chams] modules not found\n");
			g_state = State::Failed;
			return false;
		}

		// 1. resolve the game's CreateMaterial + tier0!LoadKV3
		const uintptr_t createMaterialFn = ScanModule(materialsystem,
			"48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 41 56 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 8B F2");
		const uintptr_t loadKv3 = RemoteGetProcAddress(tier0,
			"?LoadKV3@@YA_NPEAVKeyValues3@@PEAVCUtlString@@PEBDAEBUKV3ID_t@@2I@Z");

		if (!createMaterialFn || !loadKv3)
		{
			printf("[chams] createMaterial(%p) / LoadKV3(%p) not found\n",
				reinterpret_cast<void*>(createMaterialFn), reinterpret_cast<void*>(loadKv3));
			g_state = State::Failed;
			return false;
		}

		printf("[chams] CreateMaterial %p, LoadKV3 %p\n",
			reinterpret_cast<void*>(createMaterialFn), reinterpret_cast<void*>(loadKv3));

		// 2. write strings + kv3 buffer + context, run the material thread in-game
		// kv3 material, tuned for current builds (translucent, no z-test = through-wall)
		const char* kv3Text =
			"<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d}\n"
			"format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->\n"
			"{\n"
			"    shader = \"csgo_character.vfx\"\n"
			"    F_TRANSLUCENT = 1\n"
			"    F_BLEND_MODE = 1\n"
			"    F_DISABLE_Z_BUFFERING = 1\n"
			"    g_vColorTint = [1.0, 1.0, 1.0, 1.0]\n"
			"    TextureAmbientOcclusion = resource:\"materials/default/default_mask_tga_fde710a5.vtex\"\n"
			"    g_tAmbientOcclusion = resource:\"materials/default/default_mask_tga_fde710a5.vtex\"\n"
			"    g_tColor = resource:\"materials/default/default_mask_tga_fde710a5.vtex\"\n"
			"}";

		void* remoteKv3Text = RemoteAllocRW(strlen(kv3Text) + 1);
		void* remoteKvIdName = RemoteAllocRW(8);
		void* remoteMatName = RemoteAllocRW(32);
		void* remoteKv = RemoteAllocRW(sizeof(KV3Buffer));
		void* remoteCtx = RemoteAllocRW(sizeof(CreateMaterialCtx));

		if (!remoteKv3Text || !remoteKvIdName || !remoteMatName || !remoteKv || !remoteCtx)
		{
			printf("[chams] remote alloc failed\n");
			g_state = State::Failed;
			return false;
		}

		WriteMem(reinterpret_cast<uintptr_t>(remoteKv3Text), kv3Text, strlen(kv3Text) + 1);
		WriteMem(reinterpret_cast<uintptr_t>(remoteKvIdName), "generic", 8);
		WriteMem(reinterpret_cast<uintptr_t>(remoteMatName), "VisionChamsMaterial", 20);

		CreateMaterialCtx ctx{};
		ctx.kv = static_cast<KV3Buffer*>(remoteKv);
		ctx.loadKv3 = reinterpret_cast<void*>(loadKv3);
		ctx.createMaterial = reinterpret_cast<void*>(createMaterialFn);
		ctx.outMaterial = nullptr;
		ctx.kv3Text = static_cast<const char*>(remoteKv3Text);
		ctx.kvIdName = static_cast<const char*>(remoteKvIdName);
		ctx.matName = static_cast<const char*>(remoteMatName);
		ctx.hadErrors = false;
		ctx.finished = false;

		WriteValue<CreateMaterialCtx>(reinterpret_cast<uintptr_t>(remoteCtx), ctx);

		void* matThread = CopyFunctionToGame(CreateMaterialThread, CreateMaterialThreadEnd);

		if (!matThread)
		{
			printf("[chams] failed to copy material thread\n");
			g_state = State::Failed;
			return false;
		}

		g_materialRemote = matThread;

		HANDLE thread = CreateRemoteThread(mem.RawHandle(), nullptr, 0,
			reinterpret_cast<LPTHREAD_START_ROUTINE>(matThread), remoteCtx, 0, nullptr);

		if (!thread)
		{
			printf("[chams] CreateRemoteThread failed, error %lu\n", GetLastError());
			g_state = State::Failed;
			return false;
		}

		if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0)
		{
			printf("[chams] material thread timed out\n");
			CloseHandle(thread);
			g_state = State::Failed;
			return false;
		}

		CloseHandle(thread);

		CreateMaterialCtx result{};
		ReadMem(reinterpret_cast<uintptr_t>(remoteCtx), &result, sizeof(result));

		if (result.hadErrors || !result.outMaterial)
		{
			printf("[chams] material creation failed (hadErrors=%d, mat=%p)\n",
				result.hadErrors ? 1 : 0, result.outMaterial);
			g_state = State::Failed;
			return false;
		}

		printf("[chams] material created: %p\n", result.outMaterial);

		// 3. find the animatable render function and its vtable entries
		const uintptr_t renderFn = ScanModule(scenesystem,
			"48 8B C4 48 89 58 ? 48 89 50 ? 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ? ? ? ?");

		if (!renderFn)
		{
			printf("[chams] render function pattern not found\n");
			g_state = State::Failed;
			return false;
		}

		// the same implementation is referenced by two classes - hook every
		// vtable entry that points at it, original fn is shared
		struct VTableHit { uintptr_t addr; int index; };
		std::vector<VTableHit> vtableHits;

		{
			uintptr_t rdata = 0;
			size_t rdataSize = 0;

			if (!GetSection(scenesystem, ".rdata", rdata, rdataSize))
			{
				printf("[chams] .rdata not found\n");
				g_state = State::Failed;
				return false;
			}

			std::vector<uint8_t> data(rdataSize);

			if (!ReadMem(rdata, data.data(), data.size()))
			{
				g_state = State::Failed;
				return false;
			}

			for (size_t i = 0; i + 8 <= data.size(); i += 8)
			{
				uint64_t value = 0;
				std::memcpy(&value, &data[i], 8);

				if (value != renderFn)
					continue;

				size_t back = i;
				int found = 0;
				uintptr_t start = 0;

				while (back >= 8)
				{
					back -= 8;
					uint64_t candidate = 0;
					std::memcpy(&candidate, &data[back], 8);

					if (candidate > scenesystem.base && candidate < scenesystem.base + scenesystem.size)
						++found;
					else
					{
						start = rdata + back + 8;
						break;
					}
				}

				if (!start)
					start = rdata + (i - static_cast<size_t>(found) * 8);

				vtableHits.push_back({ rdata + i, found });
			}
		}

		if (vtableHits.empty())
		{
			printf("[chams] vtable entries for render fn not found\n");
			g_state = State::Failed;
			return false;
		}

		for (const auto& hit : vtableHits)
			printf("[chams] vtable entry %p (index %d)\n",
				reinterpret_cast<void*>(hit.addr), hit.index);

		uint64_t originalFn = 0;
		ReadMem(vtableHits[0].addr, &originalFn, 8);
		g_originalFn = originalFn;

		// 4. hook data block in the game
		void* dataRemote = RemoteAllocRW(sizeof(HookData));

		if (!dataRemote)
		{
			printf("[chams] hook data alloc failed\n");
			g_state = State::Failed;
			return false;
		}

		HookData data{};
		data.originalFn = originalFn;
		data.material = reinterpret_cast<uint64_t>(result.outMaterial);
		data.enabled = false;
		data.tint = 0xFF00FFFF;        // magenta rgba (bytes r,g,b,a)
		data.stride = 0x68;            // CMeshDrawPrimitive stride, current builds
		data.maxBatch = 4096;

		WriteValue<HookData>(reinterpret_cast<uintptr_t>(dataRemote), data);
		g_dataRemote = dataRemote;

		// 5. copy the render hook and patch its rip references
		void* hookRemote = CopyFunctionToGame(RenderObjectsHook, RenderObjectsHookEnd);

		if (!hookRemote)
		{
			printf("[chams] failed to copy render hook\n");
			g_state = State::Failed;
			return false;
		}

		g_hookRemote = hookRemote;

		// storage holding the HookData pointer, referenced by the hook code
		void* dataPtrStorage = RemoteAllocRW(8);

		if (!dataPtrStorage)
		{
			printf("[chams] data ptr storage alloc failed\n");
			g_state = State::Failed;
			return false;
		}

		WriteValue<uint64_t>(reinterpret_cast<uintptr_t>(dataPtrStorage),
			reinterpret_cast<uint64_t>(dataRemote));

		const uintptr_t localStart = reinterpret_cast<uintptr_t>(RenderObjectsHook);
		const uintptr_t localEnd = reinterpret_cast<uintptr_t>(RenderObjectsHookEnd);
		const size_t hookSize = localEnd > localStart ? localEnd - localStart : 0;

		if (!PatchRipReferences(reinterpret_cast<uintptr_t>(hookRemote),
			reinterpret_cast<const uint8_t*>(localStart), hookSize,
			reinterpret_cast<uint64_t>(dataPtrStorage)))
		{
			printf("[chams] no rip references patched - hook code unusable\n");
			g_state = State::Failed;
			return false;
		}

		// 6. overwrite every vtable entry that points at the render fn
		for (const auto& hit : vtableHits)
		{
			DWORD oldProtect = 0;

			if (!VirtualProtectEx(mem.RawHandle(), reinterpret_cast<void*>(hit.addr),
				8, PAGE_READWRITE, &oldProtect))
			{
				printf("[chams] VirtualProtectEx failed, error %lu\n", GetLastError());
				g_state = State::Failed;
				return false;
			}

			if (!WriteValue<uint64_t>(hit.addr, reinterpret_cast<uint64_t>(hookRemote)))
			{
				printf("[chams] vtable write failed\n");
				VirtualProtectEx(mem.RawHandle(), reinterpret_cast<void*>(hit.addr),
					8, oldProtect, &oldProtect);
				g_state = State::Failed;
				return false;
			}

			VirtualProtectEx(mem.RawHandle(), reinterpret_cast<void*>(hit.addr),
				8, oldProtect, &oldProtect);
		}

		g_hookInstalled = true;
		g_state = State::Ready;
		printf("[chams] ready - %zu vtable entries hooked\n", vtableHits.size());
		return true;
	}

	void SetEnabled(bool enabled)
	{
		if (!g_dataRemote)
			return;

		WriteValue<bool>(reinterpret_cast<uintptr_t>(g_dataRemote) + offsetof(HookData, enabled), enabled);
		printf("[chams] %s\n", enabled ? "enabled" : "disabled");
	}

	void SetColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
	{
		if (!g_dataRemote)
			return;

		// packed rgba - bytes r,g,b,a in memory order
		const uint32_t tint = static_cast<uint32_t>(r) |
			(static_cast<uint32_t>(g) << 8) |
			(static_cast<uint32_t>(b) << 16) |
			(static_cast<uint32_t>(a) << 24);

		WriteValue<uint32_t>(reinterpret_cast<uintptr_t>(g_dataRemote) + offsetof(HookData, tint), tint);
	}
}
