///////////////////////////////////////////////////////////////////////////////
// FILE:          BackendLoader.h
// PROJECT:       Micro-Manager
// SUBSYSTEM:     DeviceAdapters / ProphEBS
//-----------------------------------------------------------------------------
// DESCRIPTION:   Probes which Metavision SDK generation is actually
//                installed on this machine and dynamically loads the
//                matching ProphEBS_Backend_<tag>.dll -- see
//                IProphEBSBackend.h for why this exists instead of a
//                compile-time #if.
//
//                To add a new SDK generation: build a new backend project
//                implementing IProphEBSBackend (copy an existing
//                Backend/SDK*/ project as a template), add one row to
//                g_BackendCandidates in BackendLoader.cpp naming its marker
//                DLL and its own output DLL name, and add it to
//                ProphEBS.sln's post-build staging step. See
//                docs/BUILD_AND_USAGE.md, "Adding a new Metavision SDK
//                generation backend."
//
// COPYRIGHT:     Koen J.A. Martens, 2026
// LICENSE:       This file is distributed under the BSD license, consistent
//                with the rest of the Micro-Manager device adapter kit.
//
//                This file is distributed in the hope that it will be useful,
//                but WITHOUT ANY WARRANTY; without even the implied warranty
//                of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

#pragma once

#include "IProphEBSBackend.h"

#include <string>

// One entry per supported Metavision SDK generation, tried in array order
// (first whose markerDll actually resolves on this machine wins).
struct ProphEBSBackendCandidate
{
   const char* tag;            // short id, e.g. "sdk5x" -- for logging only
   const char* markerDll;      // a DLL name unique to this generation, e.g.
                                // "metavision_sdk_stream.dll" (5.x) or
                                // "metavision_sdk_driver.dll" (4.3.0) --
                                // presence of this DLL on the standard search
                                // path is how the installed generation is
                                // detected, without needing to actually load
                                // the (potentially large/side-effectful)
                                // Metavision SDK itself just to probe.
   const char* backendDllName; // e.g. "ProphEBS_Backend_SDK5x.dll" -- must
                                // be staged next to mmgr_dal_ProphEBS.dll.
};

// RAII owner of one loaded backend module. Move-only.
class ProphEBSBackendHandle
{
public:
   ProphEBSBackendHandle() = default;
   ~ProphEBSBackendHandle() { Reset(); }

   ProphEBSBackendHandle(const ProphEBSBackendHandle&) = delete;
   ProphEBSBackendHandle& operator=(const ProphEBSBackendHandle&) = delete;
   ProphEBSBackendHandle(ProphEBSBackendHandle&& other) noexcept { *this = std::move(other); }
   ProphEBSBackendHandle& operator=(ProphEBSBackendHandle&& other) noexcept;

   bool IsLoaded() const { return backend_ != nullptr; }
   IProphEBSBackend* Get() const { return backend_; }
   const char* Tag() const { return tag_.c_str(); }

   void Reset();

   // Implemented in BackendLoader.cpp -- constructs a handle by LoadLibrary-
   // ing backendDllName (expected alongside this DLL's own module), resolving
   // its three required exports, and verifying its ABI tag. On any failure,
   // returns a default (unloaded) handle and appends the reason to errorOut.
   static ProphEBSBackendHandle LoadNamed(const char* backendDllName, const char* tag, std::string& errorOut);

private:
   void* module_ = nullptr;       // HMODULE, opaque here to avoid <windows.h> in this header
   IProphEBSBackend* backend_ = nullptr;
   void (*destroyFn_)(IProphEBSBackend*) = nullptr;
   std::string tag_;
};

// Tries every entry in the built-in candidate table (BackendLoader.cpp) in
// order; returns the first one that both (a) has its markerDll resolvable on
// this machine and (b) actually loads and passes the ABI-tag check. If none
// match, returns an unloaded handle and appends a human-readable reason
// (mirroring ConnectToCamera()'s existing "why not connected" logging
// convention) to errorOut.
ProphEBSBackendHandle LoadBestAvailableBackend(std::string& errorOut);
