#include "cachyos/checkpoint.hpp"
#include "cachyos/orchestrator.hpp"
#include "cachyos/disk.hpp"
#include "cachyos/steps.hpp"

// import gucc
#include "gucc/logger.hpp"
#include "gucc/string_utils.hpp"

#include <cstdint>  // for uint8_t, uint32_t

#include <array>        // for array
#include <expected>     // for expected, unexpected
#include <optional>     // for optional
#include <string>       // for string
#include <string_view>  // for string_view
#include <utility>      // for move
#include <variant>      // for get_if

#include <fmt/format.h>
#include <spdlog/spdlog.h>

using namespace std::string_view_literals;

namespace {

// NOLINTNEXTLINE
using namespace cachyos::installer;

class SinkClearGuard {
 public:
    explicit SinkClearGuard(gucc::utils::ProcessRunner& runner) noexcept : m_runner(runner) { }
    ~SinkClearGuard() { m_runner.set_line_sink(nullptr); }

    SinkClearGuard(const SinkClearGuard&)                    = delete;
    SinkClearGuard(SinkClearGuard&&)                         = delete;
    auto operator=(const SinkClearGuard&) -> SinkClearGuard& = delete;
    auto operator=(SinkClearGuard&&) -> SinkClearGuard&      = delete;

 private:
    gucc::utils::ProcessRunner& m_runner;
};

enum class Step : std::uint8_t {
    Umount,
    Partition,
    Base,
    Fstab,
    EncryptSwap,
    SystemSettings,
    Users,
    MachineId,
    Desktop,
    DesktopConfigure,
    ServerPackages,
    SshKeys,
    ServerFirewall,
    Autologin,
    Chwd,
    NetworkCarryover,
    Bootloader,
    DetectCrypto,
    EnableServices,
    FinalValidation,
    BtrfsSnapshot,
    Cleanup,
    Count,
};

constexpr auto kTotalSteps = static_cast<std::int32_t>(Step::Count);

constexpr std::array<std::string_view, kTotalSteps> kStepMessages = {
    "Unmounting existing partitions..."sv,
    "Partitioning and mounting..."sv,
    "Installing base system (this may take a while)..."sv,
    "Generating fstab..."sv,
    "Configuring encrypted swap..."sv,
    "Configuring system settings..."sv,
    "Creating user accounts..."sv,
    "Generating machine ID..."sv,
    "Installing desktop environment..."sv,
    "Configuring desktop environment..."sv,
    "Installing server profile packages..."sv,
    "Installing SSH keys..."sv,
    "Configuring server firewall..."sv,
    "Configuring autologin..."sv,
    "Installing hardware-driver profiles..."sv,
    "Carrying network connections forward..."sv,
    "Installing bootloader..."sv,
    "Detecting encryption state..."sv,
    "Enabling system services..."sv,
    "Running final validation..."sv,
    "Creating installation snapshot..."sv,
    "Cleaning up..."sv,
};

constexpr auto step_index(Step s) noexcept {
    return static_cast<std::int32_t>(s);
}

constexpr auto step_message(Step s) noexcept -> std::string_view {
    return kStepMessages[static_cast<std::size_t>(s)];
}

auto emit_progress(const InstallSession& session,
    ProgressEventType type,
    std::int32_t step,
    std::string_view message) noexcept -> void {
    if (!session.on_progress) {
        return;
    }
    const auto fraction = static_cast<double>(step) / static_cast<double>(kTotalSteps);
    session.on_progress(ProgressEvent{
        .type     = type,
        .message  = std::string{message},
        .fraction = fraction,
    });
}

// fire a Failed event and hand back a ValidationResult with the formatted error
auto fail_step(const InstallSession& session,
    Step s,
    std::string_view label,
    std::string_view error,
    std::vector<std::string> prior_warnings) noexcept -> ValidationResult {
    emit_progress(session, ProgressEventType::Failed, step_index(s), label);
    return ValidationResult{
        .success  = false,
        .errors   = {fmt::format("{}: {}", label, error)},
        .warnings = std::move(prior_warnings),
    };
}

// fire a Cancelled event for the step we were about to run, hand back a result tagged cancelled
auto cancel_result(const InstallSession& session,
    Step s,
    std::vector<std::string> prior_warnings) noexcept -> ValidationResult {
    constexpr auto kCancelled = "Cancelled by user"sv;
    emit_progress(session, ProgressEventType::Cancelled, step_index(s), kCancelled);
    return ValidationResult{
        .success  = false,
        .errors   = {std::string{kCancelled}},
        .warnings = std::move(prior_warnings),
    };
}

/// Execute a single step with checkpoint save-on-success.
/// @return true if the step completed (or was skipped via resume), false on failure/cancel.
auto execute_step(InstallContext& ctx,
    const SystemSettings& sys,
    const UserSettings& user,
    std::string_view root_password,
    Step s,
    const InstallSession& session,
    std::vector<std::string>& warnings,
    Checkpoint& cp,
    int resume_from) noexcept -> bool {
    const int idx = step_index(s);

    // Skip already-completed steps when resuming
    if (resume_from >= 0 && idx < resume_from) {
        spdlog::info("Skipping completed step: {}", step_message(s));
        emit_progress(session, ProgressEventType::Completed, idx, fmt::format("[skipped] {}", step_message(s)));
        cp.last_completed_step = idx;
        (void)save_checkpoint(cp);  // best-effort
        return true;
    }

    if (session.runner.cancelled()) {
        return false;
    }

    emit_progress(session, ProgressEventType::Running, idx, step_message(s));
    spdlog::info("Step {}/{}: {}", idx + 1, kTotalSteps, step_message(s));

    // Execute the appropriate step function
    switch (s) {
    case Step::Umount: {
        if (steps::needs_umount(ctx)) {
            if (auto res = steps::umount(ctx); !res) {
                spdlog::warn("umount_partitions: {}", res.error());
                warnings.emplace_back(fmt::format("Pre-install unmount: {}", res.error()));
            }
        }
        break;
    }
    case Step::Partition: {
        if (auto res = steps::partition(ctx); !res) {
            emit_progress(session, ProgressEventType::Failed, idx, step_message(s));
            spdlog::error("Partitioning failed: {}", res.error());
            return false;
        }
        break;
    }
    case Step::Base: {
        if (auto res = steps::base(ctx); !res) {
            if (session.runner.cancelled()) {
                return false;
            }
            emit_progress(session, ProgressEventType::Failed, idx, step_message(s));
            spdlog::error("Base install failed: {}", res.error());
            return false;
        }
        break;
    }
    case Step::Fstab: {
        if (auto res = steps::fstab(ctx); !res) {
            emit_progress(session, ProgressEventType::Failed, idx, step_message(s));
            spdlog::error("fstab generation failed: {}", res.error());
            return false;
        }
        cp.context.fstab_generated = true;
        break;
    }
    case Step::EncryptSwap: {
        std::ranges::move(steps::encrypt_swap(ctx), std::back_inserter(warnings));
        cp.context.swap_configured = true;
        break;
    }
    case Step::SystemSettings: {
        if (auto res = steps::system_settings(sys, ctx); !res) {
            emit_progress(session, ProgressEventType::Failed, idx, step_message(s));
            spdlog::error("System settings failed: {}", res.error());
            return false;
        }
        cp.context.system_settings_applied = true;
        break;
    }
    case Step::Users: {
        std::ranges::move(
            steps::users(user, root_password, ctx),
            std::back_inserter(warnings));
        cp.context.users_created = true;
        break;
    }
    case Step::MachineId: {
        if (auto res = steps::machine_id(ctx); !res) {
            warnings.emplace_back(res.error());
        }
        break;
    }
    case Step::Desktop: {
        if (auto res = steps::desktop(ctx); !res) {
            warnings.emplace_back(res.error());
        } else {
            cp.context.desktop_installed = true;
        }
        break;
    }
    case Step::DesktopConfigure: {
        if (auto res = steps::desktop_configure(ctx); !res) {
            warnings.emplace_back(res.error());
        }
        break;
    }
    case Step::ServerPackages: {
        if (ctx.resolved_server) {
            if (auto res = steps::server_packages(ctx); !res) {
                warnings.emplace_back(res.error());
            }
        }
        break;
    }
    case Step::SshKeys: {
        if (ctx.resolved_server) {
            if (auto res = steps::ssh_keys(user, ctx); !res) {
                warnings.emplace_back(res.error());
            }
        }
        break;
    }
    case Step::ServerFirewall: {
        if (ctx.resolved_server) {
            if (auto res = steps::server_firewall(ctx); !res) {
                warnings.emplace_back(res.error());
            }
        }
        break;
    }
    case Step::Autologin: {
        if (auto res = steps::autologin(user, ctx); !res) {
            warnings.emplace_back(res.error());
        }
        break;
    }
    case Step::Chwd: {
        if (auto res = steps::chwd(ctx); !res) {
            warnings.emplace_back(res.error());
        }
        break;
    }
    case Step::NetworkCarryover: {
        if (ctx.carry_live_network && steps::network_carryover(ctx) < 0) {
            warnings.emplace_back("network connection carryover failed");
        }
        break;
    }
    case Step::Bootloader: {
        if (auto res = steps::bootloader(ctx); !res) {
            emit_progress(session, ProgressEventType::Failed, idx, step_message(s));
            spdlog::error("Bootloader installation failed: {}", res.error());
            return false;
        }
        cp.context.bootloader_installed = true;
        break;
    }
    case Step::DetectCrypto: {
        [[maybe_unused]] const auto crypto_res = steps::detect_crypto(ctx);
        break;
    }
    case Step::EnableServices: {
        if (auto res = steps::enable_services(ctx); !res) {
            warnings.emplace_back(res.error());
        } else {
            cp.context.services_enabled = true;
        }
        break;
    }
    case Step::FinalValidation: {
        auto check = steps::final_validation(ctx);
        for (auto& err : check.errors) {
            warnings.emplace_back(fmt::format("final_check: {}", std::move(err)));
        }
        for (auto& warn : check.warnings) {
            warnings.emplace_back(fmt::format("final_check: {}", std::move(warn)));
        }
        break;
    }
    case Step::BtrfsSnapshot: {
        if (auto res = steps::btrfs_snapshot(ctx); !res) {
            warnings.emplace_back(res.error());
        }
        break;
    }
    case Step::Cleanup: {
        std::ranges::move(steps::cleanup(ctx), std::back_inserter(warnings));
        break;
    }
    default:
        spdlog::warn("Unknown step index: {}", idx);
        break;
    }

    // Save checkpoint on success
    cp.last_completed_step = idx;
    if (auto res = save_checkpoint(cp); !res) {
        spdlog::warn("Failed to save checkpoint: {}", res.error());
    }

    emit_progress(session, ProgressEventType::Completed, idx, step_message(s));
    return true;
}

}  // namespace

