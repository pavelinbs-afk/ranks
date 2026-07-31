// Minimal runtime schema field access: offsets are resolved through the game's
// schema system at runtime, so no per-update gamedata is needed for fields.
#pragma once

#include <cstdint>

class CEntityInstance;

// Returns flattened field offset or -1 if not found (logged once).
int32_t Schema_GetOffset(const char* className, const char* fieldName);

template <typename T>
T Schema_Get(void* pInstance, const char* className, const char* fieldName, T defaultValue = T())
{
	int32_t off = Schema_GetOffset(className, fieldName);
	if (off < 0 || !pInstance)
		return defaultValue;
	return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(pInstance) + off);
}

// Write + mark the field dirty for networking (entity must be a CEntityInstance).
template <typename T>
bool Schema_SetNetworked(CEntityInstance* pEntity, const char* className, const char* fieldName, const T& value);

// Write without dirtying (non-networked fields).
template <typename T>
bool Schema_Set(void* pInstance, const char* className, const char* fieldName, const T& value)
{
	int32_t off = Schema_GetOffset(className, fieldName);
	if (off < 0 || !pInstance)
		return false;
	*reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(pInstance) + off) = value;
	return true;
}

void Schema_NetworkStateChanged(CEntityInstance* pEntity, int32_t offset);

template <typename T>
bool Schema_SetNetworked(CEntityInstance* pEntity, const char* className, const char* fieldName, const T& value)
{
	int32_t off = Schema_GetOffset(className, fieldName);
	if (off < 0 || !pEntity)
		return false;
	T* pField = reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(pEntity) + off);
	if (*pField == value)
		return true; // no change, no dirty
	*pField = value;
	Schema_NetworkStateChanged(pEntity, off);
	return true;
}

// Offset resolved once and kept. Schema_GetOffset builds a std::string key and
// hits a std::map on every call, which is fine occasionally but wasteful in
// per-frame loops (Tab_OnGameFrame runs over every player every tick).
// Declare these at file scope; cls/field must be string literals.
struct SchemaField
{
	const char* cls;
	const char* field;
	int32_t off = -2; // -2 = unresolved, -1 = no such field

	int32_t Offset()
	{
		if (off == -2)
			off = Schema_GetOffset(cls, field);
		return off;
	}

	template <typename T>
	T Get(void* pInstance, T defaultValue = T())
	{
		int32_t o = Offset();
		if (o < 0 || !pInstance)
			return defaultValue;
		return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(pInstance) + o);
	}

	template <typename T>
	bool SetNetworked(CEntityInstance* pEntity, const T& value)
	{
		int32_t o = Offset();
		if (o < 0 || !pEntity)
			return false;
		T* pField = reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(pEntity) + o);
		if (*pField == value)
			return true; // no change, no dirty
		*pField = value;
		Schema_NetworkStateChanged(pEntity, o);
		return true;
	}
};
