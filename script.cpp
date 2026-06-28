/*
	THIS FILE IS A PART OF RDR 2 SCRIPT HOOK SDK
				http://dev-c.com
			(C) Alexander Blade 2019
*/

#include "script.h"
#include "scriptmenu.h"
#include "keyboard.h"

#include <unordered_map>
#include <vector>
#include <string>
#include <ctime>
#include <cstring>
#include <fstream>

using namespace std;

#include "scriptinfo.h"

// ---------------------------------------------------------------------------
// MISSING NATIVE DECLARATIONS
// TASK_MOUNT_ANIMAL and TASK_DISMOUNT_ANIMAL exist in the game but are absent
// from the SDK's natives.h. Declare them here by JOAAT hash so they can be
// called with normal TASK:: namespace syntax.
// ---------------------------------------------------------------------------
namespace TASK
{
	// Correct RDR2 64-bit hashes from alloc8or native DB (b1207)
	// TASK_MOUNT_ANIMAL(Ped ped, Ped mount, int timer, int seatIndex, float pedSpeed, int mountStyle, Any p6, Any p7)
	static void TASK_MOUNT_ANIMAL(Ped ped, Ped animal, int timer, int seatIndex, float pedSpeed, int mountStyle, int p6, int p7)
	{
		nativeInit(0x92DB0739813C5186ULL);
		nativePush64((UINT64)ped);
		nativePush64((UINT64)animal);
		nativePush64((UINT64)timer);
		nativePush64((UINT64)seatIndex);
		nativePush64(*(UINT64*)&pedSpeed);
		nativePush64((UINT64)mountStyle);
		nativePush64((UINT64)p6);
		nativePush64((UINT64)p7);
		nativeCall();
	}
	// TASK_DISMOUNT_ANIMAL(Ped rider, int taskFlag, Any p2, Any p3, Any p4, Ped targetPed)
	static void TASK_DISMOUNT_ANIMAL(Ped ped, int taskFlag, int p2, int p3, int p4, Ped targetPed)
	{
		nativeInit(0x48E92D3DDE23C23AULL);
		nativePush64((UINT64)ped);
		nativePush64((UINT64)taskFlag);
		nativePush64((UINT64)p2);
		nativePush64((UINT64)p3);
		nativePush64((UINT64)p4);
		nativePush64((UINT64)targetPed);
		nativeCall();
	}

	// TASK_FOLLOW_TO_OFFSET_OF_ENTITY
	// Makes a ped actively navigate toward an offset of a target entity using
	// the navmesh — the same internal task story-mode companion/gang-member
	// follow uses. Hash from alloc8or native DB (b1207). Signature matches
	// GTAV's equivalent; the extra trailing booleans observed in RDR2 forum
	// usage are passed as 0/false here (safe defaults).
	// TASK_FOLLOW_TO_OFFSET_OF_ENTITY(Ped ped, Entity target,
	//   float offsetX, float offsetY, float offsetZ,
	//   float moveSpeed, int timeout, float stoppingRange,
	//   BOOL persistFollowing,
	//   BOOL p9, BOOL walkOnly, BOOL p11, BOOL p12, BOOL p13)
	static void TASK_FOLLOW_TO_OFFSET_OF_ENTITY(Ped ped, Entity target,
		float offsetX, float offsetY, float offsetZ,
		float moveSpeed, int timeout, float stoppingRange,
		BOOL persistFollowing,
		BOOL p9, BOOL walkOnly, BOOL p11, BOOL p12, BOOL p13)
	{
		nativeInit(0x304AE42E357B8C7EULL); // alloc8or b1207
		nativePush64((UINT64)ped);
		nativePush64((UINT64)target);
		nativePush64(*(UINT64*)&offsetX);
		nativePush64(*(UINT64*)&offsetY);
		nativePush64(*(UINT64*)&offsetZ);
		nativePush64(*(UINT64*)&moveSpeed);
		nativePush64((UINT64)timeout);
		nativePush64(*(UINT64*)&stoppingRange);
		nativePush64((UINT64)persistFollowing);
		nativePush64((UINT64)p9);
		nativePush64((UINT64)walkOnly);
		nativePush64((UINT64)p11);
		nativePush64((UINT64)p12);
		nativePush64((UINT64)p13);
		nativeCall();
	}
}

// ---------------------------------------------------------------------------
// DEBUG LOGGING – buffered in memory, flushed to disk only periodically.
//
// FIX: the original version opened, wrote, flushed, and closed
// MercDebug.log on EVERY single call — and FindNearbyEnemy alone calls this
// 6-8+ times per candidate ped, every throttle tick. That's synchronous
// disk I/O happening repeatedly inside a hot loop on the game's monitored
// script thread. Across every crash log collected tonight, the "last
// successful call before the gap" kept moving between different natives
// (IS_PED_HUMAN, then GET_ENTITY_COORDS) with no consistent single culprit
// — which is exactly what you'd see if the real instability was this
// surrounding file I/O rather than any specific native call. Buffering in
// memory and flushing only occasionally removes that variable entirely.
// ---------------------------------------------------------------------------
// MercLog: define MERC_DEBUG to re-enable disk logging for debugging.
// Disabled in release builds -- every MercLog call in OnFrame was doing
// string allocations + periodic disk I/O at 1 Hz, causing noticeable lag.
#ifdef MERC_DEBUG
static vector<string> g_mercLogBuffer;
static void MercLog(const string& msg)
{
	DWORD t = GetTickCount();
	g_mercLogBuffer.push_back("[" + to_string(t) + "] " + msg);
	if (g_mercLogBuffer.size() >= 20)
	{
		ofstream f("MercDebug.log", ios::app);
		if (f.is_open()) { for (auto& line : g_mercLogBuffer) f << line << "\n"; }
		g_mercLogBuffer.clear();
	}
}
#else
static inline void MercLog(const string&) {} // no-op
#endif

// --- mercenary mod globals ---
vector<Ped>  g_mercenaryPeds;
vector<Ped>  g_mercenaryHorses;
// Tick timestamp of last TASK_FOLLOW_TO_OFFSET_OF_ENTITY issue per merc.
// 0 = never issued. Prevents re-issuing every second (AMD TDR fix).
vector<DWORD> g_mercFollowTaskTick;
Hash         g_mercenaryRelGroup = 0;

// ---------------------------------------------------------------------------
// SET_PED_COMBAT_ATTRIBUTES  hash 0x9F7794730795E019
// ---------------------------------------------------------------------------
static void MercSetCombatAttr(Ped ped, int index, bool enabled)
{
	nativeInit(0x9F7794730795E019ULL);
	nativePush64((UINT64)ped);
	nativePush64((UINT64)index);
	nativePush64((UINT64)(enabled ? TRUE : FALSE));
	nativeCall();
}

// Apply a battle-ready combat profile to a mercenary ped
static void SetMercCombatAttributes(Ped ped)
{
	MercSetCombatAttr(ped, 0, true);   // CA_USE_COVER
	MercSetCombatAttr(ped, 5, true);   // CA_ALWAYS_FIGHT
	MercSetCombatAttr(ped, 13, true);   // CA_AGGRESSIVE – will advance
	MercSetCombatAttr(ped, 17, false);  // CA_ALWAYS_FLEE – off
	MercSetCombatAttr(ped, 21, true);   // CA_CAN_CHASE_TARGET_ON_FOOT
	MercSetCombatAttr(ped, 23, false);  // CA_REQUIRES_LOS_TO_SHOOT – off
	MercSetCombatAttr(ped, 30, true);   // CA_CAN_SHOOT_WITHOUT_LOS
	MercSetCombatAttr(ped, 46, true);   // CA_CAN_FIGHT_ARMED_PEDS_WHEN_NOT_ARMED
	MercSetCombatAttr(ped, 54, true);   // CA_ALWAYS_EQUIP_BEST_WEAPON
	MercSetCombatAttr(ped, 58, true);   // CA_DISABLE_FLEE_FROM_COMBAT

	// SET_PED_ACCURACY  – reasonable but not perfect
	PED::SET_PED_ACCURACY(ped, 60);
}


// ===========================================================================
//  ORIGINAL MENU ITEM CLASSES  (unchanged from AB's trainer)
// ===========================================================================

class MenuItemPlayerFastHeal : public MenuItemSwitchable
{
	virtual void OnSelect()
	{
		bool newState = !GetState();
		if (!newState)
			PLAYER::SET_PLAYER_HEALTH_RECHARGE_MULTIPLIER(PLAYER::PLAYER_ID(), 1.0);
		SetState(newState);
	}
	virtual void OnFrame()
	{
		if (GetState())
			PLAYER::SET_PLAYER_HEALTH_RECHARGE_MULTIPLIER(PLAYER::PLAYER_ID(), 1000.0);
	}
public:
	MenuItemPlayerFastHeal(string caption)
		: MenuItemSwitchable(caption) {
	}
};

class MenuItemPlayerFix : public MenuItemDefault
{
	virtual void OnSelect()
	{
		Ped playerPed = PLAYER::PLAYER_PED_ID();
		ENTITY::SET_ENTITY_HEALTH(playerPed, ENTITY::GET_ENTITY_MAX_HEALTH(playerPed, FALSE), FALSE);
		PED::CLEAR_PED_WETNESS(playerPed);
		PLAYER::RESTORE_PLAYER_STAMINA(PLAYER::PLAYER_ID(), 100.0);
		PLAYER::RESTORE_SPECIAL_ABILITY(PLAYER::PLAYER_ID(), -1, FALSE);

		if (PED::IS_PED_ON_MOUNT(playerPed))
		{
			Ped horse = PED::GET_MOUNT(playerPed);
			ENTITY::SET_ENTITY_HEALTH(horse, ENTITY::GET_ENTITY_MAX_HEALTH(horse, FALSE), FALSE);
			PED::SET_PED_STAMINA(horse, 100.0);
			SetStatusText("player and horse fixed");
		}
		else
			if (PED::IS_PED_IN_ANY_VEHICLE(playerPed, FALSE))
			{
				Vehicle veh = PED::GET_VEHICLE_PED_IS_USING(playerPed);
				ENTITY::SET_ENTITY_HEALTH(veh, ENTITY::GET_ENTITY_MAX_HEALTH(veh, FALSE), FALSE);
				SetStatusText("player and vehicle fixed");
			}
			else
				SetStatusText("player fixed");
	}
public:
	MenuItemPlayerFix(string caption)
		: MenuItemDefault(caption) {
	}
};

class MenuItemVehicleBoost : public MenuItemSwitchable
{
	virtual void OnSelect()
	{
		bool newState = !GetState();
		if (newState)
			SetStatusText("PAGEUP / NUM9\nPAGEDOWN / NUM6");
		SetState(newState);
	}
	virtual void OnFrame()
	{
		if (!GetState())
			return;
		Ped playerPed = PLAYER::PLAYER_PED_ID();
		if (!PED::IS_PED_IN_ANY_VEHICLE(playerPed, 0))
			return;

		Vehicle veh = PED::GET_VEHICLE_PED_IS_USING(playerPed);
		DWORD model = ENTITY::GET_ENTITY_MODEL(veh);
		BOOL bTrain = VEHICLE::IS_THIS_MODEL_A_TRAIN(model);

		bool bUp = IsKeyDownLong(VK_NUMPAD9) || IsKeyDownLong(VK_PRIOR);
		bool bDown = IsKeyDown(VK_NUMPAD3) || IsKeyDown(VK_NEXT);

		if (!(bUp || bDown))
			return;

		if (bTrain)
		{
			float speed = bUp ? 30.0f : 0.0f;
			VEHICLE::SET_TRAIN_SPEED(veh, speed);
			VEHICLE::SET_TRAIN_CRUISE_SPEED(veh, speed);
			return;
		}

		float speed = ENTITY::GET_ENTITY_SPEED(veh);
		if (bUp)
		{
			if (speed < 3.0f) speed = 3.0f;
			speed += speed * 0.03f;
			VEHICLE::SET_VEHICLE_FORWARD_SPEED(veh, speed);
		}
		else
			if (ENTITY::IS_ENTITY_IN_AIR(veh, 0) || speed > 5.0)
				VEHICLE::SET_VEHICLE_FORWARD_SPEED(veh, 0.0);
	}
public:
	MenuItemVehicleBoost(string caption)
		: MenuItemSwitchable(caption) {
	}
};

