// a build without the drive writer (the default, and every build that isn't windows): drives can be planned for, and
// every plan said what it would do, but nothing here can carry one out

#include "storage/drive_erase.h"

bool storage::drive_erase_built() noexcept { return false; }

storage::Result<storage::SanitizeReport> storage::DriveOverwriteBackend::execute(const Authorization&, const CancellationToken*, ProgressSink*) {
	return fail(Errc::unsupported, "erasing a drive: this build has no code that can write to one (configure with -DFENRIR_DRIVE_ERASE=ON on windows)");
}
