#pragma once

#include <span>
#include <string_view>

namespace irx::gameplay {

enum class WeaponClass { Pistol, Smg, Rifle, Shotgun, Sniper, Grenade };
enum class Rarity { Common, Rare, Epic, Legendary, Mythic };

struct WeaponSpec {
    std::string_view id;
    std::string_view displayName;
    WeaponClass weaponClass;
    Rarity rarity;
    int price;
    int magazine;
    int reserve;
    float damage;
    float fireRate;
    float recoil;
    float spread;
    std::string_view killEffect;
};

std::span<const WeaponSpec> weaponCatalog();
const WeaponSpec* findWeapon(std::string_view id);

}