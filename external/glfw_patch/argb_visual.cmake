# run by FetchContent in glfw's source directory after it's downloaded (PATCH_COMMAND in CMakeLists.txt)
# glfw 3.4 gives windows without an opengl context (vulkan) the default x11 visual, which has no alpha channel, so a
# transparent framebuffer stays opaque on x11. this makes it pick a 32 bit visual with alpha for them, the way it does for glx
set(file src/x11_window.c)
file(READ ${file} source)

set(anchor "    if (!visual)
    {
        visual = DefaultVisual(_glfw.x11.display, _glfw.x11.screen);")
set(patch "    if (!visual && fbconfig->transparent) // celosui: a visual with alpha for vulkan windows too
    {
        XVisualInfo wanted = {0};
        wanted.screen = _glfw.x11.screen;
        wanted.depth = 32;
        wanted.class = TrueColor;
        int count = 0;
        XVisualInfo* infos = XGetVisualInfo(_glfw.x11.display, VisualScreenMask | VisualDepthMask | VisualClassMask, &wanted, &count);
        for (int i = 0; i < count; i++)
        {
            if (_glfwIsVisualTransparentX11(infos[i].visual))
            {
                visual = infos[i].visual;
                depth = infos[i].depth;
                break;
            }
        }
        if (infos)
            XFree(infos);
    }

")

string(REPLACE "\r" "" anchor "${anchor}") # this file may be checked out with windows line endings, glfw's sources aren't
string(REPLACE "\r" "" patch "${patch}")

string(FIND "${source}" "celosui: a visual with alpha" patched)
if(patched GREATER -1)
    return()
endif()
string(FIND "${source}" "${anchor}" found)
if(found EQUAL -1)
    message(FATAL_ERROR "glfw_patch/argb_visual.cmake: ${file} doesn't look like glfw 3.4's anymore, update the patch")
endif()
string(REPLACE "${anchor}" "${patch}${anchor}" source "${source}")
file(WRITE ${file} "${source}")
