#include "../../ui.h"
#include "app.h"
#include "screens.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <variant>

#include "storage/exporter.h"
#include "storage/image_erase.h"
#include "storage/sanitize.h"
#include "storage/simulated_drive.h"

// erase: the P7 simulator. a simulated drive (with faults to order) or a disposable test image file, a plan that says
// what it may be called and what it can't promise, a deliberate confirmation, then the run on a thread of its own and its
// report. no real drive can be picked: there's no code in this build that could erase one.
// celosui has no text field yet, so the confirmation isn't typed here as it is in fenrir-cli: the plan's confirmation
// has to be picked out of four look-alikes, in an order that changes every time, and no choice is the default

using namespace celosia;
namespace app = fenrir::app;

namespace {
    float px(float v) { return v * style::scale; }

    // the run: a backend and its approval handed to a thread, the report handed back
    struct t_job {
        std::mutex mutex;
        std::optional<storage::SanitizeReport> report;
        std::string error;
        std::atomic<bool> running = false;
        std::atomic<std::uint64_t> done = 0, total = 0;
        storage::CancellationToken cancel;
    };

    struct t_progress final : storage::ProgressSink {
        std::shared_ptr<t_job> job;
        void progress(std::uint64_t done, std::uint64_t total) noexcept override {
            job->done = done;
            job->total = total;
        }
    };

    std::shared_ptr<t_job> job;
    std::thread worker;

    struct t_join { // the thread is told to stop and waited for when fenrir closes, an overwrite stops at its next piece
        ~t_join() {
            if (job)
                job->cancel.cancel();
            if (worker.joinable())
                worker.join();
        }
    } joiner;

    void start(std::unique_ptr<storage::ISanitizationBackend> backend, storage::Authorization approved) {
        if (worker.joinable())
            worker.join();
        job = std::make_shared<t_job>();
        job->running = true;
        worker = std::thread([j = job, backend = std::move(backend), approved = std::move(approved)] {
            t_progress progress;
            progress.job = j;
            auto report = backend->execute(approved, &j->cancel, &progress);
            std::lock_guard lock(j->mutex);
            if (report)
                j->report = std::move(*report);
            else
                j->error = std::string(report.error().context) + ": " + std::string(storage::to_string(report.error().code));
            j->running = false;
        });
    }

    // what's picked on the page
    int kind = 0;              // 0 a simulated drive, 1 a test image file
    int profile = 2;           // nvme-ssd
    int method_index = 0;
    bool fail_partway = false, power_loss = false, lies = false, unreadable = false;
    bool random_pattern = false, read_back = true;
    std::optional<std::filesystem::path> test_image;
    constexpr int image_dialog = 101, report_dialog = 102;

    // the confirmation challenge: the plan's confirmation among three that differ from it a little
    std::vector<std::string> choices;
    std::string choices_for;   // the confirmation they were made for
    std::string wrong_pick;
    std::string saved; // where the last report went

    void make_choices(const std::string& confirmation) {
        choices = { confirmation };
        std::mt19937 random(std::random_device{}());
        while (choices.size() < 4) {
            std::string decoy = confirmation;
            const std::size_t at = 6 + random() % std::max<std::size_t>(1, decoy.size() - 6); // after "ERASE "
            if (at < decoy.size()) {
                const char c = decoy[at];
                decoy[at] = c >= '0' && c <= '8' ? char(c + 1) : c == '9' ? '0' : c >= 'a' && c < 'z' ? char(c + 1) : c >= 'A' && c < 'Z' ? char(c + 1) : '7';
            } else
                decoy += '1';
            if (std::ranges::find(choices, decoy) == choices.end())
                choices.push_back(decoy);
        }
        std::ranges::shuffle(choices, random);
        choices_for = confirmation;
        wrong_pick.clear();
    }

    void lines(const char* heading, const std::vector<std::string>& items) {
        widgets::text(heading);
        for (const std::string& item : items) {
            const std::string line = "  - " + item;
            widgets::text(app::fit(line, core::available().x).c_str());
        }
    }

