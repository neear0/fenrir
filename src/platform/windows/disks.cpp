// physical drives on windows, read-only. the only DeviceIoControl calls in fenrir are here, and
// tests/check_no_write_apis.cmake keeps them to three queries that change nothing: a drive's description
// (IOCTL_STORAGE_QUERY_PROPERTY), its size and sector size (IOCTL_DISK_GET_DRIVE_GEOMETRY_EX), and which drives a
// volume is on (IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS). drives are opened with no access to query them, and with read
// access, sharing reads and writes with the os that has them mounted, to read them

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <format>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "storage/disks.h"
#include "win_disks.h"

namespace {
	using namespace storage;
	namespace fs = std::filesystem;

	constexpr std::uint32_t max_drives = 64;
	constexpr std::size_t bounce_bytes = std::size_t(1) << 20; // reads go through this, a multiple of every sector size

	std::unexpected<Error> from_os(DWORD error, const char* context) {
		switch (error) {
		case ERROR_FILE_NOT_FOUND:
		case ERROR_PATH_NOT_FOUND:
			return fail(Errc::not_found, context, error);
		case ERROR_ACCESS_DENIED:
			return fail(Errc::access_denied, context, error);
		case ERROR_SHARING_VIOLATION:
		case ERROR_LOCK_VIOLATION:
			return fail(Errc::busy, context, error);
		case ERROR_NOT_ENOUGH_MEMORY:
		case ERROR_OUTOFMEMORY:
			return fail(Errc::out_of_memory, context, error);
		default:
			return fail(Errc::io_error, context, error);
		}
	}

	struct t_handle {
		HANDLE value = INVALID_HANDLE_VALUE;
		t_handle() = default;
		explicit t_handle(HANDLE h) : value(h) {}
		t_handle(t_handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
		t_handle& operator=(t_handle&&) = delete;
		~t_handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
		bool ok() const { return value != INVALID_HANDLE_VALUE; }
	};

	std::string to_utf8(std::wstring_view text) {
		if (text.empty())
			return {};
		const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
		std::string out(static_cast<std::size_t>(std::max(size, 0)), '\0');
		WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
		return out;
	}

	std::wstring drive_path(std::uint32_t number) { return L"\\\\.\\PhysicalDrive" + std::to_wstring(number); }

	// for queries only: no read or write access at all, which needs no administrator rights
	t_handle open_for_queries(const std::wstring& path) {
		return t_handle(CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
	}

	// a string the descriptor points at: inside what was returned, ended by a 0, trimmed (drives pad them with spaces)
	std::string descriptor_string(const std::vector<BYTE>& buffer, DWORD got, DWORD offset) {
		if (offset == 0 || offset >= got)
			return {};
		std::string out;
		for (DWORD i = offset; i < got && buffer[i] != 0 && out.size() < 256; i++)
			out += static_cast<char>(buffer[i] >= 0x20 && buffer[i] < 0x7F ? buffer[i] : '?');
		const std::size_t first = out.find_first_not_of(' '), last = out.find_last_not_of(' ');
		return first == std::string::npos ? std::string() : out.substr(first, last - first + 1);
	}

	std::string bus_name(STORAGE_BUS_TYPE bus) {
		switch (bus) {
		case BusTypeUsb: return "USB";
		case BusTypeSata: return "SATA";
		case BusTypeAta: return "ATA";
		case BusTypeNvme: return "NVMe";
		case BusTypeScsi: return "SCSI";
		case BusTypeSas: return "SAS";
		case BusTypeSd: return "SD";
		case BusTypeMmc: return "MMC";
		case BusTypeRAID: return "RAID";
		case BusTypeiScsi: return "iSCSI";
		case BusType1394: return "FireWire";
		case BusTypeVirtual: return "virtual";
		case BusTypeFileBackedVirtual: return "virtual (a file)";
		case BusTypeSpaces: return "Storage Spaces";
		default: return "other";
		}
	}

	// description and size, into info. false (with info.error) when the drive can't be asked
	bool describe(HANDLE handle, DiskInfo& info) {
		STORAGE_PROPERTY_QUERY query{};
		query.PropertyId = StorageDeviceProperty;
		query.QueryType = PropertyStandardQuery;
		std::vector<BYTE> buffer(4096);
		DWORD got = 0;
		if (DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof query, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr)
			&& got >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
			const auto* d = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());
			info.vendor = descriptor_string(buffer, got, d->VendorIdOffset);
			info.model = descriptor_string(buffer, got, d->ProductIdOffset);
			info.serial = descriptor_string(buffer, got, d->SerialNumberOffset);
			info.removable = d->RemovableMedia != FALSE;
			info.bus = bus_name(d->BusType);
		} else
			info.error = std::format("its description couldn't be read (os error {})", GetLastError());