namespace cachyos::installer {

auto parse_pacman_progress(std::string_view line) noexcept -> std::optional<double> {
    const auto open = line.find('(');
    if (open == std::string_view::npos) {
        return std::nullopt;
    }
    const auto slash = line.find('/', open + 1);
    if (slash == std::string_view::npos) {
        return std::nullopt;
    }
    const auto close = line.find(')', slash + 1);
    if (close == std::string_view::npos) {
        return std::nullopt;
    }

    const auto num_str   = gucc::utils::trim(line.substr(open + 1, slash - open - 1));
    const auto denom_str = gucc::utils::trim(line.substr(slash + 1, close - slash - 1));

    const auto num   = gucc::utils::parse_uint<std::uint32_t>(num_str);
    const auto denom = gucc::utils::parse_uint<std::uint32_t>(denom_str);
    if (!num || !denom || *denom == 0 || *num > *denom) {
        return std::nullopt;
    }
    return static_cast<double>(*num) / static_cast<double>(*denom);
}

auto run(InstallContext& ctx,
    const SystemSettings& sys,
    const UserSettings& user,
    std::string_view root_password,
    const InstallSession& session,
    int resume_from) noexcept -> ValidationResult {
    using enum ProgressEventType;
    std::vector<std::string> warnings;

    // reset run state
    session.runner.reset_cancel();

    // register secrets
    gucc::logger::register_secret(root_password);
    gucc::logger::register_secret(user.password);
    if (const auto* layout = std::get_if<partition_strategy::CreateLayout>(&ctx.strategy);
        layout != nullptr && layout->zfs_setup && layout->zfs_setup->passphrase) {
        gucc::logger::register_secret(*layout->zfs_setup->passphrase);
    }

    // parse pacman progress into session context
    Step current_step{Step::Umount};
    std::string current_msg;
    session.runner.set_line_sink([&session, &current_step, &current_msg](std::string_view line) {
        if (!session.on_progress) {
            return;
        }
        const auto frac = parse_pacman_progress(line);
        if (!frac) {
            return;
        }
        constexpr auto total = static_cast<double>(kTotalSteps);
        const double base    = static_cast<double>(step_index(current_step)) / total;
        session.on_progress(ProgressEvent{
            .type     = ProgressEventType::Running,
            .message  = current_msg,
            .fraction = base + (*frac / total),
        });
    });
    const SinkClearGuard sink_guard{session.runner};

    spdlog::info("Install orchestrator starting...");
    emit_progress(session, Started, 0, "Starting installation..."sv);

    // Initialize checkpoint
    Checkpoint cp = make_checkpoint(-1, ctx, kTotalSteps);

    // If resuming, update checkpoint with saved state
    if (resume_from >= 0) {
        if (auto loaded = load_checkpoint(); loaded) {
            cp = *loaded;
            spdlog::info("Resuming from step {}", resume_from);
            emit_progress(session, ProgressEventType::Running, resume_from,
                fmt::format("Resuming from step {}...", resume_from));
        } else {
            spdlog::warn("Checkpoint load failed: {}; proceeding with fresh install", loaded.error());
            resume_from = -1;  // fall back to full install
        }
    }

    // Execute all steps sequentially
    for (int idx = 0; idx < kTotalSteps; ++idx) {
        const Step s = static_cast<Step>(idx);

        if (!execute_step(ctx, sys, user, root_password, s, session, warnings, cp, resume_from)) {
            // Step failed or was cancelled — clean up checkpoint and return
            remove_checkpoint();
            return cancel_result(session, s, std::move(warnings));
        }
    }

    // Installation complete — remove checkpoint
    remove_checkpoint();

    emit_progress(session, Completed, kTotalSteps, "Installation complete!"sv);
    spdlog::info("Install orchestrator finished.");

    return ValidationResult{
        .success  = true,
        .errors   = {},
        .warnings = std::move(warnings),
    };
}

}  // namespace cachyos::installer
