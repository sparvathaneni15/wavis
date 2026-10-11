#include "EscapeGrab.h"

#if defined(__linux__)
 #include <X11/Xlib.h>
 #include <X11/keysym.h>
#endif

EscapeGrab::~EscapeGrab()
{
    setArmed(false);
#if defined(__linux__)
    if (display != nullptr)
        XCloseDisplay(static_cast<Display*>(display));
    display = nullptr;
#endif
}

void EscapeGrab::setArmed(bool shouldArm)
{
#if defined(__linux__)
    if (shouldArm == armed)
        return;

    if (display == nullptr)
        display = XOpenDisplay(nullptr);
    if (display == nullptr)
        return;

    auto* dpy = static_cast<Display*>(display);
    if (root == 0)
        root = static_cast<unsigned long>(DefaultRootWindow(dpy));
    if (code == 0)
        code = XKeysymToKeycode(dpy, XK_Escape);

    const auto previous = XSetErrorHandler(+[](Display*, XErrorEvent*) { return 0; });
    if (shouldArm)
        XGrabKey(dpy, static_cast<int>(code), AnyModifier, static_cast<Window>(root), False, GrabModeAsync, GrabModeAsync);
    else
        XUngrabKey(dpy, static_cast<int>(code), AnyModifier, static_cast<Window>(root));
    XSync(dpy, False);
    XSetErrorHandler(previous);
    armed = shouldArm;
#else
    (void) shouldArm;
#endif
}

bool EscapeGrab::consumeEscape()
{
#if defined(__linux__)
    if (!armed || display == nullptr)
        return false;

    auto* dpy = static_cast<Display*>(display);
    bool hit = false;
    while (XPending(dpy) > 0)
    {
        XEvent event;
        XNextEvent(dpy, &event);
        if (event.type == KeyPress)
            hit = true;
    }
    return hit;
#else
    return false;
#endif
}