class MenuItemPlayerAddCash : public MenuItemDefault
{
	int m_value;
	virtual void OnSelect() { CASH::PLAYER_ADD_CASH(m_value, 0x2cd419dc); }
public:
	MenuItemPlayerAddCash(string caption, int value)
		: MenuItemDefault(caption),
		m_value(value) {
	}
};

class MenuItemPlayerInvincible : public MenuItemSwitchable
{
	virtual void OnSelect()
	{
		bool newState = !GetState();
		if (!newState)
			PLAYER::SET_PLAYER_INVINCIBLE(PLAYER::PLAYER_ID(), FALSE);
		SetState(newState);
	}
	virtual void OnFrame()
	{
		if (GetState())
			PLAYER::SET_PLAYER_INVINCIBLE(PLAYER::PLAYER_ID(), TRUE);
	}
public:
	MenuItemPlayerInvincible(string caption)
		: MenuItemSwitchable(caption) {
	}
};

class MenuItemPlayerHorseInvincible : public MenuItemSwitchable
{
	void SetPlayerHorseInvincible(bool set)
	{
		Ped playerPed = PLAYER::PLAYER_PED_ID();
		if (PED::IS_PED_ON_MOUNT(playerPed))
		{
			Ped horse = PED::GET_MOUNT(playerPed);
			ENTITY::SET_ENTITY_INVINCIBLE(horse, set);
		}
	}
	virtual void OnSelect()
	{
		bool newState = !GetState();
		if (!newState)
			SetPlayerHorseInvincible(false);
		SetState(newState);
	}
	virtual void OnFrame()
	{
		if (GetState())
			SetPlayerHorseInvincible(true);
	}
public:
	MenuItemPlayerHorseInvincible(string caption)
		: MenuItemSwitchable(caption) {
	}
};

class MenuItemPlayerUnlimStamina : public MenuItemSwitchable
{
	virtual void OnFrame()
	{
		if (GetState())
			PLAYER::RESTORE_PLAYER_STAMINA(PLAYER::PLAYER_ID(), 100.0);
	}
public:
	MenuItemPlayerUnlimStamina(string caption)
		: MenuItemSwitchable(caption) {
	}
};

class MenuItemPlayerUnlimAbility : public MenuItemSwitchable
{
	virtual void OnFrame()
	{
		if (GetState())
			PLAYER::RESTORE_SPECIAL_ABILITY(PLAYER::PLAYER_ID(), -1, FALSE);
	}
public:
	MenuItemPlayerUnlimAbility(string caption)
		: MenuItemSwitchable(caption) {
	}
};

class MenuItemPlayerHorseUnlimStamina : public MenuItemSwitchable
{
	virtual void OnFrame()
	{
		if (GetState())
		{
			Ped playerPed = PLAYER::PLAYER_PED_ID();
			if (PED::IS_PED_ON_MOUNT(playerPed))
			{
				Ped horse = PED::GET_MOUNT(playerPed);
				PED::SET_PED_STAMINA(horse, 100.0);
			}
		}
	}
public:
	MenuItemPlayerHorseUnlimStamina(string caption)
		: MenuItemSwitchable(caption) {
	}
};

class MenuItemPlayerSuperJump : public MenuItemSwitchable
{
	virtual void OnFrame()
	{
		if (GetState())
			GAMEPLAY::SET_SUPER_JUMP_THIS_FRAME(PLAYER::PLAYER_ID());
	}
public:
	MenuItemPlayerSuperJump(string caption)
		: MenuItemSwitchable(caption) {
	}
};

class MenuItemPlayerNoiseless : public MenuItemSwitchable
{
	virtual void OnSelect()
	{
		bool newState = !GetState();
		if (!newState)
		{
			PLAYER::SET_PLAYER_NOISE_MULTIPLIER(PLAYER::PLAYER_ID(), 1.0);
			PLAYER::SET_PLAYER_SNEAKING_NOISE_MULTIPLIER(PLAYER::PLAYER_ID(), 1.0);
		}
		SetState(newState);
	}
	virtual void OnFrame()
	{
		if (GetState())
		{
			PLAYER::SET_PLAYER_NOISE_MULTIPLIER(PLAYER::PLAYER_ID(), 0.0);
			PLAYER::SET_PLAYER_SNEAKING_NOISE_MULTIPLIER(PLAYER::PLAYER_ID(), 0.0);
		}
	}
public:
	MenuItemPlayerNoiseless(string caption)
		: MenuItemSwitchable(caption) {
	}
};

class MenuItemPlayerEveryoneIgnored : public MenuItemSwitchable
{
	virtual void OnSelect()
	{
		bool newState = !GetState();
		if (!newState)
			PLAYER::SET_EVERYONE_IGNORE_PLAYER(PLAYER::PLAYER_ID(), FALSE);
		SetState(newState);
	}
	virtual void OnFrame()
	{
		if (GetState())
			PLAYER::SET_EVERYONE_IGNORE_PLAYER(PLAYER::PLAYER_ID(), TRUE);
	}
public:
	MenuItemPlayerEveryoneIgnored(string caption)
		: MenuItemSwitchable(caption) {
	}
};

class MenuItemPlayerTeleport : public MenuItemDefault
{
	Vector3 m_pos;
	virtual void OnSelect()
	{
		Entity e = PLAYER::PLAYER_PED_ID();
		if (PED::IS_PED_ON_MOUNT(e))
			e = PED::GET_MOUNT(e);
		else
			if (PED::IS_PED_IN_ANY_VEHICLE(e, FALSE))
				e = PED::GET_VEHICLE_PED_IS_USING(e);
		ENTITY::SET_ENTITY_COORDS(e, m_pos.x, m_pos.y, m_pos.z, 0, 0, 1, FALSE);
	}
public:
	MenuItemPlayerTeleport(string caption, Vector3 pos)
		: MenuItemDefault(caption),
		m_pos(pos) {
	}
};

class MenuItemPlayerTeleportToMarker : public MenuItemDefault
{
	virtual void OnSelect()
	{
		if (!RADAR::IS_WAYPOINT_ACTIVE())
		{
			SetStatusText("map marker isn't set");
			return;
		}

		Vector3 coords = RADAR::GET_WAYPOINT_COORDS_3D();

		Entity e = PLAYER::PLAYER_PED_ID();
		if (PED::IS_PED_ON_MOUNT(e))
			e = PED::GET_MOUNT(e);
		else
			if (PED::IS_PED_IN_ANY_VEHICLE(e, FALSE))
				e = PED::GET_VEHICLE_PED_IS_USING(e);

		if (!GAMEPLAY::GET_GROUND_Z_FOR_3D_COORD(coords.x, coords.y, 100.0, &coords.z, FALSE))
		{
			static const float groundCheckHeight[] = {
				100.0, 150.0, 50.0, 0.0, 200.0, 250.0, 300.0, 350.0, 400.0,
				450.0, 500.0, 550.0, 600.0, 650.0, 700.0, 750.0, 800.0
			};
			for each(float height in groundCheckHeight)
			{
				ENTITY::SET_ENTITY_COORDS_NO_OFFSET(e, coords.x, coords.y, height, 0, 0, 1);
				WaitAndDraw(100);
				if (GAMEPLAY::GET_GROUND_Z_FOR_3D_COORD(coords.x, coords.y, height, &coords.z, FALSE))
				{
					coords.z += 3.0;
					break;
				}
			}
		}

		ENTITY::SET_ENTITY_COORDS(e, coords.x, coords.y, coords.z, 0, 0, 1, FALSE);
	}
public:
	MenuItemPlayerTeleportToMarker(string caption)
		: MenuItemDefault(caption) {
	}
};

class MenuItemPlayerClearWanted : public MenuItemDefault
{
	bool m_headPrice;
	bool m_pursuit;
	virtual void OnSelect()
	{
		Player player = PLAYER::PLAYER_ID();
		if (m_headPrice)
			PURSUIT::SET_PLAYER_PRICE_ON_A_HEAD(player, 0);
		if (m_pursuit)
		{
			PURSUIT::CLEAR_CURRENT_PURSUIT();
			PURSUIT::SET_PLAYER_WANTED_INTENSITY(player, 0);
		}
		SetStatusText("Player has to be in pursuit\n\n"
			"head price: " + to_string(PURSUIT::GET_PLAYER_PRICE_ON_A_HEAD(player) / 100) + "\n" +
			"wanted intensity: " + to_string(PURSUIT::GET_PLAYER_WANTED_INTENSITY(player) / 100));
	}
public:
	MenuItemPlayerClearWanted(string caption, bool headPrice, bool pursuit)
		: MenuItemDefault(caption),
		m_headPrice(headPrice), m_pursuit(pursuit) {
	}
};

class MenuItemPlayerNeverWanted : public MenuItemSwitchable
{
	virtual void OnSelect()
	{
		bool newstate = !GetState();
		if (!newstate)
			PLAYER::SET_WANTED_LEVEL_MULTIPLIER(1.0);
		SetState(newstate);
	}
	virtual void OnFrame()
	{
		if (GetState())
		{
			Player player = PLAYER::PLAYER_ID();
			PURSUIT::CLEAR_CURRENT_PURSUIT();
			PURSUIT::SET_PLAYER_PRICE_ON_A_HEAD(player, 0);
			PURSUIT::SET_PLAYER_WANTED_INTENSITY(player, 0);
			PLAYER::SET_WANTED_LEVEL_MULTIPLIER(0.0);
		}
	}
public:
	MenuItemPlayerNeverWanted(string caption)
		: MenuItemSwitchable(caption) {
	}
};

class MenuItemChangePlayerModel : public MenuItemDefault
{
	string		m_model;

	virtual void OnSelect()
	{
		DWORD model = GAMEPLAY::GET_HASH_KEY(const_cast<char*>(m_model.c_str()));
		if (STREAMING::IS_MODEL_IN_CDIMAGE(model) && STREAMING::IS_MODEL_VALID(model))
		{
			UINT64* ptr1 = getGlobalPtr(0x28) + 0x27;
			UINT64* ptr2 = getGlobalPtr(((DWORD)7 << 18) | 0x1890C) + 2;
			UINT64 bcp1 = *ptr1;
			UINT64 bcp2 = *ptr2;
			*ptr1 = *ptr2 = model;
			WaitAndDraw(1000);
			Ped playerPed = PLAYER::PLAYER_PED_ID();
			PED::SET_PED_VISIBLE(playerPed, TRUE);
			if (ENTITY::GET_ENTITY_MODEL(playerPed) != model)
			{
				*ptr1 = bcp1;
				*ptr2 = bcp2;
			}
		}
	}
public:
	MenuItemChangePlayerModel(string caption, string model)
		: MenuItemDefault(caption),
		m_model(model) {
	}
};

