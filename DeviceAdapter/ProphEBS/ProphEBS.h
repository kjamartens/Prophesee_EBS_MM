///////////////////////////////////////////////////////////////////////////////
// FILE:          ProphEBS.h
// PROJECT:       Micro-Manager
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   Goal 1 (barebones) device adapter for the Prophesee event-based
//                sensor (EBS) camera.
//
//                This is intentionally NOT connected to any Prophesee/Metavision
//                hardware or SDK yet. It exists purely to prove that a custom
//                MicroManager camera DeviceAdapter can be built, loaded, and
//                used from MicroManager (Hardware Configuration Wizard, Live
//                view, Snap). Upon Initialize() it writes a debug message to
//                the MicroManager CoreLog, and SnapImage() always returns the
//                same static test pattern.
//
//                Real EBS connectivity, event integration, recording,
//                properties, and ROI/pixel masking are added in later goals
//                (see docs/DEVLOG.md at the repository root for the roadmap).
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

#include <string>

// Device name (defined in ProphEBS.cpp, referenced by ProphEBSModule.cpp)
extern const char* g_ProphEBSCameraDeviceName;

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