    // the backend for what's picked, made again only when the pick changes (a simulated drive is 4 MiB of data). a run
    // takes it, the next frame makes a fresh one
    std::unique_ptr<storage::ISanitizationBackend> cached;
    std::string cached_for;

    std::unique_ptr<storage::ISanitizationBackend> make_backend() {
        if (kind == 1) {
            if (!test_image)
                return nullptr;
            return std::make_unique<storage::TestImageBackend>(*test_image);
        }
        storage::SimulatedFaults faults;
        if (fail_partway)
            faults.fail_at = 0.5;
        if (power_loss)
            faults.power_loss_at = 0.3;
        faults.lies = lies;
        faults.unreadable_after = unreadable;
        auto drive = storage::SimulatedDrive::make(storage::SimulatedDrive::profiles()[static_cast<std::size_t>(profile)], faults);
        return drive ? std::make_unique<storage::SimulatedDrive>(std::move(*drive)) : nullptr;
    }

    void show_report(const storage::SanitizeReport& r) {
        const std::string status = "Status: " + std::string(storage::to_string(r.status)) + "    " + app::thousands(r.bytes_done) + " bytes done";
        widgets::text(status.c_str());
        lines("What happened:", r.events);
        if (!r.errors.empty())
            lines("Errors:", r.errors);
        const std::string checked = r.verification.done
            ? "Read back: " + app::thousands(r.verification.bytes_checked) + " bytes, " + app::thousands(r.verification.mismatches) + " sectors wrong"
            : std::string("Read back: no");
        widgets::text(checked.c_str());
        if (!r.verification.note.empty())
            widgets::text(app::fit(r.verification.note, core::available().x).c_str());
        widgets::text(app::fit("Validation: " + r.validation, core::available().x).c_str());
    }
}

