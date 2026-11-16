#define NOMINMAX
#include "../../win32/ui_win32.h"
#include "app.h"

#include <mutex>
#include <shellapi.h>
#include <shobjidl.h>
#include <thread>

#pragma comment(lib, "ole32.lib")

// the windows file dialogs (IFileOpenDialog), each on a thread of its own: Show() runs its own message loop until it's
// closed, which would stop the window drawing if it ran on the render thread. fenrir's window is its owner, so it stays
// above it and the window takes no input while it's open. the state is shared with the thread and outlives it

namespace {
	struct t_state {
		std::mutex mutex;
		bool showing = false;
		int key = 0;
		std::optional<std::filesystem::path> picked;
	};
	std::shared_ptr<t_state> state = std::make_shared<t_state>();

	HWND owner() {
		const auto& windows = celosia_win32::variables::windows;
		return windows.empty() ? nullptr : windows.begin()->second.second;
	}

	std::optional<std::filesystem::path> run(fenrir::app::dialogs::e_kind kind, HWND parent) {
		std::optional<std::filesystem::path> result;
		IFileDialog* dialog = nullptr;
		const bool save = kind == fenrir::app::dialogs::e_kind::new_image;
		if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
			return result;
		FILEOPENDIALOGOPTIONS options = 0;
		dialog->GetOptions(&options);
		options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR;
		if (save) {
			// no "replace it?": nothing is ever written over. a name that's taken is either a stopped copy with its map
			// next to it, which goes on, or it's refused
			options &= ~FOS_OVERWRITEPROMPT;
			const COMDLG_FILTERSPEC types[] = { { L"Disk images (*.img)", L"*.img" }, { L"All files", L"*.*" } };
			dialog->SetFileTypes(ARRAYSIZE(types), types);
			dialog->SetDefaultExtension(L"img");
			dialog->SetTitle(L"Where the copy goes: a new file, or a stopped copy to go on with");
		} else if (kind == fenrir::app::dialogs::e_kind::folder) {
			options |= FOS_PICKFOLDERS;
			dialog->SetTitle(L"Where recovered files go: a folder on a different drive than the one being recovered");
		} else {
			options |= FOS_FILEMUSTEXIST;
			const COMDLG_FILTERSPEC types[] = {
				{ L"Disk images (*.img, *.dd, *.raw, *.bin, *.iso, *.001)", L"*.img;*.dd;*.raw;*.bin;*.iso;*.001" },
				{ L"All files", L"*.*" },
			};
			dialog->SetFileTypes(ARRAYSIZE(types), types);
			dialog->SetTitle(L"A disk image, opened read-only");
		}
		dialog->SetOptions(options);
		if (SUCCEEDED(dialog->Show(parent))) {
			IShellItem* item = nullptr;
			if (SUCCEEDED(dialog->GetResult(&item))) {
				PWSTR path = nullptr;
				if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
					result = std::filesystem::path(path);
					CoTaskMemFree(path);
				}
				item->Release();
			}
		}
		dialog->Release();
		return result;
	}
}

bool fenrir::app::dialogs::available() { return true; }

bool fenrir::app::dialogs::showing() {
	std::lock_guard lock(state->mutex);
	return state->showing;
}

void fenrir::app::dialogs::show(e_kind kind, int key) {
	{
		std::lock_guard lock(state->mutex);
		if (state->showing)
			return;
		state->showing = true;
		state->key = key;
		state->picked.reset();
	}
	std::thread([shared = state, kind, parent = owner()] {
		const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
		const std::optional<std::filesystem::path> picked = run(kind, parent);
		if (com)
			CoUninitialize();
		std::lock_guard lock(shared->mutex);
		shared->picked = picked;
		shared->showing = false;
	}).detach(); // the window can't close while it's open: it's disabled under the dialog
}

std::optional<std::filesystem::path> fenrir::app::dialogs::take(int key) {
	std::lock_guard lock(state->mutex);
	if (state->showing || state->key != key || !state->picked)
		return std::nullopt;
	std::optional<std::filesystem::path> picked = std::move(state->picked);
	state->picked.reset();
	return picked;
}

bool fenrir::app::drives::elevated() {
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return false;
	TOKEN_ELEVATION elevation{};
	DWORD size = 0;
	const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &size) && elevation.TokenIsElevated != 0;
	CloseHandle(token);
	return ok;
}

bool fenrir::app::drives::restart_elevated() {
	wchar_t exe[MAX_PATH + 1]{};
	if (!GetModuleFileNameW(nullptr, exe, MAX_PATH))
		return false;
	SHELLEXECUTEINFOW run{ sizeof run };
	run.fMask = SEE_MASK_NOASYNC;
	run.hwnd = owner();
	run.lpVerb = L"runas"; // windows asks first, saying fenrir wants administrator rights
	run.lpFile = exe;
	run.nShow = SW_SHOWNORMAL;
	return ShellExecuteExW(&run) != FALSE; // false too when it was turned down
}
