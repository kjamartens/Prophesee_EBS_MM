///////////////////////////////////////////////////////////////////////////////
// FILE:          ProphEBSBackendSDK430Exports.cpp
// PROJECT:       Micro-Manager
// SUBSYSTEM:     DeviceAdapters / ProphEBS / Backend / SDK430
//-----------------------------------------------------------------------------
// DESCRIPTION:   The three plain-C exports every ProphEBS_Backend_<tag>.dll
//                must provide -- see IProphEBSBackend.h and BackendLoader.cpp
//                for how mmgr_dal_ProphEBS.dll finds and calls these by name.
//
// COPYRIGHT:     Koen J.A. Martens, 2026
// LICENSE:       This file is distributed under the BSD license.
///////////////////////////////////////////////////////////////////////////////

#include "ProphEBSBackendSDK430.h"

extern "C" __declspec(dllexport) IProphEBSBackend* CreateProphEBSBackend()
{
   return new ProphEBSBackendSDK430();
}

extern "C" __declspec(dllexport) void DestroyProphEBSBackend(IProphEBSBackend* backend)
{
   delete backend;
}

extern "C" __declspec(dllexport) const char* ProphEBSBackendAbiTag()
{
   return PROPHEBS_BACKEND_ABI_TAG;
}
