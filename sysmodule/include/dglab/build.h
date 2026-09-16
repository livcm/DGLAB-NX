#pragma once

// Which build of the sysmodule this is.
//
// The console keeps a boot2 sysmodule that has to be replaced by hand, so "I
// rebuilt, but the console is still running yesterday's binary" is a real
// failure mode - and it looks exactly like a bug: the old process dies, every
// IPC goes unanswered, and the front end reads that as "it froze" (the round
// this rule came from: docs/dglab-socket.md, "栈上不要放 KB 级缓冲区").
//
// The Makefile stamps the binary with `git describe --always --dirty`, and the
// transport writes that stamp into the log at startup and at every server
// start, so `dglab-sys.log` says which build produced it. A log with no stamp
// line at all is an old build.
#ifndef DGLAB_BUILD_STAMP
#define DGLAB_BUILD_STAMP "unknown"
#endif
