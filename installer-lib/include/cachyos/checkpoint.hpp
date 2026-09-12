#ifndef CHECKPOINT_HPP
#define CHECKPOINT_HPP

#include "cachyos/types.hpp"

#include <cstdint>   // for uint32_t
#include <expected>  // for expected, unexpected
#include <string>    // for string
#include <string_view>// for string_view
#include <vector>    // for vector

#include <spdlog/fwd.h> // for spdlog::logger

namespace cachyos::installer {

/// @brief Represents a saved installation checkpoint.
struct Checkpoint final {
    /// Version of the checkpoint format (increment when schema changes).
    uint32_t version{1};

    /// Timestamp when this checkpoint was created (seconds since epoch).
    uint64_t timestamp{0};

    /// The step that was just completed (0-indexed).
    /// A value of 0 means no steps completed yet.
    int last_completed_step{-1};

    /// Total number of steps in the installation sequence.
    int total_steps{0};

    /// Target device path (e.g., "/dev/nvme0n1").
    std::string device;

    /// Mount point for the target system.
    std::string mountpoint;

    /// Whether this is a UEFI system.
    bool is_efi{true};

    /// Serialised InstallContext state (subset needed to resume).
    struct ContextSnapshot {
        std::string device;
        std::string mountpoint;
        bool is_efi;
        bool hostcache;
        std::vector<std::string> partition_devices;  // devices that were created
        bool fstab_generated;
        bool swap_configured;
        bool system_settings_applied;
        bool users_created;
        bool desktop_installed;
        bool bootloader_installed;
        bool services_enabled;
    } context;

    /// Error message from the step that failed (if any).
    std::string last_error;

    /// Human-readable summary of what has been done.
    [[nodiscard]] auto summary() const -> std::string;

    /// Human-readable list of remaining steps.
    [[nodiscard]] auto remaining_steps() const -> std::string;
};

/// @brief Path where the checkpoint file is stored.
inline constexpr std::string_view kCheckpointPath = "/tmp/cachyos-installer-checkpoint";

/// @brief Load a checkpoint from disk if it exists.
/// @param path Override path (default: /tmp/cachyos-installer-checkpoint).
/// @return Checkpoint on success, or error string if file is missing/corrupt.
[[nodiscard]] auto load_checkpoint(std::string_view path = std::string{kCheckpointPath}) noexcept
    -> std::expected<Checkpoint, std::string>;

/// @brief Save the current installation state to disk.
/// @param cp The checkpoint data to persist.
/// @param path Override path (default: /tmp/cachyos-installer-checkpoint).
/// @return void on success, or error string on failure.
auto save_checkpoint(const Checkpoint& cp, std::string_view path = std::string{kCheckpointPath}) noexcept
    -> std::expected<void, std::string>;

/// @brief Remove the checkpoint file from disk.
/// @param path Override path (default: /tmp/cachyos-installer-checkpoint).
auto remove_checkpoint(std::string_view path = std::string{kCheckpointPath}) noexcept -> void;

/// @brief Check if a valid checkpoint exists on disk.
/// @param path Override path.
[[nodiscard]] auto has_checkpoint(std::string_view path = std::string{kCheckpointPath}) noexcept -> bool;

/// @brief Build a checkpoint from the current install state.
/// @param last_completed The index of the step that just completed.
/// @param ctx The install context with current configuration.
/// @param total_steps Total number of steps in the sequence.
[[nodiscard]] auto make_checkpoint(int last_completed, const InstallContext& ctx, int total_steps) noexcept -> Checkpoint;

/// @brief Determine which steps need to be re-run when resuming.
/// @param cp The loaded checkpoint.
/// @return Vector of step indices that still need execution (0-based).
[[nodiscard]] auto resume_step_range(const Checkpoint& cp) noexcept -> std::vector<int>;

}  // namespace cachyos::installer

#endif  // CHECKPOINT_HPP
