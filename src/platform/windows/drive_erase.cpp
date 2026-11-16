// THE code in fenrir that can write to a drive. compiled only with -DFENRIR_DRIVE_ERASE=ON, used only by fenrir-cli
// running as administrator. tests/check_no_write_apis.cmake lets this one file open drives and volumes for writing,
// write, and lock and dismount volumes, and nothing else, nowhere else.
// order, every step of which stops the run with nothing written when it fails:
//   1. the plan is an overwrite of this drive number
//   2. the drive, listed again, is the one the plan was approved for (model, serial, size) and has no blocker
//      (drive_blockers(): USB only, not windows', not fenrir's, its temp folder's or a kept folder's)
//   3. each of its volumes is locked and dismounted: one in use stops it
//   4. the drive is opened for writing, and asked once more on that same handle who it is and how it's attached
// then it's written front to back, flushed, and read back. a drive that disappears midway ends interrupted

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <format>
#include <utility>
#include <vector>

#include "storage/drive_erase.h"
#include "storage/simulated_drive.h"
#include "win_disks.h"

namespace {
	using namespace storage;

	constexpr DWORD chunk = 1u << 20; // a multiple of every sector size up to 64 KiB

	struct t_handle {
		HANDLE value = INVALID_HANDLE_VALUE;
		explicit t_handle(HANDLE h) : value(h) {}
		t_handle(t_handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
		t_handle& operator=(t_handle&&) = delete;
		~t_handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } // a volume's lock goes with its handle
		bool ok() const { return value != INVALID_HANDLE_VALUE; }
	};

