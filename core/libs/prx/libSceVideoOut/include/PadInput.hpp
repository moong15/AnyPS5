#ifndef CORE_LIBS_PRX_LIBSCEVIDEOOUT_PADINPUT_HPP
#define CORE_LIBS_PRX_LIBSCEVIDEOOUT_PADINPUT_HPP

#include "SDL_events.h"
#include "SDL_gamecontroller.h"
#include "prx/libScePad/include/KeyboardMouse.hpp"
#include "prx/libScePad/include/PadState.hpp"
#include <chrono>

class DisplayWindow;

class PadInput {
public:
    PadInput();
    ~PadInput();
    void HandleEvent(const SDL_Event& event, DisplayWindow& window);
    void Update();
    bool MouseModeEnabled() const { return keyboardMouse.MouseMode(); }
    // Diagnostic aid: holds a pad button with no physical controller behind it, so an
    // unattended run can get past a screen that waits for one. Cleared by CloseScripted.
    void SetScriptedButton(Pad::PadButton button, bool down);

private:
    explicit PadInput(Pad::InputConfiguration configuration);
    void publish();
    void setMouseMode(bool enabled);
    void openFirstAvailableController();
    void openController(int deviceIndex);
    void closeController();
    PadInputState sampleController() const;
    void applyOutput();
    void enableSensors();

    Pad::KeyboardMouse keyboardMouse;
    SDL_GameController* controller = nullptr;
    PadInputState controllerState{};
    std::uint32_t scriptedButtons = 0;
    std::uint32_t outputSequence = 0;
    PadOutputState outputState{};
    bool outputPending = false;
    std::chrono::steady_clock::time_point nextRumbleRefresh{};
};

#endif
