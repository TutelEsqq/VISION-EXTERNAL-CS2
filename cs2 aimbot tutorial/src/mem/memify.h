#pragma once
// literally all of these are in handle_hijack.h, so feel free to include only that one.
// ke these are here so memify.h stands on its own too. *except for psapi.h*
#include <Windows.h> // RPM + WPM
#include <TlHelp32.h>
#include <Psapi.h>   // enumprocessmodules
#include <string>
#include <string_view>
#include <iostream>

#include "handle_hijack.h"

/*
Created By https://github.com/carlgwastaken/
Please Support Open Source and leave this message here if you're using in your own source
Besides that, enjoy!
*/

// ReadProcessMemory is basically this function + error handling, so we save some performance.
// 5% according to the post below:
// unknowncheats.me/forum/general-programming-and-reversing/230813-readprocessmemory-vs-ntreadvirtualmemory-performance-benchmark-comparison.html
typedef NTSTATUS(WINAPI* pNtReadVirtualMemory)(HANDLE ProcessHandle, PVOID BaseAddress, PVOID Buffer, ULONG NumberOfBytesToRead, PULONG NumberOfBytesRead);
typedef NTSTATUS(WINAPI* pNtWriteVirtualMemory)(HANDLE Processhandle, PVOID BaseAddress, PVOID Buffer, ULONG NumberOfBytesToWrite, PULONG NumberOfBytesWritten);

class memify
{
private:
	HANDLE handle = nullptr;
	DWORD processID = 0;

	std::string targetName;
	std::wstring targetNameW;

	bool hijackAttempted = false;

	pNtReadVirtualMemory VRead = nullptr;
	pNtWriteVirtualMemory VWrite = nullptr;

	// toolhelp walk, case-insensitive, snapshot handle always closed
	DWORD FindProcessId() const
	{
		const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot != INVALID_HANDLE_VALUE)
		{
			// toolhelp's A/W split follows UNICODE; pin W explicitly so both configs agree
			PROCESSENTRY32W pe{};
			pe.dwSize = sizeof(PROCESSENTRY32W);

			if (Process32FirstW(snapshot, &pe))
			{
				do
				{
					if (_wcsicmp(pe.szExeFile, targetNameW.c_str()) == 0)
					{
						const DWORD pid = pe.th32ProcessID;
						CloseHandle(snapshot);
						return pid;
					}
				} while (Process32NextW(snapshot, &pe));
			}

			CloseHandle(snapshot);
		}

		// fallback: enumerate every pid and compare main module names.
		// each pid gets its own probe handle - the old code queried GetModuleBaseNameA
		// on our (still null) target handle, so this branch could never match anything.
		DWORD ids[1024]{};
		DWORD needed = 0;

		if (!EnumProcesses(ids, sizeof(ids), &needed))
			return 0;

		const DWORD count = needed / sizeof(DWORD);

		for (DWORD i = 0; i < count; ++i)
		{
			if (!ids[i])
				continue;

			const HANDLE probe = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, ids[i]);
			if (!probe)
				continue;

			char moduleName[MAX_PATH]{};
			const DWORD length = GetModuleBaseNameA(probe, nullptr, moduleName, sizeof(moduleName));
			CloseHandle(probe);

			if (length && _stricmp(moduleName, targetName.c_str()) == 0)
				return ids[i];
		}

		return 0;
	}

	HANDLE OpenTarget(DWORD pid) const
	{
		return OpenProcess(
			PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION,
			FALSE, pid);
	}

	uintptr_t GetBaseModule(std::string_view moduleName) const
	{
		HMODULE modules[1024]{};
		DWORD needed = 0;

		if (!handle || !EnumProcessModules(handle, modules, sizeof(modules), &needed))
			return 0;

		const DWORD count = needed / sizeof(HMODULE);

		for (DWORD i = 0; i < count; ++i)
		{
			char buffer[MAX_PATH]{};

			if (GetModuleBaseNameA(handle, modules[i], buffer, sizeof(buffer)) &&
				moduleName.compare(buffer) == 0)
			{
				return reinterpret_cast<uintptr_t>(modules[i]);
			}
		}

		return 0;
	}