class MenuItemSpawnPed : public MenuItemDefault
{
	string		m_model;

	virtual string GetModel() { return m_model; }

	virtual void OnSelect()
	{
		DWORD model = GAMEPLAY::GET_HASH_KEY(const_cast<char*>(GetModel().c_str()));
		if (STREAMING::IS_MODEL_IN_CDIMAGE(model) && STREAMING::IS_MODEL_VALID(model))
		{
			STREAMING::REQUEST_MODEL(model, FALSE);
			while (!STREAMING::HAS_MODEL_LOADED(model))
				WaitAndDraw(0);
			Vector3 coords = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(PLAYER::PLAYER_PED_ID(), 0.0, 3.0, -0.3);
			Ped ped = PED::CREATE_PED(model, coords.x, coords.y, coords.z, static_cast<float>(rand() % 360), 0, 0, 0, 0);
			PED::SET_PED_VISIBLE(ped, TRUE);
			ENTITY::SET_PED_AS_NO_LONGER_NEEDED(&ped);
			STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(model);
		}
	}
public:
	MenuItemSpawnPed(string caption, string model)
		: MenuItemDefault(caption),
		m_model(model) {
	}
};

class MenuItemSpawnHorseRandom : public MenuItemSpawnPed
{
	virtual string GetModel()
	{
		while (true)
		{
			int index = rand() % ARRAY_LENGTH(pedModelInfos);
			if (pedModelInfos[index].horse)
			{
				SetStatusText(pedModelInfos[index].name);
				return pedModelInfos[index].model;
			}
		}
	}
public:
	MenuItemSpawnHorseRandom(string caption)
		: MenuItemSpawnPed(caption, "") {
	}
};

class MenuItemSpawnAnimalRandom : public MenuItemSpawnPed
{
	virtual string GetModel()
	{
		while (true)
		{
			int index = rand() % ARRAY_LENGTH(pedModelInfos);
			if (pedModelInfos[index].animal && !pedModelInfos[index].fish && !pedModelInfos[index].horse)
			{
				SetStatusText(pedModelInfos[index].name);
				return pedModelInfos[index].model;
			}
		}
	}
public:
	MenuItemSpawnAnimalRandom(string caption)
		: MenuItemSpawnPed(caption, "") {
	}
};

class MenuItemSpawnPedRandom : public MenuItemSpawnPed
{
	virtual string GetModel()
	{
		while (true)
		{
			int index = rand() % ARRAY_LENGTH(pedModelInfos);
			if (!pedModelInfos[index].animal)
			{
				SetStatusText(pedModelInfos[index].name);
				return pedModelInfos[index].model;
			}
		}
	}
public:
	MenuItemSpawnPedRandom(string caption)
		: MenuItemSpawnPed(caption, "") {
	}
};

class MenuItemSpawnVehicle : public MenuItemDefault
{
	string		m_model;
	Vector3		m_pos;
	float		m_heading;
	bool		m_resetHeading;
	bool		m_noPeds;

	MenuItemSwitchable* m_menuItemWrapIn;
	MenuItemSwitchable* m_menuItemSetProperly;

	virtual void OnSelect()
	{
		DWORD model = GAMEPLAY::GET_HASH_KEY(const_cast<char*>(m_model.c_str()));
		if (STREAMING::IS_MODEL_IN_CDIMAGE(model) && STREAMING::IS_MODEL_VALID(model))
		{
			STREAMING::REQUEST_MODEL(model, FALSE);
			while (!STREAMING::HAS_MODEL_LOADED(model))
				WaitAndDraw(0);
			Ped playerPed = PLAYER::PLAYER_PED_ID();
			float playerHeading = ENTITY::GET_ENTITY_HEADING(playerPed) + 5.0f;
			float heading = playerHeading + m_heading;
			Vector3 coords = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(playerPed, m_pos.x, m_pos.y, m_pos.z);
			Vehicle veh = VEHICLE::CREATE_VEHICLE(model, coords.x, coords.y, coords.z, heading, 0, 0, m_noPeds, 0);
			DECORATOR::DECOR_SET_BOOL(veh, "wagon_block_honor", TRUE);
			bool wrapIn = m_menuItemWrapIn && m_menuItemWrapIn->GetState();
			bool setProperly = m_menuItemSetProperly && m_menuItemSetProperly->GetState();
			if (setProperly)
			{
				VEHICLE::SET_VEHICLE_ON_GROUND_PROPERLY(veh, 0);
				WaitAndDraw(100);
			}
			if (m_resetHeading || wrapIn)
				ENTITY::SET_ENTITY_HEADING(veh, wrapIn ? playerHeading : heading);
			if (wrapIn)
				PED::SET_PED_INTO_VEHICLE(playerPed, veh, -1);
			ENTITY::SET_VEHICLE_AS_NO_LONGER_NEEDED(&veh);
			STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(model);
		}
	}
public:
	MenuItemSpawnVehicle(string model,
		Vector3 pos, float heading,
		MenuItemSwitchable* menuItemWrapIn,
		MenuItemSwitchable* menuItemSetProperly,
		bool resetHeading,
		bool noPeds)
		: MenuItemDefault(model),
		m_model(model),
		m_pos(pos), m_heading(heading),
		m_menuItemWrapIn(menuItemWrapIn),
		m_menuItemSetProperly(menuItemSetProperly),
		m_resetHeading(resetHeading),
		m_noPeds(noPeds) {
	}
};

class MenuItemGiveWeapon : public MenuItemDefault
{
	string m_name;

	virtual void OnSelect()
	{
		Hash hash = GAMEPLAY::GET_HASH_KEY(const_cast<char*>(("WEAPON_" + m_name).c_str()));
		Ped playerPed = PLAYER::PLAYER_PED_ID();
		WEAPON::GIVE_DELAYED_WEAPON_TO_PED(playerPed, hash, 100, 1, 0x2cd419dc);
		WEAPON::SET_PED_AMMO(playerPed, hash, 100);
		WEAPON::SET_CURRENT_PED_WEAPON(playerPed, hash, 1, 0, 0, 0);
	}
public:
	MenuItemGiveWeapon(string caption, string weaponName)
		: MenuItemDefault(caption),
		m_name(weaponName) {
	}
};

class MenuItemWeaponPowerfullGuns : public MenuItemSwitchable
{
	virtual void OnSelect()
	{
		bool newState = !GetState();
		if (!newState)
			PLAYER::SET_PLAYER_WEAPON_DAMAGE_MODIFIER(PLAYER::PLAYER_ID(), 1.0);
		SetState(newState);
	}
	virtual void OnFrame()
	{
		if (GetState())
			PLAYER::SET_PLAYER_WEAPON_DAMAGE_MODIFIER(PLAYER::PLAYER_ID(), 100.0);
	}
public:
	MenuItemWeaponPowerfullGuns(string caption)
		: MenuItemSwitchable(caption) {
	}
};

class MenuItemWeaponPowerfullMelee : public MenuItemSwitchable
{
	virtual void OnSelect()
	{
		bool newState = !GetState();
		if (!newState)
			PLAYER::SET_PLAYER_MELEE_WEAPON_DAMAGE_MODIFIER(PLAYER::PLAYER_ID(), 1.0);
		SetState(newState);
	}
	virtual void OnFrame()
	{
		if (GetState())
			PLAYER::SET_PLAYER_MELEE_WEAPON_DAMAGE_MODIFIER(PLAYER::PLAYER_ID(), 100.0);
	}
public:
	MenuItemWeaponPowerfullMelee(string caption)
		: MenuItemSwitchable(caption) {
	}
};

class MenuItemWeaponNoReload : public MenuItemSwitchable
{
	virtual void OnFrame()
	{
		if (!GetState())
			return;
		Ped playerPed = PLAYER::PLAYER_PED_ID();
		Hash cur;
		if (WEAPON::GET_CURRENT_PED_WEAPON(playerPed, &cur, 0, 0, 0) && WEAPON::IS_WEAPON_VALID(cur))
		{
			int maxAmmo;
			if (WEAPON::GET_MAX_AMMO(playerPed, &maxAmmo, cur))
				WEAPON::SET_PED_AMMO(playerPed, cur, maxAmmo);
			maxAmmo = WEAPON::GET_MAX_AMMO_IN_CLIP(playerPed, cur, 1);
			if (maxAmmo > 0)
				WEAPON::SET_AMMO_IN_CLIP(playerPed, cur, maxAmmo);
		}
	}
public:
	MenuItemWeaponNoReload(string caption)
		: MenuItemSwitchable(caption) {
	}
};

class MenuItemWeaponDropCurrent : public MenuItemDefault
{
	virtual void OnSelect()
	{
		Ped playerPed = PLAYER::PLAYER_PED_ID();
		Hash unarmed = GAMEPLAY::GET_HASH_KEY("WEAPON_UNARMED");
		Hash cur;
		if (WEAPON::GET_CURRENT_PED_WEAPON(playerPed, &cur, 0, 0, 0) && WEAPON::IS_WEAPON_VALID(cur) && cur != unarmed)
			WEAPON::SET_PED_DROPS_INVENTORY_WEAPON(playerPed, cur, 0.0, 0.0, 0.0, 1);
	}
public:
	MenuItemWeaponDropCurrent(string caption)
		: MenuItemDefault(caption) {
	}
};

class MenuItemTimeTitle : public MenuItemTitle
{
	virtual string GetCaption()
	{
		time_t now = time(0);
		tm t;
		localtime_s(&t, &now);
		char str[32];
		sprintf_s(str, "%02d%s%02d",
			TIME::GET_CLOCK_HOURS(),
			t.tm_sec % 2 ? ":" : " ",
			TIME::GET_CLOCK_MINUTES()
		);
		return MenuItemTitle::GetCaption() + "   " + str;
	}
public:
	MenuItemTimeTitle(string caption)
		: MenuItemTitle(caption) {
	}
};

class MenuItemTimeAdjust : public MenuItemDefault
{
	int m_difHours;
	virtual void OnSelect()
	{
		TIME::ADD_TO_CLOCK_TIME(m_difHours, 0, 0);
	}
public:
	MenuItemTimeAdjust(string caption, int difHours)
		: MenuItemDefault(caption),
		m_difHours(difHours) {
	}
};

class MenuItemTimePause : public MenuItemSwitchable
{
	virtual void OnSelect()
	{
		bool newState = !GetState();
		TIME::PAUSE_CLOCK(newState, 0);
		SetState(newState);
	}
public:
	MenuItemTimePause(string caption)
		: MenuItemSwitchable(caption) {
	}
};

class MenuItemTimeRealistic : public MenuItemSwitchable
{
	int m_difHour;
	int m_difMin;
	virtual void OnSelect()
	{
		bool newState = !GetState();
		if (newState)
		{
			time_t now = time(0);
			tm t;
			localtime_s(&t, &now);
			m_difHour = TIME::GET_CLOCK_HOURS() - t.tm_hour;
			m_difMin = TIME::GET_CLOCK_MINUTES() - t.tm_min;
		}
		SetState(newState);
	}
	virtual void OnFrame()
	{
		if (!GetState())
			return;
		time_t now = time(0);
		tm t;
		localtime_s(&t, &now);
		int hours = t.tm_hour + m_difHour;
		int mins = t.tm_min + m_difMin;
		if (mins >= 60)
		{
			mins -= 60;
			hours++;
		}
		else
			if (mins < 0)
			{
				mins += 60;
				hours--;
			}
		if (hours >= 24)
			hours -= 24;
		else
			if (hours < 0)
				hours += 24;
		TIME::SET_CLOCK_TIME(hours, mins, t.tm_sec);
	}
public:
	MenuItemTimeRealistic(string caption)
		: MenuItemSwitchable(caption),
		m_difHour(0), m_difMin(0) {
	}
};

