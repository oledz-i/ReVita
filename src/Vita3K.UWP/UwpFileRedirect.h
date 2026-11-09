// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <string>
#include <vector>

// Lets the Vita3K core (and the C runtime it calls) reach USB drives and
// other storage the user granted, the way OnyxEMU does for Azahar.
//
// A UWP app's ordinary Win32 file calls (CreateFileW, _wfopen, std::filesystem)
// only reach the app's own folders. The *FromApp variants also reach storage
// granted through broadFileSystemAccess / removableStorage. Vita3K opens files
// in hundreds of places, so instead of patching each one, the file functions
// a module imports are pointed at wrappers. Win32 calls on paths inside the
// app's own storage keep the original functions (the FromApp ones go through
// a broker and are many times slower); other paths go to the FromApp
// variants. The C runtime's fopen family tries the original first and, when
// it fails, opens the file through CreateFile2FromAppW.
namespace vita3k_uwp {
// The app's own storage (its package data folder and install folder). Paths
// under these use the plain, fast file functions. Call before redirecting.
void SetAppStorageRoots(const std::vector<std::wstring>& roots);
// Redirects the imports of one loaded module. Safe to call more than once:
// slots already redirected are left alone.
// Returns the number of import slots redirected.
int RedirectFileImports(HMODULE module);
// The module that contains an address (for runtime DLLs whose handle we
// cannot ask for by name in an app container).
HMODULE ModuleFromAddress(const void* address);
// Redirects the core, and the C/C++ runtime DLLs it imports file functions
// through (msvcp140_app, ucrtbase). Returns a short report for the log.
std::string RedirectCoreFileAccess(HMODULE core);
}
