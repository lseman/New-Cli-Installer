#include "doctest_compatibility.h"

#include "cachyos/checkpoint.hpp"
#include "cachyos/types.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>

namespace {
auto init_test_logger() -> void {
    auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>([](const spdlog::details::log_msg&) {});
    auto logger = std::make_shared<spdlog::logger>("test", sink);
    spdlog::set_default_logger(logger);
}
}  // namespace

using namespace std::string_literals;
using namespace std::string_view_literals;

using cachyos::installer::Checkpoint;
using cachyos::installer::InstallContext;
using cachyos::installer::make_checkpoint;
using cachyos::installer::resume_step_range;
using cachyos::installer::save_checkpoint;
using cachyos::installer::load_checkpoint;
using cachyos::installer::remove_checkpoint;
using cachyos::installer::has_checkpoint;

namespace {

auto temp_path() -> std::string {
    return "/tmp/cachyos-test-checkpoint-"s + std::to_string(std::hash<std::string>{}(std::to_string(__COUNTER__)));
}

}  // namespace

TEST_CASE("checkpoint summary and remaining steps")
{
    Checkpoint cp{};
    cp.version = 1;
    cp.last_completed_step = 2;
    cp.total_steps = 5;
    cp.device = "/dev/nvme0n1";
    cp.mountpoint = "/mnt";

    const auto summary = cp.summary();
    REQUIRE(summary.find("[x]") != std::string::npos);
    REQUIRE(summary.find("Base system") != std::string::npos);

    const auto remaining = cp.remaining_steps();
    REQUIRE(remaining.find("[ ]") != std::string::npos);
    REQUIRE(remaining.find("fstab") != std::string::npos);
}

TEST_CASE("checkpoint summary — no steps completed")
{
    Checkpoint cp{};
    cp.version = 1;
    cp.last_completed_step = -1;
    cp.total_steps = 5;

    const auto summary = cp.summary();
    REQUIRE(summary.find("No steps completed") != std::string::npos);
}

TEST_CASE("checkpoint resume step range — middle")
{
    Checkpoint cp{};
    cp.last_completed_step = 3;
    cp.total_steps = 10;

    const auto steps = resume_step_range(cp);
    // Steps 4,5,6,7,8,9 = 6 remaining
    REQUIRE_EQ(steps.size(), 6u);
    REQUIRE_EQ(steps[0], 4);
    REQUIRE_EQ(steps.back(), 9);
}

TEST_CASE("checkpoint resume step range — beginning")
{
    Checkpoint cp{};
    cp.last_completed_step = -1;
    cp.total_steps = 5;

    const auto steps = resume_step_range(cp);
    REQUIRE_EQ(steps.size(), 5u);
    REQUIRE_EQ(steps[0], 0);
}

TEST_CASE("checkpoint resume step range — end")
{
    Checkpoint cp{};
    cp.last_completed_step = 4;
    cp.total_steps = 5;

    const auto steps = resume_step_range(cp);
    REQUIRE(steps.empty());
}

TEST_CASE("make_checkpoint")
{
    InstallContext ctx{};
    ctx.device = "/dev/sda";
    ctx.mountpoint = "/mnt";
    ctx.system_mode = InstallContext::SystemMode::UEFI;
    ctx.hostcache = true;

    auto cp = make_checkpoint(0, ctx, 10);

    REQUIRE_EQ(cp.version, 1u);
    REQUIRE_EQ(cp.last_completed_step, 0);
    REQUIRE_EQ(cp.total_steps, 10);
    REQUIRE_EQ(cp.device, "/dev/sda");
    REQUIRE_EQ(cp.mountpoint, "/mnt");
    REQUIRE(cp.is_efi);
    REQUIRE_EQ(cp.context.device, "/dev/sda");
    REQUIRE_EQ(cp.context.hostcache, true);
}

TEST_CASE("save and load checkpoint")
{
    init_test_logger();
    const auto path = temp_path();
    remove_checkpoint(path);

    Checkpoint cp{};
    cp.version = 1;
    cp.last_completed_step = 5;
    cp.total_steps = 20;
    cp.device = "/dev/nvme0n1";
    cp.mountpoint = "/mnt";
    cp.is_efi = true;
    cp.context.fstab_generated = true;
    cp.context.users_created = true;
    cp.last_error = "simulated failure";

    auto save_res = save_checkpoint(cp, path);
    REQUIRE(save_res.has_value());
    REQUIRE(has_checkpoint(path));

    auto load_res = load_checkpoint(path);
    REQUIRE(load_res.has_value());

    auto& loaded = *load_res;
    REQUIRE_EQ(loaded.version, 1u);
    REQUIRE_EQ(loaded.last_completed_step, 5);
    REQUIRE_EQ(loaded.total_steps, 20);
    REQUIRE_EQ(loaded.device, "/dev/nvme0n1");
    REQUIRE_EQ(loaded.mountpoint, "/mnt");
    REQUIRE(loaded.is_efi);
    REQUIRE(loaded.context.fstab_generated);
    REQUIRE(loaded.context.users_created);
    REQUIRE_EQ(loaded.last_error, "simulated failure");

    remove_checkpoint(path);
    REQUIRE(!has_checkpoint(path));
}

TEST_CASE("load_checkpoint — missing file")
{
    init_test_logger();
    const auto path = temp_path() + "-nonexistent";
    auto res = load_checkpoint(path);
    REQUIRE(!res.has_value());
}

TEST_CASE("load_checkpoint — corrupt JSON")
{
    init_test_logger();
    const auto path = temp_path();
    std::ofstream f(path);
    f << "not valid json {{{";
    f.close();

    auto res = load_checkpoint(path);
    REQUIRE(!res.has_value());

    remove_checkpoint(path);
}

TEST_CASE("load_checkpoint — unsupported version")
{
    init_test_logger();
    const auto path = temp_path();
    std::ofstream f(path);
    f << R"({"version": 99, "last_completed_step": 0, "total_steps": 1})";
    f.close();

    auto res = load_checkpoint(path);
    REQUIRE(!res.has_value());

    remove_checkpoint(path);
}

TEST_CASE("has_checkpoint — clean state")
{
    init_test_logger();
    const auto path = temp_path();
    remove_checkpoint(path);
    REQUIRE(!has_checkpoint(path));

    std::ofstream f(path);
    f << "{}";
    f.close();
    REQUIRE(has_checkpoint(path));

    remove_checkpoint(path);
}