		// spinning or not: the same query, another property. a drive that doesn't say stays unknown
		STORAGE_PROPERTY_QUERY seek{};
		seek.PropertyId = StorageDeviceSeekPenaltyProperty;
		seek.QueryType = PropertyStandardQuery;
		DEVICE_SEEK_PENALTY_DESCRIPTOR penalty{};
		if (DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &seek, sizeof seek, &penalty, sizeof penalty, &got, nullptr) && got >= sizeof penalty)
			info.rotational = penalty.IncursSeekPenalty != FALSE;

		alignas(DISK_GEOMETRY_EX) BYTE geometry_buffer[512]{};
		if (!DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, geometry_buffer, sizeof geometry_buffer, &got, nullptr)
			|| got < sizeof(DISK_GEOMETRY_EX) - 1) {
			info.error = std::format("its size couldn't be read (os error {}), there may be no medium in it", GetLastError());
			return false;
		}
		const auto* g = reinterpret_cast<const DISK_GEOMETRY_EX*>(geometry_buffer);
		info.size = g->DiskSize.QuadPart > 0 ? static_cast<std::uint64_t>(g->DiskSize.QuadPart) : 0;
		info.sector_size = g->Geometry.BytesPerSector;
		return true;
	}

	// the drives a volume is on, from its \\?\Volume{...} name without the trailing backslash. nothing when that can't
	// be found out: callers fail closed on it
	std::optional<std::vector<std::uint32_t>> volume_disks(const std::wstring& volume, DWORD* why = nullptr) {
		std::vector<std::uint32_t> out;
		const t_handle handle = open_for_queries(volume);
		if (!handle.ok()) {
			if (why)
				*why = GetLastError();
			return std::nullopt;
		}
		std::vector<BYTE> buffer(sizeof(VOLUME_DISK_EXTENTS) + 15 * sizeof(DISK_EXTENT));
		DWORD got = 0;
		for (int tries = 0; tries < 2; tries++) {
			if (DeviceIoControl(handle.value, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr))
				break;
			const DWORD error = GetLastError();
			if (why)
				*why = error;
			if (error != ERROR_MORE_DATA || got < sizeof(VOLUME_DISK_EXTENTS))
				return std::nullopt;
			const auto* partial = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
			buffer.resize(sizeof(VOLUME_DISK_EXTENTS) + std::min<DWORD>(partial->NumberOfDiskExtents, 4096) * sizeof(DISK_EXTENT));
			got = 0;
		}
		if (got < sizeof(VOLUME_DISK_EXTENTS))
			return std::nullopt;
		const auto* extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
		const DWORD fits = static_cast<DWORD>((got - offsetof(VOLUME_DISK_EXTENTS, Extents)) / sizeof(DISK_EXTENT));
		for (DWORD i = 0; i < std::min(extents->NumberOfDiskExtents, fits); i++)
			if (std::ranges::find(out, extents->Extents[i].DiskNumber) == out.end())
				out.push_back(extents->Extents[i].DiskNumber);
		return out;
	}

	std::wstring without_trailing_slash(std::wstring path) {
		if (!path.empty() && path.back() == L'\\')
			path.pop_back();
		return path;
	}

	// the \\?\Volume{...}\ name of the volume a path is on
	std::optional<std::wstring> volume_of(const fs::path& path) {
		wchar_t mount[MAX_PATH + 1]{}, volume[MAX_PATH + 1]{};
		if (!GetVolumePathNameW(path.c_str(), mount, MAX_PATH))
			return std::nullopt;
		if (!GetVolumeNameForVolumeMountPointW(mount, volume, MAX_PATH))
			return std::nullopt;
		return std::wstring(volume);
	}

	// where a volume (its \\?\Volume{...}\ name) is: the drives it's on, or none for an empty CD drive or card slot
	// (nothing on any drive). unknown otherwise: callers fail closed
	enum class e_place { placed, empty, unknown };
	e_place place(const std::wstring& name, std::vector<std::uint32_t>& disks) {
		if (GetDriveTypeW(name.c_str()) == DRIVE_CDROM)
			return e_place::empty;
		DWORD why = 0;
		if (auto on = volume_disks(without_trailing_slash(name), &why)) {
			disks = std::move(*on);
			return e_place::placed;
		}
		return why == ERROR_NOT_READY || why == ERROR_NO_MEDIA_IN_DRIVE ? e_place::empty : e_place::unknown;
	}

	// says on every drive what couldn't be found out, which drive_blockers() refuses erasure on
	void unsure(std::vector<DiskInfo>& disks, const std::string& what) {
		for (DiskInfo& d : disks)
			d.error += (d.error.empty() ? "" : "; ") + what;
	}

	// every volume, and the drives each is on. which drive windows is on, or where a volume is, that can't be told is
	// written on every drive: an erase would have to guess, so it's refused
	void add_volumes(std::vector<DiskInfo>& disks) {
		std::optional<std::vector<std::uint32_t>> system;
		wchar_t windows[MAX_PATH + 1]{};
		if (GetSystemWindowsDirectoryW(windows, MAX_PATH))
			if (const auto volume = volume_of(windows))
				system = volume_disks(without_trailing_slash(*volume));
		if (!system || system->empty())
			unsure(disks, "which drive Windows is on couldn't be told");

		wchar_t name[MAX_PATH + 1]{};
		const HANDLE find = FindFirstVolumeW(name, MAX_PATH);
		if (find == INVALID_HANDLE_VALUE) {
			unsure(disks, "the volumes couldn't be listed");
		} else {
			do {
				std::wstring mounts(1024, L'\0');
				DWORD length = 0;
				std::string shown;
				if (GetVolumePathNamesForVolumeNameW(name, mounts.data(), static_cast<DWORD>(mounts.size()), &length))
					for (const wchar_t* m = mounts.c_str(); *m; m += wcslen(m) + 1)
						shown += (shown.empty() ? "" : ", ") + to_utf8(m);
				if (shown.empty())
					shown = to_utf8(name);
				std::vector<std::uint32_t> on;
				const e_place where = place(name, on);
				if (where == e_place::unknown)
					unsure(disks, "which drive volume " + shown + " is on couldn't be told");
				for (std::uint32_t disk : on)
					for (DiskInfo& d : disks)
						if (d.number == disk) {
							d.volumes.push_back(shown);
							d.spanned_volume |= on.size() > 1;
						}
			} while (FindNextVolumeW(find, name, MAX_PATH));
			FindVolumeClose(find);
		}

		for (DiskInfo& d : disks)
			d.system = system && std::ranges::find(*system, d.number) != system->end();
	}
}