	struct t_buffer { // sector aligned, which unbuffered drive i/o needs
		std::byte* data = static_cast<std::byte*>(VirtualAlloc(nullptr, chunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
		~t_buffer() { if (data) VirtualFree(data, 0, MEM_RELEASE); }
	};

	// the drive went away (unplugged, reset), as opposed to a sector that failed
	bool gone(DWORD error) {
		switch (error) {
		case ERROR_DEVICE_NOT_CONNECTED:
		case ERROR_NO_SUCH_DEVICE:
		case ERROR_DEV_NOT_EXIST:
		case ERROR_NOT_READY:
		case ERROR_FILE_NOT_FOUND:
		case ERROR_DEVICE_REMOVED:
		case ERROR_MEDIA_CHANGED:
			return true;
		default:
			return false;
		}
	}

	OVERLAPPED at(std::uint64_t offset) {
		OVERLAPPED o{};
		o.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
		o.OffsetHigh = static_cast<DWORD>(offset >> 32);
		return o;
	}

	std::string narrow(const std::wstring& text) {
		std::string out;
		for (wchar_t c : text)
			out += c < 0x80 ? static_cast<char>(c) : '?'; // volume names are ascii
		return out;
	}
}

bool storage::drive_erase_built() noexcept { return true; }

storage::Result<storage::SanitizeReport> storage::DriveOverwriteBackend::execute(const Authorization& approved, const CancellationToken* cancel, ProgressSink* progress) {
	const SanitizePlan& plan = approved.plan();
	const std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(number_);

	// 1. the plan is an overwrite of this drive
	if (plan.request.method != Method::overwrite)
		return fail(Errc::unsupported, "erasing a drive: only an overwrite, nothing was done");
	if (plan.target.identity.path != std::filesystem::path(path))
		return fail(Errc::mismatch, "erasing a drive: the plan is for another drive, nothing was done");

	// 2. still that drive, and nothing blocks it
	Result<Target> now = inspect();
	if (!now)
		return std::unexpected(now.error());
	if (now->identity != plan.target.identity)
		return fail(Errc::mismatch, "erasing a drive: it isn't the drive the plan was approved for any more, nothing was done");
	if (!now->capabilities.blockers.empty())
		return fail(Errc::unsupported, "erasing a drive: it's one this build won't erase, nothing was done");
	const std::uint64_t size = plan.target.identity.size;
	const std::uint32_t sector = plan.target.capabilities.sector_size;
	if (size == 0 || sector == 0 || chunk % sector != 0 || size % sector != 0)
		return fail(Errc::unsupported, "erasing a drive: its size or sector size isn't usable, nothing was done");

	SanitizeReport r;
	r.plan = plan;
	r.started = iso_now();
	r.events.push_back("listed the drives again: it's the one the plan was approved for, attached over USB, and nothing fenrir needs is on it");

	// 3. its volumes offline, kept that way while their handles are open
	std::vector<t_handle> offline;
	for (const std::wstring& volume : win::volumes_on(number_)) {
		t_handle handle(CreateFileW(volume.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
		if (!handle.ok())
			return fail(Errc::busy, "erasing a drive: one of its volumes couldn't be opened to take it offline, nothing was written", GetLastError());
		DWORD got = 0;
		if (!DeviceIoControl(handle.value, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr))
			return fail(Errc::busy, "erasing a drive: one of its volumes is in use (a program has a file on it open), nothing was written", GetLastError());
		if (!DeviceIoControl(handle.value, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr))
			return fail(Errc::io_error, "erasing a drive: one of its volumes couldn't be dismounted, nothing was written", GetLastError());
		r.events.push_back("took volume " + narrow(volume) + " offline (locked and dismounted)");
		offline.push_back(std::move(handle));
	}

	// 4. open it for writing, and ask once more on that handle
	t_handle drive(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
		FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, nullptr));
	if (!drive.ok())
		return fail(GetLastError() == ERROR_ACCESS_DENIED ? Errc::access_denied : Errc::io_error, "erasing a drive: it couldn't be opened for writing (administrator rights are needed), nothing was written", GetLastError());
	DiskInfo opened;
	opened.number = number_;
	opened.path = path;
	if (!win::describe_disk(drive.value, opened) || drive_identity(opened) != plan.target.identity || opened.bus != "USB")
		return fail(Errc::mismatch, "erasing a drive: what it said about itself once opened isn't what was approved, nothing was written");
	r.events.push_back("opened it for writing and asked again on that handle: the same drive");

	t_buffer buffer, expected;
	if (!buffer.data || !expected.data)
		return fail(Errc::out_of_memory, "erasing a drive, nothing was written");
	auto end = [&](SanitizeStatus status) {
		r.status = status;
		r.finished = iso_now();
		return r;
	};

	r.events.push_back(std::format("writing {} over all {} bytes", plan.request.zeros ? "zeros" : "the pattern", size));
	for (std::uint64_t pos = 0; pos < size;) {
		if (cancel && cancel->cancelled()) {
			FlushFileBuffers(drive.value);
			r.events.push_back(std::format("stopped at byte {}, as asked: what's past it wasn't written", pos));
			return end(SanitizeStatus::interrupted);
		}
		const DWORD length = static_cast<DWORD>(std::min<std::uint64_t>(chunk, size - pos));
		sanitation::pattern(std::span(buffer.data, length), pos, plan.request);
		OVERLAPPED where = at(pos);
		DWORD wrote = 0;
		if (!WriteFile(drive.value, buffer.data, length, &wrote, &where) || wrote != length) {
			const DWORD error = GetLastError();
			if (gone(error)) {
				r.errors.push_back(std::format("the drive went away at byte {} (os error {}): what's past it wasn't written", pos, error));
				return end(SanitizeStatus::interrupted);
			}
			r.errors.push_back(std::format("writing at byte {} failed (os error {}): what's past it wasn't written", pos, error));
			return end(SanitizeStatus::failed);
		}
		pos += length;
		r.bytes_done = pos;
		if (progress)
			progress->progress(pos, plan.request.verify ? size * 2 : size);
	}
	if (!FlushFileBuffers(drive.value)) {
		const DWORD error = GetLastError();
		r.errors.push_back(std::format("flushing it failed (os error {})", error));
		return end(gone(error) ? SanitizeStatus::interrupted : SanitizeStatus::failed);
	}
	r.events.push_back("all of it was written (write-through) and flushed");

	if (!plan.request.verify) {
		r.verification.note = "not read back, as the plan said";
		return end(SanitizeStatus::completed);
	}
	Verification& v = r.verification;
	v.done = true;
	for (std::uint64_t pos = 0; pos < size;) {
		const DWORD length = static_cast<DWORD>(std::min<std::uint64_t>(chunk, size - pos));
		OVERLAPPED where = at(pos);
		DWORD got = 0;
		if (!ReadFile(drive.value, buffer.data, length, &got, &where) || got != length) {
			const DWORD error = GetLastError();
			r.errors.push_back(std::format("reading back at byte {} failed (os error {})", pos, error));
			v.note = "it couldn't all be read back: whether it worked can't be told";
			return end(gone(error) ? SanitizeStatus::interrupted : SanitizeStatus::indeterminate);
		}
		sanitation::pattern(std::span(expected.data, length), pos, plan.request);
		for (DWORD s = 0; s < length; s += sector)
			if (!std::equal(buffer.data + s, buffer.data + s + sector, expected.data + s))
				if (v.mismatches++ == 0)
					v.first_mismatch = pos + s;
		pos += length;
		v.bytes_checked = pos;
		if (progress)
			progress->progress(size + pos, size * 2);
	}
	v.note = "every sector was read back from the drive (unbuffered) and compared with what was written. that's what the drive's controller shows: blocks it keeps aside can't be read or written from here";
	if (v.mismatches != 0) {
		r.errors.push_back(std::format("{} sectors didn't read back as written, the first at byte {}", v.mismatches, v.first_mismatch));
		return end(SanitizeStatus::failed);
	}
	return end(SanitizeStatus::verified);
}
