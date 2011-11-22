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

#include "serviceinstall.h"
#include "maintenanceservice.h"
#include "servicebase.h"
#include "workmonitor.h"

PRLogModuleInfo *gServiceLog = NULL;

SERVICE_STATUS gSvcStatus = { 0 }; 
SERVICE_STATUS_HANDLE gSvcStatusHandle = NULL; 
HANDLE ghSvcStopEvent = NULL;
BOOL gServiceStopping = FALSE;

int 
wmain(int argc, WCHAR **argv)
{
#ifdef PR_LOGGING
  if (!gServiceLog) {
    gServiceLog = PR_NewLogModule("nsMaintenanceService");
  }
#endif

  // If command-line parameter is "install", install the service
  // or upgrade if already installed.
  // If command-line parameter is "upgrade", upgrade the service
  // but do not install it if it is not already installed.
  // If command line parameter is "uninstall", uninstall the service.
  // Otherwise, the service is probably being started by the SCM.
  if (lstrcmpi(argv[1], L"install") == 0) {
    PR_LOG(gServiceLog, PR_LOG_ALWAYS,
      ("Installing service...\n"));

    if (!SvcInstall(FALSE)) {
      PR_LOG(gServiceLog, PR_LOG_ALWAYS,
        ("Could not install service (%d)\n", GetLastError()));
      return 1;
    }

    PR_LOG(gServiceLog, PR_LOG_ALWAYS,
      ("The service was installed successfully\n"));

    return 0;
  } else if (lstrcmpi(argv[1], L"upgrade") == 0) {
    PR_LOG(gServiceLog, PR_LOG_ALWAYS,
      ("Upgrading service if installed...\n"));

    if (!SvcInstall(TRUE)) {
      PR_LOG(gServiceLog, PR_LOG_ALWAYS,
        ("Could not upgrade service (%d)\n", GetLastError()));
      return 1;
    }

    PR_LOG(gServiceLog, PR_LOG_ALWAYS,
      ("The service was upgraded successfully\n"));
    return 0;
  } else if (lstrcmpi(argv[1], L"uninstall") == 0) {
    PR_LOG(gServiceLog, PR_LOG_ALWAYS,
      ("Uninstalling service...\n"));

    if (!SvcUninstall()) {
      PR_LOG(gServiceLog, PR_LOG_ALWAYS,
        ("Could not uninstall service (%d)\n", GetLastError()));
      return 1;
    }

    PR_LOG(gServiceLog, PR_LOG_ALWAYS,
      ("The service was uninstalled successfully\n"));

    return 0;
  } else if (lstrcmpi(argv[1], L"debug") == 0) {
    PR_LOG(gServiceLog, PR_LOG_ALWAYS,
      ("Starting service as a process in debug mode...\n"));

    StartDirectoryChangeMonitor();

    PR_LOG(gServiceLog, PR_LOG_ALWAYS,
      ("Exiting service as a process in debug mode...\n"));

    return 0;
  }

  SERVICE_TABLE_ENTRYW DispatchTable[] = { 
    { SVC_NAME, (LPSERVICE_MAIN_FUNCTION) SvcMain }, 
    { NULL, NULL } 
  }; 

  // This call returns when the service has stopped. 
  // The process should simply terminate when the call returns.
  if (!StartServiceCtrlDispatcher(DispatchTable)) {
    PR_LOG(gServiceLog, PR_LOG_ALWAYS,
      ("StartServiceCtrlDispatcher failed (%d)\n", GetLastError()));
  }

  return 0;
}

/**
 * Wrapper callback for the monitoring thread.
 *
 * @param param Unused thread callback parameter
 */
DWORD
WINAPI StartMonitoringThreadProc(LPVOID param) 
{
  StartDirectoryChangeMonitor();
  return 0;
}

/**
 * Main entry point when running as a service.
 */
