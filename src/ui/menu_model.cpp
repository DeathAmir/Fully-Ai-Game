#include "ui/menu_model.hpp"

#include <algorithm>

namespace irx::ui {

MenuModel::MenuModel() : mainItems_{{"play", "بازی", "Play", true}, {"loadout", "تجهیزات", "Loadout", true}, {"inventory", "انبار", "Inventory", true}, {"store", "فروشگاه", "Store", true}, {"settings", "تنظیمات", "Settings", true}} {}

Screen MenuModel::screen() const noexcept { return screen_; }
void MenuModel::setScreen(Screen value) noexcept { screen_ = value; }
const std::vector<MenuItem>& MenuModel::mainItems() const noexcept { return mainItems_; }
bool MenuModel::fullscreen() const noexcept { return fullscreen_; }
void MenuModel::setFullscreen(bool value) noexcept { fullscreen_ = value; }
float MenuModel::masterVolume() const noexcept { return masterVolume_; }
void MenuModel::setMasterVolume(float value) noexcept { masterVolume_ = std::clamp(value, 0.0f, 1.0f); }

}