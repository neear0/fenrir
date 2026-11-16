#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

#include "storage/result.h"

// the files imaging writes, and the only ones: the image (created new, or one a matching map was made with) and its
// mapfile (always replaced whole, through a ".new" file next to it). one implementation per platform
namespace storage::imaging {
	class ImageFile {
	public:
		// a new file at path, size bytes long (all zeros). exists when there's something at path already
		static Result<ImageFile> create_new(const std::filesystem::path& path, std::uint64_t size) noexcept;
		// an image a copy was already writing into
		static Result<ImageFile> open_existing(const std::filesystem::path& path) noexcept;

		ImageFile(ImageFile&& other) noexcept;
		ImageFile& operator=(ImageFile&& other) noexcept;
		ImageFile(const ImageFile&) = delete;
		ImageFile& operator=(const ImageFile&) = delete;
		~ImageFile();

		std::uint64_t size() const noexcept { return size_; }
		// data at offset, all of it, inside size()
		Result<void> write_at(std::uint64_t offset, std::span<const std::byte> data) noexcept;
		Result<void> read_at(std::uint64_t offset, std::span<std::byte> data) noexcept;
		// everything written so far on the disk, not just in the os's cache
		Result<void> flush() noexcept;

	private:
		ImageFile() = default;
		std::uint64_t size_ = 0;
#ifdef _WIN32
		void* handle_ = nullptr;
#else
		int fd_ = -1;
#endif
	};

	// path + ".new", where save_text() writes before moving it over path
	std::filesystem::path pending_path(const std::filesystem::path& path);

	// text as the file at path: written to pending_path(path), flushed, then moved over path, so a crash leaves either
	// the old file or the new one, never half of one. replace false: exists when there's a file at path already.
	// exists too when pending_path(path) is there, it's never written over
	Result<void> save_text(const std::filesystem::path& path, std::string_view text, bool replace) noexcept;

	// deletes the file at path: only ever a pending_path() that save_text() left behind when it was interrupted
	Result<void> remove_pending(const std::filesystem::path& path) noexcept;
}
