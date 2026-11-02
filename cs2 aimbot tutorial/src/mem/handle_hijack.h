/*
	This is a stand alone bypass made by Apxaey. Feel free to use this in your cheats but credit me for the bypass as i put alot of time into this.
	If you have some brain cells you will be able to incorporate this into your cheats and remain undetected by user-mode anticheats.
	Obviously standard cheat 'recommendations' still apply:
	1.) Use self-written or not signatured code
	2.) Dont write impossible values
	3.) If your going internal use a manual map injector

	If you follow the guidelines above and use this bypass you will be safe from usermode anticheats like VAC.
	Obviously you can build and adapt upon my code to suit your needs.
	If I was to make a cheat for myself i would put this bypass into something i call an 'external internal' cheat.
	Whereby you make a cheat and inject into a legitimate program like discord and add a check to the this bypass to only hijack a handle from the process you inject into, giving the appearence that nothing is out of the ordinary
	However you can implement this bypass into any form of cheat, its your decision.
	If you need want some more info i recommend you watch my YT video on this bypass.
	Anyways if you want to see more of my stuff feel free to join my discord server [uc does not approve!]. Here's my YT as well https://www.youtube.com/channel/UCPN6OOLxn1OaBP5jPThIiog.
	[support his yt! although he doesn't post anymore...]

	--- rewrite notes ---
	The original loop closed procHandle without nulling it, then NtOpenProcess failures left the
	stale value in place and the next iteration called CloseHandle on an already-closed handle.
	That is exactly 0xC0000008 (invalid handle) raised from ntdll. Every close now goes through
	CloseIfValid(), which nulls the handle, and both outputs are reset at entry.
*/

#include <Windows.h>
#include <iostream>
#include <TlHelp32.h>
#include <string>
#include <cstdint>
#include <vector>

// macros we use. Some can be found in wintrnl.h
#define SeDebugPriv 20
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004)
#define NtCurrentProcess ( (HANDLE)(LONG_PTR) -1 )
#define ProcessHandleType 0x7
#define SystemHandleInformation 16

/*
STRUCTURES NEEDED FOR NTOPENPROCESS:
*/
typedef struct _UNICODE_STRING {
	USHORT Length;
	USHORT MaximumLength;
	PWCH   Buffer;
} UNICODE_STRING, * PUNICODE_STRING;

typedef struct _OBJECT_ATTRIBUTES {
	ULONG           Length;
	HANDLE          RootDirectory;
	PUNICODE_STRING ObjectName;
	ULONG           Attributes;
	PVOID           SecurityDescriptor;
	PVOID           SecurityQualityOfService;
}  OBJECT_ATTRIBUTES, * POBJECT_ATTRIBUTES;

typedef struct _CLIENT_ID
{
	PVOID UniqueProcess;
	PVOID UniqueThread;
} CLIENT_ID, * PCLIENT_ID;

/*
STRUCTURES NEEDED FOR HANDLE INFORMATION:
*/

typedef struct _SYSTEM_HANDLE_TABLE_ENTRY_INFO
{
	ULONG ProcessId;
	BYTE ObjectTypeNumber;
	BYTE Flags;
	USHORT Handle;
	PVOID Object;
	ACCESS_MASK GrantedAccess;
} SYSTEM_HANDLE, * PSYSTEM_HANDLE; //i shortened it to SYSTEM_HANDLE for the sake of typing

typedef struct _SYSTEM_HANDLE_INFORMATION
{
	ULONG HandleCount;
	SYSTEM_HANDLE Handles[1];
} SYSTEM_HANDLE_INFORMATION, * PSYSTEM_HANDLE_INFORMATION;

/*
FUNCTION PROTOTYPES:
*/
typedef NTSTATUS(NTAPI* _NtDuplicateObject)(
	HANDLE SourceProcessHandle,
	HANDLE SourceHandle,
	HANDLE TargetProcessHandle,
	PHANDLE TargetHandle,
	ACCESS_MASK DesiredAccess,
	ULONG Attributes,
	ULONG Options
	);

typedef NTSTATUS(NTAPI* _RtlAdjustPrivilege)(
	ULONG Privilege,
	BOOLEAN Enable,
	BOOLEAN CurrentThread,
	PBOOLEAN Enabled
	);

typedef NTSYSAPI NTSTATUS(NTAPI* _NtOpenProcess)(
	PHANDLE            ProcessHandle,
	ACCESS_MASK        DesiredAccess,
	POBJECT_ATTRIBUTES ObjectAttributes,
	PCLIENT_ID         ClientId
	);

typedef NTSTATUS(NTAPI* _NtQuerySystemInformation)(
	ULONG SystemInformationClass, //your supposed to supply the whole class but microsoft kept the enum mostly empty so I just passed 16 instead for handle info. Thats why you get a warning in your code btw
	PVOID SystemInformation,
	ULONG SystemInformationLength,
	PULONG ReturnLength
	);

//the handles we will need to use later on

namespace hj
{
	inline HANDLE procHandle = NULL;
	inline HANDLE hProcess = NULL;
	inline HANDLE HijackedHandle = NULL;

