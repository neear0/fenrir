#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include "imaging.h"
#include "result.h"

// one copy of a disk into an image (acquire_image()) on a thread of its own, for the window: it starts it, stops it and
// draws snapshots of how it's going, it never waits for the disk
namespace storage {
	struct ImagingSnapshot {
		std::uint64_t version = 0;   // goes up with every change
		bool running = false;
		std::filesystem::path source, image, mapfile;
		ImagingStatus status;        // the last one the copy reported
		std::vector<MapCell> bar;    // the map, summarize()d
		double seconds = 0.0;        // the copy's, so far or in all
		bool done = false;           // every pass ran: the map is final (bad sectors stay bad)
		std::optional<Error> error;  // why it stopped short, Errc::cancelled when it was stopped
	};

	class ImagingSession {
	public:
		static constexpr std::size_t bar_cells = 400;

		ImagingSession();
		~ImagingSession(); // stops the copy and waits for it, with the map saved
		ImagingSession(const ImagingSession&) = delete;
		ImagingSession& operator=(const ImagingSession&) = delete;

		// starts copying source (an image file, read through its own map if it has one) into image, with its map at
		// default_map_path(image), going on from it when it's there. returns at once, false while a copy runs
		bool start(const std::filesystem::path& source, const std::filesystem::path& image, const ImagingOptions& options = {});
		void cancel(); // the copy stops at its next read and saves its map
		bool busy() const;
		void wait();

		std::shared_ptr<const ImagingSnapshot> snapshot() const; // from any thread, a copy that doesn't change

	private:
		struct t_impl;
		std::unique_ptr<t_impl> impl_;
	};
}
