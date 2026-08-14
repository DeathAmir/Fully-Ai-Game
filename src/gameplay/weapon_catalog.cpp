#include "gameplay/weapon_catalog.hpp"

#include <array>

namespace irx::gameplay {

namespace {
constexpr std::array<WeaponSpec, 13> kWeapons{{
    {"pistol", "P200", WeaponClass::Pistol, Rarity::Common, 0, 13, 52, 24.0f, 6.0f, 0.45f, 0.012f, "default"},
    {"deagle", "D-Eagle", WeaponClass::Pistol, Rarity::Rare, 700, 7, 35, 54.0f, 3.66f, 1.15f, 0.020f, "default"},
    {"smg", "Vector", WeaponClass::Smg, Rarity::Rare, 1250, 30, 120, 22.0f, 13.0f, 0.35f, 0.030f, "default"},
    {"shotgun", "M1014", WeaponClass::Shotgun, Rarity::Epic, 1800, 8, 32, 66.0f, 1.58f, 1.35f, 0.080f, "default"},
    {"rifle", "M4-R", WeaponClass::Rifle, Rarity::Epic, 2700, 30, 90, 34.0f, 10.33f, 0.78f, 0.017f, "default"},
    {"ak", "AK-X", WeaponClass::Rifle, Rarity::Legendary, 2900, 30, 90, 36.0f, 10.0f, 0.92f, 0.020f, "sparks"},
    {"sniper", "AWM", WeaponClass::Sniper, Rarity::Legendary, 4750, 5, 25, 115.0f, 0.70f, 2.1f, 0.002f, "shatter"},
    {"mythic_ember", "Ember Crown", WeaponClass::Rifle, Rarity::Mythic, 0, 30, 120, 36.0f, 10.0f, 0.80f, 0.016f, "ember"},
    {"mythic_void", "Void Reaper", WeaponClass::Rifle, Rarity::Mythic, 0, 32, 128, 34.0f, 10.66f, 0.72f, 0.015f, "void"},
    {"mythic_frost", "Zero Pulse", WeaponClass::Smg, Rarity::Mythic, 0, 36, 144, 25.0f, 13.5f, 0.31f, 0.024f, "frost"},
    {"flash", "Flashbang", WeaponClass::Grenade, Rarity::Common, 200, 1, 0, 0.0f, 0.0f, 0.0f, 0.0f, "flash"},
    {"smoke", "Smoke", WeaponClass::Grenade, Rarity::Common, 300, 1, 0, 0.0f, 0.0f, 0.0f, 0.0f, "smoke"},
    {"he", "HE Grenade", WeaponClass::Grenade, Rarity::Common, 300, 1, 0, 88.0f, 0.0f, 0.0f, 0.0f, "explosion"},
}};
}

std::span<const WeaponSpec> weaponCatalog() { return kWeapons; }

const WeaponSpec* findWeapon(std::string_view id) {
    for (const auto& weapon : kWeapons) if (weapon.id == id) return &weapon;
    return nullptr;
}

}