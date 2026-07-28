///////////////////////////////////////////////////////////////////////////////
// FILE:          ProphEBS.h
// PROJECT:       Micro-Manager
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   Prophesee event-based sensor (EBS) camera device adapter.
//
//                Goal 1 (barebones): proved that a custom MicroManager camera
//                DeviceAdapter can be built, loaded, and used from
//                MicroManager (Hardware Configuration Wizard, Live view,
//                Snap) with no real hardware -- SnapImage() always returned a
//                static test pattern.
//
//                Goal 2 (this revision) adds real Metavision SDK connectivity:
//                Initialize() now tries to open the first available Prophesee
//                camera via Metavision::Camera::from_first_available() and,
//                if successful, reads back its model/serial/connection type
//                via the HAL's I_HW_Identification facility and exposes them
//                as read-only MM properties. If no camera is found (or the
//                Metavision SDK/driver isn't installed), Initialize() does
//                NOT fail -- it logs why and falls back to the Goal 1 static
//                test image, so the adapter still loads and is testable on a
//                machine with no EBS attached. SnapImage() still returns the
//                static test pattern; real event-driven frames are Goal 3.
//
//                Event integration, recording, tunable properties, and
//                ROI/pixel masking are added in later goals (see
//                docs/DEVLOG.md at the repository root for the roadmap).
//
// COPYRIGHT:     Koen J.A. Martens, 2026
// LICENSE:       This file is distributed under the BSD license, consistent
//                with the rest of the Micro-Manager device adapter kit.
//
//                This file is distributed in the hope that it will be useful,
//                but WITHOUT ANY WARRANTY; without even the implied warranty
//                of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

#pragma once

#include "DeviceBase.h"
#include "DeviceThreads.h"
#include "ImgBuffer.h"

#include <metavision/sdk/stream/camera.h>

#include <string>

// Device name (defined in ProphEBS.cpp, referenced by ProphEBSModule.cpp)
extern const char* g_ProphEBSCameraDeviceName;

// Read-only MM property names used to surface Goal 2 connection info.
extern const char* g_PropConnectionStatus;
extern const char* g_PropModel;
extern const char* g_PropSerial;
extern const char* g_PropConnectionType;
extern const char* g_PropIntegrator;

// Fixed test-image geometry for Goal 1. Real sensor geometry will replace
// this once the adapter actually queries the Prophesee HAL (Goal 2/3).
const unsigned g_TestImageWidth = 640;
const unsigned g_TestImageHeight = 480;

class ProphEBSSequenceThread;

//////////////////////////////////////////////////////////////////////////////
// CProphEBSCamera class
//
// Minimal MM::CameraDevice implementation. See the MM::Camera pure-virtual
// list in MMDevice/MMDevice.h and MMDevice/DeviceBase.h (CCameraBase<U>) for
// exactly which methods below are required overrides versus which are
// already given sane defaults by CCameraBase (e.g. multi-ROI, exposure
// sequencing) and therefore intentionally not overridden here.
//////////////////////////////////////////////////////////////////////////////
class CProphEBSCamera : public CCameraBase<CProphEBSCamera>
{
public:
   CProphEBSCamera();
   ~CProphEBSCamera();

   // MMDevice API
   // ------------
   int Initialize();
   int Shutdown();
   void GetName(char* name) const;
   bool Busy();

   // MMCamera API
   // ------------
   int SnapImage();
   const unsigned char* GetImageBuffer();
   unsigned GetImageWidth() const;
   unsigned GetImageHeight() const;
   unsigned GetImageBytesPerPixel() const;
   unsigned GetBitDepth() const;
   long GetImageBufferSize() const;
   double GetExposure() const;
   void SetExposure(double exp_ms);
   int SetROI(unsigned x, unsigned y, unsigned xSize, unsigned ySize);
   int GetROI(unsigned& x, unsigned& y, unsigned& xSize, unsigned& ySize);
   int ClearROI();
   int GetBinning() const;
   int SetBinning(int binSize);
   int IsExposureSequenceable(bool& isSequenceable) const;

   // Continuous/sequence acquisition (used by MicroManager's Live view)
   int StartSequenceAcquisition(double interval_ms);
   int StartSequenceAcquisition(long numImages, double interval_ms, bool stopOnOverflow);
   int StopSequenceAcquisition();
   bool IsCapturing();

   // Called by ProphEBSSequenceThread on its worker thread to push one frame
   // into the MMCore circular buffer.
   int InsertImage();

private:
   void GenerateTestImage();

   // Tries Metavision::Camera::from_first_available(). Never throws and
   // never fails Initialize() -- on any error it logs the reason and leaves
   // cameraConnected_ false so the rest of Initialize() can fall back to
   // Goal 1 behavior (static test image, no hardware-derived properties).
   void ConnectToCamera();

   ImgBuffer img_;
   bool initialized_;
   double exposure_ms_;
   unsigned roiX_;
   unsigned roiY_;
   unsigned roiXSize_;
   unsigned roiYSize_;
   MM::MMTime sequenceStartTime_;

   ProphEBSSequenceThread* thd_;
   friend class ProphEBSSequenceThread;

   // Goal 2: real EBS connection state. cam_ is default-constructed (not
   // connected to anything) until ConnectToCamera() succeeds; it is not used
   // for image acquisition yet (that's Goal 3).
   Metavision::Camera cam_;
   bool cameraConnected_;
   std::string connectionStatus_;
   std::string cameraModel_;
   std::string cameraSerial_;
   std::string connectionType_;
   std::string integrator_;
};

//////////////////////////////////////////////////////////////////////////////
// ProphEBSSequenceThread
//
// Drives MicroManager's Live view by repeatedly re-inserting the same static
// test image at roughly the requested interval. There is no real hardware
// polling loop yet; this only exists so Goal 1 can be visually verified in
// the Live window, not just via single Snap.
//////////////////////////////////////////////////////////////////////////////
class ProphEBSSequenceThread : public MMDeviceThreadBase
{
public:
   explicit ProphEBSSequenceThread(CProphEBSCamera* pCamera);
   ~ProphEBSSequenceThread();

   void Start(long numImages, double intervalMs);
   void Stop();
   bool IsStopped();

private:
   int svc();

   CProphEBSCamera* camera_;
   double intervalMs_;
   long numImages_;
   long imageCounter_;
   bool stop_;
   MMThreadLock stopLock_;
};
