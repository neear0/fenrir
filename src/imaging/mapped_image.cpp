#include <algorithm>
#include <format>
#include <new>
#include <system_error>

#include "storage/bytes.h"
#include "storage/file_image_source.h"
#include "storage/mapped_image.h"

namespace {
	using namespace storage;
	namespace fs = std::filesystem;

	std::string_view why_missing(BlockStatus status) {
		switch (status) {
		case BlockStatus::bad: return "the disk couldn't read it";
		case BlockStatus::non_tried: return "the copy hadn't got to it yet";
		default: return "the copy hadn't finished trying to read it";
		}
	}

	std::string to_utf8(const fs::path& path) {
		const std::u8string text = path.filename().u8string();
		return { reinterpret_cast<const char*>(text.data()), text.size() };
	}
}

storage::Result<std::unique_ptr<storage::MappedImageSource>> storage::MappedImageSource::wrap(std::unique_ptr<IBlockSource> image, ImageMap map) noexcept {
	if (!image)
		return fail(Errc::invalid_argument, "reading an image with its map: there's no image");
	if (image->size_bytes() != map.size())
		return fail(Errc::mismatch, "reading an image with its map: the map is of a disk with a different size");
	std::unique_ptr<MappedImageSource> source(new (std::nothrow) MappedImageSource(std::move(image), std::move(map)));
	if (!source)
		return fail(Errc::out_of_memory, "reading an image with its map");
	return source;
}

storage::Result<std::size_t> storage::MappedImageSource::read_at(std::uint64_t offset, std::span<std::byte> output) noexcept {
	const std::uint64_t size = map_.size();
	if (offset > size)
		return fail(Errc::out_of_range, "reading the image: the offset is past its end");
	if (offset == size || output.empty())
		return std::size_t(0);
	const MapBlock block = map_.block_at(offset);
	if (block.status != BlockStatus::finished)
		return fail(Errc::unreadable, "reading the image: the disk wasn't read there when it was imaged");
	if (output.size() > block.end() - offset)
		output = output.first(static_cast<std::size_t>(block.end() - offset));
	return image_->read_at(offset, output);
}

std::filesystem::path storage::default_map_path(const std::filesystem::path& image) {
	fs::path map = image;
	if (image.extension() == ".map")
		map += ".map";
	else
		map.replace_extension(".map");
	return map;
}

storage::Result<storage::OpenedImage> storage::open_image(const std::filesystem::path& image) noexcept {
	try {
		Result<std::unique_ptr<FileImageSource>> file = FileImageSource::open(image);
		if (!file)
			return std::unexpected(file.error());
		OpenedImage out;
		std::error_code error;
		for (fs::path candidate : { default_map_path(image), fs::path(image) += ".map" })
			if (fs::is_regular_file(candidate, error)) {
				out.mapfile = std::move(candidate);
				break;
			}
		if (out.mapfile.empty()) {
			out.source = std::move(*file);
			return out;
		}

		Result<ImageMap> map = load_map(out.mapfile);
		if (!map) {
			out.note = std::format("{} is next to it but couldn't be read as its map ({}), so it's read as a whole image", to_utf8(out.mapfile), to_string(map.error().code));
			out.source = std::move(*file);
			return out;
		}
		if (map->size() != (*file)->size_bytes()) {
			out.note = std::format("{} is next to it but is the map of a disk of {} bytes, not this one, so it's read as a whole image", to_utf8(out.mapfile), map->size());
			out.source = std::move(*file);
			return out;
		}
		auto shared = std::make_shared<const ImageMap>(*map);
		Result<std::unique_ptr<MappedImageSource>> mapped = MappedImageSource::wrap(std::move(*file), std::move(*map));
		if (!mapped)
			return std::unexpected(mapped.error());
		out.map = std::move(shared);
		out.source = std::move(*mapped);
		return out;
	} catch (const std::bad_alloc&) {
		return fail(Errc::out_of_memory, "opening an image");
	}
}

bool storage::mark_unread(RecoveryCandidate& candidate, const ImageMap& map, std::uint64_t volume_offset) {
	if (candidate.directory || candidate.extents.empty())
		return false;
	std::uint64_t kept = 0; // bytes of the file before the first one that wasn't copied
	for (std::size_t i = 0; i < candidate.extents.size(); i++) {
		Extent& extent = candidate.extents[i];
		if (extent.zeros) {
			kept += extent.length;
			continue;
		}
		const std::optional<std::uint64_t> start = checked_add(volume_offset, extent.offset);
		const std::optional<std::uint64_t> end = start ? checked_add(*start, extent.length) : std::nullopt;
		std::uint64_t pos = start.value_or(map.size());
		BlockStatus missing = BlockStatus::non_tried;
		bool cut = !end;
		while (!cut && pos < *end) {
			const MapBlock block = map.block_at(pos); // past the map's end: non_tried, size 0
			if (block.status != BlockStatus::finished) {
				missing = block.status;
				cut = true;
				break;
			}
			pos = block.end();
		}
		if (!cut) {
			kept += extent.length;
			continue;
		}

		extent.length = start && pos > *start ? pos - *start : 0;
		kept += extent.length;
		candidate.extents.resize(extent.length > 0 ? i + 1 : i);
		candidate.recoverable_bytes = kept;
		if (kept == 0)
			candidate.integrity = Integrity::unrecoverable;
		else if (candidate.integrity != Integrity::unrecoverable)
			candidate.integrity = Integrity::partial;
		candidate.reasons.push_back(kept == 0
			? std::format("none of it is in the disk image: {} when the disk was imaged", why_missing(missing))
			: std::format("only its first {} bytes are in the disk image: the rest of it wasn't, {} when the disk was imaged", kept, why_missing(missing)));
		return true;
	}
	return false;
}
