// the session's threads: one worker for scans and exports, one for previews. the worker builds the next state on its
// own and publishes it under the mutex, the snapshot readers only ever copy what was published. found files are kept
// in an immutable vector behind a shared_ptr, a step that finds more publishes a new one

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <format>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_set>

#include "storage/carving.h"
#include "storage/exporter.h"
#include "storage/filesystem.h"
#include "storage/file_image_source.h"
#include "storage/session.h"

namespace {
	using namespace storage;
	using clock_type = std::chrono::steady_clock;

	constexpr std::uint64_t min_slice = std::uint64_t(64) << 20; // the deep scan publishes what it found after each
	constexpr std::uint64_t slices = 200;                        // and makes at most about this many of them
	constexpr std::size_t max_diagnostics = 500;
	constexpr std::size_t preview_bytes = 256;

	// what the jobs read: the image, a view of each partition, and which of those each volume is
	struct t_sources {
		std::unique_ptr<FileImageSource> image;
		std::vector<std::unique_ptr<PartitionView>> views;
		std::vector<IBlockSource*> volumes;
	};

	std::string to_utf8(const std::filesystem::path& path) {
		const std::u8string text = path.u8string();
		return { reinterpret_cast<const char*>(text.data()), text.size() };
	}

	std::string error_text(const Error& error) { return std::format("{}: {}", error.context, to_string(error.code)); }
}

std::string_view storage::to_string(SessionPhase phase) noexcept {
	switch (phase) {
	case SessionPhase::idle: return "idle";
	case SessionPhase::opening: return "opening";
	case SessionPhase::filesystems: return "reading filesystems";
	case SessionPhase::carving: return "deep scan";
	case SessionPhase::exporting: return "exporting";
	}
	return "?";
}

struct storage::RecoverySession::t_impl {
	mutable std::mutex mutex;
	SessionSnapshot state;                      // what's published, without the progress below
	std::shared_ptr<t_sources> sources;         // replaced by a scan, kept alive by whoever's reading it
	ScanOptions options;
	std::shared_ptr<CancellationToken> cancel;  // the running job's
	bool running = false;
	clock_type::time_point started;
	double last_seconds = 0.0;
	std::atomic<std::uint64_t> done{ 0 }, total{ 0 };
	std::thread worker;

	std::condition_variable wake;
	std::optional<std::uint64_t> wanted;
	FilePreview preview;
	bool stopping = false;
	std::thread previewer; // last, it uses everything above

	// progress from a step that counts from its own zero, onto the job's
	class t_progress final : public ProgressSink {
	public:
		t_progress(t_impl& impl, std::uint64_t base) : impl_(impl), base_(base) {}
		void progress(std::uint64_t done, std::uint64_t) noexcept override { impl_.done.store(base_ + done, std::memory_order_relaxed); }

	private:
		t_impl& impl_;
		std::uint64_t base_;
	};

	t_impl() {
		state.files = std::make_shared<const std::vector<FoundFile>>();
		state.exported = std::make_shared<const std::vector<ExportRecord>>();
		previewer = std::thread([this] { preview_loop(); });
	}

	~t_impl() {
		{
			std::lock_guard lock(mutex);
			if (cancel)
				cancel->cancel();
			stopping = true;
		}
		wake.notify_all();
		if (worker.joinable())
			worker.join();
		previewer.join();
	}

	void publish(const std::function<void(SessionSnapshot&)>& change) {
		std::lock_guard lock(mutex);
		change(state);
		state.version++;
	}

	void note(SessionSnapshot& s, Diagnostic::Severity severity, std::string message) {
		if (s.diagnostics.size() < max_diagnostics)
			s.diagnostics.push_back({ severity, std::move(message) });
	}

	bool start(std::function<void(CancellationToken&)> job) {
		std::lock_guard lock(mutex);
		if (running)
			return false;
		if (worker.joinable())
			worker.join(); // done already, it only has to be reaped
		running = true;
		cancel = std::make_shared<CancellationToken>();
		started = clock_type::now();
		done = 0;
		total = 0;
		state.error.reset();
		state.version++;
		worker = std::thread([this, token = cancel, job = std::move(job)] {
			try {
				job(*token);
			} catch (const std::exception&) { // out of memory, mostly
				publish([](SessionSnapshot& s) {
					s.error = Error{ Errc::out_of_memory, "the session", 0 };
					s.status = "stopped: it ran out of memory";
				});
			}
			std::lock_guard lock(mutex);
			last_seconds = std::chrono::duration<double>(clock_type::now() - started).count();
			state.phase = SessionPhase::idle;
			state.version++;
			running = false; // the last thing it does under the lock, so start() can join it
		});
		return true;
	}

