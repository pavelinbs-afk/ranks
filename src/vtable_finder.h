// Locates a C++ virtual table of a class inside a loaded game module by its
// RTTI records (Linux/ELF). Lets us SourceHook classes like CGameEventManager
// without per-update signatures.
#pragma once

#include <cstddef>

// className without mangling, e.g. "CGameEventManager".
// moduleSuffix e.g. "libserver.so".
// Returns pointer to the first virtual function slot, or nullptr.
void* FindVirtualTable(const char* moduleSuffix, const char* className);

// IDA-style signature in a loaded module section (Linux). '?' = wildcard byte.
// sectionName e.g. ".text". Returns nullptr if not found.
void* FindFunctionSignature(const char* moduleSuffix, const char* sectionName, const char* idaSig);
