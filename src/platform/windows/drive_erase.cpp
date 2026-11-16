// THE code in fenrir that can write to a drive. compiled only with -DFENRIR_DRIVE_ERASE=ON, into a library only
// fenrir-cli (and the tests) link, used only by fenrir-cli running as administrator. tests/check_no_write_apis.cmake
// lets this one file open a drive and its volumes for writing, write, and lock and dismount volumes, nothing else,
// nowhere else, and pins how many times each appears.
// order, every step of which stops the run with nothing written when it fails:
//   1. the plan is an overwrite of this drive number
//   2. the drive, listed again, is the one the plan was approved for (model, serial, size) and has no blocker
//      (drive_blockers(): USB only, not windows', not fenrir's, its temp folder's, the current or a kept folder's)
//   3. the drive is opened for writing and asked on that handle who it is, how it's attached and its sector size
//   4. each of its volumes is locked and dismounted, and they have to be exactly the ones the listing found: one in
//      use, one that can't be placed, or one that's also on another drive stops it
//   5. the handle is asked who it is once more, now that nothing else can be using the drive
// then it's written front to back, flushed and read back. a drive that stops answering on that handle midway ends
// interrupted, one that answers but failed a write ends failed. reviewed before its first run (Fable, see the README)

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

	struct t_buffer { // page aligned, which unbuffered drive i/o needs
		std::byte* data = static_cast<std::byte*>(VirtualAlloc(nullptr, chunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
		~t_buffer() { if (data) VirtualFree(data, 0, MEM_RELEASE); }
	};

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

	// asked on the open handle: the approved drive, on USB, with the approved sector size, and nothing unreadable
	bool still_approved(HANDLE drive, std::uint32_t number, const std::wstring& path, const SanitizePlan& plan) {
		DiskInfo now;
		now.number = number;
		now.path = path;
		return win::describe_disk(drive, now) && now.error.empty() && now.bus == "USB" && now.sector_size == plan.target.capabilities.sector_size
			&& drive_identity(now) == plan.target.identity;
	}
}

bool storage::drive_erase_built() noexcept { return true; }

storage::Result<storage::SanitizeReport> storage::DriveOverwriteBackend::execute(const Authorization& approved, const CancellationToken* cancel, ProgressSink* progress) {
	const SanitizePlan& plan = approved.plan();
	const std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(number_);

	// 1. the plan is an overwrite of this drive
	if (plan.request.method != Method::overwrite)
		return fail(Errc::unsupported, "erasing a drive: only an overwrite, nothing was written");
	if (plan.target.identity.path != std::filesystem::path(path))
		return fail(Errc::mismatch, "erasing a drive: the plan is for another drive, nothing was written");

	// 2. still that drive, and nothing blocks it
	Result<Target> now = inspect();
	if (!now)
		return std::unexpected(now.error());
	if (now->identity != plan.target.identity)
		return fail(Errc::mismatch, "erasing a drive: it isn't the drive the plan was approved for any more, nothing was written");
	if (!now->capabilities.blockers.empty())
		return fail(Errc::unsupported, "erasing a drive: it's one this build won't erase, nothing was written");
	const std::uint64_t size = plan.target.identity.size;
	const std::uint32_t sector = plan.target.capabilities.sector_size;
	if (size == 0 || sector == 0 || chunk % sector != 0 || size % sector != 0 || now->capabilities.sector_size != sector)
		return fail(Errc::unsupported, "erasing a drive: its size or sector size isn't usable, nothing was written");

	// the volumes taken offline in 4: declared before the drive, so the drive is closed before they come back
	std::vector<t_handle> offline;

	// 3. opened for writing, and asked on that handle
	t_handle drive(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
		FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, nullptr));
	if (!drive.ok()) {
		const DWORD error = GetLastError();
		return fail(error == ERROR_ACCESS_DENIED ? Errc::access_denied : Errc::io_error,
			error == ERROR_ACCESS_DENIED ? "erasing a drive: it needs administrator rights, nothing was written" : "erasing a drive: it couldn't be opened for writing, nothing was written", error);
	}
	if (!still_approved(drive.value, number_, path, plan))
		return fail(Errc::mismatch, "erasing a drive: what it said about itself once opened isn't what was approved, nothing was written");

	SanitizeReport r;
	r.plan = plan;
	r.started = iso_now();
	r.events.push_back("listed the drives again: it's the one the plan was approved for, attached over USB, and nothing fenrir needs is on it");
	r.events.push_back("opened it for writing and asked on that handle: the same drive");

	// 4. its volumes offline, exactly the ones the listing found, kept offline while their handles are open
	Result<std::vector<std::wstring>> volumes = win::volumes_on(number_);
	if (!volumes)
		return std::unexpected(Error{ volumes.error().code, "erasing a drive: its volumes couldn't all be found, nothing was written", volumes.error().os_error });
	if (volumes->size() != now->capabilities.volumes.size())
		return fail(Errc::mismatch, "erasing a drive: its volumes changed since it was listed, nothing was written");
	for (const std::wstring& volume : *volumes) {
		t_handle handle(CreateFileW(volume.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
		if (!handle.ok())
			return fail(Errc::busy, "erasing a drive: one of its volumes couldn't be opened to take it offline, nothing was written", GetLastError());
		DWORD got = 0;
		if (!DeviceIoControl(handle.value, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr))
			return fail(Errc::busy, "erasing a drive: one of its volumes is in use (a program or an Explorer window has it open), nothing was written", GetLastError());
		if (!DeviceIoControl(handle.value, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr))
			return fail(Errc::io_error, "erasing a drive: one of its volumes couldn't be dismounted, nothing was written", GetLastError());
		r.events.push_back("took volume " + narrow(volume) + " offline (locked and dismounted)");
		offline.push_back(std::move(handle));
	}

	// 5. once more, now that nothing else can be using it
	if (!still_approved(drive.value, number_, path, plan))
		return fail(Errc::mismatch, "erasing a drive: what it said about itself after its volumes went offline isn't what was approved, nothing was written");

	t_buffer buffer, expected;
	if (!buffer.data || !expected.data)
		return fail(Errc::out_of_memory, "erasing a drive, nothing was written");
	auto end = [&](SanitizeStatus status) {
		r.events.push_back("Windows may show its old partitions until it's unplugged and plugged in again");
		r.status = status;
		r.finished = iso_now();
		return r;
	};
	// a failed write, flush or read: the drive went away if its handle can't say it's still the approved drive
	auto went_away = [&] { return !still_approved(drive.value, number_, path, plan); };

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
		const bool ok = WriteFile(drive.value, buffer.data, length, &wrote, &where) != FALSE;
		if (!ok || wrote != length) {
			const DWORD error = ok ? 0 : GetLastError(); // a short write says no error
			if (went_away()) {
				r.errors.push_back(std::format("the drive stopped answering at byte {} (os error {}): what's past it wasn't written", pos, error));
				return end(SanitizeStatus::interrupted);
			}
			r.errors.push_back(std::format("writing at byte {} failed (os error {}, {} of {} bytes written): what's past it wasn't written", pos, error, wrote, length));
			return end(SanitizeStatus::failed);
		}
		pos += length;
		r.bytes_done = pos;
		if (progress)
			progress->progress(pos, plan.request.verify ? size * 2 : size);
	}

	// some cheap controllers refuse to flush: that's noted, and the read back still says what's there
	bool flushed = true;
	if (!FlushFileBuffers(drive.value)) {
		const DWORD error = GetLastError();
		if (went_away()) {
			r.errors.push_back(std::format("the drive stopped answering when it was flushed (os error {})", error));
			return end(SanitizeStatus::interrupted);
		}
		r.errors.push_back(std::format("the drive refused to flush (os error {}): the writes went through write-through, but it didn't confirm they're on the medium", error));
		flushed = false;
	} else
		r.events.push_back("all of it was written (write-through) and flushed");

	if (!plan.request.verify) {
		r.verification.note = "not read back, as the plan said";
		return end(flushed ? SanitizeStatus::completed : SanitizeStatus::indeterminate);
	}
	Verification& v = r.verification;
	// a read back that ends early still counts what it found: a sector that read back wrong fails the run, however it ended
	auto stopped_early = [&](SanitizeStatus otherwise) {
		if (v.mismatches != 0) {
			r.errors.push_back(std::format("{} sectors of the {} bytes read back didn't read back as written, the first at byte {}", v.mismatches, v.bytes_checked, v.first_mismatch));
			return end(SanitizeStatus::failed);
		}
		return end(otherwise);
	};
	for (std::uint64_t pos = 0; pos < size;) {
		if (cancel && cancel->cancelled()) {
			v.note = std::format("reading back was stopped at byte {}, as asked: the write finished, what it left wasn't all checked", pos);
			return stopped_early(flushed ? SanitizeStatus::completed : SanitizeStatus::indeterminate);
		}
		const DWORD length = static_cast<DWORD>(std::min<std::uint64_t>(chunk, size - pos));
		OVERLAPPED where = at(pos);
		DWORD got = 0;
		const bool ok = ReadFile(drive.value, buffer.data, length, &got, &where) != FALSE;
		if (!ok || got != length) {
			const DWORD error = ok ? 0 : GetLastError();
			r.errors.push_back(std::format("reading back at byte {} failed (os error {})", pos, error));
			v.note = "it couldn't all be read back: whether it worked can't be told";
			return stopped_early(went_away() ? SanitizeStatus::interrupted : SanitizeStatus::indeterminate);
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
	v.done = true;
	v.note = "every sector was read back from the drive (unbuffered) and compared with what was written. that's what its controller shows: blocks it keeps aside can't be read or written from here";
	if (v.mismatches != 0) {
		r.errors.push_back(std::format("{} sectors didn't read back as written, the first at byte {}", v.mismatches, v.first_mismatch));
		return end(SanitizeStatus::failed);
	}
	return end(flushed ? SanitizeStatus::verified : SanitizeStatus::indeterminate);
}
