#pragma once

#include <atomic>
#include <cstdint>

// what a long-running engine call (hashing, scanning, later imaging) is given by whoever runs it: a way to be asked to
// stop, and somewhere to say how far along it is. the ui's job manager (P4) owns both, the engine only checks and reports
namespace storage {
	// set from any thread, checked by the job between steps. a job that sees it stops at the next step and returns
	// Errc::cancelled, it never leaves a half-done result behind
	class CancellationToken {
	public:
		void cancel() noexcept { cancelled_.store(true, std::memory_order_relaxed); }
		bool cancelled() const noexcept { return cancelled_.load(std::memory_order_relaxed); }

	private:
		std::atomic<bool> cancelled_ = false;
	};

	// told how far along a job is. called on the job's own thread, so an implementation that hands this to the ui has to
	// pass it through something thread safe. done only goes up and never passes total
	class ProgressSink {
	public:
		virtual ~ProgressSink() = default;
		virtual void progress(std::uint64_t done, std::uint64_t total) noexcept = 0;
	};
}