	void stopped(const Error& error, std::string status) {
		publish([&](SessionSnapshot& s) {
			s.error = error;
			s.status = std::move(status);
		});
	}

	void scan_job(const std::filesystem::path& path, CancellationToken& token) {
		publish([&](SessionSnapshot& s) {
			const std::uint64_t version = s.version;
			s = SessionSnapshot{};
			s.version = version;
			s.files = std::make_shared<const std::vector<FoundFile>>();
			s.exported = std::make_shared<const std::vector<ExportRecord>>();
			s.phase = SessionPhase::opening;
			s.image = path;
			s.carving = options.carve;
			s.status = "opening the image";
		});
		{
			std::lock_guard lock(mutex);
			sources.reset();
		}

		auto sources_now = std::make_shared<t_sources>();
		auto image = FileImageSource::open(path);
		if (!image)
			return stopped(image.error(), "it couldn't be opened: " + error_text(image.error()));
		sources_now->image = std::move(*image);
		auto map = scan_partitions(*sources_now->image);
		if (!map)
			return stopped(map.error(), "its partition table couldn't be read: " + error_text(map.error()));

		std::vector<SessionVolume> volumes;
		volumes.push_back({ "the whole image", 0, sources_now->image->size_bytes(), map->partitions.empty() ? map->whole_disk : FileSystem::unknown });
		sources_now->volumes.push_back(sources_now->image.get());
		std::vector<Diagnostic> notes;
		for (const Diagnostic& d : map->diagnostics)
			notes.push_back({ d.severity, "partition table: " + d.message });
		for (const Partition& p : map->partitions) {
			if (!p.inside_disk)
				continue;
			auto view = open_partition(*sources_now->image, p);
			if (!view) {
				notes.push_back({ Diagnostic::Severity::warning, std::format("partition {}: couldn't be opened: {}", p.number, error_text(view.error())) });
				continue;
			}
			volumes.push_back({ std::format("partition {}", p.number), p.offset_bytes, p.size_bytes, p.filesystem });
			sources_now->volumes.push_back(view->get());
			sources_now->views.push_back(std::move(*view));
		}
		{
			std::lock_guard lock(mutex);
			sources = sources_now;
			state.open = true;
			state.image_size = sources_now->image->size_bytes();
			state.partitions = std::move(*map);
			state.volumes = volumes;
			state.diagnostics = std::move(notes);
			state.version++;
		}

		// the filesystems: every volume fenrir can read the records of, live and deleted files
		std::vector<std::uint32_t> readable;
		for (std::uint32_t v = 0; v < volumes.size(); v++)
			if (can_scan(volumes[v].filesystem))
				readable.push_back(v);
		total = readable.size();
		for (std::size_t i = 0; i < readable.size(); i++) {
			const std::uint32_t v = readable[i];
			const std::string_view kind = to_string(volumes[v].filesystem);
			publish([&](SessionSnapshot& s) {
				s.phase = SessionPhase::filesystems;
				s.status = std::format("reading the {} records on {}", kind, volumes[v].name);
			});
			auto scanned = scan_volume(*sources_now->volumes[v], volumes[v].filesystem, &token);
			done = i + 1;
			if (!scanned && scanned.error().code == Errc::cancelled)
				return stopped(scanned.error(), std::format("stopped while reading {}", volumes[v].name));
			publish([&](SessionSnapshot& s) {
				if (!scanned) {
					note(s, Diagnostic::Severity::error, std::format("{}: couldn't be read as {}: {}", volumes[v].name, kind, error_text(scanned.error())));
					return;
				}
				for (const Diagnostic& d : scanned->diagnostics)
					note(s, d.severity, std::format("{}: {}", volumes[v].name, d.message));
				auto files = std::make_shared<std::vector<FoundFile>>(*s.files);
				const std::uint64_t base = files->size();
				for (RecoveryCandidate& c : scanned->candidates) {
					c.id += base;
					if (c.parent != RecoveryCandidate::no_parent)
						c.parent += base;
					files->push_back({ std::move(c), v });
				}
				s.volumes[v].scanned = true;
				s.volumes[v].files = files->size() - base;
				s.files = std::move(files);
			});
		}

		if (options.carve)
			carve_job(0, token);
		else
			finished();
	}