	// simple function i made that will just initialize our Object_Attributes structure as NtOpenProcess will fail otherwise
	inline OBJECT_ATTRIBUTES InitObjectAttributes(PUNICODE_STRING name, ULONG attributes, HANDLE hRoot, PSECURITY_DESCRIPTOR security)
	{
		OBJECT_ATTRIBUTES object;

		object.Length = sizeof(OBJECT_ATTRIBUTES);
		object.ObjectName = name;
		object.Attributes = attributes;
		object.RootDirectory = hRoot;
		object.SecurityDescriptor = security;
		object.SecurityQualityOfService = nullptr;

		return object;
	}

	inline bool IsHandleValid(HANDLE handle) // i made this to simply check if a handle is valid rather than repeating the if statments
	{
		return handle != nullptr && handle != INVALID_HANDLE_VALUE;
	}

	// close + null in one step. closing twice is what raises 0xC0000008 out of ntdll.
	inline void CloseIfValid(HANDLE& handle)
	{
		if (IsHandleValid(handle))
			CloseHandle(handle);

		handle = nullptr;
	}

	inline HANDLE HijackExistingHandle(DWORD dwTargetProcessId)
	{
		// a failed attempt must never hand back a handle from a previous run
		HijackedHandle = nullptr;
		hProcess = nullptr;

		const HMODULE Ntdll = GetModuleHandleA("ntdll");
		if (!Ntdll)
			return nullptr;

		const auto RtlAdjustPrivilege = reinterpret_cast<_RtlAdjustPrivilege>(GetProcAddress(Ntdll, "RtlAdjustPrivilege"));
		const auto NtQuerySystemInformation = reinterpret_cast<_NtQuerySystemInformation>(GetProcAddress(Ntdll, "NtQuerySystemInformation"));
		const auto NtDuplicateObject = reinterpret_cast<_NtDuplicateObject>(GetProcAddress(Ntdll, "NtDuplicateObject"));
		const auto NtOpenProcess = reinterpret_cast<_NtOpenProcess>(GetProcAddress(Ntdll, "NtOpenProcess"));

		if (!NtQuerySystemInformation || !NtDuplicateObject || !NtOpenProcess)
			return nullptr;

		// SeDebugPrivilege so NtOpenProcess can reach whatever process already holds the target handle
		if (RtlAdjustPrivilege)
		{
			BOOLEAN oldPriv = FALSE;
			RtlAdjustPrivilege(SeDebugPriv, TRUE, FALSE, &oldPriv);
		}

		// the old code aborted after 50 handles *we* opened; measure once, it does not change mid-scan
		DWORD openHandles = 0;
		if (GetProcessHandleCount(GetCurrentProcess(), &openHandles) && openHandles > 50)
			return nullptr;

		OBJECT_ATTRIBUTES objAttribute = InitObjectAttributes(nullptr, 0, nullptr, nullptr);
		CLIENT_ID clientID = { nullptr, nullptr };

		// grow the buffer until the kernel stops reporting STATUS_INFO_LENGTH_MISMATCH
		std::vector<std::uint8_t> buffer(0x10000);
		NTSTATUS status = STATUS_INFO_LENGTH_MISMATCH;

		while (true)
		{
			status = NtQuerySystemInformation(
				SystemHandleInformation, buffer.data(),
				static_cast<ULONG>(buffer.size()), nullptr);

			if (status != STATUS_INFO_LENGTH_MISMATCH)
				break;

			if (buffer.size() >= (64u << 20))
				return nullptr;

			buffer.resize(buffer.size() * 2);
		}

		if (!NT_SUCCESS(status))
			return nullptr;

		const auto* hInfo = reinterpret_cast<const SYSTEM_HANDLE_INFORMATION*>(buffer.data());

		for (ULONG i = 0; i < hInfo->HandleCount; ++i)
		{
			const SYSTEM_HANDLE& entry = hInfo->Handles[i];

			// handle type 0x7 = process, so we skip file/event/section handles
			if (entry.ObjectTypeNumber != ProcessHandleType)
				continue;

			clientID.UniqueProcess = reinterpret_cast<PVOID>(static_cast<std::uintptr_t>(entry.ProcessId));

			// open the process that owns the handle - NOT our target
			procHandle = nullptr;
			status = NtOpenProcess(&procHandle, PROCESS_DUP_HANDLE, &objAttribute, &clientID);

			if (!NT_SUCCESS(status) || !IsHandleValid(procHandle))
			{
				CloseIfValid(procHandle);
				continue;
			}

			HANDLE duplicate = nullptr;
			status = NtDuplicateObject(
				procHandle, reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(entry.Handle)),
				NtCurrentProcess, &duplicate, PROCESS_ALL_ACCESS, 0, 0);

			// source process is no longer needed either way
			CloseIfValid(procHandle);

			if (!NT_SUCCESS(status) || !IsHandleValid(duplicate))
			{
				CloseIfValid(duplicate);
				continue;
			}

			if (::GetProcessId(duplicate) != dwTargetProcessId)
			{
				CloseIfValid(duplicate);
				continue;
			}

			HijackedHandle = duplicate;
			hProcess = duplicate;
			break;
		}

		CloseIfValid(procHandle);

		return hProcess;
	}
}