class MenuItemTimeSystemSynced : public MenuItemSwitchable
{
	virtual void OnFrame()
	{
		if (GetState())
		{
			time_t now = time(0);
			tm t;
			localtime_s(&t, &now);
			TIME::SET_CLOCK_TIME(t.tm_hour, t.tm_min, t.tm_sec);
		}
	}
public:
	MenuItemTimeSystemSynced(string caption)
		: MenuItemSwitchable(caption) {
	}
};

class MenuItemWeatherFreeze : public MenuItemSwitchable
{
	virtual void OnSelect()
	{
		bool newstate = !GetState();
		if (!newstate)
			GAMEPLAY::FREEZE_WEATHER(false);
		SetState(newstate);
	}
	virtual void OnFrame()
	{
		if (GetState())
			GAMEPLAY::FREEZE_WEATHER(true);
	}
public:
	MenuItemWeatherFreeze(string caption)
		: MenuItemSwitchable(caption) {
	}
};

class MenuItemWeatherWind : public MenuItemSwitchable
{
	virtual void OnSelect()
	{
		bool newstate = !GetState();
		if (newstate)
		{
			GAMEPLAY::SET_WIND_SPEED(50.0);
			GAMEPLAY::SET_WIND_DIRECTION(ENTITY::GET_ENTITY_HEADING(PLAYER::PLAYER_PED_ID()));
		}
		else
		{
			GAMEPLAY::SET_WIND_SPEED(0.0);
		}
		SetState(newstate);
	}
public:
	MenuItemWeatherWind(string caption)
		: MenuItemSwitchable(caption) {
	}
};

class MenuItemWeatherSelect : public MenuItemDefault
{
	virtual void OnSelect()
	{
		GAMEPLAY::CLEAR_OVERRIDE_WEATHER();
		Hash weather = GAMEPLAY::GET_HASH_KEY(const_cast<char*>(GetCaption().c_str()));
		GAMEPLAY::SET_WEATHER_TYPE(weather, TRUE, TRUE, FALSE, 0.0, FALSE);
		GAMEPLAY::CLEAR_WEATHER_TYPE_PERSIST();
	}
public:
	MenuItemWeatherSelect(string caption)
		: MenuItemDefault(caption) {
	}
};

class MenuItemMiscHideHud : public MenuItemSwitchable
{
	virtual void OnFrame()
	{
		if (GetState())
			UI::HIDE_HUD_AND_RADAR_THIS_FRAME();
	}
public:
	MenuItemMiscHideHud(string caption)
		: MenuItemSwitchable(caption) {
	}
};

class MenuItemMiscAddHonor : public MenuItemDefault
{
	virtual void OnSelect()
	{
		Hash unarmed = GAMEPLAY::GET_HASH_KEY("WEAPON_UNARMED");
		WEAPON::SET_CURRENT_PED_WEAPON(PLAYER::PLAYER_PED_ID(), unarmed, 1, 0, 0, 0);
		DWORD model = GAMEPLAY::GET_HASH_KEY("U_M_O_VHTEXOTICSHOPKEEPER_01");
		STREAMING::REQUEST_MODEL(model, FALSE);
		while (!STREAMING::HAS_MODEL_LOADED(model))
			WaitAndDraw(0);
		Vector3 coords = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(PLAYER::PLAYER_PED_ID(), 0.0, 3.0, -0.3);
		Ped ped = PED::CREATE_PED(model, coords.x, coords.y, coords.z, 0.0, 0, 0, 0, 0);
		PED::SET_PED_VISIBLE(ped, TRUE);
		DECORATOR::DECOR_SET_INT(ped, "honor_override", -9999);
		AI::TASK_COMBAT_PED(ped, PLAYER::PLAYER_PED_ID(), 0, 0);
		STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(model);
	}
public:
	MenuItemMiscAddHonor(string caption)
		: MenuItemDefault(caption) {
	}
};

class MenuItemMiscRevealMap : public MenuItemDefault
{
	virtual void OnSelect()
	{
		RADAR::_SET_MINIMAP_REVEALED(TRUE);
		RADAR::REVEAL_MAP(0);
		SetStatusText("map revealed");
	}
public:
	MenuItemMiscRevealMap(string caption) :
		MenuItemDefault(caption) {
	}
};

class MenuItemMiscTransportGuns : public MenuItemSwitchable
{
	bool m_isHorse;
	bool m_isBullet;

	DWORD m_lastShootTime;

	virtual void OnSelect()
	{
		bool newstate = !GetState();
		if (newstate)
			SetStatusText("WARN: may cause ERR_GFX_STATE while breaking certain objects\n\nINSERT / NUM+", 5000);
		SetState(newstate);
	}

	virtual void OnFrame()
	{
		if (!GetState())
			return;

		if (!IsKeyDownLong(VK_ADD) && !IsKeyDownLong(VK_INSERT))
			return;

		if (m_lastShootTime + (m_isBullet ? 50 : 250) > GetTickCount())
			return;

		Player player = PLAYER::PLAYER_ID();
		Ped playerPed = PLAYER::PLAYER_PED_ID();

		if (!PLAYER::IS_PLAYER_CONTROL_ON(player))
			return;

		Entity transport;
		if (m_isHorse)
			if (PED::IS_PED_ON_MOUNT(playerPed))
				transport = PED::GET_MOUNT(playerPed);
			else
				return;
		else
			if (PED::IS_PED_IN_ANY_VEHICLE(playerPed, 0))
				transport = PED::GET_VEHICLE_PED_IS_USING(playerPed);
			else
				return;

		Vector3 v0, v1;
		GAMEPLAY::GET_MODEL_DIMENSIONS(ENTITY::GET_ENTITY_MODEL(transport), &v0, &v1);

		Hash modelHash = m_isBullet ? 0 : GAMEPLAY::GET_HASH_KEY("S_CANNONBALL");
		Hash weaponHash = GAMEPLAY::GET_HASH_KEY(m_isBullet ? "WEAPON_TURRET_GATLING" : "WEAPON_TURRET_REVOLVING_CANNON");

		if (modelHash && !STREAMING::HAS_MODEL_LOADED(modelHash))
		{
			STREAMING::REQUEST_MODEL(modelHash, FALSE);
			while (!STREAMING::HAS_MODEL_LOADED(modelHash))
				WAIT(0);
		}

		Vector3 coords0from = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(transport, -(v1.x + 0.25f), v1.y + 1.25f, 0.1);
		Vector3 coords1from = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(transport, (v1.x + 0.25f), v1.y + 1.25f, 0.1);
		Vector3 coords0to = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(transport, -v1.x, v1.y + 100.0f, 0.1f);
		Vector3 coords1to = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(transport, v1.x, v1.y + 100.0f, 0.1f);

		GAMEPLAY::SHOOT_SINGLE_BULLET_BETWEEN_COORDS(coords0from.x, coords0from.y, coords0from.z,
			coords0to.x, coords0to.y, coords0to.z,
			250, 1, weaponHash, playerPed, 1, 1, -1.0, 0);
		GAMEPLAY::SHOOT_SINGLE_BULLET_BETWEEN_COORDS(coords1from.x, coords1from.y, coords1from.z,
			coords1to.x, coords1to.y, coords1to.z,
			250, 1, weaponHash, playerPed, 1, 1, -1.0, 0);

		if (m_isBullet)
		{
			weaponHash = GAMEPLAY::GET_HASH_KEY("WEAPON_SNIPERRIFLE_CARCANO");
			GAMEPLAY::SHOOT_SINGLE_BULLET_BETWEEN_COORDS(coords0from.x, coords0from.y, coords0from.z,
				coords0to.x, coords0to.y, coords0to.z,
				250, 1, weaponHash, playerPed, 1, 0, -1.0, 0);
			GAMEPLAY::SHOOT_SINGLE_BULLET_BETWEEN_COORDS(coords1from.x, coords1from.y, coords1from.z,
				coords1to.x, coords1to.y, coords1to.z,
				250, 1, weaponHash, playerPed, 1, 0, -1.0, 0);
		}

		m_lastShootTime = GetTickCount();
	}
public:
	MenuItemMiscTransportGuns(string caption, bool isHorse, bool isBullet)
		: MenuItemSwitchable(caption),
		m_isHorse(isHorse), m_isBullet(isBullet),
		m_lastShootTime(0) {
	}
};

// ===========================================================================
//  MERCENARY MOD  –  FIXED
// ===========================================================================

static Hash GetOrCreateMercenaryRelationshipGroup()
{
	if (g_mercenaryRelGroup != 0)
		return g_mercenaryRelGroup;

	MercLog("GetOrCreateMercenaryRelationshipGroup: enter (runs once)");
	Hash group;
	PED::ADD_RELATIONSHIP_GROUP(const_cast<char*>("MERCENARY_FRIENDLY"), &group);
	MercLog("GetOrCreateMercenaryRelationshipGroup: ADD_RELATIONSHIP_GROUP ok, about to GET_PED_RELATIONSHIP_GROUP_HASH on player");
	Hash playerGroup = PED::GET_PED_RELATIONSHIP_GROUP_HASH(PLAYER::PLAYER_PED_ID());
	MercLog("GetOrCreateMercenaryRelationshipGroup: GET_PED_RELATIONSHIP_GROUP_HASH ok");
	PED::SET_RELATIONSHIP_BETWEEN_GROUPS(2, group, playerGroup);
	PED::SET_RELATIONSHIP_BETWEEN_GROUPS(2, playerGroup, group);
	g_mercenaryRelGroup = group;
	MercLog("GetOrCreateMercenaryRelationshipGroup: exit ok");
	return group;
}

static void PruneDeadMercenaries()
{
	// Always keep both vectors the same length before touching either.
	while (g_mercenaryHorses.size() < g_mercenaryPeds.size())  g_mercenaryHorses.push_back(0);
	while (g_mercenaryPeds.size() < g_mercenaryHorses.size()) g_mercenaryPeds.push_back(0);

	MercLog("PruneDeadMercenaries: size peds=" + to_string(g_mercenaryPeds.size()));

	for (size_t i = 0; i < g_mercenaryPeds.size();)
	{
		Ped p = g_mercenaryPeds[i];
		bool pedDead = !ENTITY::DOES_ENTITY_EXIST(p) || ENTITY::IS_ENTITY_DEAD(p);
		if (pedDead)
		{
			MercLog("PruneDeadMercenaries: erasing index " + to_string(i));
			// Also delete the paired horse if it is still alive.
			Ped h = g_mercenaryHorses[i];
			if (h != 0 && ENTITY::DOES_ENTITY_EXIST(h) && !ENTITY::IS_ENTITY_DEAD(h))
				ENTITY::DELETE_ENTITY(&h);
			g_mercenaryPeds.erase(g_mercenaryPeds.begin() + i);
			g_mercenaryHorses.erase(g_mercenaryHorses.begin() + i);
			if (i < g_mercFollowTaskTick.size()) g_mercFollowTaskTick.erase(g_mercFollowTaskTick.begin() + i);
		}
		else
			++i;
	}
}

