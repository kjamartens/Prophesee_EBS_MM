///////////////////////////////////////////////////////////////////////////////
// FILE:          BackendLoader.h
// PROJECT:       Micro-Manager
// SUBSYSTEM:     DeviceAdapters / ProphEBS
//-----------------------------------------------------------------------------
// DESCRIPTION:   Probes which Metavision SDK generation is actually
//                installed on this machine and dynamically loads the
//                matching ProphEBS_Backend_SDK<version>.dll -- see
//                IProphEBSBackend.h for why this exists instead of a
//                compile-time #if.
//
//                Detection is by real file version, not just DLL presence:
//                metavision_sdk_base.dll exists (and is common to every SDK
//                generation this project has seen so far, whether the
//                "stream" or "driver" module is installed alongside it) and
//                carries a normal Win32 version resource, so
//                GetInstalledMetavisionVersion() reads its
//                major.minor.build straight from that -- see the .cpp. This
//                is required (not just convenient) once more than one
//                installed generation can share the same module name: SDK
//                5.0.0/5.1.0/5.1.1 all ship metavision_sdk_stream.dll, so
//                "which marker DLL exists" alone can no longer distinguish
//                them the way it could when there were only two generations
//                (4.3.0 "driver" vs. everything-else "stream").
//
//                To add a new SDK generation: build a new backend project
//                implementing IProphEBSBackend (copy an existing
//                Backend/SDK*/ project as a template -- SDK511 if the new
//                generation ships the "stream" module, SDK430 if somehow it
//                doesn't), add one row to g_BackendCandidates in
//                BackendLoader.cpp naming its exact version and its own
//                output DLL name, and add it to ProphEBS.sln. See
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

// One entry per supported (built) Metavision SDK generation. major/minor/
// patch identify exactly which installed version this backend was built
// against -- see GetInstalledMetavisionVersion() in BackendLoader.cpp for
// how the installed version is actually read.
struct ProphEBSBackendCandidate
{
   const char* tag;            // short id, e.g. "sdk511" -- for logging only
   int major;
   int minor;
   int patch;
   const char* backendDllName; // e.g. "ProphEBS_Backend_SDK511.dll" -- must
                                // be staged next to mmgr_dal_ProphEBS.dll.
};

// Reads metavision_sdk_base.dll's Win32 file version resource (major.minor.
// build) off the standard DLL search path, without loading/executing the
// DLL itself. Returns false (leaving the out-params untouched) if no
// Metavision SDK install can be found on the search path at all.
bool GetInstalledMetavisionVersion(int& major, int& minor, int& patch, std::string& resolvedPath);

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

// Reads the installed Metavision SDK's version (GetInstalledMetavisionVersion())
// and picks the matching entry from the built-in candidate table
// (BackendLoader.cpp): an exact major.minor.patch match if one exists;
// otherwise the candidate sharing the same major.minor (any patch), logged
// as an inexact match via errorOut since patch-level ABI compatibility
// within a minor version is an assumption, not a guarantee; otherwise no
// match. Returns an unloaded handle with a human-readable reason appended to
// errorOut (mirroring ConnectToCamera()'s existing "why not connected"
// logging convention) if no Metavision SDK is installed at all, or no
// backend covers the installed version, or the matching backend DLL itself
// fails to load.
ProphEBSBackendHandle LoadBestAvailableBackend(std::string& errorOut);
