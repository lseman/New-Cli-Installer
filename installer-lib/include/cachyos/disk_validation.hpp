#ifndef DISK_VALIDATION_HPP
#define DISK_VALIDATION_HPP

#include "cachyos/installer_config.hpp"
#include "cachyos/types.hpp"

#include <cstdint>    // for uint64_t
#include <expected>   // for expected, unexpected
#include <optional>   // for optional
#include <string>     // for string
#include <string_view>// for string_view
#include <vector>     // for vector

namespace cachyos::installer {

/// @brief Result of a single size-related validation check.
struct SizeCheckResult final {
    std::string partition;   ///< e.g. "/boot", "/", "swap"
    uint64_t available{0};   ///< bytes available on disk/partition
    uint64_t required{0};    ///< bytes required by config
    bool fits{false};        ///< true if available >= required
    std::string message;     ///< human-readable description
};

/// @brief Overall pre-flight validation report for a target disk.
struct DiskValidationReport final {
    bool is_valid{true};                     ///< false if any hard errors exist
    std::string device;                      ///< e.g. "/dev/nvme0n1"
    uint64_t total_disk_bytes{0};            ///< total capacity of the target disk
    uint64_t used_by_partitions{0};          ///< sum of all explicitly-sized partitions
    uint64_t remaining_bytes{0};             ///< total - used (space left for root/fill)
    std::vector<SizeCheckResult> checks;     ///< per-partition size checks
    std::vector<std::string> errors;         ///< hard errors that block installation
    std::vector<std::string> warnings;       ///< soft warnings that don't block install

    /// @brief Return a human-readable summary suitable for logging or TUI display.
    [[nodiscard]] auto to_string() const -> std::string;
};

/// @brief Parse a size string (e.g. "512M", "4G", "1024KiB", "100%") into bytes.
/// @param size_str The size string as used in partition configs.
/// @return The size in bytes, or std::nullopt if the string is unparseable.
[[nodiscard]] auto parse_size_bytes(std::string_view size_str) noexcept -> std::optional<uint64_t>;

/// @brief Get total system RAM in bytes by reading /proc/meminfo.
/// @return Total RAM in bytes, or 0 if unreadable.
[[nodiscard]] auto get_total_ram_bytes() noexcept -> uint64_t;

/// @brief Recommend a swap size in bytes based on installed RAM.
/// Follows Arch Linux guidelines:
///   - ≤ 2 GiB RAM  → 2× RAM
///   - 2–8 GiB RAM  → equal to RAM
///   - 8–64 GiB RAM → 0.5× RAM (min 4 GiB)
///   - ≥ 64 GiB RAM → 4 GiB (or hibernation size if needed)
/// @param ram_bytes Total installed RAM in bytes.
/// @return Recommended swap size in bytes.
[[nodiscard]] auto recommend_swap_size(uint64_t ram_bytes) noexcept -> uint64_t;

/// @brief Run a full pre-flight disk space validation before installation.
///
/// This function checks:
/// 1. That each explicitly-sized partition fits within the target disk.
/// 2. EFI partition size adequacy (minimum 100 MiB, recommended ≥ 512 MiB).
/// 3. Swap size relative to available RAM (warning if too small or missing).
/// 4. That total explicit sizes don't exceed disk capacity.
///
/// @param config The parsed installer configuration with partition specs.
/// @param is_efi Whether the target system is UEFI.
/// @return A DiskValidationReport with errors, warnings, and per-check details.
[[nodiscard]] auto validate_disk_space(const InstallerConfig& config, bool is_efi) noexcept -> DiskValidationReport;

}  // namespace cachyos::installer

#endif  // DISK_VALIDATION_HPP
