// the imaging session's thread: acquire_image() with an observer that publishes snapshots, a few times a second and
// whenever the map is saved. the ui only ever copies a published snapshot

#include <algorithm>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>

#include "storage/disks.h"
#include "storage/imaging_session.h"
#include "storage/mapped_image.h"

namespace {
	using namespace storage;
	using clock_type = std::chrono::steady_clock;
	constexpr auto publish_every = std::chrono::milliseconds(100);
}

struct storage::ImagingSession::t_impl {
	mutable std::mutex mutex;
	std::shared_ptr<const ImagingSnapshot> published = std::make_shared<const ImagingSnapshot>();
	std::shared_ptr<CancellationToken> cancel;
	std::thread worker;
	bool running = false;

	void publish(const std::function<void(ImagingSnapshot&)>& change) {
		std::lock_guard lock(mutex);
		auto next = std::make_shared<ImagingSnapshot>(*published);
		change(*next);
		next->version++;
		published = std::move(next);
	}

	struct t_observer final : ImagingObserver {
		t_impl& impl;
		clock_type::time_point started = clock_type::now(), last{};
		explicit t_observer(t_impl& i) : impl(i) {}

		void show(const ImagingStatus& status, const ImageMap* map) noexcept {
			try {
				std::vector<MapCell> bar;
				if (map)
					bar = summarize(*map, bar_cells);
				const double seconds = std::chrono::duration<double>(clock_type::now() - started).count();
				impl.publish([&](ImagingSnapshot& s) {
					s.status = status;
					s.seconds = seconds;
					if (map)
						s.bar = std::move(bar);
				});
			} catch (const std::bad_alloc&) {
				// the next one, then
			}
		}
		void update(const ImagingStatus& status, const ImageMap& map) noexcept override {
			const auto now = clock_type::now();
			if (now - last < publish_every)
				return;
			last = now;
			show(status, &map);
		}
		void saved(const ImagingStatus& status) noexcept override { show(status, nullptr); }
	};

	void job(const std::filesystem::path& source_path, const std::filesystem::path& image, const std::filesystem::path& mapfile,
		const ImagingOptions& options, CancellationToken& token) {
		t_observer observer(*this);
		Result<ImagingStatus> result = fail(Errc::io_error, "imaging");
		if (Result<OpenedImage> source = open_source(source_path); !source)
			result = std::unexpected(source.error());
		else {
			ImagingOptions o = options;
			if (const auto* disk = dynamic_cast<const DiskSource*>(source->source.get())) // its own sectors, not parts of them
				o.sector_size = std::max(o.sector_size, disk->info().sector_size);
			result = acquire_image(*source->source, image, mapfile, o, &token, &observer);
		}
		// the final map, for the bar: what the copy ended with, saved
		Result<ImageMap> map = load_map(mapfile);
		const double seconds = std::chrono::duration<double>(clock_type::now() - observer.started).count();
		publish([&](ImagingSnapshot& s) {
			if (result) {
				s.status = *result;
				s.done = true;
			} else
				s.error = result.error();
			if (map) {
				s.bar = summarize(*map, bar_cells);
				s.status.size = map->size();
				s.status.finished = map->bytes(BlockStatus::finished);
				s.status.bad = map->bytes(BlockStatus::bad);
				s.status.unknown = map->unknown_bytes();
			}
			s.seconds = seconds;
			s.running = false;
		});
		std::lock_guard lock(mutex);
		running = false;
	}
};

storage::ImagingSession::ImagingSession() : impl_(std::make_unique<t_impl>()) {}

storage::ImagingSession::~ImagingSession() {
	cancel();
	if (impl_->worker.joinable())
		impl_->worker.join();
}

bool storage::ImagingSession::start(const std::filesystem::path& source, const std::filesystem::path& image, const ImagingOptions& options) {
	{
		std::lock_guard lock(impl_->mutex);
		if (impl_->running)
			return false;
	}
	if (impl_->worker.joinable())
		impl_->worker.join(); // done already, only to be reaped
	const std::filesystem::path mapfile = default_map_path(image);
	auto token = std::make_shared<CancellationToken>();
	{
		std::lock_guard lock(impl_->mutex);
		impl_->running = true;
		impl_->cancel = token;
		auto next = std::make_shared<ImagingSnapshot>();
		next->version = impl_->published->version + 1;
		next->running = true;
		next->source = source;
		next->image = image;
		next->mapfile = mapfile;
		impl_->published = std::move(next);
	}
	impl_->worker = std::thread([impl = impl_.get(), source, image, mapfile, options, token] {
		try {
			impl->job(source, image, mapfile, options, *token);
		} catch (const std::exception&) {
			impl->publish([](ImagingSnapshot& s) {
				s.error = Error{ Errc::out_of_memory, "imaging", 0 };
				s.running = false;
			});
			std::lock_guard lock(impl->mutex);
			impl->running = false;
		}
	});
	return true;
}

void storage::ImagingSession::cancel() {
	std::lock_guard lock(impl_->mutex);
	if (impl_->cancel)
		impl_->cancel->cancel();
}

bool storage::ImagingSession::busy() const {
	std::lock_guard lock(impl_->mutex);
	return impl_->running;
}

void storage::ImagingSession::wait() {
	if (impl_->worker.joinable())
		impl_->worker.join();
}

std::shared_ptr<const storage::ImagingSnapshot> storage::ImagingSession::snapshot() const {
	std::lock_guard lock(impl_->mutex);
	return impl_->published;
}
