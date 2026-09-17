#pragma once

// Which release of the front end this is, and which build of it.
//
// Two version numbers live in this project and they are not the same number
// (docs/ipc.md, "版本"):
//
//   - the release version of this NRO. It is written down once, in VERSION at
//     the repository root; the NACP carries it, the About page shows it, and
//     nro/Makefile passes it in here;
//   - the IPC interface version, DGLAB_IPC_PROTOCOL_VERSION in dglab/ipc.h. It
//     belongs to the sysmodule and reaches the front end over GET_VERSION.
//
// The build stamp answers a third question - "did the console get the build I
// just made?". That is a real failure mode for a front end copied onto the SD
// card by hand: the About page of the old build looks exactly like the new one
// otherwise (sysmodule/include/dglab/build.h makes the same point for the
// sysmodule). nro/Makefile stamps it with `git describe --always --dirty`.
//
// Both are defined by the build. A compilation that never saw them - a host
// test, or a build made by hand - gets the placeholder instead, which shows up
// on the About page rather than pretending to be a release.
#ifndef DGLAB_APP_VERSION
#define DGLAB_APP_VERSION "unknown"
#endif

#ifndef DGLAB_BUILD_STAMP
#define DGLAB_BUILD_STAMP "unknown"
#endif