// ---- Spawn ----------------------------------------------------------------
// Parameterized so the same logic spawns any of the three recruitable gang
// members (Charles, John, Lenny) with their own ped model and named horse.
class MenuItemSpawnMercenary : public MenuItemDefault
{
	string m_charName;
	string m_pedModelName;
	string m_horseModelName;

	virtual void OnSelect()
	{
		// Using a story-mode ped model (e.g. CS_charlessmith) instead of a
		// generic bounty hunter model, per request. This is a pure cosmetic
		// swap – CREATE_PED, weapon setup, combat attributes, and the
		// follow/combat logic in MenuItemMercenaryFollow are all completely
		// unchanged, so it carries none of the native-call risk that's been
		// the actual source of every crash so far. Unlike the real story
		// character, this one is a normal spawned ped and CAN die.
		DWORD model = GAMEPLAY::GET_HASH_KEY(const_cast<char*>(m_pedModelName.c_str()));
		if (!(STREAMING::IS_MODEL_IN_CDIMAGE(model) && STREAMING::IS_MODEL_VALID(model)))
		{
			SetStatusText("model name invalid – check spelling");
			return;
		}
		STREAMING::REQUEST_MODEL(model, FALSE);
		while (!STREAMING::HAS_MODEL_LOADED(model))
			WaitAndDraw(0);

		Vector3 coords = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(
			PLAYER::PLAYER_PED_ID(), 0.0, 3.5, -0.3);
		Ped ped = PED::CREATE_PED(model, coords.x, coords.y, coords.z,
			static_cast<float>(rand() % 360), 0, 0, 0, 0);
		PED::SET_PED_VISIBLE(ped, TRUE);

		Hash mercGroup = GetOrCreateMercenaryRelationshipGroup();
		PED::SET_PED_RELATIONSHIP_GROUP_HASH(ped, mercGroup);
		PED::SET_PED_KEEP_TASK(ped, TRUE);
		PED::SET_PED_CAN_RAGDOLL(ped, TRUE);

		// Strip the character's default story-mode loadout (Charles in
		// particular comes with a bow baked into his model's inventory)
		// before giving him our own weapons, so he only ever uses the
		// carbine/revolver.
		WEAPON::REMOVE_ALL_PED_WEAPONS(ped, TRUE, TRUE);

		// Give weapons: repeater carbine + schofield revolver
		Hash carbine = GAMEPLAY::GET_HASH_KEY("WEAPON_REPEATER_CARBINE");
		Hash revolver = GAMEPLAY::GET_HASH_KEY("WEAPON_REVOLVER_SCHOFIELD");
		WEAPON::GIVE_DELAYED_WEAPON_TO_PED(ped, carbine, 999, 1, 0x2cd419dc);
		WEAPON::GIVE_DELAYED_WEAPON_TO_PED(ped, revolver, 999, 1, 0x2cd419dc);
		WEAPON::SET_PED_AMMO(ped, carbine, 999);
		WEAPON::SET_PED_AMMO(ped, revolver, 999);
		WEAPON::SET_CURRENT_PED_WEAPON(ped, carbine, 1, 0, 0, 0);

		ENTITY::SET_ENTITY_HEALTH(ped, 300, FALSE);
		SetMercCombatAttributes(ped);

		STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(model);

		// --- Spawn the character's own named horse ---
		DWORD horseModel = GAMEPLAY::GET_HASH_KEY(const_cast<char*>(m_horseModelName.c_str()));
		Ped horse = 0;
		if (STREAMING::IS_MODEL_IN_CDIMAGE(horseModel) && STREAMING::IS_MODEL_VALID(horseModel))
		{
			STREAMING::REQUEST_MODEL(horseModel, FALSE);
			while (!STREAMING::HAS_MODEL_LOADED(horseModel))
				WaitAndDraw(0);

			Vector3 horseCoords = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(
				ped, 1.5, 0.0, 0.0);
			horse = PED::CREATE_PED(horseModel, horseCoords.x, horseCoords.y, horseCoords.z,
				static_cast<float>(rand() % 360), 0, 0, 0, 0);
			PED::SET_PED_VISIBLE(horse, TRUE);
			ENTITY::SET_ENTITY_AS_MISSION_ENTITY(horse, TRUE, TRUE);

			// Mount the character on his horse right away.
			TASK::TASK_MOUNT_ANIMAL(ped, horse, -1, -1, 2.0f, 1, 0, 0);

			STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(horseModel);
		}

		g_mercenaryPeds.push_back(ped);
		g_mercenaryHorses.push_back(horse);
		g_mercFollowTaskTick.push_back(0);

		SetStatusText(m_charName + " spawned (" + to_string(g_mercenaryPeds.size()) + " active)\n"
			"weapons: carbine + schofield  |  horse: " + m_charName);
	}
public:
	MenuItemSpawnMercenary(string caption, string charName, string pedModelName, string horseModelName)
		: MenuItemDefault(caption), m_charName(charName),
		m_pedModelName(pedModelName), m_horseModelName(horseModelName) {
	}
};

// ---- Despawn --------------------------------------------------------------
class MenuItemDespawnMercenary : public MenuItemDefault
{
	virtual void OnSelect()
	{
		PruneDeadMercenaries();
		if (g_mercenaryPeds.empty())
		{
			SetStatusText("no mercenaries to despawn");
			return;
		}
		for (auto& p : g_mercenaryPeds)
			if (ENTITY::DOES_ENTITY_EXIST(p)) ENTITY::DELETE_ENTITY(&p);
		for (auto& h : g_mercenaryHorses)
			if (ENTITY::DOES_ENTITY_EXIST(h)) ENTITY::DELETE_ENTITY(&h);

		g_mercenaryPeds.clear();
		g_mercenaryHorses.clear();
		g_mercFollowTaskTick.clear();
		SetStatusText("mercenaries despawned");
	}
public:
	MenuItemDespawnMercenary(string caption)
		: MenuItemDefault(caption) {
	}
};

// ---- Follow + Fight --------------------------------------------------
class MenuItemMercenaryFollow : public MenuItemSwitchable
{
	virtual void OnSelect()
	{
		bool newState = !GetState();
		SetState(newState);
		SetStatusText(newState ? "mercenary will follow + auto-defend" : "mercenary follow off");
	}

