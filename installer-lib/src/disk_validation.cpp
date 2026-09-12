#include "cachyos/disk_validation.hpp"

// import gucc
#include "gucc/io_utils.hpp"
#include "gucc/string_utils.hpp"
#include "gucc/system_query.hpp"

#include <algorithm>    // for max, min
#include <charconv>     // for from_chars
#include <expected>     // for expected
#include <filesystem>   // for exists, ifstream
#include <fstream>      // for ifstream
#include <optional>     // for optional
#include <ranges>       // for ranges::*
#include <string>       // for string
#include <string_view>  // for string_view
#include <vector>       // for vector
#include <iterator>     // for back_inserter

#include <fmt/compile.h>
#include <fmt/format.h>

using namespace std::string_view_literals;
using namespace std::string_literals;

namespace {

/// @brief Convert a string to lowercase (locale-independent).
constexpr auto to_lower(std::string_view s) noexcept -> std::string {
    std::string result{};
    result.reserve(s.size());
    std::ranges::transform(s, std::back_inserter(result), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

}  // namespace

namespace cachyos::installer {

// ---------------------------------------------------------------------------
// Size parsing helpers
// ---------------------------------------------------------------------------

namespace {

/// Suffix multipliers for size strings (binary units).
constexpr auto kSuffixes = std::array{
    std::pair{"YiB"sv, uint64_t{1} << 60},
    std::pair{"PiB"sv, uint64_t{1} << 50},
    std::pair{"TiB"sv, uint64_t{1} << 40},
    std::pair{"GiB"sv, uint64_t{1} << 30},
    std::pair{"MiB"sv, uint64_t{1} << 20},
    std::pair{"KiB"sv, uint64_t{1} << 10},
    // Decimal suffixes (SI) — commonly used in practice
    std::pair{"TB"sv,  uint64_t{1000} * 1000 * 1000 * 1000},
    std::pair{"GB"sv,  uint64_t{1000} * 1000 * 1000},
    std::pair{"MB"sv,  uint64_t{1000} * 1000},
    std::pair{"KB"sv,  uint64_t{1000}},
    // Legacy single-letter (treat as binary)
    std::pair{"P"sv,   uint64_t{1} << 50},
    std::pair{"T"sv,   uint64_t{1} << 40},
    std::pair{"G"sv,   uint64_t{1} << 30},
    std::pair{"M"sv,   uint64_t{1} << 20},
    std::pair{"K"sv,   uint64_t{1} << 10},
};

/// Minimum recommended EFI partition size: 512 MiB.
constexpr uint64_t kRecommendedEfiSize = 512ULL * 1024 * 1024;

/// Absolute minimum EFI partition size per UEFI spec: 100 MiB.
constexpr uint64_t kMinimumEfiSize = 100ULL * 1024 * 1024;

}  // namespace

auto parse_size_bytes(std::string_view size_str) noexcept -> std::optional<uint64_t> {
    if (size_str.empty()) {
        return std::nullopt;
    }

    auto trimmed = gucc::utils::trim(size_str);

    // Handle "100%" or empty (fill remaining space) — not a concrete size
    if (trimmed == "100%" || trimmed.empty()) {
        return std::nullopt;
    }

    // Try to find a suffix match from the longest to shortest
    for (const auto& [suffix, multiplier] : kSuffixes) {
        if (trimmed.size() >= suffix.size() &&
            trimmed.substr(trimmed.size() - suffix.size()) == suffix) {
            auto num_str = trimmed.substr(0, trimmed.size() - suffix.size());
            uint64_t value{};
            if (std::from_chars(num_str.data(), num_str.data() + num_str.size(), value).ec == std::errc{}) {
                // Overflow guard
                if (value > 0 && multiplier > UINT64_MAX / value) {
                    return std::nullopt;  // would overflow
                }
                return value * multiplier;
            }
            break;
        }
    }

    // No suffix — treat as raw bytes (sector count for sfdisk, but we interpret as bytes here)
    uint64_t value{};
    if (std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), value).ec == std::errc{}) {
        return value;
    }

    return std::nullopt;
}

auto get_total_ram_bytes() noexcept -> uint64_t {
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo.is_open()) {
        return 0;
    }

