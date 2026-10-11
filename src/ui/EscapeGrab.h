#pragma once

// While the borderless stage is open, Escape is grabbed at the display so the
// shortcut still arrives when the embedded web view owns the keyboard.
class EscapeGrab
{
public:
    EscapeGrab() = default;
    ~EscapeGrab();

    void setArmed(bool armed);
    bool consumeEscape();

private:
    void* display = nullptr;
    unsigned long root = 0;
    unsigned int code = 0;
    bool armed = false;
};
