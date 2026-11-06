// --crc-out writer shared by every alamo_headless run mode. By default it writes the full v1 log (ALAMO_CRC 1: one line
// per tick; the worker-count tests rely on it). With --crc-checkpoint N [--crc-keep T,T,...] it writes the v2 checkpoint
// log instead (ALAMO_CRC 2: the first and last tick, every tick that is a multiple of N and the kept ticks; the checked-in
// goldens use it, see docs/status/golden-checkpoints.md and sim_crc.hpp). Process-wide because one process is one run.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "alamo/core/sim_crc.hpp"

namespace alamo_headless {

struct CrcOutSpec {
    uint32_t checkpoint = 0;     // 0: full v1 log
    std::vector<uint64_t> keep;  // extra ticks to store (v2 only)
};

inline CrcOutSpec& crc_out_spec() {
    static CrcOutSpec spec;
    return spec;
}

// Saves `log` to `path` as the current spec says (full v1 unless --crc-checkpoint was given).
inline bool save_crc_log(const alamo::CrcLog& log, const std::string& path, std::string* err) {
    const CrcOutSpec& s = crc_out_spec();
    return s.checkpoint != 0 ? log.checkpointed(s.checkpoint, s.keep).save(path, err) : log.save(path, err);
}

}  // namespace alamo_headless
