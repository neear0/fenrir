// precompiled header, force-included into every project source file (ImGui's own sources don't use it)
// only add headers that rarely change here, editing this file rebuilds everything

#pragma once
#define IMGUI_DEFINE_MATH_OPERATORS
#define _CRT_SECURE_NO_WARNINGS

#ifdef _WIN32
#include <Windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#endif

#include <iostream>
#include <array>
#include <cstdint>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_map>
#include <map>
#include <vector>

#include "../external/imgui/imgui.h"
#include "../external/imgui/imgui_internal.h"
