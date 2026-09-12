#include "cachyos/checkpoint.hpp"
#include "cachyos/orchestrator.hpp"

// import gucc
#include "gucc/file_utils.hpp"
#include "gucc/io_utils.hpp"

#include <chrono>     // for system_clock
#include <ctime>      // for time_t, localtime_r
#include <expected>   // for expected, unexpected
#include <fstream>    // for ifstream, ofstream
#include <filesystem> // for exists, remove
#include <ranges>     // for ranges::*
#include <string>     // for string
#include <string_view>// for string_view
#include <vector>     // for vector

#include <fmt/compile.h>
#include <fmt/format.h>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wold-style-cast"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuseless-cast"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif

#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/ostreamwrapper.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

using namespace std::string_view_literals;
using namespace std::string_literals;

namespace cachyos::installer {

// ---------------------------------------------------------------------------
// Checkpoint formatting
// ---------------------------------------------------------------------------

auto Checkpoint::summary() const -> std::string {
    if (last_completed_step < 0) {
        return "No steps completed yet.";
    }

    // Map step indices to names using the orchestrator's step_message
    // We reconstruct the mapping here since we can't include orchestrator.cpp
    static constexpr std::array kStepNames = {
        "Unmount"sv, "Partition"sv, "Base system"sv, "fstab"sv,
        "Encrypted swap"sv, "System settings"sv, "Users"sv, "Machine ID"sv,
        "Desktop"sv, "Desktop config"sv, "Server packages"sv, "SSH keys"sv,
        "Server firewall"sv, "Autologin"sv, "HW drivers"sv, "Network carryover"sv,
        "Bootloader"sv, "Detect crypto"sv, "Enable services"sv, "Final validation"sv,
        "Btrfs snapshot"sv, "Cleanup"sv,
    };

    std::string out{};
    const auto safe_name = [&](int idx) -> std::string_view {
        if (idx < 0 || idx >= static_cast<int>(kStepNames.size())) return "Unknown"s;
        return kStepNames[static_cast<size_t>(idx)];
    };

    for (int i = 0; i <= last_completed_step && i < static_cast<int>(kStepNames.size()); ++i) {
        out += fmt::format(FMT_COMPILE("  [x] {}\n"), safe_name(i));
    }

    return out;
}

auto Checkpoint::remaining_steps() const -> std::string {
    static constexpr std::array kStepNames = {
        "Unmount"sv, "Partition"sv, "Base system"sv, "fstab"sv,
        "Encrypted swap"sv, "System settings"sv, "Users"sv, "Machine ID"sv,
        "Desktop"sv, "Desktop config"sv, "Server packages"sv, "SSH keys"sv,
        "Server firewall"sv, "Autologin"sv, "HW drivers"sv, "Network carryover"sv,
        "Bootloader"sv, "Detect crypto"sv, "Enable services"sv, "Final validation"sv,
        "Btrfs snapshot"sv, "Cleanup"sv,
    };

    std::string out{};
    const auto safe_name = [&](int idx) -> std::string_view {
        if (idx < 0 || idx >= static_cast<int>(kStepNames.size())) return "Unknown"s;
        return kStepNames[static_cast<size_t>(idx)];
    };

    for (int i = last_completed_step + 1; i < total_steps && i < static_cast<int>(kStepNames.size()); ++i) {
        out += fmt::format(FMT_COMPILE("  [ ] {}\n"), safe_name(i));
    }

    if (out.empty()) {
        out = "  (no remaining steps)\n";
    }

    return out;
}

// ---------------------------------------------------------------------------
// Checkpoint I/O
// ---------------------------------------------------------------------------

namespace {

/// @brief Serialize a checkpoint to JSON.
auto checkpoint_to_json(const Checkpoint& cp) -> std::string {
    rapidjson::Document doc;
    doc.SetObject();
    rapidjson::Document::AllocatorType& alloc = doc.GetAllocator();

    doc.AddMember("version", cp.version, alloc);
    doc.AddMember("timestamp", static_cast<rapidjson::SizeType>(cp.timestamp), alloc);
    doc.AddMember("last_completed_step", cp.last_completed_step, alloc);
    doc.AddMember("total_steps", cp.total_steps, alloc);

    // Device and mountpoint
    {
        rapidjson::Value device(rapidjson::kStringType);
        device.SetString(cp.device.data(), static_cast<rapidjson::SizeType>(cp.device.size()), alloc);
        doc.AddMember("device", device, alloc);

        rapidjson::Value mountpoint(rapidjson::kStringType);
        mountpoint.SetString(cp.mountpoint.data(), static_cast<rapidjson::SizeType>(cp.mountpoint.size()), alloc);
        doc.AddMember("mountpoint", mountpoint, alloc);
    }

    doc.AddMember("is_efi", cp.is_efi, alloc);

    // Context snapshot
    {
        rapidjson::Value ctx(rapidjson::kObjectType);
        {
            rapidjson::Value d(rapidjson::kStringType);
            d.SetString(cp.context.device.data(), static_cast<rapidjson::SizeType>(cp.context.device.size()), alloc);
            ctx.AddMember("device", d, alloc);

            rapidjson::Value m(rapidjson::kStringType);
            m.SetString(cp.context.mountpoint.data(), static_cast<rapidjson::SizeType>(cp.context.mountpoint.size()), alloc);
            ctx.AddMember("mountpoint", m, alloc);

            ctx.AddMember("is_efi", cp.context.is_efi, alloc);
            ctx.AddMember("hostcache", cp.context.hostcache, alloc);
            ctx.AddMember("fstab_generated", cp.context.fstab_generated, alloc);
            ctx.AddMember("swap_configured", cp.context.swap_configured, alloc);
            ctx.AddMember("system_settings_applied", cp.context.system_settings_applied, alloc);
            ctx.AddMember("users_created", cp.context.users_created, alloc);
            ctx.AddMember("desktop_installed", cp.context.desktop_installed, alloc);
            ctx.AddMember("bootloader_installed", cp.context.bootloader_installed, alloc);
            ctx.AddMember("services_enabled", cp.context.services_enabled, alloc);

            // Partition devices array
            if (!cp.context.partition_devices.empty()) {
                rapidjson::Value parts(rapidjson::kArrayType);
                for (const auto& dev : cp.context.partition_devices) {
                    rapidjson::Value s(rapidjson::kStringType);
                    s.SetString(dev.data(), static_cast<rapidjson::SizeType>(dev.size()), alloc);
                    parts.PushBack(s, alloc);
                }
                ctx.AddMember("partition_devices", parts, alloc);
            }
        }
        doc.AddMember("context", ctx, alloc);
    }

    // Last error
    if (!cp.last_error.empty()) {
        rapidjson::Value err(rapidjson::kStringType);
        err.SetString(cp.last_error.data(), static_cast<rapidjson::SizeType>(cp.last_error.size()), alloc);
        doc.AddMember("last_error", err, alloc);
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    doc.Accept(writer);
    return buf.GetString();
}

/// @brief Deserialize a checkpoint from JSON.
auto checkpoint_from_json(std::string_view json) -> std::expected<Checkpoint, std::string> {
    Checkpoint cp{};

    if (json.empty()) {
        return std::unexpected("Empty checkpoint data");
    }

    rapidjson::Document doc;
    doc.Parse(json.data(), json.size());
    if (doc.HasParseError() || doc.IsNull() || !doc.IsObject()) {
        return std::unexpected("Checkpoint file contains invalid JSON");
    }

    // Version check — only support v1
    if (doc.HasMember("version") && doc["version"].IsUint()) {
        cp.version = doc["version"].GetUint();
        if (cp.version != 1) {
            return std::unexpected(fmt::format("Unsupported checkpoint version: {}", cp.version));
        }
    }

    // Basic fields
    if (doc.HasMember("timestamp") && doc["timestamp"].IsUint64()) {
        cp.timestamp = doc["timestamp"].GetUint64();
    }
    if (doc.HasMember("last_completed_step") && doc["last_completed_step"].IsInt()) {
        cp.last_completed_step = doc["last_completed_step"].GetInt();
    }
    if (doc.HasMember("total_steps") && doc["total_steps"].IsInt()) {
        cp.total_steps = doc["total_steps"].GetInt();
    }

    // Device and mountpoint
    if (doc.HasMember("device") && doc["device"].IsString()) {
        cp.device = doc["device"].GetString();
    }
    if (doc.HasMember("mountpoint") && doc["mountpoint"].IsString()) {
        cp.mountpoint = doc["mountpoint"].GetString();
    }
    if (doc.HasMember("is_efi") && doc["is_efi"].IsBool()) {
        cp.is_efi = doc["is_efi"].GetBool();
    }

    // Context snapshot
    if (doc.HasMember("context") && doc["context"].IsObject()) {
        const auto& ctx = doc["context"];
        if (ctx.HasMember("device") && ctx["device"].IsString()) {
            cp.context.device = ctx["device"].GetString();
        }
        if (ctx.HasMember("mountpoint") && ctx["mountpoint"].IsString()) {
            cp.context.mountpoint = ctx["mountpoint"].GetString();
        }
        if (ctx.HasMember("is_efi") && ctx["is_efi"].IsBool()) {
            cp.context.is_efi = ctx["is_efi"].GetBool();
        }
        if (ctx.HasMember("hostcache") && ctx["hostcache"].IsBool()) {
            cp.context.hostcache = ctx["hostcache"].GetBool();
        }
        if (ctx.HasMember("fstab_generated") && ctx["fstab_generated"].IsBool()) {
            cp.context.fstab_generated = ctx["fstab_generated"].GetBool();
        }
        if (ctx.HasMember("swap_configured") && ctx["swap_configured"].IsBool()) {
            cp.context.swap_configured = ctx["swap_configured"].GetBool();
        }
        if (ctx.HasMember("system_settings_applied") && ctx["system_settings_applied"].IsBool()) {
            cp.context.system_settings_applied = ctx["system_settings_applied"].GetBool();
        }
        if (ctx.HasMember("users_created") && ctx["users_created"].IsBool()) {
            cp.context.users_created = ctx["users_created"].GetBool();
        }
        if (ctx.HasMember("desktop_installed") && ctx["desktop_installed"].IsBool()) {
            cp.context.desktop_installed = ctx["desktop_installed"].GetBool();
        }
        if (ctx.HasMember("bootloader_installed") && ctx["bootloader_installed"].IsBool()) {
            cp.context.bootloader_installed = ctx["bootloader_installed"].GetBool();
        }
        if (ctx.HasMember("services_enabled") && ctx["services_enabled"].IsBool()) {
            cp.context.services_enabled = ctx["services_enabled"].GetBool();
        }
        if (ctx.HasMember("partition_devices") && ctx["partition_devices"].IsArray()) {
            for (const auto& elem : ctx["partition_devices"].GetArray()) {
                if (elem.IsString()) {
                    cp.context.partition_devices.emplace_back(elem.GetString());
                }
            }
        }
    }

    // Last error
    if (doc.HasMember("last_error") && doc["last_error"].IsString()) {
        cp.last_error = doc["last_error"].GetString();
    }

    return cp;
}

}  // namespace

auto load_checkpoint(std::string_view path) noexcept -> std::expected<Checkpoint, std::string> {
    const auto content = gucc::file_utils::read_whole_file(path);
    if (content.empty()) {
        return std::unexpected("Checkpoint file not found or empty");
    }
    return checkpoint_from_json(content);
}

auto save_checkpoint(const Checkpoint& cp, std::string_view path) noexcept
    -> std::expected<void, std::string> {
    const auto json = checkpoint_to_json(cp);
    std::ofstream out(std::string{path}, std::ios::trunc);
    if (!out.is_open()) {
        return std::unexpected(fmt::format("Cannot open checkpoint file for writing: {}", path));
    }
    out << json;
    out.flush();
    if (!out) {
        return std::unexpected(fmt::format("Failed to write checkpoint to {}", path));
    }
    return {};
}

auto remove_checkpoint(std::string_view path) noexcept -> void {
    std::error_code ec;
    std::filesystem::remove(std::string{path}, ec);
}

auto has_checkpoint(std::string_view path) noexcept -> bool {
    return std::filesystem::exists(std::string{path}) && !gucc::file_utils::read_whole_file(path).empty();
}

auto make_checkpoint(int last_completed, const InstallContext& ctx, int total_steps) noexcept -> Checkpoint {
    Checkpoint cp{};
    cp.version = 1;
    cp.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    cp.last_completed_step = last_completed;
    cp.total_steps = total_steps;

    cp.device = ctx.device;
    cp.mountpoint = ctx.mountpoint;
    cp.is_efi = (ctx.system_mode == InstallContext::SystemMode::UEFI);

    cp.context.device = ctx.device;
    cp.context.mountpoint = ctx.mountpoint;
    cp.context.is_efi = cp.is_efi;
    cp.context.hostcache = ctx.hostcache;

    // Collect partition devices from the strategy
    if (const auto* layout = std::get_if<partition_strategy::CreateLayout>(&ctx.strategy); layout) {
        for (const auto& part : layout->partitions) {
            cp.context.partition_devices.emplace_back(part.device);
        }
    }

    return cp;
}

auto resume_step_range(const Checkpoint& cp) noexcept -> std::vector<int> {
    std::vector<int> steps{};
    // Resume from the step after the last completed one
    const int start = std::max(0, cp.last_completed_step + 1);
    for (int i = start; i < cp.total_steps; ++i) {
        steps.emplace_back(i);
    }
    return steps;
}

}  // namespace cachyos::installer