    std::string line;
    while (std::getline(meminfo, line)) {
        if (line.starts_with("MemTotal:"s)) {
            // Format: "MemTotal:       16384000 kB"
            auto pos = line.find(':');
            if (pos == std::string::npos) return 0;
            auto num_str = line.substr(pos + 1);
            uint64_t kb{};
            if (std::from_chars(num_str.data(), num_str.data() + num_str.size(), kb).ec == std::errc{}) {
                // Overflow guard for very large systems
                if (kb > UINT64_MAX / 1024) return 0;
                return kb * 1024;
            }
            break;
        }
    }
    return 0;
}

auto recommend_swap_size(uint64_t ram_bytes) noexcept -> uint64_t {
    constexpr uint64_t one_gib = uint64_t{1} << 30;

    if (ram_bytes < 2 * one_gib) {
        return 2 * ram_bytes;  // < 2 GiB → 2× RAM
    } else if (ram_bytes < 8 * one_gib) {
        return ram_bytes;      // 2–<8 GiB → equal to RAM
    } else if (ram_bytes < 64 * one_gib) {
        return std::max(ram_bytes / 2, 4 * one_gib);  // 8–<64 GiB → 0.5× RAM, min 4 GiB
    } else {
        return 4 * one_gib;    // ≥ 64 GiB → 4 GiB (or more if hibernation needed)
    }
}

// ---------------------------------------------------------------------------
// Validation report formatting
// ---------------------------------------------------------------------------

auto DiskValidationReport::to_string() const -> std::string {
    std::string out{};

    out += fmt::format(FMT_COMPILE("=== Disk Space Pre-Validation ===\n"));
    out += fmt::format(FMT_COMPILE("Target device: {}\n"), device);

    if (total_disk_bytes > 0) {
        out += fmt::format(FMT_COMPILE("Disk capacity: {} ({:.2f} GiB)\n"),
            gucc::disk::format_size(total_disk_bytes),
            static_cast<double>(total_disk_bytes) / (1ULL << 30));
    }

    if (!checks.empty()) {
        out += fmt::format(FMT_COMPILE("\n--- Partition Size Checks ---\n"));
        out += fmt::format(FMT_COMPILE("{:<20} {:<15} {:<15} {:<8}\n"),
            "Partition", "Available", "Required", "Status");
        out += std::string(60, '-') + "\n";

        for (const auto& check : checks) {
            const auto status = check.fits ? "OK" : "FAIL";
            out += fmt::format(FMT_COMPILE("{:<20} {:<15} {:<15} {}\n"),
                check.partition,
                gucc::disk::format_size(check.available),
                gucc::disk::format_size(check.required),
                status);
        }
    }

    if (!errors.empty()) {
        out += fmt::format(FMT_COMPILE("\n--- ERRORS (installation blocked) ---\n"));
        for (const auto& err : errors) {
            out += fmt::format(FMT_COMPILE("  ✗ {}\n"), err);
        }
    }

    if (!warnings.empty()) {
        out += fmt::format(FMT_COMPILE("\n--- WARNINGS (installation can proceed) ---\n"));
        for (const auto& warn : warnings) {
            out += fmt::format(FMT_COMPILE("  ⚠ {}\n"), warn);
        }
    }

    const auto verdict = is_valid ? "PASS" : "FAIL";
    out += fmt::format(FMT_COMPILE("\nVerdict: {}\n"), verdict);
    return out;
}

// ---------------------------------------------------------------------------
// Main validation entry point
// ---------------------------------------------------------------------------