std::optional<std::uint32_t> storage::physical_drive_number(const std::filesystem::path& path) noexcept {
	const std::wstring& p = path.native();
	constexpr std::wstring_view prefix = L"\\\\.\\physicaldrive";
	if (p.size() <= prefix.size() || p.size() > prefix.size() + 3)
		return std::nullopt;
	for (std::size_t i = 0; i < prefix.size(); i++) {
		wchar_t c = p[i] == L'/' ? L'\\' : p[i];
		if (c >= L'A' && c <= L'Z')
			c = static_cast<wchar_t>(c - L'A' + L'a');
		if (c != prefix[i])
			return std::nullopt;
	}
	std::uint32_t number = 0;
	for (std::size_t i = prefix.size(); i < p.size(); i++) {
		if (p[i] < L'0' || p[i] > L'9')
			return std::nullopt;
		number = number * 10 + static_cast<std::uint32_t>(p[i] - L'0');
	}
	return number;
}

storage::Result<std::vector<storage::DiskInfo>> storage::list_disks() noexcept {
	try {
		std::vector<DiskInfo> disks;
		for (std::uint32_t n = 0; n < max_drives; n++) {
			const std::wstring path = drive_path(n);
			const t_handle handle = open_for_queries(path);
			if (!handle.ok())
				continue; // no drive with that number
			DiskInfo info;
			info.number = n;
			info.path = path;
			describe(handle.value, info);
			disks.push_back(std::move(info));
		}
		add_volumes(disks);
		return disks;
	} catch (const std::bad_alloc&) {
		return fail(Errc::out_of_memory, "listing the drives");
	}
}

