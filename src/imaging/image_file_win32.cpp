// imaging's files on windows. tests/check_no_write_apis.cmake lets src/imaging/ write, and only here: a new image file,
// an image a matching map was made with, and the map itself replaced whole

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <utility>

#include "image_file.h"
#include "storage/file_image_source.h"

namespace {
	using namespace storage;

	std::unexpected<Error> from_os(DWORD error, const char* context) {
		switch (error) {
		case ERROR_FILE_NOT_FOUND:
		case ERROR_PATH_NOT_FOUND:
		case ERROR_INVALID_NAME:
		case ERROR_BAD_NETPATH:
			return fail(Errc::not_found, context, error);
		case ERROR_FILE_EXISTS:
		case ERROR_ALREADY_EXISTS:
			return fail(Errc::exists, context, error);
		case ERROR_ACCESS_DENIED:
			return fail(Errc::access_denied, context, error);
		case ERROR_SHARING_VIOLATION:
		case ERROR_LOCK_VIOLATION:
			return fail(Errc::busy, context, error);
		case ERROR_DISK_FULL:
		case ERROR_HANDLE_DISK_FULL:
			return fail(Errc::no_space, context, error);
		case ERROR_NOT_ENOUGH_MEMORY:
		case ERROR_OUTOFMEMORY:
			return fail(Errc::out_of_memory, context, error);
		default:
			return fail(Errc::io_error, context, error);
		}
	}

	struct t_handle {
		HANDLE value = INVALID_HANDLE_VALUE;
		~t_handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
		HANDLE release() { return std::exchange(value, INVALID_HANDLE_VALUE); }
	};

	OVERLAPPED at(std::uint64_t offset) {
		OVERLAPPED position{};
		position.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
		position.OffsetHigh = static_cast<DWORD>(offset >> 32);
		return position;
	}

	Result<void> write_all(HANDLE handle, std::uint64_t offset, std::span<const std::byte> data, const char* context) {
		std::size_t done = 0;
		while (done < data.size()) {
			const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, std::size_t(1) << 30));
			OVERLAPPED position = at(offset + done);
			DWORD wrote = 0;
			if (!WriteFile(handle, data.data() + done, chunk, &wrote, &position))
				return from_os(GetLastError(), context);
			if (wrote == 0)
				return fail(Errc::io_error, context);
			done += wrote;
		}
		return {};
	}

	// a regular file on a disk, not a directory, device, pipe or console
	Result<std::uint64_t> check_file(HANDLE handle, const char* context) {
		if (GetFileType(handle) != FILE_TYPE_DISK)
			return fail(Errc::unsupported, context);
		BY_HANDLE_FILE_INFORMATION info{};
		if (!GetFileInformationByHandle(handle, &info))
			return from_os(GetLastError(), context);
		if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE))
			return fail(Errc::unsupported, context);
		LARGE_INTEGER size{};
		if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0)
			return from_os(GetLastError(), context);
		return static_cast<std::uint64_t>(size.QuadPart);
	}
}

storage::Result<storage::imaging::ImageFile> storage::imaging::ImageFile::create_new(const std::filesystem::path& path, std::uint64_t size) noexcept {
	if (path.empty() || is_device_path(path))
		return fail(Errc::unsupported, "creating the image: not a file path");
	if (size > static_cast<std::uint64_t>(INT64_MAX))
		return fail(Errc::overflow, "creating the image");

	// CREATE_NEW: never a file that's already there. others can read it while it's written, nobody else can write
	t_handle handle{ CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr) };
	if (handle.value == INVALID_HANDLE_VALUE)
		return from_os(GetLastError(), "creating the image");

	FILE_END_OF_FILE_INFO end{};
	end.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
	if (!SetFileInformationByHandle(handle.value, FileEndOfFileInfo, &end, sizeof end)) {
		const DWORD error = GetLastError();
		CloseHandle(handle.release());
		DeleteFileW(path.c_str()); // the empty file just created, nothing else
		return from_os(error, "creating the image: making it the source's size");
	}

	ImageFile file;
	file.size_ = size;
	file.handle_ = handle.release();
	return file;
}

