#pragma once

#include <filesystem>

#include "sanitize.h"

// erasing a disposable test image file: an overwrite of its bytes, flushed, then read back. it's how fenrir's erasure
// is tried on something real before any drive is (P8). a drive path is refused, and so is anything that isn't a
// regular file. what it does to the file isn't sanitization of the drive the file is on: the plan says so
namespace storage {
	class TestImageBackend final : public ISanitizationBackend {
	public:
		explicit TestImageBackend(std::filesystem::path path) : path_(std::move(path)) {}

		// its size and last write time are its identity: a file that changed since it was planned for isn't erased
		Result<Target> inspect() override;
		Result<SanitizeReport> execute(const Authorization& approved, const CancellationToken* cancel = nullptr, ProgressSink* progress = nullptr) override;

	private:
		std::filesystem::path path_;
	};
}