struct storage::DiskSource::t_impl {
	HANDLE handle = INVALID_HANDLE_VALUE;
	std::byte* bounce = nullptr;
	std::mutex mutex; // the bounce buffer is shared
	~t_impl() {
		if (handle != INVALID_HANDLE_VALUE)
			CloseHandle(handle);
		if (bounce)
			VirtualFree(bounce, 0, MEM_RELEASE);
	}
};

storage::Result<std::unique_ptr<storage::DiskSource>> storage::DiskSource::open(std::uint32_t number, const DiskInfo* expected) noexcept {
	try {
		std::unique_ptr<DiskSource> source(new DiskSource());
		source->impl_ = std::make_unique<t_impl>();
		DiskInfo& info = source->info_;
		info.number = number;
		info.path = drive_path(number);

		// read access only. the os keeps it mounted and writing to it, so writes are shared: what's read is the drive as
		// it is at that moment, which is why a drive that's in use is better imaged than scanned
		t_impl& impl = *source->impl_;
		impl.handle = CreateFileW(info.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (impl.handle == INVALID_HANDLE_VALUE)
			return from_os(GetLastError(), "opening the drive for reading");
		if (!describe(impl.handle, info))
			return fail(Errc::io_error, "opening the drive: its size couldn't be read");
		if (info.size == 0 || info.sector_size == 0 || (info.sector_size & (info.sector_size - 1)) != 0 || info.sector_size > 65536 || info.size % info.sector_size != 0)
			return fail(Errc::unsupported, "opening the drive: its size or sector size isn't usable");
		if (expected && (expected->size != info.size || expected->serial != info.serial || expected->model != info.model))
			return fail(Errc::mismatch, "opening the drive: it isn't the drive that was listed under that number");

		impl.bounce = static_cast<std::byte*>(VirtualAlloc(nullptr, bounce_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)); // page aligned
		if (!impl.bounce)
			return fail(Errc::out_of_memory, "opening the drive");
		return source;
	} catch (const std::bad_alloc&) {
		return fail(Errc::out_of_memory, "opening the drive");
	}
}

storage::DiskSource::~DiskSource() = default;

storage::Result<std::size_t> storage::DiskSource::read_at(std::uint64_t offset, std::span<std::byte> output) noexcept {
	const std::uint64_t size = info_.size, sector = info_.sector_size;
	if (offset > size)
		return fail(Errc::out_of_range, "reading the drive: the offset is past its end");
	const std::uint64_t want = std::min<std::uint64_t>(output.size(), size - offset);
	if (want == 0)
		return std::size_t(0);

	// whole sectors around what's asked for, at most the bounce buffer at a time
	const std::uint64_t start = offset - offset % sector;
	const std::uint64_t end = std::min<std::uint64_t>((offset + want + sector - 1) / sector * sector, size);
	const auto length = static_cast<DWORD>(std::min<std::uint64_t>(end - start, bounce_bytes));

	std::lock_guard lock(impl_->mutex);
	OVERLAPPED position{};
	position.Offset = static_cast<DWORD>(start & 0xFFFFFFFFu);
	position.OffsetHigh = static_cast<DWORD>(start >> 32);
	DWORD got = 0;
	if (!ReadFile(impl_->handle, impl_->bounce, length, &got, &position))
		return from_os(GetLastError(), "reading the drive");
	const std::uint64_t skip = offset - start;
	if (got <= skip)
		return fail(Errc::io_error, "reading the drive: it returned less than was asked for");
	const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(want, got - skip));
	std::copy_n(impl_->bounce + skip, count, output.data());
	return count;
}

