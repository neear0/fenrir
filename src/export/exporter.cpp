// the only file in the engine allowed to write (tests/check_no_write_apis.cmake lets src/export/ create files and remove
// the ones it created itself, nothing else). it reads through IBlockSource and creates new files with noreplace

#include <algorithm>
#include <fstream>
#include <memory>
#include <new>

#include "storage/exporter.h"
#include "storage/file_image_source.h"

namespace {
	using namespace storage;
	namespace fs = std::filesystem;

	constexpr std::size_t max_name_bytes = 200;
	constexpr std::size_t piece = std::size_t(1) << 20;
	constexpr int max_duplicates = 1000;

	fs::path from_utf8(std::string_view text) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size())); }
	std::string to_utf8(const fs::path& path) {
		const std::u8string text = path.u8string();
		return { reinterpret_cast<const char*>(text.data()), text.size() };
	}

	// windows treats these as devices in any directory, with any extension: "nul.txt" is NUL
	bool device_name(std::string base) {
		while (!base.empty() && base.back() == ' ')
			base.pop_back();
		for (char& c : base)
			if (c >= 'a' && c <= 'z')
				c = static_cast<char>(c - 32);
		if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" || base == "CONIN$" || base == "CONOUT$")
			return true;
		if (base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT")) && base[3] >= '0' && base[3] <= '9')
			return true;
		// COM¹ COM² COM³ and the same for LPT
		return base.size() == 5 && (base.starts_with("COM") || base.starts_with("LPT"))
			&& (base.ends_with("\xC2\xB9") || base.ends_with("\xC2\xB2") || base.ends_with("\xC2\xB3"));
	}

	std::string cut_utf8(std::string text, std::size_t max) {
		if (text.size() <= max)
			return text;
		std::size_t cut = max;
		while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
			cut--;
		text.resize(cut);
		return text;
	}

	struct t_file_guard { // removes the file it created unless told it's done
		fs::path path;
		bool keep = false;
		~t_file_guard() {
			if (!keep && !path.empty()) {
				std::error_code ignored;
				fs::remove(path, ignored);
			}
		}
	};

	// a new file in dir called name, never an existing one: noreplace fails if the name's taken, then "name (2).ext" and
	// up are tried. out is left open on it and guard set to remove it, busy when every name's taken
	Result<void> create_new(std::ofstream& out, t_file_guard& guard, const fs::path& dir, const std::string& name) {
		const std::string stem = to_utf8(from_utf8(name).stem()), extension = to_utf8(from_utf8(name).extension());
		std::error_code error;
		for (int n = 1; n <= max_duplicates; n++) {
			const fs::path path = dir / from_utf8(n == 1 ? name : cut_utf8(stem, max_name_bytes) + " (" + std::to_string(n) + ")" + extension);
			out.open(path, std::ios::binary | std::ios::out | std::ios::noreplace);
			if (out.is_open()) {
				guard.path = path;
				return {};
			}
			out.clear();
			if (!fs::exists(path, error))
				return fail(Errc::io_error, "creating a file");
		}
		return fail(Errc::busy, "creating a file");
	}

	Result<ExportedFile> export_one(IBlockSource& volume, const RecoveryCandidate& candidate, const fs::path& destination, const CancellationToken* cancel, ProgressSink* progress) {
		if (destination.empty())
			return fail(Errc::invalid_argument, "exporting: no destination");
		if (is_device_path(destination))
			return fail(Errc::unsupported, "exporting: the destination is a device, not a directory");
		if (candidate.integrity == Integrity::unrecoverable && !candidate.directory)
			return fail(Errc::unsupported, "exporting: nothing of it is recoverable");

		fs::path target = destination;
		bool any = false;
		for (std::size_t from = 0; from <= candidate.path.size();) {
			const std::size_t to = std::min(candidate.path.find('/', from), candidate.path.size());
			if (to > from) {
				target /= from_utf8(safe_name(std::string_view(candidate.path).substr(from, to - from)));
				any = true;
			}
			from = to + 1;
		}
		if (!any)
			return fail(Errc::invalid_argument, "exporting: the candidate has no path");

		std::error_code error;
		if (candidate.directory) {
			fs::create_directories(target, error);
			if (error)
				return fail(Errc::io_error, "exporting: creating a directory", static_cast<std::uint32_t>(error.value()));
			return ExportedFile{ target, 0, Sha256::of({}), true };
		}

		std::uint64_t in_extents = candidate.resident.size();
		for (const Extent& extent : candidate.extents)
			in_extents += extent.length;
		if (in_extents != candidate.recoverable_bytes || (!candidate.resident.empty() && !candidate.extents.empty()))
			return fail(Errc::invalid_argument, "exporting: its extents don't add up to its recoverable bytes");

		fs::create_directories(target.parent_path(), error);
		if (error)
			return fail(Errc::io_error, "exporting: creating its directory", static_cast<std::uint32_t>(error.value()));
		const fs::space_info space = fs::space(target.parent_path(), error);
		if (!error && space.available < candidate.recoverable_bytes)
			return fail(Errc::no_space, "exporting");

		const bool partial = candidate.integrity == Integrity::partial || candidate.recoverable_bytes < candidate.size;
		t_file_guard guard; // before out, so the file's closed by the time the guard removes it
		std::ofstream out;
		if (Result<void> created = create_new(out, guard, target.parent_path(), to_utf8(target.filename()) + (partial ? ".partial" : "")); !created)
			return created.error().code == Errc::busy ? fail(Errc::busy, "exporting: every name for it is taken") : fail(Errc::io_error, "exporting: creating the file");

		std::unique_ptr<std::byte[]> buffer(new std::byte[piece]);
		Sha256 hasher;
		std::uint64_t done = 0;
		if (!candidate.resident.empty()) {
			hasher.update(candidate.resident);
			out.write(reinterpret_cast<const char*>(candidate.resident.data()), static_cast<std::streamsize>(candidate.resident.size()));
			if (!out)
				return fail(Errc::io_error, "exporting: writing the file");
			done = candidate.resident.size();
		}
		for (const Extent& extent : candidate.extents) {
			if (extent.zeros)
				std::fill_n(buffer.get(), std::min<std::uint64_t>(piece, extent.length), std::byte{ 0 });
			for (std::uint64_t offset = 0; offset < extent.length;) {
				if (cancel && cancel->cancelled())
					return fail(Errc::cancelled, "exporting");
				const std::span<std::byte> part(buffer.get(), static_cast<std::size_t>(std::min<std::uint64_t>(piece, extent.length - offset)));
				if (!extent.zeros)
					if (Result<void> read = read_exact(volume, extent.offset + offset, part); !read)
						return std::unexpected(read.error());
				hasher.update(part);
				out.write(reinterpret_cast<const char*>(part.data()), static_cast<std::streamsize>(part.size()));
				if (!out)
					return fail(Errc::io_error, "exporting: writing the file");
				offset += part.size();
				done += part.size();
				if (progress)
					progress->progress(done, candidate.recoverable_bytes);
			}
		}
		out.close();
		if (!out)
			return fail(Errc::io_error, "exporting: finishing the file");
		guard.keep = true;
		return ExportedFile{ guard.path, done, hasher.finish(), !partial };
	}
}

