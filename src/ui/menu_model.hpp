#pragma once

#include <string>
#include <vector>

namespace irx::ui {

enum class Screen { Splash, MainMenu, Play, Inventory, Loadout, Store, Settings, Connecting, Match };

struct MenuItem {
    std::string id;
    std::string labelFa;
    std::string labelEn;
    bool enabled = true;
};

class MenuModel {
public:
    MenuModel();
    Screen screen() const noexcept;
    void setScreen(Screen value) noexcept;
    const std::vector<MenuItem>& mainItems() const noexcept;
    bool fullscreen() const noexcept;
    void setFullscreen(bool value) noexcept;
    float masterVolume() const noexcept;
    void setMasterVolume(float value) noexcept;

private:
    Screen screen_ = Screen::Splash;
    bool fullscreen_ = false;
    float masterVolume_ = 0.85f;
    std::vector<MenuItem> mainItems_;
};

}