storage::Result<std::vector<std::uint32_t>> storage::disks_of(const std::filesystem::path& path) noexcept {
	try {
		std::error_code error;
		fs::path existing = fs::absolute(path, error);
		if (error)
			return fail(Errc::unsupported, "finding which drive a path is on");
		while (!fs::exists(existing, error) && existing.has_relative_path())
			existing = existing.parent_path();
		// where it really is: a junction or link to another drive is followed (GetVolumePathNameW doesn't)
		existing = fs::canonical(existing, error);
		if (error)
			return fail(Errc::unsupported, "finding which drive a path is on: where it leads couldn't be resolved", static_cast<std::uint32_t>(error.value()));

		wchar_t mount[MAX_PATH + 1]{};
		if (!GetVolumePathNameW(existing.c_str(), mount, MAX_PATH))
			return fail(Errc::unsupported, "finding which drive a path is on", GetLastError());
		if (GetDriveTypeW(mount) == DRIVE_REMOTE)
			return std::vector<std::uint32_t>{}; // a network share isn't on any drive here
		const auto volume = volume_of(existing);
		if (!volume)
			return fail(Errc::unsupported, "finding which drive a path is on", GetLastError());
		const std::vector<std::uint32_t> disks = volume_disks(without_trailing_slash(*volume)).value_or(std::vector<std::uint32_t>{});
		if (disks.empty())
			return fail(Errc::unsupported, "finding which drive a path is on: its volume didn't say");
		return disks;
	} catch (const std::bad_alloc&) {
		return fail(Errc::out_of_memory, "finding which drive a path is on");
	}
}

storage::Result<void> storage::check_destination(const std::filesystem::path& destination, std::uint32_t source_disk) noexcept {
	Result<std::vector<std::uint32_t>> disks = disks_of(destination);
	if (!disks)
		return fail(Errc::unsupported, "checking the destination: which drive it's on can't be told, so it isn't used while reading a drive");
	if (std::ranges::find(*disks, source_disk) != disks->end())
		return fail(Errc::invalid_argument, "checking the destination: it's on the drive being read, where writing could overwrite what's being recovered");
	return {};
}

bool storage::win::describe_disk(void* handle, DiskInfo& info) { return describe(static_cast<HANDLE>(handle), info); }

storage::Result<std::vector<std::wstring>> storage::win::volumes_on(std::uint32_t disk) {
	std::vector<std::wstring> out;
	wchar_t name[MAX_PATH + 1]{};
	const HANDLE find = FindFirstVolumeW(name, MAX_PATH);
	if (find == INVALID_HANDLE_VALUE)
		return fail(Errc::io_error, "finding the volumes on the drive: they couldn't be listed", GetLastError());
	Result<std::vector<std::wstring>> result = out;
	do {
		std::vector<std::uint32_t> disks;
		const e_place where = place(name, disks);
		if (where == e_place::unknown) {
			result = fail(Errc::io_error, "finding the volumes on the drive: which drive one of them is on couldn't be told");
			break;
		}
		if (std::ranges::find(disks, disk) == disks.end())
			continue;
		if (disks.size() > 1) {
			result = fail(Errc::unsupported, "finding the volumes on the drive: one of them is also on another drive");
			break;
		}
		result->push_back(without_trailing_slash(name));
	} while (FindNextVolumeW(find, name, MAX_PATH));
	FindVolumeClose(find);
	return result;
}