std::string storage::safe_name(std::string_view name) {
	std::string out;
	for (char ch : name) {
		const auto c = static_cast<unsigned char>(ch);
		const bool reserved = c < 0x20 || c == 0x7F || std::string_view("<>:\"/\\|?*").contains(ch);
		out += reserved ? '_' : ch;
	}
	while (!out.empty() && (out.back() == '.' || out.back() == ' '))
		out.pop_back(); // windows drops them, so "a." and "a" would be the same file
	if (out.empty())
		out = "_";
	if (device_name(out.substr(0, out.find('.'))))
		out = "_" + out;
	if (out.size() > max_name_bytes) {
		const std::size_t dot = out.rfind('.');
		const std::string extension = dot != std::string::npos && out.size() - dot <= 16 ? out.substr(dot) : "";
		out = cut_utf8(out.substr(0, out.size() - extension.size()), max_name_bytes - extension.size()) + extension;
	}
	return out;
}

storage::Result<storage::ExportedFile> storage::export_candidate(IBlockSource& volume, const RecoveryCandidate& candidate, const std::filesystem::path& destination,
	const CancellationToken* cancel, ProgressSink* progress) noexcept {
	try {
		return export_one(volume, candidate, destination, cancel, progress);
	} catch (const std::bad_alloc&) {
		return fail(Errc::out_of_memory, "exporting");
	} catch (const std::filesystem::filesystem_error& e) {
		return fail(Errc::io_error, "exporting", static_cast<std::uint32_t>(e.code().value()));
	}
}

storage::Result<std::filesystem::path> storage::write_report(const std::filesystem::path& destination, std::string_view name, std::string_view text) noexcept {
	try {
		if (destination.empty() || is_device_path(destination))
			return fail(Errc::invalid_argument, "writing the report: the destination isn't a directory");
		std::error_code error;
		fs::create_directories(destination, error);
		if (error)
			return fail(Errc::io_error, "writing the report: creating its directory", static_cast<std::uint32_t>(error.value()));
		t_file_guard guard;
		std::ofstream out;
		if (Result<void> created = create_new(out, guard, destination, safe_name(name)); !created)
			return fail(created.error().code, "writing the report");
		out.write(text.data(), static_cast<std::streamsize>(text.size()));
		out.close();
		if (!out)
			return fail(Errc::io_error, "writing the report");
		guard.keep = true;
		return guard.path;
	} catch (const std::bad_alloc&) {
		return fail(Errc::out_of_memory, "writing the report");
	} catch (const std::filesystem::filesystem_error& e) {
		return fail(Errc::io_error, "writing the report", static_cast<std::uint32_t>(e.code().value()));
	}
}
