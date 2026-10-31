#include <format>
#include <new>

#include "storage/exfat.h"
#include "storage/fat32.h"
#include "storage/filesystem.h"

namespace {
	using namespace storage;

	template <class Scan>
	VolumeScan from(Scan&& scan, FileSystem filesystem, std::string summary) {
		return { filesystem, std::move(summary), std::move(scan.candidates), std::move(scan.diagnostics) };
	}

	std::string with_label(std::string summary, const std::string& label) {
		return label.empty() ? summary : summary + ", label " + label;
	}
}

bool storage::can_scan(FileSystem filesystem) noexcept {
	return filesystem == FileSystem::fat32 || filesystem == FileSystem::exfat;
}

storage::Result<storage::VolumeScan> storage::scan_volume(IBlockSource& volume, FileSystem filesystem, const CancellationToken* cancel) noexcept {
	try {
		switch (filesystem) {
		case FileSystem::fat32: {
			auto scanned = scan_fat32(volume, cancel);
			if (!scanned)
				return std::unexpected(scanned.error());
			const Fat32Volume& v = scanned->volume;
			return from(std::move(*scanned), filesystem, with_label(std::format("FAT32, {} byte clusters, {} clusters ({} free)", v.cluster_size, v.cluster_count, v.free_clusters), v.label));
		}
		case FileSystem::exfat: {
			auto scanned = scan_exfat(volume, cancel);
			if (!scanned)
				return std::unexpected(scanned.error());
			const ExfatVolume& v = scanned->volume;
			return from(std::move(*scanned), filesystem, with_label(std::format("exFAT, {} byte clusters, {} clusters ({} free)", v.cluster_size, v.cluster_count, v.free_clusters), v.label));
		}
		default:
			return fail(Errc::unsupported, "scanning a volume: fenrir can't read this filesystem's records yet");
		}
	} catch (const std::bad_alloc&) {
		return fail(Errc::out_of_memory, "scanning a volume");
	}
}
