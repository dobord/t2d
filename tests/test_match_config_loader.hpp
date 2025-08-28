// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "server/matchmaking/matchmaker.hpp"

#include <yaml-cpp/yaml.h>

#include <exception>
#include <string>

namespace t2d::test {

// Apply YAML overrides onto an existing MatchConfig (only keys present are overridden).
// This lets each test keep its fast baseline values unless explicitly changed in the file.
inline void apply_match_config_overrides(t2d::mm::MatchConfig &cfg, const std::string &path)
{
    try {
        YAML::Node root = YAML::LoadFile(path);
        if (root["max_players_per_match"])
            cfg.max_players = root["max_players_per_match"].as<uint32_t>();
        if (root["fill_timeout_seconds"])
            cfg.fill_timeout_seconds = root["fill_timeout_seconds"].as<uint32_t>();
        if (root["tick_rate"])
            cfg.tick_rate = root["tick_rate"].as<uint32_t>();
        if (root["matchmaker_poll_ms"])
            cfg.poll_interval_ms = root["matchmaker_poll_ms"].as<uint32_t>();
        if (root["snapshot_interval_ticks"])
            cfg.snapshot_interval_ticks = root["snapshot_interval_ticks"].as<uint32_t>();
        if (root["full_snapshot_interval_ticks"])
            cfg.full_snapshot_interval_ticks = root["full_snapshot_interval_ticks"].as<uint32_t>();
        if (root["bot_fire_interval_ticks"])
            cfg.bot_fire_interval_ticks = root["bot_fire_interval_ticks"].as<uint32_t>();
        if (root["movement_speed"])
            cfg.movement_speed = root["movement_speed"].as<float>();
        if (root["projectile_damage"])
            cfg.projectile_damage = root["projectile_damage"].as<uint32_t>();
        if (root["reload_interval_sec"])
            cfg.reload_interval_sec = root["reload_interval_sec"].as<float>();
        if (root["projectile_speed"])
            cfg.projectile_speed = root["projectile_speed"].as<float>();
        if (root["projectile_density"])
            cfg.projectile_density = root["projectile_density"].as<float>();
        if (root["fire_cooldown_sec"])
            cfg.fire_cooldown_sec = root["fire_cooldown_sec"].as<float>();
        if (root["hull_density"])
            cfg.hull_density = root["hull_density"].as<float>();
        if (root["turret_density"])
            cfg.turret_density = root["turret_density"].as<float>();
        if (root["disable_bot_fire"])
            cfg.disable_bot_fire = root["disable_bot_fire"].as<bool>();
        if (root["disable_bot_ai"])
            cfg.disable_bot_ai = root["disable_bot_ai"].as<bool>();
        if (root["test_mode"])
            cfg.test_mode = root["test_mode"].as<bool>();
        if (root["map_width"])
            cfg.map_width = root["map_width"].as<float>();
        if (root["map_height"])
            cfg.map_height = root["map_height"].as<float>();
        if (root["force_line_spawn"])
            cfg.force_line_spawn = root["force_line_spawn"].as<bool>();
    } catch (const std::exception &) {
        // Swallow errors: tests fall back to embedded defaults if file missing or invalid.
    }
}

} // namespace t2d::test
