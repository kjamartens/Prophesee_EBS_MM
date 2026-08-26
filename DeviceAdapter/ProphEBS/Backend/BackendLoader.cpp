///////////////////////////////////////////////////////////////////////////////
// FILE:          BackendLoader.cpp
// PROJECT:       Micro-Manager
// SUBSYSTEM:     DeviceAdapters / ProphEBS
//-----------------------------------------------------------------------------
// See BackendLoader.h for the design rationale.
//
// COPYRIGHT:     Koen J.A. Martens, 2026
// LICENSE:       This file is distributed under the BSD license.
///////////////////////////////////////////////////////////////////////////////

#include "BackendLoader.h"

#include <windows.h>

#include <vector>

// Add one row here per supported Metavision SDK generation, once its
// backend project has actually been built -- see docs/BUILD_AND_USAGE.md,
// "Adding a new Metavision SDK generation backend."
static const ProphEBSBackendCandidate g_BackendCandidates[] = {
   { "sdk511", 5, 1, 1, "ProphEBS_Backend_SDK511.dll" },
   { "sdk510", 5, 1, 0, "ProphEBS_Backend_SDK510.dll" },
   { "sdk500", 5, 0, 0, "ProphEBS_Backend_SDK500.dll" },
   { "sdk430", 4, 3, 0, "ProphEBS_Backend_SDK430.dll" },
};

ProphEBSBackendHandle& ProphEBSBackendHandle::operator=(ProphEBSBackendHandle&& other) noexcept
{
   if (this != &other)
   {
      Reset();
      module_ = other.module_;
      backend_ = other.backend_;
      destroyFn_ = other.destroyFn_;
      tag_ = std::move(other.tag_);
      other.module_ = nullptr;
      other.backend_ = nullptr;
      other.destroyFn_ = nullptr;
   }
   return *this;
}

void ProphEBSBackendHandle::Reset()
{
   if (backend_ && destroyFn_)
      destroyFn_(backend_);
   backend_ = nullptr;
   destroyFn_ = nullptr;
   if (module_)
      FreeLibrary(static_cast<HMODULE>(module_));
   module_ = nullptr;
   tag_.clear();
}

// Resolves a DLL's full path via the exact same search order LoadLibrary
// itself would use, without loading it -- SearchPathA just walks that order
// and reports where it would find the file. Used so
// GetInstalledMetavisionVersion() reads the version resource off whichever
// metavision_sdk_base.dll would actually be loaded (e.g. by a backend DLL
// sitting next to this one), not some unrelated copy elsewhere on disk.
static bool ResolveDllPath(const char* dllName, std::string& outPath)
{
   char buf[MAX_PATH];
   DWORD len = SearchPathA(nullptr, dllName, nullptr, MAX_PATH, buf, nullptr);
   if (len == 0 || len >= MAX_PATH)
      return false;
   outPath.assign(buf, len);
   return true;
}

bool GetInstalledMetavisionVersion(int& major, int& minor, int& patch, std::string& resolvedPath)
{
   if (!ResolveDllPath("metavision_sdk_base.dll", resolvedPath))
      return false;

   DWORD handle = 0;
   DWORD infoSize = GetFileVersionInfoSizeA(resolvedPath.c_str(), &handle);
   if (infoSize == 0)
      return false;

   std::vector<char> data(infoSize);
   if (!GetFileVersionInfoA(resolvedPath.c_str(), handle, infoSize, data.data()))
      return false;

   VS_FIXEDFILEINFO* fixed = nullptr;
   UINT fixedLen = 0;
   if (!VerQueryValueA(data.data(), "\\", reinterpret_cast<LPVOID*>(&fixed), &fixedLen) || !fixed)
      return false;

   // Metavision packs major.minor into the high DWORD and build.revision
   // into the low DWORD -- confirmed against this project's own vendored
   // SDKs/<version>/bin/metavision_sdk_base.dll files (e.g. 5.1.1 reads back
   // as FileMajorPart=5, FileMinorPart=1, FileBuildPart=1).
   major = static_cast<int>(HIWORD(fixed->dwFileVersionMS));
   minor = static_cast<int>(LOWORD(fixed->dwFileVersionMS));
   patch = static_cast<int>(HIWORD(fixed->dwFileVersionLS));
   return true;
}