auto validate_disk_space(const InstallerConfig& config, bool is_efi) noexcept -> DiskValidationReport {
    DiskValidationReport report{};
    report.device = config.device.has_value() ? *config.device : "<not specified>"s;

    // Get disk info from the live environment
    const auto disk_info = gucc::disk::get_disk_info(report.device);
    if (!disk_info) {
        report.is_valid     = false;
        report.errors.emplace_back(fmt::format("Cannot read disk info for '{}'. Is the device present?", report.device));
        return report;
    }

    report.total_disk_bytes = disk_info->size;

    if (report.total_disk_bytes == 0) {
        report.is_valid     = false;
        report.errors.emplace_back(fmt::format("Disk '{}' reports zero size — cannot validate", report.device));
        return report;
    }

    // -----------------------------------------------------------------------
    // Phase 1: Validate each explicitly-sized partition against disk capacity
    // -----------------------------------------------------------------------
    uint64_t explicit_total{};

    for (const auto& part : config.partitions) {
        SizeCheckResult check{};
        check.partition = fmt::format("{} ({})", part.mountpoint, part.type == PartitionType::Root ? "root" :
            (part.type == PartitionType::Boot ? "boot" : "additional"));

        const auto size_opt = parse_size_bytes(part.size);
        if (!size_opt) {
            // No explicit size — this partition will fill remaining space. Skip check.
            continue;
        }

        const uint64_t size = *size_opt;
        check.required      = size;
        check.available     = report.total_disk_bytes;
        check.fits          = (size <= report.total_disk_bytes);

        if (!check.fits) {
            check.message = fmt::format("Partition '{}' requires {} but disk is only {}",
                check.partition, gucc::disk::format_size(size), gucc::disk::format_size(report.total_disk_bytes));
            report.checks.emplace_back(std::move(check));
            report.is_valid     = false;
            report.errors.emplace_back(std::move(check.message));
            continue;
        }

        explicit_total += size;
        check.message   = fmt::format("{} of {} available", gucc::disk::format_size(size), gucc::disk::format_size(report.total_disk_bytes));
        report.checks.emplace_back(std::move(check));
    }

    // -----------------------------------------------------------------------
    // Phase 2: Check total explicit sizes don't exceed disk capacity
    // -----------------------------------------------------------------------
    report.used_by_partitions = explicit_total;
    report.remaining_bytes    = report.total_disk_bytes - explicit_total;

    if (explicit_total > report.total_disk_bytes) {
        const auto overage = explicit_total - report.total_disk_bytes;
        report.is_valid     = false;
        report.errors.emplace_back(fmt::format(
            "Total explicit partition sizes ({}) exceed disk capacity ({}) by {}",
            gucc::disk::format_size(explicit_total),
            gucc::disk::format_size(report.total_disk_bytes),
            gucc::disk::format_size(overage)));
    }

    // -----------------------------------------------------------------------
    // Phase 3: EFI partition size adequacy (UEFI only)
    // -----------------------------------------------------------------------
    if (is_efi) {
        uint64_t efi_size{};
        bool has_efi = false;

        for (const auto& part : config.partitions) {
            const auto fs_lower = to_lower(part.fs_name);
            if (fs_lower == "vfat" || fs_lower == "fat32" || fs_lower == "fat16") {
                has_efi = true;
                if (const auto sz = parse_size_bytes(part.size); sz) {
                    efi_size = *sz;
                }
                break;
            }
        }

        // Also check the config-level fs_name for EFI scenarios
        if (!has_efi && config.fs_name) {
            const auto fs_lower = to_lower(*config.fs_name);
            if (fs_lower == "vfat" || fs_lower == "fat32" || fs_lower == "fat16") {
                has_efi = true;
                efi_size = report.total_disk_bytes;  // EFI takes whole partition
            }
        }

        if (has_efi) {
            SizeCheckResult efi_check{};
            efi_check.partition   = "/boot/efi (ESP)";
            efi_check.required    = kMinimumEfiSize;
            efi_check.available   = efi_size;
            efi_check.fits        = (efi_size >= kMinimumEfiSize);

            if (!efi_check.fits) {
                efi_check.message = fmt::format(
                    "EFI partition is {} — minimum required is {} (UEFI spec). Install may fail.",
                    gucc::disk::format_size(efi_size), gucc::disk::format_size(kMinimumEfiSize));
                report.checks.emplace_back(std::move(efi_check));
                report.is_valid     = false;
                report.errors.emplace_back(std::move(efi_check.message));
            } else if (efi_size < kRecommendedEfiSize) {
                efi_check.message = fmt::format(
                    "EFI partition is {} — meets minimum but {} is recommended for multiple bootloaders.",
                    gucc::disk::format_size(efi_size), gucc::disk::format_size(kRecommendedEfiSize));
                report.checks.emplace_back(std::move(efi_check));
                report.warnings.emplace_back(std::move(efi_check.message));
            } else {
                efi_check.message = fmt::format("EFI partition size ({}) is adequate", gucc::disk::format_size(efi_size));
                report.checks.emplace_back(std::move(efi_check));
            }
        } else if (is_efi) {
            // No EFI partition found in config — this might be caught elsewhere, but flag it
            report.warnings.emplace_back("UEFI system detected but no EFI (vfat) partition found in config");
        }
    }

    // -----------------------------------------------------------------------
    // Phase 4: Swap size relative to RAM
    // -----------------------------------------------------------------------
    const uint64_t ram_bytes = get_total_ram_bytes();
    if (ram_bytes > 0) {
        const auto recommended_swap = recommend_swap_size(ram_bytes);

        // Check configured swap
        bool has_swap = false;
        uint64_t swap_size{};

        for (const auto& part : config.partitions) {
            const auto fs_lower = to_lower(part.fs_name);
            if (fs_lower == "linuxswap" || fs_lower == "swap") {
                has_swap = true;
                if (const auto sz = parse_size_bytes(part.size); sz) {
                    swap_size = *sz;
                }
                break;
            }
        }

        // Also check encrypt_swap flag — it implies a swap partition will be created
        const bool swap_encrypted = config.encrypt_swap && config.device.has_value();

        if (has_swap) {
            SizeCheckResult swap_check{};
            swap_check.partition   = "swap";
            swap_check.required    = recommended_swap;
            swap_check.available   = swap_size;
            swap_check.fits        = (swap_size >= recommended_swap / 2);  // allow 50% tolerance

            if (!swap_check.fits) {
                swap_check.message = fmt::format(
                    "Swap partition ({}) is smaller than recommended ({}) for {} RAM. Consider increasing.",
                    gucc::disk::format_size(swap_size),
                    gucc::disk::format_size(recommended_swap),
                    gucc::disk::format_size(ram_bytes));
                report.checks.emplace_back(std::move(swap_check));
                report.warnings.emplace_back(std::move(swap_check.message));
            } else {
                swap_check.message = fmt::format("Swap size ({}) is adequate for {} RAM",
                    gucc::disk::format_size(swap_size), gucc::disk::format_size(ram_bytes));
                report.checks.emplace_back(std::move(swap_check));
            }
        } else if (swap_encrypted) {
            // Swap will be created as a partition — no size check needed, it'll use the device
            report.warnings.emplace_back("encrypt_swap is enabled but no swap partition defined; swap will be created on the target device");
        } else if (!config.allow_auto_partition && !config.partitions.empty()) {
            // User provided explicit partitions but no swap — warn for systems with > 4 GiB RAM
            if (ram_bytes > 4ULL * (1ULL << 30)) {
                report.warnings.emplace_back(fmt::format(
                    "No swap partition defined. {} RAM detected — consider adding a swap partition.",
                    gucc::disk::format_size(ram_bytes)));
            }
        }
    } else {
        // Could not read RAM info — skip swap validation
        report.warnings.emplace_back("Could not read system RAM (/proc/meminfo unreadable) — swap size check skipped");
    }

    // -----------------------------------------------------------------------
    // Phase 5: Remaining space sanity check
    // -----------------------------------------------------------------------
    if (report.remaining_bytes < 1ULL << 30) {  // less than 1 GiB remaining
        report.warnings.emplace_back(fmt::format(
            "Only {} remaining after explicit partitions — root partition will have minimal space",
            gucc::disk::format_size(report.remaining_bytes)));
    }

    return report;
}

}  // namespace cachyos::installer
