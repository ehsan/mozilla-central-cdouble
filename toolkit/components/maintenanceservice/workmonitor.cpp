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
 * The Original Code is Maintenance service file system monitoring.
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

#include <shlobj.h>
#include <shlwapi.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <shellapi.h>

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "shlwapi.lib")

#include "nsWindowsHelpers.h"
#include "nsAutoPtr.h"

#include "workmonitor.h"
#include "serviceinstall.h"
#include "servicebase.h"
#include "registrycertificates.h"
#include "uachelper.h"

extern BOOL gServiceStopping;

// Wait 45 minutes for an update operation to run at most.
// Updates usually take less than a minute so this seems like a 
// significantly large and safe amount of time to wait.
static const int TIME_TO_WAIT_ON_UPDATER = 45 * 60 * 1000;
PRUnichar* MakeCommandLine(int argc, PRUnichar **argv);
BOOL WriteStatusFailure(LPCWSTR updateDirPath, int errorCode);
BOOL WriteStatusPending(LPCWSTR updateDirPath);
BOOL StartCallbackApp(int argcTmp, LPWSTR *argvTmp, DWORD callbackSessionID);
BOOL StartSelfUpdate(int argcTmp, LPWSTR *argvTmp);
void LaunchWinPostProcess(const WCHAR *appExe, HANDLE userToken = NULL);
BOOL PathGetSiblingFilePath(LPWSTR destinationBuffer,  LPCWSTR siblingFilePath, 
                            LPCWSTR newFileName);

// Error codes is 16000 since Windows system error codes only go up to 15999.
const int SERVICE_UPDATE_ERROR = 16000;

/**
 * Gets the installation directory from the arguments passed to updater.exe.
 *
 * @param argcTmp    The argc value normally sent to updater.exe
 * @param argvTmp    The argv value normally sent to updater.exe
 * @param installDir Buffer to hold the installation directory.
 *                   The size of the buffer should be 2 * MAX_PATH.
 */
bool
GetInstallationDir(int argcTmp, LPWSTR *argvTmp, LPWSTR installDir)
{
  if (argcTmp < 2) {
    return false;
  }
  wcscpy(installDir, argvTmp[2]);
  bool backgroundUpdate = (argcTmp == 4 && !wcscmp(argvTmp[3], L"-1"));
  bool replaceRequest = (argcTmp >= 4 && wcsstr(argvTmp[3], L"/replace"));
  if (backgroundUpdate || replaceRequest) {
    LPWSTR pathSeparator = wcschr(installDir, L';');
    if (pathSeparator) {
      wcscpy(installDir, pathSeparator + 1);
    }
    LPWSTR backSlash = wcsrchr(installDir, L'\\');
    // Go one level up, ignoring trailing backslashes
    if (backSlash && !backSlash[1]) {
      *backSlash = L'\0';
      backSlash = wcsrchr(installDir, L'\\');
    }
    if (!pathSeparator) {
      *backSlash = L'\0';
    }
  }
  return true;
}

/**
 * Gets the destination directory from the arguments passed to updater.exe.
 *
 * @param argcTmp The argc value normally sent to updater.exe
 * @param argvTmp The argv value normally sent to updater.exe
 * @param destDir Buffer to hold the destination directory.
 *                The size of the buffer should be 2 * MAX_PATH.
 */
bool
GetDestinationDir(int argcTmp, LPWSTR *argvTmp, LPWSTR destDir)
{
  if (argcTmp < 2) {
    return false;
  }
  wcscpy(destDir, argvTmp[2]);
  bool backgroundUpdate = (argcTmp == 4 && !wcscmp(argvTmp[3], L"-1"));
  bool replaceRequest = (argcTmp >= 4 && wcsstr(argvTmp[3], L"/replace"));
  if (backgroundUpdate || replaceRequest) {
    LPWSTR pathSeparator = wcschr(destDir, L';');
    if (pathSeparator) {
      *pathSeparator = L'\0';
    }
    LPWSTR backSlash = wcsrchr(destDir, L'\\');
    // Trim any trailing slashes
    if (backSlash && !backSlash[1]) {
      *backSlash = L'\0';
    }
  }
  return true;
}