	// the deep scan from byte from, a slice at a time. files a filesystem found already are left out
	void carve_job(std::uint64_t from, CancellationToken& token) {
		std::shared_ptr<t_sources> src;
		std::shared_ptr<const std::vector<FoundFile>> known_files;
		std::vector<SessionVolume> volumes;
		{
			std::lock_guard lock(mutex);
			src = sources;
			known_files = state.files;
			volumes = state.volumes;
			state.phase = SessionPhase::carving;
			state.carving = true;
			state.can_resume = false;
			state.version++;
		}
		if (!src)
			return stopped(Error{ Errc::invalid_argument, "the deep scan", 0 }, "there's no image open");
		std::unordered_set<std::uint64_t> known;
		for (const FoundFile& f : *known_files)
			if (f.file.origin == Origin::filesystem && !f.file.directory && !f.file.extents.empty() && f.file.extents.front().length != 0 && !f.file.extents.front().zeros)
				known.insert(volumes[f.volume].offset + f.file.extents.front().offset);

		const std::uint64_t size = src->image->size_bytes();
		const std::uint64_t slice = std::max(min_slice, size / slices);
		total = size;
		for (std::uint64_t pos = from; pos < size;) {
			done = pos;
			publish([&](SessionSnapshot& s) { s.status = std::format("deep scan: looking for files by their content, {}% of the image", pos * 100 / size); });
			t_progress progress(*this, pos);
			auto carved = carve(*src->image, { .alignment = options.alignment, .from = pos, .to = std::min(size, pos + slice) }, &token, &progress);
			if (!carved) {
				const bool cancelled = carved.error().code == Errc::cancelled;
				publish([&](SessionSnapshot& s) {
					s.carved_to = pos;
					s.can_resume = cancelled;
					s.error = carved.error();
					s.status = cancelled ? std::format("deep scan stopped at {}% of the image, it can go on from there", pos * 100 / size)
						: "the deep scan failed: " + error_text(carved.error());
				});
				return;
			}
			publish([&](SessionSnapshot& s) {
				for (const Diagnostic& d : carved->diagnostics)
					note(s, d.severity, "deep scan: " + d.message);
				s.carve_rejected += carved->rejected;
				auto files = std::make_shared<std::vector<FoundFile>>(*s.files);
				for (RecoveryCandidate& c : carved->candidates) {
					if (known.contains(c.extents.front().offset)) {
						s.carve_dropped++;
						continue;
					}
					c.id = files->size();
					files->push_back({ std::move(c), 0 });
					s.volumes[0].files++;
				}
				s.files = std::move(files);
				s.carved_to = std::min(carved->next, size);
			});
			pos = carved->next;
		}
		done = size;
		finished();
	}

	void finished() {
		publish([&](SessionSnapshot& s) {
			std::uint64_t deleted = 0, carved = 0;
			for (const FoundFile& f : *s.files) {
				deleted += f.file.deleted;
				carved += f.file.origin == Origin::carved;
			}
			s.can_resume = false;
			s.status = std::format("done: {} found, {} of them deleted and {} carved", s.files->size(), deleted, carved);
		});
	}