ProphEBSBackendHandle ProphEBSBackendHandle::LoadNamed(
   const char* backendDllName, const char* tag, std::string& errorOut)
{
   ProphEBSBackendHandle handle;

   // Plain LoadLibraryA relies on the default search order's "directory the
   // calling executable/DLL was loaded from" entry -- ProphEBS_Backend_*.dll
   // is staged into the same MicroManager install folder as
   // mmgr_dal_ProphEBS.dll itself by the build's post-build step, so this
   // resolves without needing to compute our own module directory.
   HMODULE mod = LoadLibraryA(backendDllName);
   if (!mod)
   {
      errorOut += std::string("could not load ") + backendDllName +
         " (error " + std::to_string(GetLastError()) + "); ";
      return handle;
   }

   auto createFn = reinterpret_cast<IProphEBSBackend* (*)()>(
      GetProcAddress(mod, PROPHEBS_BACKEND_CREATE_FN_NAME));
   auto destroyFn = reinterpret_cast<void (*)(IProphEBSBackend*)>(
      GetProcAddress(mod, PROPHEBS_BACKEND_DESTROY_FN_NAME));
   auto abiTagFn = reinterpret_cast<ProphEBSBackendAbiTagFn>(
      GetProcAddress(mod, PROPHEBS_BACKEND_ABITAG_FN_NAME));
   if (!createFn || !destroyFn || !abiTagFn)
   {
      errorOut += std::string(backendDllName) + " is missing one or more required exports; ";
      FreeLibrary(mod);
      return handle;
   }

   const char* abiTag = abiTagFn();
   if (!abiTag || std::string(abiTag) != PROPHEBS_BACKEND_ABI_TAG)
   {
      errorOut += std::string(backendDllName) + " reports ABI tag '" +
         (abiTag ? abiTag : "(null)") + "', expected '" + PROPHEBS_BACKEND_ABI_TAG +
         "' -- stale/mismatched build, refusing to use it; ";
      FreeLibrary(mod);
      return handle;
   }

   IProphEBSBackend* backend = createFn();
   if (!backend)
   {
      errorOut += std::string(backendDllName) + "'s CreateProphEBSBackend() returned null; ";
      FreeLibrary(mod);
      return handle;
   }

   handle.module_ = mod;
   handle.backend_ = backend;
   handle.destroyFn_ = destroyFn;
   handle.tag_ = tag;
   return handle;
}

ProphEBSBackendHandle LoadBestAvailableBackend(std::string& errorOut)
{
   int installedMajor = 0, installedMinor = 0, installedPatch = 0;
   std::string resolvedPath;
   if (!GetInstalledMetavisionVersion(installedMajor, installedMinor, installedPatch, resolvedPath))
   {
      errorOut += "no installed Metavision SDK found (metavision_sdk_base.dll not on the search path); ";
      return ProphEBSBackendHandle();
   }

   // Pass 1: exact major.minor.patch match.
   for (const ProphEBSBackendCandidate& candidate : g_BackendCandidates)
   {
      if (candidate.major == installedMajor && candidate.minor == installedMinor &&
         candidate.patch == installedPatch)
      {
         ProphEBSBackendHandle handle = ProphEBSBackendHandle::LoadNamed(
            candidate.backendDllName, candidate.tag, errorOut);
         if (handle.IsLoaded())
            return handle;
      }
   }

   // Pass 2: same major.minor, different patch -- Metavision patch releases
   // within a minor version are assumed (not guaranteed) to be ABI-stable
   // bugfix-only updates. Logged explicitly since this is a real assumption,
   // not a confirmed fact for every generation.
   for (const ProphEBSBackendCandidate& candidate : g_BackendCandidates)
   {
      if (candidate.major == installedMajor && candidate.minor == installedMinor)
      {
         errorOut += std::string("no backend built for exactly ") + std::to_string(installedMajor) +
            "." + std::to_string(installedMinor) + "." + std::to_string(installedPatch) +
            " -- trying nearest match '" + candidate.tag + "' (" + std::to_string(candidate.major) +
            "." + std::to_string(candidate.minor) + "." + std::to_string(candidate.patch) +
            ") instead, same major.minor; ";
         ProphEBSBackendHandle handle = ProphEBSBackendHandle::LoadNamed(
            candidate.backendDllName, candidate.tag, errorOut);
         if (handle.IsLoaded())
            return handle;
      }
   }

   errorOut += "installed Metavision SDK is " + std::to_string(installedMajor) + "." +
      std::to_string(installedMinor) + "." + std::to_string(installedPatch) + " (" + resolvedPath +
      "), but no ProphEBS backend covers that generation; ";
   return ProphEBSBackendHandle();
}