void fenrir::screens::erase() {
    if (auto picked = app::dialogs::take(image_dialog))
        test_image = std::move(picked);
    const bool running = job && job->running;

    render::groupbox::begin("Erase", "A simulator and disposable test images. No real drive can be erased in this build");
    const char* kinds[] = { "Simulated drive", "Test image file" };
    if (!running)
        widgets::segmented("Target", &kind, kinds, IM_ARRAYSIZE(kinds));

    if (kind == 0) {
        const char* profiles[] = { "HDD", "SATA SSD", "NVMe SSD", "USB stick" };
        if (!running && widgets::segmented("Drive", &profile, profiles, IM_ARRAYSIZE(profiles)))
            method_index = 0;
        if (!running) {
            widgets::toggle("Fails partway", &fail_partway);
            core::same_line(style::frame::size_padding.x * 2.f);
            widgets::toggle("Loses power", &power_loss);
            core::same_line(style::frame::size_padding.x * 2.f);
            widgets::toggle("Firmware lies", &lies);
            core::same_line(style::frame::size_padding.x * 2.f);
            widgets::toggle("Unreadable after", &unreadable);
        }
    } else {
        const std::string which = test_image ? "Test image: " + app::utf8(*test_image) : std::string("Test image: (pick a disposable file)");
        widgets::text(app::fit(which, core::available().x).c_str());
        if (!running && app::dialogs::available() && widgets::button("Pick a test image..."))
            app::dialogs::show(app::dialogs::e_kind::image, image_dialog);
        widgets::text("Its bytes are overwritten: use a copy made for this, never an image you still need.");
    }

    const std::string pick = std::to_string(kind) + "/" + std::to_string(profile) + "/" + std::to_string(fail_partway) + std::to_string(power_loss) +
        std::to_string(lies) + std::to_string(unreadable) + "/" + (test_image ? app::utf8(*test_image) : std::string());
    if (!cached || cached_for != pick) {
        cached = make_backend();
        cached_for = pick;
    }
    std::unique_ptr<storage::ISanitizationBackend>& backend = cached;
    std::optional<storage::Target> target;
    std::string problem;
    if (backend) {
        if (auto inspected = backend->inspect())
            target = std::move(*inspected);
        else
            problem = std::string(inspected.error().context) + ": " + std::string(storage::to_string(inspected.error().code));
    }
    if (!problem.empty())
        widgets::text(app::fit(problem, core::available().x).c_str());

    std::optional<storage::SanitizePlan> plan;
    if (target) {
        std::vector<std::string> names;
        for (storage::Method m : target->capabilities.methods)
            names.emplace_back(storage::to_string(m));
        std::vector<const char*> items;
        for (const std::string& n : names)
            items.push_back(n.c_str());
        method_index = std::clamp(method_index, 0, (int)items.size() - 1);
        if (!running) {
            widgets::combo("Method", &method_index, items.data(), (int)items.size());
            if (target->capabilities.methods[(std::size_t)method_index] == storage::Method::overwrite) {
                widgets::toggle("Random pattern", &random_pattern);
                core::same_line(style::frame::size_padding.x * 2.f);
            }
            widgets::toggle("Read back after", &read_back);
        }
        storage::SanitizeRequest request{ .method = target->capabilities.methods[(std::size_t)method_index], .zeros = !random_pattern, .seed = 0x5EED, .verify = read_back };
        const auto planned = storage::plan_sanitize(*target, request);
        if (const auto* refusal = std::get_if<storage::PlanRefusal>(&planned))
            lines("Refused, nothing would be done:", refusal->reasons);
        else
            plan = std::get<storage::SanitizePlan>(planned);
    }
    render::groupbox::end();

    render::groupbox::begin(running ? "Running" : "The plan", plan ? plan->scope.c_str() : "Pick a target");
    if (running) {
        const std::uint64_t total = job->total, done = job->done;
        const std::string how = total ? app::thousands(done * 100 / total) + "% done" : std::string("starting");
        widgets::text(how.c_str());
        if (widgets::button("Stop"))
            job->cancel.cancel(); // an overwrite stops at its next piece, a firmware sanitize goes on: the report says which
    } else if (plan) {
        const std::string category = "At most: " + std::string(storage::to_string(plan->category)) + (plan->cancellable ? "" : "    can't be stopped once it starts");
        widgets::text(category.c_str());
        lines("Steps:", plan->steps);
        lines("It can't promise:", plan->caveats);

        if (choices_for != plan->confirmation)
            make_choices(plan->confirmation);
        core::layout(ImVec2(0.f, core::font_size() * 0.5f));
        widgets::text("To run it, pick the confirmation that matches this target exactly:");
        for (std::size_t i = 0; i < choices.size(); i++) {
            if (i > 0)
                core::same_line(style::general::inner_spacing.x);
            const std::string label = choices[i] + "##confirm" + std::to_string(i);
            if (widgets::button(label.c_str())) {
                auto now = backend->inspect();
                auto approved = now ? storage::authorize(*plan, now->identity, choices[i]) : storage::Result<storage::Authorization>(std::unexpected(now.error()));
                if (approved) {
                    start(std::move(backend), std::move(*approved));
                    choices_for.clear(); // new choices next time
                } else {
                    const std::string why = approved.error().code == storage::Errc::invalid_argument ? "That isn't it: nothing was done. The choices have changed."
                        : "The target changed since the plan: nothing was done.";
                    make_choices(plan->confirmation); // clears wrong_pick
                    wrong_pick = why;
                }
                break;
            }
        }
        if (!wrong_pick.empty())
            widgets::text(wrong_pick.c_str());
    }

    if (job && !job->running) {
        core::layout(ImVec2(0.f, core::font_size() * 0.5f));
        std::lock_guard lock(job->mutex);
        if (job->report) {
            show_report(*job->report);
            if (app::dialogs::available() && widgets::button("Save the report..."))
                app::dialogs::show(app::dialogs::e_kind::folder, report_dialog);
            if (auto folder = app::dialogs::take(report_dialog)) {
                auto written = storage::write_report(*folder, "fenrir erase report.json", storage::to_json(*job->report));
                saved = written ? "Saved: " + app::utf8(*written) : std::string("The report couldn't be saved there.");
            }
            if (!saved.empty())
                widgets::text(app::fit(saved, core::available().x).c_str());
        } else if (!job->error.empty())
            widgets::text(app::fit("Not run: " + job->error, core::available().x).c_str());
    }
    render::groupbox::end();
}