/**
 * Runs an update process in the specified sessionID as an elevated process.
 *
 * @param  appToStart    The path to the update process to start.
 * @param  workingDir    The working directory to execute the update process in.
 * @param  cmdLine       The command line parameters to pass to the update
 *         process. If they specify a callback application, it will be
 *         executed with an associated unelevated token for the sessionID.
 * @param processStarted Returns TRUE if the process was started.
 * @param  callbackSessionID 
 *         If 0 and Windows Vista, the callback application will
 *         not be run.  If non zero the callback application will
 *         be injected into the user's session as a non-elevated process.
 * @return TRUE if the update process was run had a return code of 0.
 */
BOOL
StartUpdateProcess(LPCWSTR appToStart, 
                   LPCWSTR workingDir, 
                   int argcTmp,
                   LPWSTR *argvTmp,
                   BOOL &processStarted,
                   DWORD callbackSessionID = 0)
{
  DWORD myProcessID = GetCurrentProcessId();
  DWORD mySessionID = 0;
  ProcessIdToSessionId(myProcessID, &mySessionID);

  STARTUPINFO si = {0};
  si.cb = sizeof(STARTUPINFO);
  si.lpDesktop = L"winsta0\\Default";
  PROCESS_INFORMATION pi = {0};

  PR_LOG(gServiceLog, PR_LOG_ALWAYS,
    ("Starting process in an elevated session.  Service "
     "session ID: %d; Requested callback session ID: %d", 
     mySessionID, callbackSessionID));

  // The updater command line is of the form:
  // updater.exe update-dir apply [wait-pid [callback-dir callback-path args]]
  // So update callback-dir is the 4th, callback-path is the 5th and its args 
  // are the 6th index.  So that we can execute the callback out of line we
  // won't call updater.exe with those callback args and we will manage the
  // callback ourselves.
  LPVOID environmentBlock = NULL;
  LPWSTR cmdLineMinusCallback = MakeCommandLine(min(argcTmp, 4), argvTmp);

  // If we're about to start the update process from session 0 on Vista
  // or later, then we should not show a GUI.
  if (UACHelper::IsVistaOrLater() && argcTmp >= 2 ) {
    // Setting the desktop to blank will ensure no GUI is displayed
    si.lpDesktop = L"";
    si.dwFlags |= STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
  }

  // We move the updater.ini file out of the way because we will handle 
  // executing PostUpdate through the service.  We handle PostUpdate from
  // the service because there are some per user things that happen that
  // can't run in session 0 which we run updater.exe in.
  // Once we are done running updater.exe we rename updater.ini back so
  // that if there were any errors the next updater.exe will run correctly.
  WCHAR updaterINI[MAX_PATH + 1];
  WCHAR updaterINITemp[MAX_PATH + 1];
  BOOL selfHandlePostUpdate = FALSE;
  // We use the updater.ini from the same directory as the updater.exe
  // because of background updates.
  if (PathGetSiblingFilePath(updaterINI, argvTmp[0], L"updater.ini") &&
      PathGetSiblingFilePath(updaterINITemp, argvTmp[0], L"updater.tmp")) {
    selfHandlePostUpdate = MoveFileEx(updaterINI, updaterINITemp, 
                                      MOVEFILE_REPLACE_EXISTING);
  }

  processStarted = CreateProcessW(appToStart, cmdLineMinusCallback, 
                                  NULL, NULL, FALSE, 
                                  CREATE_DEFAULT_ERROR_MODE | 
                                  CREATE_UNICODE_ENVIRONMENT, 
                                  NULL, workingDir, &si, &pi);
  BOOL updateWasSuccessful = FALSE;
  if (processStarted) {
    PR_LOG(gServiceLog, PR_LOG_ALWAYS,
      ("Process was started... waiting on result.")); 

    // Wait for the updater process to finish
    DWORD waitRes = WaitForSingleObject(pi.hProcess, TIME_TO_WAIT_ON_UPDATER);
    if (WAIT_TIMEOUT == waitRes) {
      // We waited a long period of time for updater.exe and it never finished
      // so kill it.
      ::TerminateProcess(pi.hProcess, 1);
    } else {
      // Check the return code of updater.exe to make sure we get 0
      DWORD returnCode;
      if (GetExitCodeProcess(pi.hProcess, &returnCode)) {
        PR_LOG(gServiceLog, PR_LOG_ALWAYS,
          ("Process finished with return code %d.", returnCode)); 
        // updater returns 0 if successful.
        updateWasSuccessful = (returnCode == 0);
      } else {
        PR_LOG(gServiceLog, PR_LOG_ALWAYS,
          ("Process finished but could not obtain return code.")); 
      }
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
  } else {
    DWORD lastError = GetLastError();
    PR_LOG(gServiceLog, PR_LOG_ALWAYS,
      ("Could not create process as current user, last error: "
       "%d; appToStart: %S; cmdLine: %S",
       lastError, appToStart, cmdLineMinusCallback));
  }

  // Now that we're done with the update, restore back the updater.ini file
  // We use it ourselves, and also we want it back in case we had any type 
  // of error so that the normal update process can use it.
  if (selfHandlePostUpdate) {
    MoveFileEx(updaterINITemp, updaterINI, MOVEFILE_REPLACE_EXISTING);

    // Only run the PostUpdate if the update was successful and if we have
    // a callback application.  This is the same thing updater.exe does.
    if (updateWasSuccessful && argcTmp > 5) {
      LPWSTR callbackApplication = argvTmp[5];
      // Launch the PostProcess with admin access in session 0 followed 
      // by user access with the user token.
      LaunchWinPostProcess(callbackApplication);
      nsAutoHandle userToken(UACHelper::OpenUserToken(callbackSessionID));
      LaunchWinPostProcess(callbackApplication, userToken);
    }
  }

  // The callback app Will not run if session ID is 0 and on Vista
  StartCallbackApp(argcTmp, argvTmp, callbackSessionID);
  free(cmdLineMinusCallback);
  return updateWasSuccessful;
}

/**
 * Processes a work item (file by the name of .mz) and executes
 * the command within.
 *
 * @param  monitoringBasePath The base path that is being monitored.
 * @param  notifyInfo         the notifyInfo struct for the changes.
 * @return TRUE if we want the service to stop.
 */
BOOL
ProcessWorkItem(LPCWSTR monitoringBasePath, 
                FILE_NOTIFY_INFORMATION &notifyInfo)
{
  int filenameLength = notifyInfo.FileNameLength / 
                       sizeof(notifyInfo.FileName[0]); 
  notifyInfo.FileName[filenameLength] = L'\0';

  PR_LOG(gServiceLog, PR_LOG_ALWAYS,
    ("Processing new command meta file: %S", notifyInfo.FileName));

  // When the file is ready for processing it will be renamed 
  // to have a .mz extension
  BOOL haveWorkItem = notifyInfo.Action == FILE_ACTION_RENAMED_NEW_NAME && 
                      notifyInfo.FileNameLength > 3 && 
                      notifyInfo.FileName[filenameLength - 3] == L'.' &&
                      notifyInfo.FileName[filenameLength - 2] == L'm' &&
                      notifyInfo.FileName[filenameLength - 1] == L'z';
  if (!haveWorkItem) {
    // We don't have a work item, keep looking
    return FALSE;
  }

  WCHAR fullMetaUpdateFilePath[MAX_PATH + 1];
  wcscpy(fullMetaUpdateFilePath, monitoringBasePath);

  // We only support file paths in monitoring directories that are MAX_PATH
  // chars or less.
  if (!PathAppendSafe(fullMetaUpdateFilePath, notifyInfo.FileName)) {
    // Cannot process update, skipfileSize it.
    return TRUE;
  }

  nsAutoHandle metaUpdateFile(CreateFile(fullMetaUpdateFilePath, 
                                         GENERIC_READ, 
                                         0, NULL, 
                                         OPEN_EXISTING,
                                         0, NULL));
  if (metaUpdateFile == INVALID_HANDLE_VALUE) {
    PR_LOG(gServiceLog, PR_LOG_ALWAYS,
      ("Could not open command meta file: %S", notifyInfo.FileName));
    return TRUE;
  }

  DWORD fileSize = GetFileSize(metaUpdateFile, NULL);
  DWORD sessionID = 0;
  // The file should be in wide characters so if it's of odd size it's
  // an invalid file.
  const int kSanityCheckFileSize = 1024 * 64;
  if (fileSize == INVALID_FILE_SIZE || 
      (fileSize %2) != 0 ||
      fileSize > kSanityCheckFileSize ||
      fileSize < sizeof(DWORD)) {
    PR_LOG(gServiceLog, PR_LOG_ALWAYS,
      ("Could not obtain file size or an improper file size was encountered "
       "for command meta file: %S",
       notifyInfo.FileName));
    return TRUE;
  }

  // The first 4 bytes are for the process ID
  DWORD sessionIDCount;
  BOOL result = ReadFile(metaUpdateFile, &sessionID, 
                         sizeof(DWORD), &sessionIDCount, NULL);
  fileSize -= sizeof(DWORD);

  // The next MAX_PATH wchar's are for the app to start
  WCHAR appToStart[MAX_PATH + 1];
  ZeroMemory(appToStart, sizeof(appToStart));
  DWORD appToStartCount;
  result |= ReadFile(metaUpdateFile, appToStart, 
                     MAX_PATH * sizeof(WCHAR), &appToStartCount, NULL);
  fileSize -= appToStartCount;

  // The next MAX_PATH wchar's are for the app to start
  WCHAR workingDirectory[MAX_PATH + 1];
  ZeroMemory(workingDirectory, sizeof(workingDirectory));
  DWORD workingDirectoryCount;
  result |= ReadFile(metaUpdateFile, workingDirectory, 
                     MAX_PATH * sizeof(WCHAR), &workingDirectoryCount, NULL);
  fileSize -= workingDirectoryCount;

  // + 2 for wide char termination
  nsAutoArrayPtr<char> cmdlineBuffer = new char[fileSize + 2];
  DWORD cmdLineBufferRead;
  result |= ReadFile(metaUpdateFile, cmdlineBuffer, 
                     fileSize, &cmdLineBufferRead, NULL);
  fileSize -= cmdLineBufferRead;

  // We have all the info we need from the work item file, get rid of it.
  metaUpdateFile.reset();
  DeleteFileW(fullMetaUpdateFilePath);

  if (!result ||
      sessionIDCount != sizeof(DWORD) || 
      appToStartCount != MAX_PATH * sizeof(WCHAR) ||
      workingDirectoryCount != MAX_PATH * sizeof(WCHAR) ||
      fileSize != 0) {
    PR_LOG(gServiceLog, PR_LOG_ALWAYS,
      ("Could not read command data for command meta file: %S",
       notifyInfo.FileName));
    return TRUE;
  }
  cmdlineBuffer[cmdLineBufferRead] = '\0';
  cmdlineBuffer[cmdLineBufferRead + 1] = '\0';
  WCHAR *cmdlineBufferWide = reinterpret_cast<WCHAR*>(cmdlineBuffer.get());

  PR_LOG(gServiceLog, PR_LOG_ALWAYS,
    ("An update command was detected and is being processed for command meta "
     "file: %S",
     notifyInfo.FileName));

  int argcTmp = 0;
  LPWSTR* argvTmp = CommandLineToArgvW(cmdlineBufferWide, &argcTmp);

  WCHAR installDir[2 * MAX_PATH];
  ZeroMemory(installDir, 2 * MAX_PATH * sizeof(WCHAR));
  if (!GetInstallationDir(argcTmp, argvTmp, installDir)) {
    return false;
  }

  // Validate the certificate of the app the user wants us to start.
  // Also check to make sure the certificate is trusted.
#ifndef DISABLE_SERVICE_AUTHENTICODE_CHECK
  if (argcTmp > 2 && 
      DoesBinaryMatchAllowedCertificates(installDir, appToStart)) {
#endif

    BOOL updateProcessWasStarted = FALSE;
    if (!StartUpdateProcess(appToStart, workingDirectory, 
                            argcTmp, argvTmp,
                            updateProcessWasStarted,
                            sessionID)) {
      PR_LOG(gServiceLog, PR_LOG_ALWAYS,
        ("Error running process in session %d.  "
         "Updating update.status.  Last error: %d",
         sessionID, GetLastError()));

      // If the update process was started, then updater.exe is responsible for
      // setting the failure code.  If it could not be started then we set the
      // error code ourselves.   We set an error code instead of directly
      // setting status pending so that the app.update.service.failcount
      // pref can be updated.
      if (!updateProcessWasStarted) {
        if (!WriteStatusFailure(argvTmp[1], SERVICE_UPDATE_ERROR)) {
          PR_LOG(gServiceLog, PR_LOG_ALWAYS,
            ("Could not write update.status service update failure."
             "Last error: %d",
             GetLastError()));
        }
      }
      StartCallbackApp(argcTmp, argvTmp, sessionID);
    } else {
      // The update was executed and run successfully
      PR_LOG(gServiceLog, PR_LOG_ALWAYS,
        ("updater.exe was launched and run successfully"));

      StartSelfUpdate(argcTmp, argvTmp);
    }
#ifndef DISABLE_SERVICE_AUTHENTICODE_CHECK
  } else {
    PR_LOG(gServiceLog, PR_LOG_ALWAYS,
      ("Could not start process due to certificate check."
       "Updating update.status.  Last error: %d", GetLastError()));

    // When there is a certificate error we just want to write pending and then 
    // relaunch the callback app so that we will prompt as a usual update
    // without the service.
    if (argcTmp < 2 || !WriteStatusPending(argvTmp[1])) {
      PR_LOG(gServiceLog, PR_LOG_ALWAYS,
        ("Could not write update.status pending-service.  Last error: %d", 
         GetLastError()));
    }
    StartCallbackApp(argcTmp, argvTmp, sessionID);
  }
#endif
  LocalFree(argvTmp);

  // We processed a work item, whether or not it was successful.
  return TRUE;
}

/**
 * Starts monitoring the update directory for work items.
 *
 * @return FALSE if there was an error starting directory monitoring.
 */
BOOL
StartDirectoryChangeMonitor() 
{
  PR_LOG(gServiceLog, PR_LOG_ALWAYS,
    ("Starting directory change monitor..."));

  // Init the update directory path and ensure it exists.
  // Example: C:\programData\Mozilla\Firefox\updates\[channel]
  // The channel is not included here as we want to monitor the base directory
  WCHAR updateData[MAX_PATH + 1];
  if (!GetUpdateDirectoryPath(updateData)) {
    PR_LOG(gServiceLog, PR_LOG_ALWAYS,
      ("Could not obtain update directory path"));

    return FALSE;
  }

  // Obtain a directory handle, FILE_FLAG_BACKUP_SEMANTICS is needed for this.
  nsAutoHandle directoryHandle(CreateFile(updateData, 
                                          FILE_LIST_DIRECTORY, 
                                          FILE_SHARE_READ | FILE_SHARE_WRITE, 
                                          NULL, 
                                          OPEN_EXISTING,
                                          FILE_FLAG_BACKUP_SEMANTICS, 
                                          NULL));
  if (directoryHandle == INVALID_HANDLE_VALUE) {
    PR_LOG(gServiceLog, PR_LOG_ALWAYS,
      ("Could not obtain directory handle to monitor"));

    return FALSE;
  }

  // Allocate a buffer that is big enough to hold 128 changes
  // Note that FILE_NOTIFY_INFORMATION is a variable length struct
  // so that's why we don't create a simple array directly.
  char fileNotifyInfoBuffer[(sizeof(FILE_NOTIFY_INFORMATION) + 
                            MAX_PATH) * 128];
  ZeroMemory(&fileNotifyInfoBuffer, sizeof(fileNotifyInfoBuffer));
  
  DWORD bytesReturned;
  // Listen for directory changes to the update directory
  while (ReadDirectoryChangesW(directoryHandle, 
                               fileNotifyInfoBuffer, 
                               sizeof(fileNotifyInfoBuffer), 
                               TRUE, FILE_NOTIFY_CHANGE_FILE_NAME, 
                               &bytesReturned, NULL, NULL)) {
    char *fileNotifyInfoBufferLocation = fileNotifyInfoBuffer;

    // Make sure we have at least one entry to process
    while (bytesReturned/sizeof(FILE_NOTIFY_INFORMATION) > 0) {
      // Point to the current directory info which is changed
      FILE_NOTIFY_INFORMATION &notifyInfo = 
        *((FILE_NOTIFY_INFORMATION*)fileNotifyInfoBufferLocation);
      fileNotifyInfoBufferLocation += notifyInfo .NextEntryOffset;
      bytesReturned -= notifyInfo .NextEntryOffset;
      if (!wcscmp(notifyInfo.FileName, L"stop") && gServiceStopping) {
        PR_LOG(gServiceLog, PR_LOG_ALWAYS, ("A stop command was issued."));
        return TRUE;
      }

      BOOL processedWorkItem = ProcessWorkItem(updateData, notifyInfo);
      if (processedWorkItem) {
        // We don't return here because during stop we will write out a stop 
        // file which will stop monitoring.
        StopService();
        break;
      }

      // NextEntryOffset will be 0 if there are no more items to monitor,
      // and we're ready to make another call to ReadDirectoryChangesW.
      if (0 == notifyInfo.NextEntryOffset) {
        break;
      }
    }
  }

  return TRUE;
}

/**
 * Starts the upgrade process for update of ourselves
 *
 * @param  argcTmp The argc value normally sent to updater.exe
 * @param  argvTmp The argv value normally sent to updater.exe
 * @return TRUE if successful
 */
BOOL
StartSelfUpdate(int argcTmp, LPWSTR *argvTmp)
{
  if (argcTmp < 2) {
    return FALSE;
  }

  bool backgroundUpdate = (argcTmp == 4 && !wcscmp(argvTmp[3], L"-1"));
  if (backgroundUpdate) {
    // Skip updating the service for background updates.  We'll do this when
    // switching to the updated application later on.
    PR_LOG(gServiceLog, PR_LOG_ALWAYS,
      ("Skipping self update in background update mode"));
    return false;
  }

  STARTUPINFO si = {0};
  si.cb = sizeof(STARTUPINFO);
  // No particular desktop because no UI
  si.lpDesktop = L"";
  PROCESS_INFORMATION pi = {0};

  WCHAR destDir[2 * MAX_PATH];
  ZeroMemory(destDir, 2 * MAX_PATH * sizeof(WCHAR));
  if (!GetDestinationDir(argcTmp, argvTmp, destDir)) {
    return false;
  }

  WCHAR maintserviceInstallerPath[MAX_PATH + 1];
  wcscpy(maintserviceInstallerPath, destDir);
  PathAppendSafe(maintserviceInstallerPath, 
                 L"maintenanceservice_installer.exe");
  BOOL selfUpdateProcessStarted = CreateProcessW(maintserviceInstallerPath, 
                                                 L"/Upgrade", 
                                                 NULL, NULL, FALSE, 
                                                 CREATE_DEFAULT_ERROR_MODE | 
                                                 CREATE_UNICODE_ENVIRONMENT, 
                                                 NULL, argvTmp[2], &si, &pi);
  if (selfUpdateProcessStarted) {
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
  }
  return selfUpdateProcessStarted;
}

/**
 * Starts the callback application from the updater.exe command line arguments.
 *
 * @param  argcTmp           The argc value normally sent to updater.exe
 * @param  argvTmp           The argv value normally sent to updater.exe
 * @param  callbackSessionID The session ID to start the callback with
 * @return TRUE if successful
 */
BOOL
StartCallbackApp(int argcTmp, LPWSTR *argvTmp, DWORD callbackSessionID) 
{
  if (0 == callbackSessionID  && UACHelper::IsVistaOrLater()) {
    PR_LOG(gServiceLog, PR_LOG_ALWAYS,
      ("Attempted to run callback application in session 0, not allowed."));
    return FALSE;
  }

  if (argcTmp < 5) {
    PR_LOG(gServiceLog, PR_LOG_ALWAYS,
      ("No callback application specified."));
    return FALSE;
  }

  LPWSTR callbackArgs = NULL;
  LPWSTR callbackDirectory = argvTmp[4];
  LPWSTR callbackApplication = argvTmp[5];
  if (argcTmp > 6) {
    // Make room for all of the ending args minus the first 6 
    // + 1 for the app itself
    const size_t callbackCommandLineLen = argcTmp - 5;
    WCHAR **params = new WCHAR*[callbackCommandLineLen];
    params[0] = callbackApplication;
    for (size_t i = 1; i < callbackCommandLineLen; ++i) {
      params[i] = argvTmp[5 + i];
    }
    callbackArgs = MakeCommandLine(callbackCommandLineLen, params);
    delete[] params;
  }

  if (!callbackApplication) {
    PR_LOG(gServiceLog, PR_LOG_ALWAYS,
      ("Callback application is NULL."));
    if (callbackArgs) {
      free(callbackArgs);
    }
    return FALSE;
  }

  nsAutoHandle unelevatedToken(UACHelper::OpenUserToken(callbackSessionID));
  if (!unelevatedToken) {
    PR_LOG(gServiceLog, PR_LOG_ALWAYS,
      ("Could not obtain unelevated token for callback app."));
    if (callbackArgs) {
      free(callbackArgs);
    }
    return FALSE;
  }

  WCHAR installDir[2 * MAX_PATH];
  ZeroMemory(installDir, 2 * MAX_PATH * sizeof(WCHAR));
  if (!GetInstallationDir(argcTmp, argvTmp, installDir)) {
    return false;
  }

#ifdef ENABLE_CALLBACK_AUTHENTICODE_CHECK
  if (argcTmp > 2 && 
      DoesBinaryMatchAllowedCertificates(installDir, callbackApplication)) {
#endif

    // Create an environment block for the process we're about to start using
    // the user's token.
    LPVOID environmentBlock = NULL;
    if (!CreateEnvironmentBlock(&environmentBlock, unelevatedToken, TRUE)) {
      PR_LOG(gServiceLog, PR_LOG_ALWAYS,
        ("Could not create an environment block, setting it to NULL."));
      environmentBlock = NULL;
    }

    STARTUPINFO si = {0};
    si.cb = sizeof(STARTUPINFO);
    si.lpDesktop = L"winsta0\\Default";
    PROCESS_INFORMATION pi = {0};
    if (CreateProcessAsUserW(unelevatedToken, 
                             callbackApplication, 
                             callbackArgs, 
                             NULL, NULL, FALSE,
                             CREATE_DEFAULT_ERROR_MODE |
                             CREATE_UNICODE_ENVIRONMENT,
                             environmentBlock, 
                             callbackDirectory, 
                             &si, &pi)) {
      PR_LOG(gServiceLog, PR_LOG_ALWAYS,
        ("Callback app was run successfully."));
      if (callbackArgs) {
        free(callbackArgs);
      }
      CloseHandle(pi.hProcess);
      CloseHandle(pi.hThread);
      return TRUE;
    } else {
      PR_LOG(gServiceLog, PR_LOG_ALWAYS,
        ("Could not run callback app, last error: %d", GetLastError()));
    }
#ifdef ENABLE_CALLBACK_AUTHENTICODE_CHECK
  } else {
    PR_LOG(gServiceLog, PR_LOG_ALWAYS,
      ("Could not start callback process due to certificate check."
       " Last error: %d", GetLastError()));
  }
#endif
  if (callbackArgs) {
    free(callbackArgs);
  }
  return FALSE;
}