	// ------------------------------------------------------------------
	// ARCHITECTURE CHANGE (2026-06-28): every crash across days of testing
	// happened inside FindNearbyEnemy's per-frame scan of nearby peds —
	// never in spawn-time setup, never in this follow logic below. Rather
	// than keep hunting for a "safe" way to scan ourselves, this now
	// follows the same approach proven-stable companion/bodyguard mods use
	// (e.g. Companion System on Nexus): don't scan for enemies at all.
	// SetMercCombatAttributes() at spawn time already sets CA_ALWAYS_FIGHT,
	// CA_AGGRESSIVE, and CA_DISABLE_FLEE_FROM_COMBAT on the merc. With those
	// flags, the GAME'S OWN ambient AI makes the ped fight back
	// automatically the instant something hostile attacks them or the
	// player nearby — exactly like any other combat-capable ped in the
	// world, with zero per-frame native calls from us. This script's only
	// job now is keeping the merc near the player when nothing is
	// attacking. No FindNearbyEnemy, no TASK_COMBAT_PED, no relationship
	// scanning of ambient peds, ever.
	// ------------------------------------------------------------------
	virtual void OnFrame()
	{
		// Only the state check runs every frame -- everything else is
		// throttled. Previously PruneDeadMercenaries + IS_ENTITY_DEAD +
		// DOES_ENTITY_EXIST were all running every frame before the
		// throttle gate, costing hundreds of native calls per second.
		if (!GetState()) return;

		static DWORD lastTick = 0;
		DWORD now = GetTickCount();
		if (now - lastTick < 3000) return;
		lastTick = now;

		// --- Everything below runs at most once every 2 seconds ---

		PruneDeadMercenaries();
		if (g_mercenaryPeds.empty()) return;

		Ped playerPed = PLAYER::PLAYER_PED_ID();

		if (ENTITY::IS_ENTITY_DEAD(playerPed))
		{
			for (auto& p : g_mercenaryPeds)
				if (ENTITY::DOES_ENTITY_EXIST(p)) ENTITY::DELETE_ENTITY(&p);
			for (auto& h : g_mercenaryHorses)
				if (ENTITY::DOES_ENTITY_EXIST(h)) ENTITY::DELETE_ENTITY(&h);
			g_mercenaryPeds.clear();
			g_mercenaryHorses.clear();
			g_mercFollowTaskTick.clear();
			SetState(false);
			SetStatusText("player died - mercenaries gone");
			return;
		}

		MercLog("OnFrame: throttle passed, fetching player state");
		Vector3 playerPos = ENTITY::GET_ENTITY_COORDS(playerPed, TRUE, FALSE);
		bool playerOnMount = PED::IS_PED_ON_MOUNT(playerPed);

		MercLog("OnFrame: player state fetched, entering merc loop, count=" + to_string(g_mercenaryPeds.size()));
		for (size_t i = 0; i < g_mercenaryPeds.size(); ++i)
		{
			MercLog("loop i=" + to_string(i) + ": start");
			Ped merc = g_mercenaryPeds[i];
			if (!ENTITY::DOES_ENTITY_EXIST(merc) || ENTITY::IS_ENTITY_DEAD(merc))
				continue;

			Ped horse = (i < g_mercenaryHorses.size()) ? g_mercenaryHorses[i] : 0;
			bool horseAlive = (horse != 0 && ENTITY::DOES_ENTITY_EXIST(horse) && !ENTITY::IS_ENTITY_DEAD(horse));
			bool mercOnMount = PED::IS_PED_ON_MOUNT(merc);

			// ---------------------------------------------------------------
			// MOUNT/DISMOUNT + FOLLOW LOGIC
			//
			// Key design decisions:
			//
			// 1. NEVER re-read mercOnMount immediately after issuing
			//    TASK_MOUNT_ANIMAL. The task is async; re-reading right
			//    away gives false, which makes us issue a GOTO on foot
			//    that cancels the mount task. Use a separate bool
			//    (mercPendingMount) to skip the follow block this tick.
			//
			// 2. When the merc is mounted, we issue
			//    TASK_FOLLOW_TO_OFFSET_OF_ENTITY on the HORSE targeting
			//    the PLAYER every throttle tick. This is the same native
			//    story-mode gang companions use, so the horse actively
			//    rides toward the player on the navmesh -- not teleport.
			//
			// 3. Target the PLAYER (not the merc). Old code targeted the
			//    merc, causing body-slam + combat-AI-attacks-horse bug.
			// ---------------------------------------------------------------

			bool mercPendingMount = false;

			if (horseAlive)
			{
				if (playerOnMount && !mercOnMount)
				{
					// Make sure the horse is close enough for the merc to
					// mount. If it has drifted away, teleport it beside the
					// merc first, then issue the mount task.
					Vector3 mercPos2 = ENTITY::GET_ENTITY_COORDS(merc, TRUE, FALSE);
					Vector3 hPos2 = ENTITY::GET_ENTITY_COORDS(horse, TRUE, FALSE);
					float hd2x = hPos2.x - mercPos2.x, hd2y = hPos2.y - mercPos2.y;
					if ((hd2x * hd2x + hd2y * hd2y) > 6.0f * 6.0f)
					{
						// Teleport horse right next to the merc so mount can begin
						Vector3 beside = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(merc, 1.5f, 0.0f, 0.0f);
						ENTITY::SET_ENTITY_COORDS(horse, beside.x, beside.y, beside.z, FALSE, FALSE, FALSE, FALSE);
					}
					MercLog("loop i=" + to_string(i) + ": player mounted, issuing TASK_MOUNT_ANIMAL");
					TASK::TASK_MOUNT_ANIMAL(merc, horse, -1, -1, 2.0f, 1, 0, 0);
					// Re-apply combat attributes: TASK_MOUNT_ANIMAL clears the ped's
					// task tree, which wipes combat flags set at spawn time. Without
					// this, the mounted merc reverts to passive and won't auto-defend.
					SetMercCombatAttributes(merc);
					mercPendingMount = true; // skip follow task this tick so we don't cancel the mount
				}
				else if (!playerOnMount && mercOnMount)
				{
					MercLog("loop i=" + to_string(i) + ": player on foot, dismounting merc");
					TASK::TASK_DISMOUNT_ANIMAL(merc, 0, 0, 0, 0, 0);
				}
			}

			if (mercPendingMount)
			{
				// Mount task just issued this tick; don't issue any follow
				// task or we'll cancel the mount before it completes.
				MercLog("loop i=" + to_string(i) + ": mount pending, skipping follow this tick");
			}
			else if (mercOnMount)
			{
				// -------------------------------------------------------
				// Merc is mounted. Issue TASK_FOLLOW_TO_OFFSET_OF_ENTITY
				// on the HORSE targeting the PLAYER -- same task story-mode
				// gang companions use.
				//
				// IMPORTANT: We do NOT re-issue this every second. Doing so
				// caused an AMD GPU driver TDR crash -- RAGE initialises the
				// navmesh follow task on the GPU command queue and hammering
				// it at 1 Hz on a laptop GPU saturates it and causes a reset.
				//
				// Strategy: issue once (tick==0), then only re-issue when the
				// horse falls >20 m behind AND 10 s have elapsed since the
				// last issue. persistFollowing=TRUE keeps the horse galloping
				// continuously without any further calls from us.
				// -------------------------------------------------------
				float sideOffset = (i % 2 == 0) ? -2.0f : 2.0f;

				Vector3 hRidePos = ENTITY::GET_ENTITY_COORDS(horse, TRUE, FALSE);
				float rdx = hRidePos.x - playerPos.x;
				float rdy = hRidePos.y - playerPos.y;
				float rideDistSq = rdx * rdx + rdy * rdy;

				// Ensure g_mercFollowTaskTick is large enough
				while (g_mercFollowTaskTick.size() <= i) g_mercFollowTaskTick.push_back(0);

				DWORD lastFollowIssue = g_mercFollowTaskTick[i];
				bool taskNeverIssued = (lastFollowIssue == 0);
				bool horseTooFarBehind = (rideDistSq > 20.0f * 20.0f);
				bool cooldownExpired = (now - lastFollowIssue > 10000u); // 10 s

				if (taskNeverIssued || (horseTooFarBehind && cooldownExpired))
				{
					TASK::TASK_FOLLOW_TO_OFFSET_OF_ENTITY(
						horse, playerPed,
						sideOffset, -2.0f, 0.0f,
						4.0f,   // gallop speed
						-1,     // no timeout
						3.0f,   // stop when within 3 m
						TRUE,   // persistFollowing
						FALSE, FALSE, FALSE, FALSE, FALSE);
					g_mercFollowTaskTick[i] = now;
					MercLog("loop i=" + to_string(i) + ": TASK_FOLLOW issued (distSq=" + to_string((int)rideDistSq) + ")");
				}

				// Emergency teleport if horse is 80m+ away (totally lost)
				const float kRideTeleportDistSq = 80.0f * 80.0f;
				if (rideDistSq > kRideTeleportDistSq)
				{
					MercLog("loop i=" + to_string(i) + ": 80m+ emergency teleport");
					Vector3 dest = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(playerPed, sideOffset, -2.0f, 0.0f);
					ENTITY::SET_ENTITY_COORDS(horse, dest.x, dest.y, dest.z, FALSE, FALSE, FALSE, FALSE);
					g_mercFollowTaskTick[i] = 0; // force re-issue next tick
				}
			}
			else
			{
				// Merc is on foot. Follow the player on foot.
				Vector3 mercPos = ENTITY::GET_ENTITY_COORDS(merc, TRUE, FALSE);
				float dx = mercPos.x - playerPos.x;
				float dy = mercPos.y - playerPos.y;
				float distSq = dx * dx + dy * dy;

				const float kTeleportDistSq = 80.0f * 80.0f;
				const float kRunDistSq = 8.0f * 8.0f;

				if (distSq > kTeleportDistSq)
				{
					MercLog("loop i=" + to_string(i) + ": too far on foot, teleporting to player");
					Vector3 dest = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(playerPed, (i % 2 == 0 ? -2.0f : 2.0f), -2.0f, 0.0f);
					ENTITY::SET_ENTITY_COORDS(merc, dest.x, dest.y, dest.z, FALSE, FALSE, FALSE, FALSE);
					// Also teleport horse with him so it doesn't get left behind
					if (horseAlive)
					{
						Vector3 hDest = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(playerPed, (i % 2 == 0 ? -3.5f : 3.5f), -2.0f, 0.0f);
						ENTITY::SET_ENTITY_COORDS(horse, hDest.x, hDest.y, hDest.z, FALSE, FALSE, FALSE, FALSE);
					}
				}
				else if (distSq > kRunDistSq)
				{
					MercLog("loop i=" + to_string(i) + ": far on foot, TASK_GOTO_ENTITY_OFFSET_XY on merc");
					AI::TASK_GOTO_ENTITY_OFFSET_XY(merc, playerPed, 2000,
						0.0f, -2.0f, 0.0f, 2.0f, FALSE);
				}
				// else: close enough, let combat AI do its thing uninterrupted
			}

			MercLog("loop i=" + to_string(i) + ": end of iteration");
		}
		MercLog("OnFrame: loop complete, exiting");
	}
public:
	MenuItemMercenaryFollow(string caption)
		: MenuItemSwitchable(caption) {
	}
};

// ===========================================================================
//  MENU BUILDERS  (all original – mercenary menu kept identical signature)
// ===========================================================================

MenuBase* CreateMercenaryMenu(MenuController* controller)
{
	MenuBase* menu = new MenuBase(new MenuItemTitle("MERCENARY"));
	controller->RegisterMenu(menu);

	menu->AddItem(new MenuItemSpawnMercenary("SPAWN CHARLES", "Charles",
		"CS_charlessmith", "A_C_Horse_Gang_Charles"));
	menu->AddItem(new MenuItemSpawnMercenary("SPAWN JOHN", "John",
		"CS_johnmarston", "A_C_Horse_Gang_John"));
	menu->AddItem(new MenuItemSpawnMercenary("SPAWN LENNY", "Lenny",
		"CS_lenny", "A_C_Horse_Gang_Lenny"));
	menu->AddItem(new MenuItemDespawnMercenary("DESPAWN ALL"));
	menu->AddItem(new MenuItemMercenaryFollow("FOLLOW + AUTO-DEFEND"));

	return menu;
}

MenuBase* CreatePlayerTeleportMenu(MenuController* controller)
{
	MenuBase* menu = new MenuBase(new MenuItemListTitle("TELEPORT"));
	controller->RegisterMenu(menu);

	menu->AddItem(new MenuItemPlayerTeleportToMarker("MARKER"));
	menu->AddItem(new MenuItemPlayerTeleport("SOUTH MAP", { -5311.2583,  -4612.00,    -10.63389 }));
	menu->AddItem(new MenuItemPlayerTeleport("SOUTH GUAMA", { 1315.66381, -6815.48,    42.377101 }));
	menu->AddItem(new MenuItemPlayerTeleport("ANNESBURG", { 2898.593994, 1239.85253, 44.073299 }));
	menu->AddItem(new MenuItemPlayerTeleport("STRAWBERRY", { -1725.22143,  -418.11560, 153.55740 }));
	menu->AddItem(new MenuItemPlayerTeleport("VALENTINE", { -213.152496,  691.802979, 112.37100 }));
	menu->AddItem(new MenuItemPlayerTeleport("RHODES", { 1282.707520,-1275.7485,  74.945099 }));
	menu->AddItem(new MenuItemPlayerTeleport("SAINT DENIS", { 2336.584961,-1106.2358,  44.737598 }));
	menu->AddItem(new MenuItemPlayerTeleport("WAPITI", { 538.738525, 2217.46557, 240.23280 }));
	menu->AddItem(new MenuItemPlayerTeleport("BUTCHERCREEK", { 2552.203613,  835.510010, 81.183098 }));
	menu->AddItem(new MenuItemPlayerTeleport("BLACKWATER", { -798.338379,-1238.9395,  43.537899 }));
	menu->AddItem(new MenuItemPlayerTeleport("BEECHERS", { -1653.19738, -1448.8156,  82.503502 }));
	menu->AddItem(new MenuItemPlayerTeleport("CALIGA HALL", { 1705.509888,-1386.3237,  42.884998 }));
	menu->AddItem(new MenuItemPlayerTeleport("BRAITHWAITE", { 1011.190674,-1661.6768,  45.918301 }));
	menu->AddItem(new MenuItemPlayerTeleport("VANHORN", { 2982.234863,  445.724915, 51.491501 }));
	menu->AddItem(new MenuItemPlayerTeleport("CORNWALL", { 437.7247920,  494.582092,107.67649 }));
	menu->AddItem(new MenuItemPlayerTeleport("COLTER", { -1371.6590,   2388.5073,  307.7218 }));
	menu->AddItem(new MenuItemPlayerTeleport("EMERALD RANCH", { 1332.332642,  300.425110,  86.306297 }));
	menu->AddItem(new MenuItemPlayerTeleport("PRONGHORN", { -2616.57714,   519.256775, 144.10809 }));
	menu->AddItem(new MenuItemPlayerTeleport("MANZANITA POST", { -1977.98754, -1545.6749,  112.87020 }));
	menu->AddItem(new MenuItemPlayerTeleport("LAGRAS", { 2111.099121,  -662.25317,  41.259899 }));
	menu->AddItem(new MenuItemPlayerTeleport("ARMADILLO", { -3622.65527, -2586.5795,  -15.36900 }));
	menu->AddItem(new MenuItemPlayerTeleport("TUMBLEWEED", { -5382.39453, -2940.1596,    1.582700 }));
	menu->AddItem(new MenuItemPlayerTeleport("MACFARLANES RANCH", { -2296.26318, -2454.4101,  60.969898 }));
	menu->AddItem(new MenuItemPlayerTeleport("BENEDICT POINT", { -5269.60400, -3411.0588,  -23.15930 }));

	return menu;
}

MenuBase* CreatePlayerChangeModelHorseMenu(MenuController* controller)
{
	auto menu = new MenuBase(new MenuItemListTitle("HORSE  MODELS"));
	controller->RegisterMenu(menu);

	unordered_map<string, vector<pair<string, string>>> breeds;
	for each(auto& modelInfo in pedModelInfos)
		if (modelInfo.horse)
		{
			size_t pos = modelInfo.name.find_first_of(' ');
			string breed = modelInfo.name.substr(0, pos);
			string kind = modelInfo.name.substr(pos + 1, modelInfo.name.size() - pos - 1);
			breeds[breed].push_back({ kind, modelInfo.model });
		}

	for each(auto& breed in breeds)
	{
		auto breedMenu = new MenuBase(new MenuItemListTitle(breed.first));
		controller->RegisterMenu(breedMenu);
		menu->AddItem(new MenuItemMenu(breed.first, breedMenu));
		for each(auto& kindAndModel in breed.second)
			breedMenu->AddItem(new MenuItemChangePlayerModel(kindAndModel.first, kindAndModel.second));
	}

	return menu;
}