	void export_job(const std::vector<std::uint64_t>& ids, const std::filesystem::path& destination, CancellationToken& token) {
		std::shared_ptr<t_sources> src;
		std::shared_ptr<const std::vector<FoundFile>> files;
		SessionSnapshot about;
		{
			std::lock_guard lock(mutex);
			src = sources;
			files = state.files;
			about.image = state.image;
			about.image_size = state.image_size;
			about.volumes = state.volumes;
			state.phase = SessionPhase::exporting;
			state.destination = destination;
			state.report.clear();
			state.exported = std::make_shared<const std::vector<ExportRecord>>();
			state.status = "exporting";
			state.version++;
		}
		if (!src)
			return stopped(Error{ Errc::invalid_argument, "exporting", 0 }, "there's no image open");

		std::vector<const FoundFile*> chosen;
		std::uint64_t bytes = 0;
		for (std::uint64_t id : ids)
			if (id < files->size()) {
				const FoundFile& f = (*files)[static_cast<std::size_t>(id)];
				if (!f.file.directory && f.file.integrity != Integrity::unrecoverable) {
					chosen.push_back(&f);
					bytes += f.file.recoverable_bytes;
				}
			}
		total = bytes;

		auto records = std::make_shared<std::vector<ExportRecord>>();
		std::uint64_t written = 0, failed = 0, base = 0;
		auto last_publish = clock_type::now();
		bool cancelled = false;
		for (const FoundFile* f : chosen) {
			if (token.cancelled()) {
				cancelled = true;
				break;
			}
			// files from different partitions can have the same path, each partition gets a folder
			RecoveryCandidate c = f->file;
			if (c.origin == Origin::filesystem && f->volume != 0)
				c.path = "/" + about.volumes[f->volume].name + c.path;
			t_progress progress(*this, base);
			auto exported = export_candidate(*src->volumes[f->volume], c, destination, &token, &progress);
			base += f->file.recoverable_bytes;
			ExportRecord r{ .id = f->file.id };
			if (exported) {
				r.path = to_utf8(exported->path);
				r.bytes = exported->bytes;
				r.sha256 = exported->sha256;
				r.complete = exported->complete;
				written++;
			} else if (exported.error().code == Errc::cancelled) {
				cancelled = true;
				break;
			} else {
				r.error = error_text(exported.error());
				failed++;
			}
			records->push_back(std::move(r));
			if (clock_type::now() - last_publish > std::chrono::milliseconds(250)) {
				last_publish = clock_type::now();
				publish([&](SessionSnapshot& s) {
					s.exported = std::make_shared<const std::vector<ExportRecord>>(*records);
					s.status = std::format("exporting: {} of {} written", written, chosen.size());
				});
			}
		}
		done = base;

		auto report = write_report(destination, "fenrir report.txt", report_text(about, *files, *records, destination, chosen.size(), cancelled));
		publish([&](SessionSnapshot& s) {
			s.exported = std::make_shared<const std::vector<ExportRecord>>(*records);
			std::string status = std::format("{} of {} written to {}", written, chosen.size(), to_utf8(destination));
			if (failed != 0)
				status += std::format(", {} couldn't be", failed);
			if (cancelled) {
				status += ", then it was stopped";
				s.error = Error{ Errc::cancelled, "exporting", 0 };
			}
			if (report)
				s.report = *report;
			else
				status += ". the report couldn't be written: " + error_text(report.error());
			s.status = std::move(status);
		});
	}

	// what was exported and why each has the status it has, written next to the files
	static std::string report_text(const SessionSnapshot& about, const std::vector<FoundFile>& files, const std::vector<ExportRecord>& records,
		const std::filesystem::path& destination, std::size_t chosen, bool cancelled) {
		const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
		std::string out = "Fenrir recovery report\n\n";
		out += std::format("written:      {:%Y-%m-%d %H:%M:%S} UTC\n", now);
		out += std::format("image:        {} ({} bytes), opened read-only\n", to_utf8(about.image), about.image_size);
		out += std::format("destination:  {}\n", to_utf8(destination));
		std::size_t written = 0;
		for (const ExportRecord& r : records)
			written += r.error.empty();
		out += std::format("files:        {} of {} written{}\n\n", written, chosen, cancelled ? ", then the export was stopped" : "");
		out += "A file's SHA-256 is of what was written. Only intact ones are believed to be whole: check the others\n";
		out += "before trusting them. Each one's reasons say what its status is based on.\n";
		for (const ExportRecord& r : records) {
			const FoundFile& f = files[static_cast<std::size_t>(r.id)];
			out += std::format("\n{}  {}\n", to_string(f.file.integrity), f.file.path);
			if (r.error.empty()) {
				out += std::format("  written to:  {}\n", r.path);
				out += std::format("  bytes:       {}{}\n", r.bytes, r.complete ? "" : " (only the part believed to be its own)");
				out += std::format("  sha-256:     {}\n", to_hex(r.sha256));
			} else {
				out += std::format("  not written: {}\n", r.error);
			}
			if (f.file.origin == Origin::carved)
				out += std::format("  found:       by its content, a {} at byte {} of the image\n", f.file.format, f.file.entry_offset);
			else
				out += std::format("  found:       in {}'s {} records, {}\n", about.volumes[f.volume].name, to_string(about.volumes[f.volume].filesystem), f.file.deleted ? "deleted" : "not deleted");
			for (const std::string& reason : f.file.reasons)
				out += "  - " + reason + "\n";
		}
		return out;
	}

