// Forward declarations of the sim CRC harness types (sim_crc.hpp). Public headers that only name
// them by reference (hash_state(StateHasher&) const, register_crc(CrcRegistry&)) include this
// instead of the full header; the .cpp that uses the members includes sim_crc.hpp.
#pragma once

namespace alamo {
class StateHasher;
class CrcSum;
class CrcRegistry;
struct CrcRecord;
struct CrcLog;
struct CrcDivergence;
struct ParityFrameOptions;
}  // namespace alamo