storage::Result<storage::imaging::ImageFile> storage::imaging::ImageFile::open_existing(const std::filesystem::path& path) noexcept {
	if (path.empty() || is_device_path(path))
		return fail(Errc::unsupported, "opening the image: not a file path");
	t_handle handle{ CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
	if (handle.value == INVALID_HANDLE_VALUE)
		return from_os(GetLastError(), "opening the image");
	Result<std::uint64_t> size = check_file(handle.value, "opening the image");
	if (!size)
		return std::unexpected(size.error());
	ImageFile file;
	file.size_ = *size;
	file.handle_ = handle.release();
	return file;
}

storage::imaging::ImageFile::ImageFile(ImageFile&& other) noexcept
	: size_(other.size_), handle_(std::exchange(other.handle_, nullptr)) {}

storage::imaging::ImageFile& storage::imaging::ImageFile::operator=(ImageFile&& other) noexcept {
	if (this != &other) {
		if (handle_)
			CloseHandle(handle_);
		size_ = other.size_;
		handle_ = std::exchange(other.handle_, nullptr);
	}
	return *this;
}

storage::imaging::ImageFile::~ImageFile() {
	if (handle_)
		CloseHandle(handle_);
}

storage::Result<void> storage::imaging::ImageFile::write_at(std::uint64_t offset, std::span<const std::byte> data) noexcept {
	if (offset > size_ || data.size() > size_ - offset)
		return fail(Errc::out_of_range, "writing the image: past its end");
	return write_all(handle_, offset, data, "writing the image");
}

storage::Result<void> storage::imaging::ImageFile::read_at(std::uint64_t offset, std::span<std::byte> data) noexcept {
	if (offset > size_ || data.size() > size_ - offset)
		return fail(Errc::out_of_range, "reading the image: past its end");
	std::size_t done = 0;
	while (done < data.size()) {
		const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, std::size_t(1) << 30));
		OVERLAPPED position = at(offset + done);
		DWORD got = 0;
		if (!ReadFile(handle_, data.data() + done, chunk, &got, &position))
			return from_os(GetLastError(), "reading the image");
		if (got == 0)
			return fail(Errc::io_error, "reading the image: it's shorter than it was");
		done += got;
	}
	return {};
}

storage::Result<void> storage::imaging::ImageFile::flush() noexcept {
	if (!FlushFileBuffers(handle_))
		return from_os(GetLastError(), "flushing the image");
	return {};
}

std::filesystem::path storage::imaging::pending_path(const std::filesystem::path& path) {
	std::filesystem::path pending = path;
	pending += L".new";
	return pending;
}

storage::Result<void> storage::imaging::save_text(const std::filesystem::path& path, std::string_view text, bool replace) noexcept {
	try {
		if (path.empty() || is_device_path(path))
			return fail(Errc::unsupported, "saving the map: not a file path");
		const std::filesystem::path pending = pending_path(path);
		{
			t_handle handle{ CreateFileW(pending.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr) };
			if (handle.value == INVALID_HANDLE_VALUE)
				return from_os(GetLastError(), "saving the map");
			Result<void> wrote = write_all(handle.value, 0, std::as_bytes(std::span(text)), "saving the map");
			if (wrote && !FlushFileBuffers(handle.value))
				wrote = from_os(GetLastError(), "saving the map: flushing it");
			if (!wrote) {
				CloseHandle(handle.release());
				DeleteFileW(pending.c_str());
				return wrote;
			}
		}
		// replace: only ever over the map the copy is going on from. MOVEFILE_WRITE_THROUGH returns once the move is on disk
		const DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
		if (!MoveFileExW(pending.c_str(), path.c_str(), flags)) {
			const DWORD error = GetLastError();
			DeleteFileW(pending.c_str());
			return from_os(error, "saving the map");
		}
		return {};
	} catch (const std::bad_alloc&) {
		return fail(Errc::out_of_memory, "saving the map");
	}
}

storage::Result<void> storage::imaging::remove_pending(const std::filesystem::path& path) noexcept {
	if (!DeleteFileW(path.c_str()))
		return from_os(GetLastError(), "removing an unfinished map");
	return {};
}