public:
	explicit memify(std::string_view processName)
		: targetName(processName), targetNameW(targetName.begin(), targetName.end())
	{
		VRead = reinterpret_cast<pNtReadVirtualMemory>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtReadVirtualMemory"));
		VWrite = reinterpret_cast<pNtWriteVirtualMemory>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtWriteVirtualMemory"));

		TryAttach();
	}

	// deconstructor, you can just use a void Exit(), which is less to type but whatever
	~memify()
	{
		Detach();
	}

	// drop the handle and forget the pid - used when cs2 restarts
	void Detach()
	{
		if (Attached() && handle != hj::HijackedHandle)
			CloseHandle(handle);

		handle = nullptr;

		if (hj::IsHandleValid(hj::HijackedHandle))
			CloseHandle(hj::HijackedHandle);

		hj::HijackedHandle = nullptr;
		hj::hProcess = nullptr;

		processID = 0;
		hijackAttempted = false;
	}

	// hijack first (no OpenProcess event in the game's audit), fall back to a plain handle.
	// call this repeatedly until it returns true - cs2 may not be running when the cheat starts.
	bool TryAttach()
	{
		if (Attached())
			return true;

		if (!processID)
		{
			processID = FindProcessId();
			hijackAttempted = false;
		}

		if (!processID)
			return false;

		// one hijack attempt per session: it walks every system handle, no reason to redo
		// that every retry if the kernel refuses SYSTEM_HANDLE_INFORMATION
		if (!hijackAttempted)
		{
			hijackAttempted = true;

			handle = hj::HijackExistingHandle(processID);

			if (hj::IsHandleValid(handle))
				return true;
		}

		handle = OpenTarget(processID);

		if (!hj::IsHandleValid(handle))
		{
			handle = nullptr;
			processID = 0;
			return false;
		}

		return true;
	}

	bool Attached() const
	{
		return hj::IsHandleValid(handle);
	}

	// handle still points at a live process
	bool Alive() const
	{
		if (!Attached())
			return false;

		DWORD exitCode = 0;

		if (!GetExitCodeProcess(handle, &exitCode))
			return false;

		return exitCode == STILL_ACTIVE;
	}

	// shorten name here
	uintptr_t GetBase(std::string_view moduleName) const
	{
		return GetBaseModule(moduleName);
	}

	// read
	template <typename T> // use types which are defined later on, so it's compatible with alot of shit.
	T Read(uintptr_t address) const
	{
		T buffer{};

		if (!handle || !VRead)
			return buffer;

		VRead(handle, reinterpret_cast<void*>(address), &buffer, sizeof(T), nullptr);
		return buffer;
	}

	template <typename T>
	T Write(uintptr_t address, T value) const
	{
		if (!handle || !VWrite)
			return value;

		VWrite(handle, reinterpret_cast<void*>(address), &value, sizeof(T), nullptr);
		return value;
	}

	// for reading structs and strings and shit
	bool ReadRaw(uintptr_t address, void* buffer, size_t size) const
	{
		if (!handle || !VRead || !buffer || !size)
			return false;

		SIZE_T bytesRead = 0;
		const NTSTATUS status = VRead(
			handle, reinterpret_cast<void*>(address), buffer,
			static_cast<ULONG>(size), reinterpret_cast<PULONG>(&bytesRead));

		// the old version wrote `return bytesRead = size;` - assignment, always true
		return NT_SUCCESS(status) && bytesRead == size;
	}

	// raw write for the chams shellcode machinery (arrays, not single PODs)
	bool WriteRaw(uintptr_t address, const void* data, size_t size) const
	{
		if (!handle || !VWrite || !data || !size)
			return false;

		SIZE_T bytesWritten = 0;
		const NTSTATUS status = VWrite(
			handle, reinterpret_cast<void*>(address), const_cast<void*>(data),
			static_cast<ULONG>(size), reinterpret_cast<PULONG>(&bytesWritten));

		return NT_SUCCESS(status) && bytesWritten == size;
	}

	// raw process handle for the chams module (VirtualAllocEx / remote threads)
	HANDLE RawHandle() const { return handle; }

	// utilities, shit that isn't required but nice to have

	bool ProcessIsOpen() const
	{
		return Attached();
	}

	bool InForeground() const
	{
		// just takes Counter-Strike 2 but change to whatever u want, or implement an input you can do that too
		const HWND current = GetForegroundWindow();

		char title[256]{};
		GetWindowTextA(current, title, sizeof(title));

		return strstr(title, "Counter-Strike 2") != nullptr;
	}
};
