/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Maintenance service.
 *
 * The Initial Developer of the Original Code is
 * Mozilla Foundation.
 * Portions created by the Initial Developer are Copyright (C) 2011
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Brian R. Bondy <netzen@gmail.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>
#include <wchar.h>
#include <shlobj.h>

#include "serviceinstall.h"
#include "maintenanceservice.h"
#include "servicebase.h"
#include "workmonitor.h"
#include "uachelper.h"
#include "updatehelper.h"

SERVICE_STATUS gSvcStatus = { 0 }; 
SERVICE_STATUS_HANDLE gSvcStatusHandle = NULL; 
HANDLE gWorkDoneEvent = NULL;
HANDLE gThread = NULL;
bool gServiceControlStopping = false;

// logs are pretty small ~10 lines, so 5 seems reasonable.
#define LOGS_TO_KEEP 5

BOOL GetLogDirectoryPath(WCHAR *path);

int 
wmain(int argc, WCHAR **argv)
{
  // If command-line parameter is "install", install the service
  // or upgrade if already installed
  // If command line parameter is "forceinstall", install the service
  // even if it is older than what is already installed.
  // If command-line parameter is "upgrade", upgrade the service
  // but do not install it if it is not already installed.
  // If command line parameter is "uninstall", uninstall the service.
  // Otherwise, the service is probably being started by the SCM.
  bool forceInstall = !lstrcmpi(argv[1], L"forceinstall");
  if (!lstrcmpi(argv[1], L"install") || forceInstall) {
    WCHAR updatePath[MAX_PATH + 1];
    if (GetLogDirectoryPath(updatePath)) {
      LogInit(updatePath, L"maintenanceservice-install.log");
    }

    LOG(("Installing service"));
    SvcInstallAction action = InstallSvc;
    if (forceInstall) {
      action = ForceInstallSvc;
      LOG((" with force specified"));
    }
    LOG(("...\n"));

    if (!SvcInstall(action)) {
      LOG(("Could not install service (%d)\n", GetLastError()));
      LogFinish();
      return 1;
    }

    LOG(("The service was installed successfully\n"));
    LogFinish();
    return 0;
  } 

  if (!lstrcmpi(argv[1], L"upgrade")) {
    WCHAR updatePath[MAX_PATH + 1];
    if (GetLogDirectoryPath(updatePath)) {
      LogInit(updatePath, L"maintenanceservice-install.log");
    }
    LOG(("Upgrading service if installed...\n"));
    if (!SvcInstall(UpgradeSvc)) {
      LOG(("Could not upgrade service (%d)\n", GetLastError()));
      LogFinish();
      return 1;
    }

    LOG(("The service was upgraded successfully\n"));
    LogFinish();
    return 0;
  }

  if (!lstrcmpi(argv[1], L"uninstall")) {
    WCHAR updatePath[MAX_PATH + 1];
    if (GetLogDirectoryPath(updatePath)) {
      LogInit(updatePath, L"maintenanceservice-uninstall.log");
    }
    LOG(("Uninstalling service...\n"));
    if (!SvcUninstall()) {
      LOG(("Could not uninstall service (%d)\n", GetLastError()));
      LogFinish();
      return 1;
    }
    LOG(("The service was uninstalled successfully\n"));
    LogFinish();
    return 0;
  }

  SERVICE_TABLE_ENTRYW DispatchTable[] = { 
    { SVC_NAME, (LPSERVICE_MAIN_FUNCTIONW) SvcMain }, 
    { NULL, NULL } 
  }; 

  // This call returns when the service has stopped. 
  // The process should simply terminate when the call returns.
  if (!StartServiceCtrlDispatcherW(DispatchTable)) {
    LOG(("StartServiceCtrlDispatcher failed (%d)\n", GetLastError()));
  }

  return 0;
}

/**
 * Obtains the base path where logs should be stored
 *
 * @param  path The out buffer for the backup log path of size MAX_PATH + 1
 * @return TRUE if successful.
 */
BOOL
GetLogDirectoryPath(WCHAR *path)
{
  HRESULT hr = SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 
    SHGFP_TYPE_CURRENT, path);
  if (FAILED(hr)) {
    return FALSE;
  }

  if (!PathAppendSafe(path, L"Mozilla")) {
    return FALSE;
  }
  // The directory should already be created from the installer, but
  // just to be safe in case someone deletes.
  CreateDirectoryW(path, NULL);

  if (!PathAppendSafe(path, L"logs")) {
    return FALSE;
  }
  CreateDirectoryW(path, NULL);
  return TRUE;
}

