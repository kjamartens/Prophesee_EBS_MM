///////////////////////////////////////////////////////////////////////////////
// FILE:          ProphEBSModule.cpp
// PROJECT:       Micro-Manager
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   Module initialization and device factory for the ProphEBS
//                (Prophesee event-based sensor) adapter.
//
//                Every MicroManager device adapter DLL must export these
//                three functions (MODULE_API, declared in ModuleInterface.h)
//                so MMCore can discover and instantiate its devices.
//
// COPYRIGHT:     Koen J.A. Martens, 2026
// LICENSE:       This file is distributed under the BSD license.

#include "ProphEBS.h"
#include "ModuleInterface.h"

#include <cstring>

MODULE_API void InitializeModuleData()
{
   RegisterDevice(g_ProphEBSCameraDeviceName, MM::CameraDevice,
      "Prophesee EBS Camera (Goal 1 barebones)");
}

MODULE_API MM::Device* CreateDevice(const char* deviceName)
{
   if (deviceName == 0)
      return 0;

   if (strcmp(deviceName, g_ProphEBSCameraDeviceName) == 0)
      return new CProphEBSCamera();

   return 0;
}

MODULE_API void DeleteDevice(MM::Device* pDevice)
{
   delete pDevice;
}
