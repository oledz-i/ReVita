// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>

// Writes what killed the app into vita3k-runtime.log: the exception, the
// module and offset it happened in and a short stack, or abort() and
// std::terminate. Without it a crash ended the log mid-line with no cause.
//
// Installed at startup and put back at the head of the filter chain after
// the core loads (ReassertCrashLog): it lets the filters after it (the core's
// guest memory fault filter, UwpMemoryFaultHandler.h) try first, so faults
// they recover from are not logged and the rest are. A fail-fast ends the
// process with no filter called; the session marker (CatalogShell) catches
// those on the next start.
namespace vita3k_uwp {
void InstallCrashLog(const std::wstring& logPath);
// For a crash the app noticed itself (a XAML unhandled exception).
void WriteCrashLine(const char* text);
// Puts the crash log back at the head of the process's exception filters
// (call after the core loads, and now and then). Safe to call any time.
void ReassertCrashLog();
// Forget the filter chained after the crash log (before unloading the core).
void DetachCrashLogChain();
}