/**
 * Calculated a backup path based on the log number.
 *
 * @param  path      The out buffer to store the log path of size MAX_PATH + 1
 * @param  basePath  The base directory where the calculated path should go
 * @param  logNumber The log number, 0 == updater.log
 * @return TRUE if successful.
 */
BOOL
GetBackupLogPath(LPWSTR path, LPCWSTR basePath, int logNumber)
{
  WCHAR logName[64];
  wcscpy(path, basePath);
  if (logNumber <= 0) {
    swprintf(logName, L"maintenanceservice.log");
  } else {
    swprintf(logName, L"maintenanceservice-%d.log", logNumber);
  }
  return PathAppendSafe(path, logName);
}

/**
 * Moves the old log files out of the way before a new one is written.
 * If you for example keep 3 logs, then this function will do:
 *   updater2.log -> updater3.log
 *   updater1.log -> updater2.log
 *   updater.log -> updater1.log
 * Which clears room for a new updater.log in the basePath directory
 * 
 * @param basePath      The base directory path where log files are stored
 * @param numLogsToKeep The number of logs to keep
 */
void
BackupOldLogs(LPCWSTR basePath, int numLogsToKeep)
{
  WCHAR oldPath[MAX_PATH + 1];
  WCHAR newPath[MAX_PATH + 1];
  for (int i = numLogsToKeep; i >= 1; i--) {
    if (!GetBackupLogPath(oldPath, basePath, i -1)) {
      continue;
    }

    if (!GetBackupLogPath(newPath, basePath, i)) {
      continue;
    }

    if (!MoveFileExW(oldPath, newPath, MOVEFILE_REPLACE_EXISTING)) {
      continue;
    }
  }
}

/**
 * Ensures the service is shutdown once all work is complete.
 * There is an issue on XP SP2 and below where the service can hang
 * in a stop pending state even though the SCM is notified of a stopped
 * state.  Control *should* be returned to StartServiceCtrlDispatcher from the 
 * call to SetServiceStatus on a stopped state in the wmain thread.  
 * Sometimes this is not the case though. This thread will terminate the process
 * if it has been 5 seconds after all work is done and the process is still not
 * terminated.  This thread is only started once a stopped state was sent to the
 * SCM. The stop pending hang can be reproduced intermittently even if you set 
 * a stopped state dirctly and never set a stop pending state.  It is safe to 
 * forcefully terminate the process ourselves since all work is done once we 
 * start this thread.
*/
DWORD WINAPI
EnsureProcessTerminatedThread(LPVOID)
{
  Sleep(5000);
  exit(0);
  return 0;
}

void
StartTerminationThread()
{
  // If the process does not self terminate like it should, this thread 
  // will terminate the process after 5 seconds.
  HANDLE thread = CreateThread(NULL, 0, EnsureProcessTerminatedThread, 
                               NULL, 0, NULL);
  if (thread) {
    CloseHandle(thread);
  }
}

/**
 * Main entry point when running as a service.
 */
