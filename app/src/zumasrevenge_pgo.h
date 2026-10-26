// Peggle: profile collection of the PGO-instrumented Switch build (ZUMASREVENGE_PGO=generate), see zumasrevenge_pgo.cpp.
// In every other build these do nothing. From sa2-nx (sa2_pgo.h).

#pragma once

namespace zumasrevenge::pgo {

// Picks this session's file (pgo/zumasrevenge_pgo_<N>.stream next to the executable) and starts the 2-minute dumps.
void Start();
// Writes every count so far to this session's file (the whole session each time, replacing the previous dump).
void DumpNow();
// The last dump, then the dump thread ends.
void Stop();

}  // namespace zumasrevenge::pgo
