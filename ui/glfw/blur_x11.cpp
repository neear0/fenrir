#include "../glfw/ui_glfw.h"

// the desktop blurred behind the window on x11, done by the compositor. kwin blurs the parts of a window listed in its
// _KDE_NET_WM_BLUR_BEHIND_REGION property (x, y, width, height per rectangle, none at all = the whole window) and shows
// it can by putting the same property on the root window. the window needs an alpha channel for any of it to show,
// see external/glfw_patch/argb_visual.cmake

#ifdef _WIN32
bool celosia_glfw::x11::blur_supported(GLFWwindow*) {
    return false;
}

void celosia_glfw::x11::set_blur(GLFWwindow*, bool, float) {}
#else
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3native.h>
#include <X11/Xatom.h>

static Atom blur_atom(Display* display, bool create) {
    return XInternAtom(display, "_KDE_NET_WM_BLUR_BEHIND_REGION", create ? False : True);
}

bool celosia_glfw::x11::blur_supported(GLFWwindow* window) {
    if (glfwGetPlatform() != GLFW_PLATFORM_X11 || !glfwGetWindowAttrib(window, GLFW_TRANSPARENT_FRAMEBUFFER)) // no alpha channel or no compositor
        return false;

    Display* display = glfwGetX11Display();
    const Atom atom = blur_atom(display, false);
    if (atom == None) // nobody ever named it, so kwin isn't running
        return false;

    int count = 0;
    Atom* properties = XListProperties(display, DefaultRootWindow(display), &count);
    bool found = false;
    for (int i = 0; i < count && !found; i++)
        found = properties[i] == atom;
    if (properties)
        XFree(properties);
    return found;
}

void celosia_glfw::x11::set_blur(GLFWwindow* window, bool enabled, float rounding) {
    if (glfwGetPlatform() != GLFW_PLATFORM_X11)
        return;

    Display* display = glfwGetX11Display();
    const Window handle = glfwGetX11Window(window);
    const Atom atom = blur_atom(display, enabled);
    if (atom == None) // never named, so the window can't have the property
        return;
    if (!enabled) {
        XDeleteProperty(display, handle, atom);
        XFlush(display);
        return;
    }

    // rounded corners as one rectangle per row of pixels in them, kept inside the window's own curve
    int width, height;
    glfwGetWindowSize(window, &width, &height);
    const int radius = ImMin((int)rounding, ImMin(width, height) / 2);
    std::vector<long> region; // xlib wants format 32 properties as longs
    for (int y = 0; y < radius; y++) {
        const float dy = radius - y - 0.5f;
        const int inset = (int)ceilf(radius - sqrtf(radius * radius - dy * dy));
        region.insert(region.end(), { inset, y, width - inset * 2, 1 });
        region.insert(region.end(), { inset, height - 1 - y, width - inset * 2, 1 });
    }
    if (radius > 0)
        region.insert(region.end(), { 0, radius, width, height - radius * 2 });

    const long whole_window = 0; // the property needs some data to point at even when it's empty
    XChangeProperty(display, handle, atom, XA_CARDINAL, 32, PropModeReplace,
        (const unsigned char*)(region.empty() ? &whole_window : region.data()), (int)region.size());
    XFlush(display);
}
#endif
