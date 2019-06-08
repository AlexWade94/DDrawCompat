#define WIN32_LEAN_AND_MEAN

#include <algorithm>
#include <list>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <Windows.h>
#include <detours.h>
#include <Psapi.h>

#include "Common/Hook.h"
#include "Common/Log.h"

namespace
{
	struct HookedFunctionInfo
	{
		HMODULE module;
		void*& origFunction;
		void* newFunction;
	};

	std::map<void*, HookedFunctionInfo> g_hookedFunctions;

	std::map<void*, HookedFunctionInfo>::iterator findOrigFunc(void* origFunc)
	{
		return std::find_if(g_hookedFunctions.begin(), g_hookedFunctions.end(),
			[=](const auto& i) { return origFunc == i.first || origFunc == i.second.origFunction; });
	}

	std::vector<HMODULE> getProcessModules(HANDLE process)
	{
		std::vector<HMODULE> modules(10000);
		DWORD bytesNeeded = 0;
		if (EnumProcessModules(process, modules.data(), modules.size(), &bytesNeeded))
		{
			modules.resize(bytesNeeded / sizeof(modules[0]));
		}
		return modules;
	}

	std::set<void*> getIatHookFunctions(const char* moduleName, const char* funcName)
	{
		std::set<void*> hookFunctions;
		if (!moduleName || !funcName)
		{
			return hookFunctions;
		}

		auto modules = getProcessModules(GetCurrentProcess());
		for (auto module : modules)
		{
			FARPROC func = Compat::getProcAddressFromIat(module, moduleName, funcName);
			if (func)
			{
				hookFunctions.insert(func);
			}
		}

		return hookFunctions;
	}

	PIMAGE_NT_HEADERS getImageNtHeaders(HMODULE module)
	{
		PIMAGE_DOS_HEADER dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(module);
		if (IMAGE_DOS_SIGNATURE != dosHeader->e_magic)
		{
			return nullptr;
		}

		PIMAGE_NT_HEADERS ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS>(
			reinterpret_cast<char*>(dosHeader) + dosHeader->e_lfanew);
		if (IMAGE_NT_SIGNATURE != ntHeaders->Signature)
		{
			return nullptr;
		}

		return ntHeaders;
	}

	std::string getModuleBaseName(HMODULE module)
	{
		char path[MAX_PATH] = {};
		GetModuleFileName(module, path, sizeof(path));
		const char* lastBackSlash = strrchr(path, '\\');
		const char* baseName = lastBackSlash ? lastBackSlash + 1 : path;
		return baseName;
	}

	void hookFunction(const char* funcName, void*& origFuncPtr, void* newFuncPtr)
	{
		const auto it = findOrigFunc(origFuncPtr);
		if (it != g_hookedFunctions.end())
		{
			origFuncPtr = it->second.origFunction;
			return;
		}

		void* const hookedFuncPtr = origFuncPtr;

		DetourTransactionBegin();
		const bool attachSuccessful = NO_ERROR == DetourAttach(&origFuncPtr, newFuncPtr);
		const bool commitSuccessful = NO_ERROR == DetourTransactionCommit();
		if (!attachSuccessful || !commitSuccessful)
		{
			if (funcName)
			{
				Compat::LogDebug() << "ERROR: Failed to hook a function: " << funcName;
			}
			else
			{
				Compat::LogDebug() << "ERROR: Failed to hook a function: " << origFuncPtr;
			}
			return;
		}

		HMODULE module = nullptr;
		GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
			reinterpret_cast<char*>(hookedFuncPtr), &module);
		g_hookedFunctions.emplace(
			std::make_pair(hookedFuncPtr, HookedFunctionInfo{ module, origFuncPtr, newFuncPtr }));
	}

	void unhookFunction(const std::map<void*, HookedFunctionInfo>::iterator& hookedFunc)
	{
		DetourTransactionBegin();
		DetourDetach(&hookedFunc->second.origFunction, hookedFunc->second.newFunction);
		DetourTransactionCommit();

		if (hookedFunc->second.module)
		{
			FreeLibrary(hookedFunc->second.module);
		}
		g_hookedFunctions.erase(hookedFunc);
	}
}

namespace Compat
{
	void redirectIatHooks(const char* moduleName, const char* funcName, void* newFunc)
	{
		auto hookFunctions(getIatHookFunctions(moduleName, funcName));

		for (auto hookFunc : hookFunctions)
		{
			HMODULE module = nullptr;
			if (!GetModuleHandleEx(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				static_cast<LPCSTR>(hookFunc), &module))
			{
				continue;
			}

			std::string moduleBaseName(getModuleBaseName(module));
			if (0 != _stricmp(moduleBaseName.c_str(), moduleName))
			{
				Compat::Log() << "Disabling external hook to " << funcName << " in " << moduleBaseName;
				static std::list<void*> origFuncs;
				origFuncs.push_back(hookFunc);
				hookFunction(origFuncs.back(), newFunc);
			}
		}
	}

