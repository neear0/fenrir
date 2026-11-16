#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "block_source.h"
#include "imaging.h"
#include "recovery_candidate.h"

// an image made by acquire_image(), read together with its map. the parts of the image the map doesn't count as copied
// are zeros only because the file was made that size, so they aren't handed out as data: a read that reaches one fails
// with Errc::unreadable. every scanner already treats a failed read as damage, so nothing it finds there passes for
// real, and found files that run into those parts are cut short where they do
namespace storage {
	class MappedImageSource final : public IBlockSource {
	public:
		// image and the map it was made with, mismatch when they aren't the same size
		static Result<std::unique_ptr<MappedImageSource>> wrap(std::unique_ptr<IBlockSource> image, ImageMap map) noexcept;

		const ImageMap& map() const noexcept { return map_; }

		std::uint64_t size_bytes() const noexcept override { return image_->size_bytes(); }
		// a read that starts on bytes that were copied is cut where they end (a short read), one that starts on bytes
		// that weren't fails with unreadable. thread safe if the image is
		Result<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> output) noexcept override;

	private:
		MappedImageSource(std::unique_ptr<IBlockSource> image, ImageMap map) noexcept : image_(std::move(image)), map_(std::move(map)) {}

		std::unique_ptr<IBlockSource> image_;
		ImageMap map_;
	};

	// where an image's map is looked for: "disk.img" has "disk.map" (what fenrir-cli image makes, and what ddrescue's
	// users mostly call it), then "disk.img.map"
	std::filesystem::path default_map_path(const std::filesystem::path& image);

	struct OpenedImage {
		std::unique_ptr<IBlockSource> source; // the image, read-only, through its map when it has one
		std::shared_ptr<const ImageMap> map;  // null without one
		std::filesystem::path mapfile;        // the map it's read with, or one next to it that wasn't used
		std::string note;                     // empty, or why a map next to it wasn't used
	};

	// a raw image (FileImageSource), and its map when there's one next to it. a map that isn't one, or isn't of an
	// image this size, is left out and said so in note: the image is read whole then, as any other image is
	Result<OpenedImage> open_image(const std::filesystem::path& image) noexcept;

	// a path from the user: \\.\PhysicalDriveN opens that drive read-only (DiskSource, administrator rights needed),
	// anything else is open_image()
	Result<OpenedImage> open_source(const std::filesystem::path& path) noexcept;

	// a found file whose data runs into what map doesn't count as copied: cut at the first byte that wasn't, partial
	// (unrecoverable when it's none of it), with a reason saying so. volume_offset is where the file's volume starts in
	// the image. false when it's all there
	bool mark_unread(RecoveryCandidate& candidate, const ImageMap& map, std::uint64_t volume_offset);
}
