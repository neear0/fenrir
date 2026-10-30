#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <new>
#include <string_view>
#include <utility>

#include "storage/file_image_source.h"

namespace {
	bool separator(wchar_t c) { return c == L'\\' || c == L'/'; }
	wchar_t lower(wchar_t c) { return c >= L'A' && c <= L'Z' ? wchar_t(c - L'A' + L'a') : c; }

	// \\.\ and \\?\ reach devices and volumes as well as files: \\?\C:\... and \\?\UNC\... are ordinary long paths,
	// anything else in those namespaces (and \??\, the nt one) is refused before it's ever opened
	bool device_path(std::wstring_view p) {
		if (p.size() >= 4 && separator(p[0]) && p[1] == L'?' && p[2] == L'?' && separator(p[3]))
			return true;
		if (p.size() < 4 || !separator(p[0]) || !separator(p[1]) || !separator(p[3]))
			return false;
		if (p[2] == L'.')
			return true;
		if (p[2] != L'?')
			return false;
		const std::wstring_view rest = p.substr(4);
		const bool drive = rest.size() >= 3 && ((lower(rest[0]) >= L'a' && lower(rest[0]) <= L'z') && rest[1] == L':' && separator(rest[2]));
		const bool unc = rest.size() >= 4 && lower(rest[0]) == L'u' && lower(rest[1]) == L'n' && lower(rest[2]) == L'c' && separator(rest[3]);
		return !drive && !unc;
	}

	storage::Error from_os(DWORD error, const char* context) {
		using storage::Errc;
		switch (error) {
		case ERROR_FILE_NOT_FOUND:
		case ERROR_PATH_NOT_FOUND:
		case ERROR_INVALID_NAME:
		case ERROR_BAD_NETPATH:
			return { Errc::not_found, context, error };
		case ERROR_ACCESS_DENIED:
			return { Errc::access_denied, context, error };
		case ERROR_SHARING_VIOLATION:
		case ERROR_LOCK_VIOLATION:
			return { Errc::busy, context, error };
		case ERROR_NOT_ENOUGH_MEMORY:
		case ERROR_OUTOFMEMORY:
			return { Errc::out_of_memory, context, error };
		default:
			return { Errc::io_error, context, error };
		}
	}

	struct t_handle { // closes it unless it's handed on
		HANDLE value = INVALID_HANDLE_VALUE;
		~t_handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
		HANDLE release() { return std::exchange(value, INVALID_HANDLE_VALUE); }
	};
}

storage::Result<std::unique_ptr<storage::FileImageSource>> storage::FileImageSource::open(const std::filesystem::path& path) noexcept {
	const std::wstring& native = path.native();
	if (native.empty())
		return Error{ Errc::invalid_argument, "opening an image: the path is empty" };
	if (device_path(native))
		return Error{ Errc::unsupported, "opening an image: device paths aren't opened as image files" };

	// read access only. sharing read only: nobody can open it for writing while it's open here, and if someone already has,
	// this fails with a sharing violation instead of reading an image that's changing
	t_handle handle{ CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
	if (handle.value == INVALID_HANDLE_VALUE) {
		const DWORD error = GetLastError();
		const DWORD attributes = error == ERROR_ACCESS_DENIED ? GetFileAttributesW(native.c_str()) : INVALID_FILE_ATTRIBUTES;
		if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))
			return Error{ Errc::unsupported, "opening an image: it's a directory", error };
		return from_os(error, "opening an image");
	}

	if (GetFileType(handle.value) != FILE_TYPE_DISK) // consoles, pipes and the like
		return Error{ Errc::unsupported, "opening an image: not a file" };
	BY_HANDLE_FILE_INFORMATION info{};
	if (!GetFileInformationByHandle(handle.value, &info))
		return from_os(GetLastError(), "opening an image: reading its attributes");
	if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE))
		return Error{ Errc::unsupported, "opening an image: not a regular file" };
	LARGE_INTEGER size{};
	if (!GetFileSizeEx(handle.value, &size) || size.QuadPart < 0)
		return from_os(GetLastError(), "opening an image: reading its size");

	std::unique_ptr<FileImageSource> source(new (std::nothrow) FileImageSource());
	if (!source)
		return Error{ Errc::out_of_memory, "opening an image" };
	source->size_ = static_cast<std::uint64_t>(size.QuadPart);
	source->handle_ = handle.release();
	return source;
}

storage::FileImageSource::~FileImageSource() {
	if (handle_)
		CloseHandle(handle_);
}

std::uint64_t storage::FileImageSource::size_bytes() const noexcept { return size_; }

storage::Result<std::size_t> storage::FileImageSource::read_at(std::uint64_t offset, std::span<std::byte> output) noexcept {
	if (offset > size_)
		return Error{ Errc::out_of_range, "reading the image: the offset is past its end" };
	const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(output.size(), size_ - offset));

	std::size_t done = 0;
	while (done < want) {
		const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(want - done, std::size_t(1) << 30)); // ReadFile counts in a DWORD
		const std::uint64_t at = offset + done; // <= size_, can't overflow
		OVERLAPPED position{}; // the offset to read at, the handle itself is synchronous
		position.Offset = static_cast<DWORD>(at & 0xFFFFFFFFu);
		position.OffsetHigh = static_cast<DWORD>(at >> 32);
		DWORD got = 0;
		if (!ReadFile(handle_, output.data() + done, chunk, &got, &position)) {
			const DWORD error = GetLastError();
			if (error == ERROR_HANDLE_EOF)
				return Error{ Errc::io_error, "reading the image: it's shorter than when it was opened", error };
			return from_os(error, "reading the image");
		}
		if (got == 0)
			return Error{ Errc::io_error, "reading the image: it's shorter than when it was opened" };
		done += got;
	}
	return done;
}
