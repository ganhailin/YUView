/*  This file is part of YUView - The YUV player with advanced analytics toolset
 *   <https://github.com/IENT/YUView>
 *   Copyright (C) 2015  Institut für Nachrichtentechnik, RWTH Aachen University, GERMANY
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   In addition, as a special exception, the copyright holders give
 *   permission to link the code of portions of this program with the
 *   OpenSSL library under certain conditions as described in each
 *   individual source file, and distribute linked combinations including
 *   the two.
 *
 *   You must obey the GNU General Public License in all respects for all
 *   of the code used other than OpenSSL. If you modify file(s) with this
 *   exception, you may extend this exception to your version of the
 *   file(s), but you are not obligated to do so. If you do not wish to do
 *   so, delete this exception statement from your version. If you delete
 *   this exception statement from all source files in the program, then
 *   also delete it here.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <QMutex>
#include <QThread>
#include <QWaitCondition>

#include "LoadingWorker.h"

namespace video
{

#define LOADINGTHREAD_DEBUG_LOADING 1
#if LOADINGTHREAD_DEBUG_LOADING && !NDEBUG
#define DEBUG_THREAD qDebug
#else
#define DEBUG_THREAD(fmt, ...) ((void)0)
#endif

class LoadingThread : public QThread
{
  Q_OBJECT
public:
  LoadingThread(QObject *parent) : QThread(parent)
  {
    // Create a new worker and move it to this thread
    this->threadWorker.reset(new LoadingWorker(nullptr));
    this->threadWorker->moveToThread(this);
  }
  ~LoadingThread() {}

  void quitWhenDone()
  {
    this->quitting = true;
#ifdef Q_OS_WASM
    // Wake the run() loop so it can observe the quitting flag and exit.
    {
      QMutexLocker lock(&jobMutex);
      hasPendingJob = false;
      jobCondition.wakeAll();
    }
#else
    if (this->threadWorker->isWorking())
    {
      // We must wait until the worker is done.
      DEBUG_THREAD("loadingThread::quitWhenDone waiting for worker to finish...");
      connect(worker(),
              &LoadingWorker::loadingFinished,
              this,
              [=]
              {
                DEBUG_THREAD("loadingThread::quitWhenDone worker done -> quit");
                quit();
              });
    }
    else
    {
      DEBUG_THREAD("loadingThread::quitWhenDone quit now");
      quit();
    }
#endif
  }

  LoadingWorker *worker() { return this->threadWorker.get(); }
  bool           isQuitting() { return this->quitting; }

#ifdef Q_OS_WASM
  // Submit a loading job to be executed on this thread. On WebAssembly the
  // thread blocks on a QWaitCondition instead of a busy event loop, so we
  // cannot rely on queued invokeMethod calls.
  void submitLoadingJob(playlistItem *item, int frame, bool playing, bool loadRawData)
  {
    QMutexLocker lock(&jobMutex);
    pendingItem        = item;
    pendingFrame       = frame;
    pendingPlaying     = playing;
    pendingLoadRawData = loadRawData;
    pendingIsCacheJob  = false;
    pendingTestMode    = false;
    hasPendingJob      = true;
    jobCondition.wakeOne();
  }

  void submitCacheJob(playlistItem *item, int frame, bool testMode)
  {
    QMutexLocker lock(&jobMutex);
    pendingItem       = item;
    pendingFrame      = frame;
    pendingPlaying    = false;
    pendingLoadRawData = false;
    pendingIsCacheJob = true;
    pendingTestMode   = testMode;
    hasPendingJob     = true;
    jobCondition.wakeOne();
  }

protected:
  void run() override
  {
    // On WebAssembly, QEventLoop::exec() busy-waits when idle, consuming a full
    // CPU core. Instead, block on a QWaitCondition and only wake up when a job
    // is submitted. This keeps the interactive threads from pegging the CPU.
    while (!quitting)
    {
      playlistItem *item;
      int           frame;
      bool          playing, loadRawData, isCacheJob, testMode;
      {
        QMutexLocker lock(&jobMutex);
        while (!hasPendingJob && !quitting)
          jobCondition.wait(&jobMutex);
        if (quitting)
          break;
        item          = pendingItem;
        frame         = pendingFrame;
        playing       = pendingPlaying;
        loadRawData   = pendingLoadRawData;
        isCacheJob    = pendingIsCacheJob;
        testMode      = pendingTestMode;
        hasPendingJob = false;
      }

      worker()->setJob(item, frame, testMode);
      worker()->setWorking(true);
      if (isCacheJob)
        worker()->processCacheJobInternal();
      else
        worker()->processLoadingJobInternal(playing, loadRawData);
      worker()->setWorking(false);
    }
  }
#endif

private:
  std::unique_ptr<LoadingWorker> threadWorker{};
  bool quitting{}; // Are er quitting the job? If yes, do not push new jobs to it.

#ifdef Q_OS_WASM
  QMutex        jobMutex;
  QWaitCondition jobCondition;
  bool          hasPendingJob{false};
  playlistItem *pendingItem{};
  int           pendingFrame{};
  bool          pendingPlaying{};
  bool          pendingLoadRawData{};
  bool          pendingIsCacheJob{};
  bool          pendingTestMode{};
#endif
};

} // namespace video
