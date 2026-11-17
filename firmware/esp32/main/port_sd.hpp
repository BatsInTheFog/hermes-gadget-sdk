// The microSD card, shared: one mount at /sdcard for everything that uses the card (the log
// mirror in port_sdlog.cpp, and the `sd` console command for checking a card), so two users
// never mount the slot twice. SDMMC, 4-bit or 1-bit as the board's SdConfig says.
//
// Console (answered on the console task, not the app task, so the face keeps going):
//   sd               the card: mounted or why not, name, type, size, bus speed, FAT space
//   sd ls [dir]      a directory's entries (default /sdcard)
//   sd test [MB]     writes a test file of MB megabytes (default 4), reads it back and checks
//                    every byte, then deletes it; reports write and read speed
#pragma once

#include <string>

#include "board.hpp"  // SdConfig
#include "sdmmc_cmd.h"

namespace hgp::sd {

// The board's slot, once at start-up (before anything mounts).
void configure(const SdConfig& sd);
// Mounts the card if it isn't yet: the card, or null (`err` says why). Thread-safe.
sdmmc_card_t* mount(esp_err_t* err = nullptr);
// Free space on the mounted card in MB (0 when not mounted).
uint32_t free_mb();
// After a failed write: the card was pulled or failed; the next mount() tries again.
void unmount();
constexpr const char* kMountPoint = "/sdcard";

// The `sd` console command (see above); args after "sd".
std::string command(int argc, char** argv);

}  // namespace hgp::sd
