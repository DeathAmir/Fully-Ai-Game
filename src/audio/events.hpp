#pragma once

#include <string_view>

namespace irx::audio {

enum class Event {
    FootstepConcrete,
    FootstepMetal,
    FootstepSand,
    Jump,
    Land,
    ReloadStart,
    ReloadInsert,
    ReloadEnd,
    FirePistol,
    FireSmg,
    FireRifle,
    FireShotgun,
    FireSniper,
    DryFire,
    GrenadePin,
    GrenadeThrow,
    BombPlant,
    BombBeep,
    BombDefuse,
    UiHover,
    UiClick,
    RoundWin,
    RoundLose
};

constexpr std::string_view eventId(Event event) {
    switch (event) {
        case Event::FootstepConcrete: return "footstep_concrete";
        case Event::FootstepMetal: return "footstep_metal";
        case Event::FootstepSand: return "footstep_sand";
        case Event::Jump: return "jump";
        case Event::Land: return "land";
        case Event::ReloadStart: return "reload_start";
        case Event::ReloadInsert: return "reload_insert";
        case Event::ReloadEnd: return "reload_end";
        case Event::FirePistol: return "fire_pistol";
        case Event::FireSmg: return "fire_smg";
        case Event::FireRifle: return "fire_rifle";
        case Event::FireShotgun: return "fire_shotgun";
        case Event::FireSniper: return "fire_sniper";
        case Event::DryFire: return "dry_fire";
        case Event::GrenadePin: return "grenade_pin";
        case Event::GrenadeThrow: return "grenade_throw";
        case Event::BombPlant: return "bomb_plant";
        case Event::BombBeep: return "bomb_beep";
        case Event::BombDefuse: return "bomb_defuse";
        case Event::UiHover: return "ui_hover";
        case Event::UiClick: return "ui_click";
        case Event::RoundWin: return "round_win";
        case Event::RoundLose: return "round_lose";
    }
    return "unknown";
}

}