MenuBase* CreatePlayerChangeModelAnimalMenuExactFilter(MenuController* controller, bool horse, bool dog, bool fish)
{
	auto menu = new MenuBase(new MenuItemListTitle("ANIMAL  MODELS"));
	controller->RegisterMenu(menu);

	for each(auto& modelInfo in pedModelInfos)
		if (modelInfo.animal &&
			modelInfo.horse == horse && modelInfo.dog == dog && modelInfo.fish == fish)
			menu->AddItem(new MenuItemChangePlayerModel(modelInfo.name, modelInfo.model));

	return menu;
}

MenuBase* CreatePlayerChangeModelAnimalMenu(MenuController* controller)
{
	auto menu = new MenuBase(new MenuItemTitle("ANIMAL  MODELS"));
	controller->RegisterMenu(menu);

	menu->AddItem(new MenuItemMenu("HORSES", CreatePlayerChangeModelHorseMenu(controller)));
	menu->AddItem(new MenuItemMenu("DOGS", CreatePlayerChangeModelAnimalMenuExactFilter(controller, false, true, false)));
	menu->AddItem(new MenuItemMenu("FISH", CreatePlayerChangeModelAnimalMenuExactFilter(controller, false, false, true)));
	menu->AddItem(new MenuItemMenu("OTHER", CreatePlayerChangeModelAnimalMenuExactFilter(controller, false, false, false)));

	return menu;
}

MenuBase* CreatePlayerChangeModelHumanMenuExactFilter(MenuController* controller, bool cutscene, bool male, bool female, bool young, bool middleaged, bool old)
{
	auto menu = new MenuBase(new MenuItemListTitle("PED  MODELS"));
	controller->RegisterMenu(menu);

	for each(auto& modelInfo in pedModelInfos)
		if (!modelInfo.animal &&
			modelInfo.cutscene == cutscene && modelInfo.male == male && modelInfo.female == female &&
			modelInfo.young == young && modelInfo.middleaged == middleaged && modelInfo.old == old)
			menu->AddItem(new MenuItemChangePlayerModel(modelInfo.name, modelInfo.model));

	return menu;
}

MenuBase* CreatePlayerChangeModelHumanMenu(MenuController* controller)
{
	auto menu = new MenuBase(new MenuItemTitle("PED  MODELS"));
	controller->RegisterMenu(menu);

	menu->AddItem(new MenuItemMenu("CUTSCENE", CreatePlayerChangeModelHumanMenuExactFilter(controller, true, false, false, false, false, false)));
	menu->AddItem(new MenuItemMenu("MALE YOUNG", CreatePlayerChangeModelHumanMenuExactFilter(controller, false, true, false, true, false, false)));
	menu->AddItem(new MenuItemMenu("MALE MIDDLE", CreatePlayerChangeModelHumanMenuExactFilter(controller, false, true, false, false, true, false)));
	menu->AddItem(new MenuItemMenu("MALE OLD", CreatePlayerChangeModelHumanMenuExactFilter(controller, false, true, false, false, false, true)));
	menu->AddItem(new MenuItemMenu("FEMALE YOUNG", CreatePlayerChangeModelHumanMenuExactFilter(controller, false, false, true, true, false, false)));
	menu->AddItem(new MenuItemMenu("FEMALE MIDDLE", CreatePlayerChangeModelHumanMenuExactFilter(controller, false, false, true, false, true, false)));
	menu->AddItem(new MenuItemMenu("FEMALE OLD", CreatePlayerChangeModelHumanMenuExactFilter(controller, false, false, true, false, false, true)));
	menu->AddItem(new MenuItemMenu("MISC", CreatePlayerChangeModelHumanMenuExactFilter(controller, false, false, false, false, false, false)));

	return menu;
}

MenuBase* CreatePlayerChangeModelMenu(MenuController* controller)
{
	auto menu = new MenuBase(new MenuItemTitle("SKIN  CHANGER"));
	controller->RegisterMenu(menu);

	menu->AddItem(new MenuItemMenu("ANIMALS", CreatePlayerChangeModelAnimalMenu(controller)));
	menu->AddItem(new MenuItemMenu("PEDS", CreatePlayerChangeModelHumanMenu(controller)));

	return menu;
}

MenuBase* CreatePlayerWantedMenu(MenuController* controller)
{
	MenuBase* menu = new MenuBase(new MenuItemTitle("WANTED  OPTIONS"));
	controller->RegisterMenu(menu);

	menu->AddItem(new MenuItemPlayerClearWanted("CLEAR BOUNTY", true, false));
	menu->AddItem(new MenuItemPlayerClearWanted("CLEAR WANTED", true, true));
	menu->AddItem(new MenuItemPlayerNeverWanted("NEVER WANTED"));

	return menu;
}

MenuBase* CreatePlayerTransportMenu(MenuController* controller)
{
	MenuBase* menu = new MenuBase(new MenuItemTitle("TRANSPORT  OPTIONS"));
	controller->RegisterMenu(menu);

	menu->AddItem(new MenuItemPlayerHorseInvincible("INVINCIBLE HORSE"));
	menu->AddItem(new MenuItemPlayerHorseUnlimStamina("UNLIM HORSE STAMINA"));
	menu->AddItem(new MenuItemVehicleBoost("VEHICLE BOOST"));

	return menu;
}

MenuBase* CreatePlayerMiscMenu(MenuController* controller)
{
	MenuBase* menu = new MenuBase(new MenuItemTitle("PLAYER  MISC"));
	controller->RegisterMenu(menu);

	menu->AddItem(new MenuItemPlayerEveryoneIgnored("EVERYONE IGNORED"));
	menu->AddItem(new MenuItemPlayerNoiseless("NOISELESS"));
	menu->AddItem(new MenuItemPlayerSuperJump("SUPER JUMP"));

	return menu;
}

MenuBase* CreatePlayerMenu(MenuController* controller)
{
	MenuBase* menu = new MenuBase(new MenuItemTitle("PLAYER  OPTIONS"));
	controller->RegisterMenu(menu);

	menu->AddItem(new MenuItemMenu("TRANSPORT", CreatePlayerTransportMenu(controller)));
	menu->AddItem(new MenuItemMenu("TELEPORT", CreatePlayerTeleportMenu(controller)));
	menu->AddItem(new MenuItemMenu("SKINS", CreatePlayerChangeModelMenu(controller)));
	menu->AddItem(new MenuItemMenu("WANTED", CreatePlayerWantedMenu(controller)));
	menu->AddItem(new MenuItemPlayerFix("FIX PLAYER"));
	menu->AddItem(new MenuItemPlayerAddCash("ADD CASH", 1000 * 100));
	menu->AddItem(new MenuItemPlayerFastHeal("FAST HEAL"));
	menu->AddItem(new MenuItemPlayerInvincible("INVINCIBLE"));
	menu->AddItem(new MenuItemPlayerUnlimStamina("UNLIM STAMINA"));
	menu->AddItem(new MenuItemPlayerUnlimAbility("UNLIM ABILITY"));
	menu->AddItem(new MenuItemMenu("MISC", CreatePlayerMiscMenu(controller)));

	return menu;
}

MenuBase* CreateHorseSpawnerMenu(MenuController* controller)
{
	auto menu = new MenuBase(new MenuItemListTitle("HORSE  SPAWNER"));
	controller->RegisterMenu(menu);

	menu->AddItem(new MenuItemSpawnHorseRandom("random horse"));

	unordered_map<string, vector<pair<string, string>>> breeds;
	for each(auto& modelInfo in pedModelInfos)
		if (modelInfo.horse)
		{
			size_t pos = modelInfo.name.find_first_of(' ');
			string breed = modelInfo.name.substr(0, pos);
			string kind = modelInfo.name.substr(pos + 1, modelInfo.name.size() - pos - 1);
			breeds[breed].push_back({ kind, modelInfo.model });
		}

	for each(auto& breed in breeds)
	{
		auto breedMenu = new MenuBase(new MenuItemListTitle(breed.first));
		controller->RegisterMenu(breedMenu);
		menu->AddItem(new MenuItemMenu(breed.first, breedMenu));
		for each(auto& kindAndModel in breed.second)
			breedMenu->AddItem(new MenuItemSpawnPed(kindAndModel.first, kindAndModel.second));
	}

	return menu;
}

MenuBase* CreateAnimalSpawnerMenuExactFilter(MenuController* controller, bool horse, bool dog, bool fish)
{
	auto menu = new MenuBase(new MenuItemListTitle("ANIMAL  SPAWNER"));
	controller->RegisterMenu(menu);

	for each(auto& modelInfo in pedModelInfos)
		if (modelInfo.animal &&
			modelInfo.horse == horse && modelInfo.dog == dog && modelInfo.fish == fish)
			menu->AddItem(new MenuItemSpawnPed(modelInfo.name, modelInfo.model));

	return menu;
}

MenuBase* CreateAnimalSpawnerMenu(MenuController* controller)
{
	auto menu = new MenuBase(new MenuItemTitle("ANIMAL  SPAWNER"));
	controller->RegisterMenu(menu);

	menu->AddItem(new MenuItemSpawnAnimalRandom("RANDOM"));
	menu->AddItem(new MenuItemMenu("HORSES", CreateHorseSpawnerMenu(controller)));
	menu->AddItem(new MenuItemMenu("DOGS", CreateAnimalSpawnerMenuExactFilter(controller, false, true, false)));
	menu->AddItem(new MenuItemMenu("FISH", CreateAnimalSpawnerMenuExactFilter(controller, false, false, true)));
	menu->AddItem(new MenuItemMenu("OTHER", CreateAnimalSpawnerMenuExactFilter(controller, false, false, false)));

	return menu;
}

MenuBase* CreateHumanSpawnerMenuExactFilter(MenuController* controller, bool cutscene, bool male, bool female, bool young, bool middleaged, bool old)
{
	auto menu = new MenuBase(new MenuItemListTitle("PED  SPAWNER"));
	controller->RegisterMenu(menu);

	for each(auto& modelInfo in pedModelInfos)
		if (!modelInfo.animal &&
			modelInfo.cutscene == cutscene && modelInfo.male == male && modelInfo.female == female &&
			modelInfo.young == young && modelInfo.middleaged == middleaged && modelInfo.old == old)
			menu->AddItem(new MenuItemSpawnPed(modelInfo.name, modelInfo.model));

	return menu;
}