	FARPROC* findProcAddressInIat(HMODULE module, const char* importedModuleName, const char* procName)
	{
		if (!module || !importedModuleName || !procName)
		{
			return nullptr;
		}

		PIMAGE_NT_HEADERS ntHeaders = getImageNtHeaders(module);
		if (!ntHeaders)
		{
			return nullptr;
		}

		char* moduleBase = reinterpret_cast<char*>(module);
		PIMAGE_IMPORT_DESCRIPTOR importDesc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(moduleBase +
			ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

		for (PIMAGE_IMPORT_DESCRIPTOR desc = importDesc;
			0 != desc->Characteristics && 0xFFFF != desc->Name;
			++desc)
		{
			if (0 != _stricmp(moduleBase + desc->Name, importedModuleName))
			{
				continue;
			}

			auto thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(moduleBase + desc->FirstThunk);
			auto origThunk = reinterpret_cast<PIMAGE_THUNK_DATA>(moduleBase + desc->OriginalFirstThunk);
			while (0 != thunk->u1.AddressOfData && 0 != origThunk->u1.AddressOfData)
			{
				auto origImport = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(
					moduleBase + origThunk->u1.AddressOfData);

				if (0 == strcmp(origImport->Name, procName))
				{
					return reinterpret_cast<FARPROC*>(&thunk->u1.Function);
				}

				++thunk;
				++origThunk;
			}

			break;
		}

		return nullptr;
	}

	FARPROC getProcAddress(HMODULE module, const char* procName)
	{
		if (!module || !procName)
		{
			return nullptr;
		}

		PIMAGE_NT_HEADERS ntHeaders = getImageNtHeaders(module);
		if (!ntHeaders)
		{
			return nullptr;
		}

		char* moduleBase = reinterpret_cast<char*>(module);
		PIMAGE_EXPORT_DIRECTORY exportDir = reinterpret_cast<PIMAGE_EXPORT_DIRECTORY>(
			moduleBase + ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
		auto exportDirSize = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;

		DWORD* rvaOfNames = reinterpret_cast<DWORD*>(moduleBase + exportDir->AddressOfNames);
		WORD* nameOrds = reinterpret_cast<WORD*>(moduleBase + exportDir->AddressOfNameOrdinals);
		DWORD* rvaOfFunctions = reinterpret_cast<DWORD*>(moduleBase + exportDir->AddressOfFunctions);

		char* func = nullptr;
		if (0 == HIWORD(procName))
		{
			WORD ord = LOWORD(procName);
			if (ord < exportDir->Base || ord >= exportDir->Base + exportDir->NumberOfFunctions)
			{
				return nullptr;
			}
			func = moduleBase + rvaOfFunctions[ord - exportDir->Base];
		}
		else
		{
			for (DWORD i = 0; i < exportDir->NumberOfNames; ++i)
			{
				if (0 == strcmp(procName, moduleBase + rvaOfNames[i]))
				{
					func = moduleBase + rvaOfFunctions[nameOrds[i]];
				}
			}
		}

		if (func &&
			func >= reinterpret_cast<char*>(exportDir) &&
			func < reinterpret_cast<char*>(exportDir) + exportDirSize)
		{
			std::string forw(func);
			auto separatorPos = forw.find_first_of('.');
			if (std::string::npos == separatorPos)
			{
				return nullptr;
			}
			HMODULE forwModule = GetModuleHandle(forw.substr(0, separatorPos).c_str());
			std::string forwFuncName = forw.substr(separatorPos + 1);
			if ('#' == forwFuncName[0])
			{
				int32_t ord = std::atoi(forwFuncName.substr(1).c_str());
				if (ord < 0 || ord > 0xFFFF)
				{
					return nullptr;
				}
				return getProcAddress(forwModule, reinterpret_cast<const char*>(ord));
			}
			else
			{
				return getProcAddress(forwModule, forwFuncName.c_str());
			}
		}

		return reinterpret_cast<FARPROC>(func);
	}

	FARPROC getProcAddressFromIat(HMODULE module, const char* importedModuleName, const char* procName)
	{
		FARPROC* proc = findProcAddressInIat(module, importedModuleName, procName);
		return proc ? *proc : nullptr;
	}

	void hookFunction(void*& origFuncPtr, void* newFuncPtr)
	{
		::hookFunction(nullptr, origFuncPtr, newFuncPtr);
	}

	void hookFunction(HMODULE module, const char* funcName, void*& origFuncPtr, void* newFuncPtr)
	{
		FARPROC procAddr = getProcAddress(module, funcName);
		if (!procAddr)
		{
			Compat::LogDebug() << "ERROR: Failed to load the address of a function: " << funcName;
			return;
		}

		origFuncPtr = procAddr;
		::hookFunction(funcName, origFuncPtr, newFuncPtr);
	}

	void hookFunction(const char* moduleName, const char* funcName, void*& origFuncPtr, void* newFuncPtr)
	{
		HMODULE module = LoadLibrary(moduleName);
		if (!module)
		{
			return;
		}
		hookFunction(module, funcName, origFuncPtr, newFuncPtr);
		FreeLibrary(module);
	}

	void hookIatFunction(HMODULE module, const char* importedModuleName, const char* funcName, void* newFuncPtr)
	{
		FARPROC* func = findProcAddressInIat(module, importedModuleName, funcName);
		if (func)
		{
			Compat::LogDebug() << "Hooking function via IAT: " << funcName;
			DWORD oldProtect = 0;
			VirtualProtect(func, sizeof(func), PAGE_READWRITE, &oldProtect);
			*func = static_cast<FARPROC>(newFuncPtr);
			DWORD dummy = 0;
			VirtualProtect(func, sizeof(func), oldProtect, &dummy);
		}
	}

	void unhookAllFunctions()
	{
		while (!g_hookedFunctions.empty())
		{
			::unhookFunction(g_hookedFunctions.begin());
		}
	}

	void unhookFunction(void* origFunc)
	{
		auto it = findOrigFunc(origFunc);
		if (it != g_hookedFunctions.end())
		{
			::unhookFunction(it);
		}
	}
}
