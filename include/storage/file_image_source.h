#pragma once

#include <filesystem>
#include <memory>

#include "block_source.h"

namespace storage {
	// a raw disk image file (.img, .dd, .bin) opened for reading only. there is no handle with write access anywhere in it.
	// windows: other programs can't open the file for writing while it's open here, and one that already has it open
	// that way makes open() fail with busy, so the image can't change under a scan. posix has no such lock, a file that
	// shrinks while it's read gives io_error.
	// device paths (\\.\PhysicalDrive0, \\?\Volume{...}) are refused with unsupported, those are for a separate device source
	class FileImageSource final : public IBlockSource {
	public:
		static Result<std::unique_ptr<FileImageSource>> open(const std::filesystem::path& path) noexcept;
		~FileImageSource() override;

		std::uint64_t size_bytes() const noexcept override;
		// safe to call from several threads, on windows the reads are serialized on the handle
		Result<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> output) noexcept override;

	private:
		FileImageSource() = default;

		std::uint64_t size_ = 0;
#ifdef _WIN32
		void* handle_ = nullptr; // HANDLE, kept out of the header so <Windows.h> stays out of it
#else
		int fd_ = -1;
#endif
	};
}