void WINAPI 
SvcMain(DWORD dwArgc, LPWSTR *lpszArgv)
{
#ifdef PR_LOGGING
  if (!gServiceLog) {
    gServiceLog = PR_NewLogModule("nsMaintenanceService");
  }
#endif

  // Register the handler function for the service
  gSvcStatusHandle = RegisterServiceCtrlHandlerW(SVC_NAME, SvcCtrlHandler);
  if (!gSvcStatusHandle) {
    PR_LOG(gServiceLog, PR_LOG_ALWAYS,
      ("RegisterServiceCtrlHandler failed (%d)\n", GetLastError()));
    return; 
  } 

  // These SERVICE_STATUS members remain as set here
  gSvcStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  gSvcStatus.dwServiceSpecificExitCode = 0;

  // Report initial status to the SCM
  ReportSvcStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

  // Perform service-specific initialization and work.
  SvcInit(dwArgc, lpszArgv);
}

/**
 * Service initialization.
 */
void
SvcInit(DWORD dwArgc, LPWSTR *lpszArgv)
{
  // Create an event. The control handler function, SvcCtrlHandler,
  // signals this event when it receives the stop control code.
  ghSvcStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
  if (NULL == ghSvcStopEvent) {
    ReportSvcStatus(SERVICE_STOPPED, 1, 0);
    return;
  }

  DWORD threadID;
  HANDLE thread = CreateThread(NULL, 0, StartMonitoringThreadProc, 0, 
                               0, &threadID);

  // Report running status when initialization is complete.
  ReportSvcStatus(SERVICE_RUNNING, NO_ERROR, 0);

  // Perform work until service stops.
  for(;;) {
    // Check whether to stop the service.
    WaitForSingleObject(ghSvcStopEvent, INFINITE);

    WCHAR stopFilePath[MAX_PATH +1];
    if (!GetUpdateDirectoryPath(stopFilePath)) {
      PR_LOG(gServiceLog, PR_LOG_ALWAYS,
        ("Could not obtain update directory path, terminating thread "
         "forcefully.\n"));
      TerminateThread(thread, 1);
    }

    // The stop file is to wake up the synchronous call for watching directory
    // changes. Directory watching will only actually be stopped if 
    // gServiceStopping is also set to TRUE.
    gServiceStopping = TRUE;
    if (!PathAppendSafe(stopFilePath, L"stop")) {
      TerminateThread(thread, 2);
    }
    HANDLE stopFile = CreateFile(stopFilePath, GENERIC_READ, 0, 
                                 NULL, CREATE_ALWAYS, 0, NULL);
    if (stopFile == INVALID_HANDLE_VALUE) {
      PR_LOG(gServiceLog, PR_LOG_ALWAYS,
        ("Could not create stop file, terminating thread forcefully.\n"));
      TerminateThread(thread, 3);
    } else {
      CloseHandle(stopFile);
      DeleteFile(stopFilePath);
    }

    ReportSvcStatus(SERVICE_STOPPED, NO_ERROR, 0);
    return;
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

  // Fill in the SERVICE_STATUS structure.
  gSvcStatus.dwCurrentState = currentState;
  gSvcStatus.dwWin32ExitCode = exitCode;
  gSvcStatus.dwWaitHint = waitHint;

  if (SERVICE_START_PENDING == currentState) {
    gSvcStatus.dwControlsAccepted = 0;
  } else {
    gSvcStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
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
 * Called by SCM whenever a control code is sent to the service
 * using the ControlService function.
 */
void WINAPI
SvcCtrlHandler(DWORD dwCtrl)
{
  // Handle the requested control code. 
  switch(dwCtrl) {
  case SERVICE_CONTROL_STOP: 
    ReportSvcStatus(SERVICE_STOP_PENDING, NO_ERROR, 0);
    // Signal the service to stop.
    SetEvent(ghSvcStopEvent);
    ReportSvcStatus(gSvcStatus.dwCurrentState, NO_ERROR, 0);
    break;
  case SERVICE_CONTROL_INTERROGATE: 
    break; 
  default: 
    break;
  }
}