	void preview_loop() {
		for (;;) {
			std::uint64_t id;
			std::shared_ptr<t_sources> src;
			std::shared_ptr<const std::vector<FoundFile>> files;
			std::uint64_t volume_offset = 0;
			{
				std::unique_lock lock(mutex);
				wake.wait(lock, [&] { return stopping || wanted.has_value(); });
				if (stopping)
					return;
				id = *wanted;
				wanted.reset();
				src = sources;
				files = state.files;
				if (id < files->size())
					volume_offset = state.volumes[(*files)[static_cast<std::size_t>(id)].volume].offset;
			}
			FilePreview p{ .id = id };
			if (!src || id >= files->size()) {
				p.error = "there's nothing to read";
			} else {
				const FoundFile& f = (*files)[static_cast<std::size_t>(id)];
				if (!f.file.resident.empty()) {
					p.offset = volume_offset + f.file.entry_offset; // the bytes are in its record, somewhere after this
					p.bytes.assign(f.file.resident.begin(), f.file.resident.begin() + static_cast<std::ptrdiff_t>(std::min(preview_bytes, f.file.resident.size())));
				} else if (f.file.extents.empty() || f.file.recoverable_bytes == 0) {
					p.error = f.file.directory ? "it's a directory" : "nothing of it is recoverable";
				} else if (f.file.extents.front().zeros) {
					p.bytes.assign(static_cast<std::size_t>(std::min<std::uint64_t>(preview_bytes, f.file.extents.front().length)), std::byte{ 0 });
				} else {
					const Extent& e = f.file.extents.front();
					p.offset = volume_offset + e.offset;
					p.bytes.resize(static_cast<std::size_t>(std::min<std::uint64_t>(preview_bytes, e.length)));
					if (auto read = read_exact(*src->volumes[f.volume], e.offset, p.bytes); !read) {
						p.bytes.clear();
						p.error = "it couldn't be read: " + error_text(read.error());
					}
				}
			}
			p.ready = true;
			std::lock_guard lock(mutex);
			if (!wanted) // else a newer one was asked for while this was read
				preview = std::move(p);
		}
	}
};

storage::RecoverySession::RecoverySession() : impl_(std::make_unique<t_impl>()) {}
storage::RecoverySession::~RecoverySession() = default;

bool storage::RecoverySession::scan(const std::filesystem::path& image, const ScanOptions& options) {
	{
		std::lock_guard lock(impl_->mutex);
		if (impl_->running)
			return false;
		impl_->options = options;
		impl_->preview = {};
	}
	return impl_->start([impl = impl_.get(), image](CancellationToken& token) { impl->scan_job(image, token); });
}

bool storage::RecoverySession::resume() {
	std::uint64_t from;
	{
		std::lock_guard lock(impl_->mutex);
		if (impl_->running || !impl_->state.can_resume)
			return false;
		from = impl_->state.carved_to;
	}
	return impl_->start([impl = impl_.get(), from](CancellationToken& token) { impl->carve_job(from, token); });
}

bool storage::RecoverySession::export_files(std::vector<std::uint64_t> ids, const std::filesystem::path& destination) {
	return impl_->start([impl = impl_.get(), ids = std::move(ids), destination](CancellationToken& token) { impl->export_job(ids, destination, token); });
}

void storage::RecoverySession::cancel() {
	std::lock_guard lock(impl_->mutex);
	if (impl_->running && impl_->cancel)
		impl_->cancel->cancel();
}

bool storage::RecoverySession::busy() const {
	std::lock_guard lock(impl_->mutex);
	return impl_->running;
}

void storage::RecoverySession::wait() {
	for (;;) {
		{
			std::lock_guard lock(impl_->mutex);
			if (!impl_->running)
				return;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

std::shared_ptr<const storage::SessionSnapshot> storage::RecoverySession::snapshot() const {
	std::lock_guard lock(impl_->mutex);
	auto s = std::make_shared<SessionSnapshot>(impl_->state);
	s->done = impl_->done.load(std::memory_order_relaxed);
	s->total = impl_->total.load(std::memory_order_relaxed);
	s->seconds = impl_->running ? std::chrono::duration<double>(clock_type::now() - impl_->started).count() : impl_->last_seconds;
	return s;
}

void storage::RecoverySession::preview(std::uint64_t id) {
	{
		std::lock_guard lock(impl_->mutex);
		impl_->wanted = id;
		impl_->preview = FilePreview{ .id = id };
	}
	impl_->wake.notify_one();
}

storage::FilePreview storage::RecoverySession::preview_result() const {
	std::lock_guard lock(impl_->mutex);
	return impl_->preview;
}