MenuBase* CreateHumanSpawnerMenu(MenuController* controller)
{
	auto menu = new MenuBase(new MenuItemTitle("PED  SPAWNER"));
	controller->RegisterMenu(menu);

	menu->AddItem(new MenuItemSpawnPedRandom("RANDOM PED"));
	menu->AddItem(new MenuItemMenu("CUTSCENE", CreateHumanSpawnerMenuExactFilter(controller, true, false, false, false, false, false)));
	menu->AddItem(new MenuItemMenu("MALE YOUNG", CreateHumanSpawnerMenuExactFilter(controller, false, true, false, true, false, false)));
	menu->AddItem(new MenuItemMenu("MALE MIDDLE", CreateHumanSpawnerMenuExactFilter(controller, false, true, false, false, true, false)));
	menu->AddItem(new MenuItemMenu("MALE OLD", CreateHumanSpawnerMenuExactFilter(controller, false, true, false, false, false, true)));
	menu->AddItem(new MenuItemMenu("FEMALE YOUNG", CreateHumanSpawnerMenuExactFilter(controller, false, false, true, true, false, false)));
	menu->AddItem(new MenuItemMenu("FEMALE MIDDLE", CreateHumanSpawnerMenuExactFilter(controller, false, false, true, false, true, false)));
	menu->AddItem(new MenuItemMenu("FEMALE OLD", CreateHumanSpawnerMenuExactFilter(controller, false, false, true, false, false, true)));
	menu->AddItem(new MenuItemMenu("MISC", CreateHumanSpawnerMenuExactFilter(controller, false, false, false, false, false, false)));

	return menu;
}

enum eVehicleType
{
	vtAirbaloon,
	vtBoat,
	vtCannon,
	vtTrain,
	vtWagon
};

eVehicleType GetVehicleTypeUsingModel(string model)
{
	Hash hash = GAMEPLAY::GET_HASH_KEY(const_cast<char*>(model.c_str()));
	if (VEHICLE::IS_THIS_MODEL_A_BOAT(hash))
		return vtBoat;
	if (VEHICLE::IS_THIS_MODEL_A_TRAIN(hash))
		return vtTrain;
	if (model == "gatling_gun" || model == "gatlingMaxim02" || model == "hotchkiss_cannon" || model == "breach_cannon")
		return vtCannon;
	if (model == "hotAirBalloon01")
		return vtAirbaloon;
	return vtWagon;
}

MenuBase* CreateCannonSpawnerMenu(MenuController* controller)
{
	auto menu = new MenuBase(new MenuItemTitle("CANNON  SPAWNER"));
	controller->RegisterMenu(menu);

	auto menuItemWrapIn = new MenuItemSwitchable("WRAP IN SPAWNED");
	menu->AddItem(menuItemWrapIn);

	auto menuItemSetProperly = new MenuItemSwitchable("SET PROPERLY");
	menu->AddItem(menuItemSetProperly);

	for each(auto& model in vehicleModels)
		if (GetVehicleTypeUsingModel(model) == vtCannon)
			menu->AddItem(new MenuItemSpawnVehicle(model, { 0.0, 3.0, 0.0 }, 0.0, menuItemWrapIn, menuItemSetProperly, true, false));

	return menu;
}

MenuBase* CreateBoatSpawnerMenu(MenuController* controller,
	MenuItemSwitchable* menuItemWrapIn, MenuItemSwitchable* menuItemSetProperly)
{
	auto menu = new MenuBase(new MenuItemListTitle("BOAT  SPAWNER"));
	controller->RegisterMenu(menu);

	for each(auto& model in vehicleModels)
		if (GetVehicleTypeUsingModel(model) == vtBoat)
			menu->AddItem(new MenuItemSpawnVehicle(model, { 0.0, 10.0, 0.0 }, 90.0, menuItemWrapIn, menuItemSetProperly, false, false));

	return menu;
}

MenuBase* CreateTrainSpawnerMenu(MenuController* controller)
{
	auto menu = new MenuBase(new MenuItemListTitle("TRAIN  SPAWNER"));
	controller->RegisterMenu(menu);

	for each(auto& model in vehicleModels)
		if (GetVehicleTypeUsingModel(model) == vtTrain)
			menu->AddItem(new MenuItemSpawnVehicle(model, { 0.0, 5.0, -1.0 }, 90.0, NULL, NULL, false, false));

	return menu;
}

MenuBase* CreateWagonSpawnerMenu(MenuController* controller,
	MenuItemSwitchable* menuItemWrapIn, MenuItemSwitchable* menuItemSetProperly, bool noPeds)
{
	auto menu = new MenuBase(new MenuItemListTitle("WAGON  SPAWNER"));
	controller->RegisterMenu(menu);

	for each(auto& model in vehicleModels)
		if (GetVehicleTypeUsingModel(model) == vtWagon)
			menu->AddItem(new MenuItemSpawnVehicle(model, { 1.0, 5.0, 0.0 }, 90.0, menuItemWrapIn, menuItemSetProperly, true, noPeds));

	return menu;
}

MenuBase* CreateVehicleMiscSpawnerMenu(MenuController* controller, MenuItemSwitchable* menuItemWrapIn)
{
	auto menu = new MenuBase(new MenuItemListTitle("MISC  SPAWNER"));
	controller->RegisterMenu(menu);

	for each(auto& model in vehicleModels)
		if (GetVehicleTypeUsingModel(model) == vtAirbaloon)
			menu->AddItem(new MenuItemSpawnVehicle(model, { 0.0, 5.0, 0.0 }, 0.0, menuItemWrapIn, NULL, false, false));

	return menu;
}

MenuBase* CreateVehicleSpawnerMenu(MenuController* controller)
{
	auto menu = new MenuBase(new MenuItemTitle("VEHICLE  SPAWNER"));
	controller->RegisterMenu(menu);

	auto menuItemWrapIn = new MenuItemSwitchable("WRAP IN SPAWNED");
	menu->AddItem(menuItemWrapIn);

	auto menuItemSetProperly = new MenuItemSwitchable("SET PROPERLY");
	menu->AddItem(menuItemSetProperly);

	menu->AddItem(new MenuItemMenu("BOATS", CreateBoatSpawnerMenu(controller, menuItemWrapIn, menuItemSetProperly)));
	menu->AddItem(new MenuItemMenu("TRAINS", CreateTrainSpawnerMenu(controller)));
	menu->AddItem(new MenuItemMenu("WAGONS", CreateWagonSpawnerMenu(controller, menuItemWrapIn, menuItemSetProperly, false)));
	menu->AddItem(new MenuItemMenu("JUST WAGONS", CreateWagonSpawnerMenu(controller, menuItemWrapIn, menuItemSetProperly, true)));
	menu->AddItem(new MenuItemMenu("MISC", CreateVehicleMiscSpawnerMenu(controller, menuItemWrapIn)));

	return menu;
}

MenuBase* CreateWeaponSelectMenu(MenuController* controller)
{
	auto menu = new MenuBase(new MenuItemListTitle("GET WEAPON"));
	controller->RegisterMenu(menu);

	for each(auto info in weaponInfos)
		menu->AddItem(new MenuItemGiveWeapon(info.uiname, info.name));

	return menu;
}

MenuBase* CreateWeaponMenu(MenuController* controller)
{
	auto menu = new MenuBase(new MenuItemTitle("WEAPON  OPTIONS"));
	controller->RegisterMenu(menu);

	menu->AddItem(new MenuItemMenu("GET ALL WEAPON", CreateWeaponSelectMenu(controller)));
	menu->AddItem(new MenuItemWeaponPowerfullGuns("POWERFULL GUNS"));
	menu->AddItem(new MenuItemWeaponPowerfullMelee("POWERFULL MELEE"));
	menu->AddItem(new MenuItemWeaponNoReload("NO RELOAD"));
	menu->AddItem(new MenuItemWeaponDropCurrent("DROP CURRENT"));

	return menu;
}

MenuBase* CreateTimeMenu(MenuController* controller)
{
	auto menu = new MenuBase(new MenuItemTimeTitle("TIME"));
	controller->RegisterMenu(menu);

	menu->AddItem(new MenuItemTimeAdjust("HOUR FORWARD", 1));
	menu->AddItem(new MenuItemTimeAdjust("HOUR BACKWARD", -1));
	menu->AddItem(new MenuItemTimePause("CLOCK PAUSED"));
	menu->AddItem(new MenuItemTimeRealistic("CLOCK REAL"));
	menu->AddItem(new MenuItemTimeSystemSynced("SYNC WITH SYSTEM"));

	return menu;
}

MenuBase* CreateWeatherMenu(MenuController* controller)
{
	auto menu = new MenuBase(new MenuItemListTitle("WEATHER"));
	controller->RegisterMenu(menu);

	menu->AddItem(new MenuItemWeatherFreeze("FREEZE CURRENT"));
	menu->AddItem(new MenuItemWeatherWind("FAST WIND"));

	for each(string weather in weatherTypes)
		menu->AddItem(new MenuItemWeatherSelect(weather));

	return menu;
}

MenuBase* CreateMiscMenu(MenuController* controller)
{
	auto menu = new MenuBase(new MenuItemTitle("MISC  OPTIONS"));
	controller->RegisterMenu(menu);

	menu->AddItem(new MenuItemMiscRevealMap("REVEAL MAP"));
	menu->AddItem(new MenuItemMiscAddHonor("ADD HONOR"));
	menu->AddItem(new MenuItemMiscHideHud("HIDE HUD"));

	menu->AddItem(new MenuItemMiscTransportGuns("HORSE TURRETS", true, true));
	menu->AddItem(new MenuItemMiscTransportGuns("HORSE CANNONS", true, false));
	menu->AddItem(new MenuItemMiscTransportGuns("VEHICLE TURRETS", false, true));
	menu->AddItem(new MenuItemMiscTransportGuns("VEHICLE CANNONS", false, false));

	return menu;
}

MenuBase* CreateMainMenu(MenuController* controller)
{
	auto menu = new MenuBase(new MenuItemTitle("NATIVE  TRAINER  (AB)"));
	controller->RegisterMenu(menu);

	menu->AddItem(new MenuItemMenu("PLAYER", CreatePlayerMenu(controller)));
	menu->AddItem(new MenuItemMenu("ANIMALS", CreateAnimalSpawnerMenu(controller)));
	menu->AddItem(new MenuItemMenu("PEDS", CreateHumanSpawnerMenu(controller)));
	menu->AddItem(new MenuItemMenu("CANNONS", CreateCannonSpawnerMenu(controller)));
	menu->AddItem(new MenuItemMenu("VEHICLES", CreateVehicleSpawnerMenu(controller)));
	menu->AddItem(new MenuItemMenu("WEAPON", CreateWeaponMenu(controller)));
	menu->AddItem(new MenuItemMenu("MERCENARY", CreateMercenaryMenu(controller)));
	menu->AddItem(new MenuItemMenu("TIME", CreateTimeMenu(controller)));
	menu->AddItem(new MenuItemMenu("WEATHER", CreateWeatherMenu(controller)));
	menu->AddItem(new MenuItemMenu("MISC", CreateMiscMenu(controller)));

	return menu;
}

void main()
{
	// Wait until the game world is fully ready before calling any natives
	// or building menus that call VEHICLE::IS_THIS_MODEL_A_BOAT etc.
	// Calling those natives before entity pools are initialised causes an
	// immediate crash when Arthur first moves.
	while (!PLAYER::IS_PLAYER_PLAYING(PLAYER::PLAYER_ID()))
		WAIT(100);
	WAIT(1000); // extra safety margin for pools to settle

	auto menuController = new MenuController();
	auto mainMenu = CreateMainMenu(menuController);

	while (true)
	{
		if (!menuController->HasActiveMenu() && MenuInput::MenuSwitchPressed())
		{
			MenuInput::MenuInputBeep();
			menuController->PushMenu(mainMenu);
		}
		menuController->Update();
		WAIT(0);
	}
}

void ScriptMain()
{
	srand(GetTickCount());
	main();
}