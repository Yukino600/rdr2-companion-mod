# RDR2 Native Trainer – Mercenary Mod

A modification built on top of Alexander Blade's RDR2 Native Trainer SDK that adds a **Mercenary system** — recruit Charles, John, or Lenny as AI companions who follow you, fight alongside you, and ride with you across the map.

---

## Features

### Mercenary System
- Spawn **Charles Smith**, **John Marston**, or **Lenny Summers** as active mercenaries
- Each mercenary spawns with their own named horse
- Each mercenary is armed with a **Repeater Carbine + Schofield Revolver**
- All three can be active at the same time

### Follow & Auto-Defend (`FOLLOW + AUTO-DEFEND` toggle)
- Mercenaries follow you on foot when you are on foot
- When you mount your horse, mercenaries mount their own horses and ride alongside you
- They attack enemies automatically — no need to manually order them
- They will charge at enemies aggressively (combat attributes: always fight, never flee, advance on target)
- They can shoot while mounted on horseback
- If a mercenary falls too far behind (80m+), they teleport to your position so they never get permanently lost
- When their horse drifts too far, it is teleported back beside them before remounting
- If you die, all mercenaries are automatically despawned

### Known Limitations
- Mercenaries **cannot** use cover — they will stand and fight in the open
- Mercenaries **cannot** ride as passengers on your horse
- Mercenaries **cannot** board wagons, coaches, or any other vehicle/transport
- The mod may cause **noticeable lag on low-spec PCs** — the follow system runs on a throttled 3-second tick to minimize impact, but low-end hardware may still struggle
- Combat AI is handled entirely by the game's own ambient AI system; mercenaries react to threats automatically but their pathfinding and decision-making is limited by the game engine

---

## Installation

### Requirements
- Red Dead Redemption 2 (PC, Story Mode only)
- [Script Hook RDR2](http://www.dev-c.com/rdr2/scripthookrdr2/) by Alexander Blade

### Steps
1. Build the project in Visual Studio (see [Building from Source](#building-from-source))
2. Copy the compiled `NativeTrainer.asi` into your RDR2 root folder (where `RDR2.exe` is)
3. Make sure `ScriptHookRDR2.dll` and `dinput8.dll` are also in the same folder
4. Launch RDR2 in Story Mode

---

## Usage

| Key | Action |
|-----|--------|
| `F5` | Open/close the trainer menu |
| Arrow keys | Navigate the menu |
| `Enter` | Select / toggle |
| `Backspace` | Go back |

In the menu, navigate to **MERCENARY** to access:
- `SPAWN CHARLES` — spawns Charles Smith with his horse
- `SPAWN JOHN` — spawns John Marston with his horse
- `SPAWN LENNY` — spawns Lenny Summers with his horse
- `DESPAWN ALL` — removes all active mercenaries
- `FOLLOW + AUTO-DEFEND` — toggle follow and combat behavior on/off

---

## Building from Source

1. Install [Visual Studio](https://visualstudio.microsoft.com/) with the **Desktop development with C++** workload
2. Download the [ScriptHookRDR2 SDK](http://www.dev-c.com/rdr2/scripthookrdr2/) and place the SDK headers where the project expects them
3. Open `NativeTrainer.vcxproj` in Visual Studio
4. Set configuration to **Release / x64**
5. Build — the `.asi` output will appear in the `bin/` folder

---

## Performance Notes

This mod is designed to be as lightweight as possible:
- The follow/combat loop runs at most **once every 3 seconds**, not every frame
- No per-frame ped scanning or enemy searching
- No disk I/O during gameplay (debug logging is compiled out in release builds)

However, RDR2 is already a very demanding game. On low-spec hardware running the game at minimum settings, any additional scripting load will be felt. If you experience lag, it is likely your hardware hitting its limit rather than a bug in the mod.

For low-spec PCs, community mods like [RDR2 Performance Booster V2](https://www.nexusmods.com/reddeadredemption2/mods/7671) may help with general game performance.

---

## Credits

- Base trainer framework: [Alexander Blade](http://www.dev-c.com/) — ScriptHookRDR2 SDK & Native Trainer sample
- Mercenary system, follow logic, combat AI integration, horse riding, performance optimizations: added on top of AB's original trainer