void WINAPI
SvcMain(DWORD argc, LPWSTR *argv)
{
  // Setup logging, and backup the old logs
  WCHAR updatePath[MAX_PATH + 1];
  if (GetLogDirectoryPath(updatePath)) {
    BackupOldLogs(updatePath, LOGS_TO_KEEP);
    LogInit(updatePath, L"maintenanceservice.log");
  }

  // Disable every privilege we don't need. Processes started using
  // CreateProcess will use the same token as this process.
  UACHelper::DisablePrivileges(NULL);

  // Register the handler function for the service
  gSvcStatusHandle = RegisterServiceCtrlHandlerW(SVC_NAME, SvcCtrlHandler);
  if (!gSvcStatusHandle) {
    LOG(("RegisterServiceCtrlHandler failed (%d)\n", GetLastError()));
    ExecuteServiceCommand(argc, argv);  
    LogFinish();
    exit(1);
  } 

  // These values will be re-used later in calls involving gSvcStatus
  gSvcStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  gSvcStatus.dwServiceSpecificExitCode = 0;

  // Report initial status to the SCM
  ReportSvcStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

  // This event will be used to tell the SvcCtrlHandler when the work is
  // done for when a stop comamnd is manually issued.
  gWorkDoneEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
  if (!gWorkDoneEvent) {
    ReportSvcStatus(SERVICE_STOPPED, 1, 0);
    StartTerminationThread();
    return;
  }

  // Initialization complete and we're about to start working on
  // the actual command.  Report the service state as running to the SCM.
  ReportSvcStatus(SERVICE_RUNNING, NO_ERROR, 0);

  // The service command was executed, stop logging and set an event
  // to indicate the work is done in case someone is waiting on a
  // service stop operation.
  ExecuteServiceCommand(argc, argv);  
  LogFinish();

  SetEvent(gWorkDoneEvent);

  // If we aren't already in a stopping state then tell the SCM we're stopped
  // now.  If we are already in a stopping state then the SERVICE_STOPPED state
  // will be set by the SvcCtrlHandler.
  if (!gServiceControlStopping) {
    ReportSvcStatus(SERVICE_STOPPED, NO_ERROR, 0);
    StartTerminationThread();
  }
}

/**
 * Sets the current service status and reports it to the SCM.
 *  
 * @param currentState  The current state (see SERVICE_STATUS)
 * @param exitCode      The system error code
 * @param waitHint      Estimated time for pending operation in milliseconds
 */
void
ReportSvcStatus(DWORD currentState, 
                DWORD exitCode, 
                DWORD waitHint)
{
  static DWORD dwCheckPoint = 1;

  gSvcStatus.dwCurrentState = currentState;
  gSvcStatus.dwWin32ExitCode = exitCode;
  gSvcStatus.dwWaitHint = waitHint;

  if (SERVICE_START_PENDING == currentState || 
      SERVICE_STOP_PENDING == currentState) {
    gSvcStatus.dwControlsAccepted = 0;
  } else {
    gSvcStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | 
                                    SERVICE_ACCEPT_SHUTDOWN;
  }

  if ((SERVICE_RUNNING == currentState) ||
      (SERVICE_STOPPED == currentState)) {
    gSvcStatus.dwCheckPoint = 0;
  } else {
    gSvcStatus.dwCheckPoint = dwCheckPoint++;
  }

  // Report the status of the service to the SCM.
  SetServiceStatus(gSvcStatusHandle, &gSvcStatus);
}

/**
 * Since the SvcCtrlHandler should only spend at most 30 seconds before 
 * returning, this function does the service stop work for the SvcCtrlHandler. 
*/
DWORD WINAPI
StopServiceAndWaitForCommandThread(LPVOID)
{
  do {
    ReportSvcStatus(SERVICE_STOP_PENDING, NO_ERROR, 1000);
  } while(WaitForSingleObject(gWorkDoneEvent, 100) == WAIT_TIMEOUT);
  CloseHandle(gWorkDoneEvent);
  gWorkDoneEvent = NULL;
  ReportSvcStatus(SERVICE_STOPPED, NO_ERROR, 0);
  StartTerminationThread();
  return 0;
}

/**
 * Called by SCM whenever a control code is sent to the service
 * using the ControlService function.
 */
void WINAPI
SvcCtrlHandler(DWORD dwCtrl)
{
  // After a SERVICE_CONTROL_STOP there should be no more commands sent to
  // the SvcCtrlHandler.
  if (gServiceControlStopping) {
    return;
  }

  // Handle the requested control code. 
  switch(dwCtrl) {
  case SERVICE_CONTROL_SHUTDOWN:
  case SERVICE_CONTROL_STOP: {
      gServiceControlStopping = true;
      ReportSvcStatus(SERVICE_STOP_PENDING, NO_ERROR, 1000);

      // The SvcCtrlHandler thread should not spend more than 30 seconds in 
      // shutdown so we spawn a new thread for stopping the service 
      HANDLE thread = CreateThread(NULL, 0, StopServiceAndWaitForCommandThread, 
                                   NULL, 0, NULL);
      if (thread) {
        CloseHandle(thread);
      } else {
        // Couldn't start the thread so just call the stop ourselves.
        // If it happens to take longer than 30 seconds the caller will
        // get an error.
        StopServiceAndWaitForCommandThread(NULL);
      }
    }
    break;
  default:
    break;
  }
}
