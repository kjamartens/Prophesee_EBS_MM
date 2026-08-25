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

// Add one row here per supported Metavision SDK generation. Tried in order;
// first markerDll that actually resolves on this machine wins. See
// docs/BUILD_AND_USAGE.md, "Adding a new Metavision SDK generation backend."
static const ProphEBSBackendCandidate g_BackendCandidates[] = {
   { "sdk5x", "metavision_sdk_stream.dll", "ProphEBS_Backend_SDK5x.dll" },
   { "sdk43", "metavision_sdk_driver.dll", "ProphEBS_Backend_SDK43.dll" },
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

// True if markerDll can be found on the standard DLL search path without
// actually running any of its code -- LOAD_LIBRARY_AS_DATAFILE +
// DONT_RESOLVE_DLL_REFERENCES loads just enough to confirm the file exists
// and is a valid PE image, with no DllMain/static initializers executed and
// no dependency resolution attempted (so this is safe to try even for an SDK
// generation that isn't actually the one physically installed).
static bool MarkerDllResolvable(const char* markerDll)
{
   HMODULE h = LoadLibraryExA(markerDll, nullptr,
      LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE | DONT_RESOLVE_DLL_REFERENCES);
   if (!h)
      return false;
   FreeLibrary(h);
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
   for (const ProphEBSBackendCandidate& candidate : g_BackendCandidates)
   {
      if (!MarkerDllResolvable(candidate.markerDll))
      {
         errorOut += std::string(candidate.markerDll) + " not found (SDK generation '" +
            candidate.tag + "' not installed); ";
         continue;
      }

      ProphEBSBackendHandle handle = ProphEBSBackendHandle::LoadNamed(
         candidate.backendDllName, candidate.tag, errorOut);
      if (handle.IsLoaded())
         return handle;
   }
   return ProphEBSBackendHandle();
}
