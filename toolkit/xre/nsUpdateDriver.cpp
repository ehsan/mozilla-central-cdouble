/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
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
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is Google Inc.
 * Portions created by the Initial Developer are Copyright (C) 2005
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *  Darin Fisher <darin@meer.net>
 *  Ben Turner <mozilla@songbirdnest.com>
 *  Robert Strong <robert.bugzilla@gmail.com>
 *  Josh Aas <josh@mozilla.com>
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

#include <stdlib.h>
#include <stdio.h>
#include "nsUpdateDriver.h"
#include "nsXULAppAPI.h"
#include "nsAppRunner.h"
#include "nsILocalFile.h"
#include "nsCOMPtr.h"
#include "nsString.h"
#include "prproces.h"
#include "prlog.h"
#include "prenv.h"
#include "nsVersionComparator.h"
#include "nsXREDirProvider.h"
#include "SpecialSystemDirectory.h"
#include "nsDirectoryServiceDefs.h"
#include "nsThreadUtils.h"
#include "nsIXULAppInfo.h"

#ifdef XP_MACOSX
#include "nsILocalFileMac.h"
#include "nsCommandLineServiceMac.h"
#include "MacLaunchHelper.h"
#endif

#if defined(XP_WIN)
# include <direct.h>
# include <process.h>
# include <windows.h>
# include <shlwapi.h>
# define getcwd(path, size) _getcwd(path, size)
# define getpid() GetCurrentProcessId()
#elif defined(XP_OS2)
# include <unistd.h>
# define INCL_DOSFILEMGR
# include <os2.h>
#elif defined(XP_UNIX)
# include <unistd.h>
#endif

//
// We use execv to spawn the updater process on all UNIX systems except Mac OSX
// since it is known to cause problems on the Mac.  Windows has execv, but it
// is a faked implementation that doesn't really replace the current process.
// Instead it spawns a new process, so we gain nothing from using execv on
// Windows.
//
// On platforms where we are not calling execv, we may need to make the
// updater executable wait for the calling process to exit.  Otherwise, the
// updater may have trouble modifying our executable image (because it might
// still be in use).  This is accomplished by passing our PID to the updater so
// that it can wait for us to exit.  This is not perfect as there is a race
// condition that could bite us.  It's possible that the calling process could
// exit before the updater waits on the specified PID, and in the meantime a
// new process with the same PID could be created.  This situation is unlikely,
// however, given the way most operating systems recycle PIDs.  We'll take our
// chances ;-)
//
// A similar #define lives in updater.cpp and should be kept in sync with this.
//
#if defined(XP_UNIX) && !defined(XP_MACOSX)
#define USE_EXECV
#endif

#ifdef PR_LOGGING
static PRLogModuleInfo *sUpdateLog = PR_NewLogModule("updatedriver");
#endif
#define LOG(args) PR_LOG(sUpdateLog, PR_LOG_DEBUG, args)

#ifdef XP_WIN
static const char kUpdaterBin[] = "updater.exe";
#else
static const char kUpdaterBin[] = "updater";
#endif
static const char kUpdaterINI[] = "updater.ini";
#ifdef XP_MACOSX
static const char kUpdaterApp[] = "updater.app";
#endif
#if defined(XP_UNIX) && !defined(XP_MACOSX)
static const char kUpdaterPNG[] = "updater.png";
#endif

static nsresult
GetCurrentWorkingDir(char *buf, size_t size)
{
  // Cannot use NS_GetSpecialDirectory because XPCOM is not yet initialized.
  // This code is duplicated from xpcom/io/SpecialSystemDirectory.cpp:

#if defined(XP_OS2)
  if (DosQueryPathInfo( ".", FIL_QUERYFULLNAME, buf, size))
    return NS_ERROR_FAILURE;
#elif defined(XP_WIN)
  wchar_t wpath[MAX_PATH];
  if (!_wgetcwd(wpath, size))
    return NS_ERROR_FAILURE;
  NS_ConvertUTF16toUTF8 path(wpath);
  strncpy(buf, path.get(), size);
#else
  if(!getcwd(buf, size))
    return NS_ERROR_FAILURE;
#endif
  return NS_OK;
}

#if defined(XP_MACOSX)
// This is a copy of OS X's XRE_GetBinaryPath from nsAppRunner.cpp with the
// gBinaryPath check removed so that the updater can reload the stub executable
// instead of xulrunner-bin. See bug 349737.
static nsresult
GetXULRunnerStubPath(const char* argv0, nsILocalFile* *aResult)
{
  // Works even if we're not bundled.
  CFBundleRef appBundle = ::CFBundleGetMainBundle();
  if (!appBundle)
    return NS_ERROR_FAILURE;

  CFURLRef bundleURL = ::CFBundleCopyExecutableURL(appBundle);
  if (!bundleURL)
    return NS_ERROR_FAILURE;

  nsCOMPtr<nsILocalFileMac> lfm;
  nsresult rv = NS_NewLocalFileWithCFURL(bundleURL, true, getter_AddRefs(lfm));

  ::CFRelease(bundleURL);

  if (NS_FAILED(rv))
    return rv;

  NS_ADDREF(*aResult = static_cast<nsILocalFile*>(lfm.get()));
  return NS_OK;
}
#endif /* XP_MACOSX */

static bool
GetFile(nsIFile *dir, const nsCSubstring &name, nsCOMPtr<nsILocalFile> &result)
{
  nsresult rv;
  
  nsCOMPtr<nsIFile> file;
  rv = dir->Clone(getter_AddRefs(file));
  if (NS_FAILED(rv))
    return false;

  rv = file->AppendNative(name);
  if (NS_FAILED(rv))
    return false;

  result = do_QueryInterface(file, &rv);
  return NS_SUCCEEDED(rv);
}

static bool
GetStatusFile(nsIFile *dir, nsCOMPtr<nsILocalFile> &result)
{
  return GetFile(dir, NS_LITERAL_CSTRING("update.status"), result);
}

template <size_t Size>
static bool
GetStatusFileContents(nsILocalFile *statusFile, char (&buf)[Size])
{
  PRFileDesc *fd = nsnull;
  nsresult rv = statusFile->OpenNSPRFileDesc(PR_RDONLY, 0660, &fd);
  if (NS_FAILED(rv))
    return false;

  const PRInt32 n = PR_Read(fd, buf, Size);
  PR_Close(fd);

  return (n >= 0);
}

typedef enum {
  eNoUpdateAction,
  ePendingUpdate,
  eAppliedUpdate
} UpdateStatus;

static UpdateStatus
GetUpdateStatus(nsIFile* dir, nsCOMPtr<nsILocalFile> &statusFile)
{
  if (GetStatusFile(dir, statusFile)) {
    char buf[32];
    if (GetStatusFileContents(statusFile, buf)) {
      const char kPending[] = "pending";
      const char kApplied[] = "applied";
      if (!strncmp(buf, kPending, sizeof(kPending) - 1)) {
        return ePendingUpdate;
      }
      if (!strncmp(buf, kApplied, sizeof(kApplied) - 1)) {
        return eAppliedUpdate;
      }
    }
  }
  return eNoUpdateAction;
}

static bool
SetStatusApplying(nsILocalFile *statusFile)
{
  PRFileDesc *fd = nsnull;
  nsresult rv = statusFile->OpenNSPRFileDesc(PR_WRONLY, 0660, &fd);
  if (NS_FAILED(rv))
    return false;

  static const char kApplying[] = "applying\n";
  PR_Write(fd, kApplying, sizeof(kApplying) - 1);
  PR_Close(fd);

  return true;
}

static bool
GetVersionFile(nsIFile *dir, nsCOMPtr<nsILocalFile> &result)
{
  return GetFile(dir, NS_LITERAL_CSTRING("update.version"), result);
}

static bool
GetChannelChangeFile(nsIFile *dir, nsCOMPtr<nsILocalFile> &result)
{
  return GetFile(dir, NS_LITERAL_CSTRING("channelchange"), result);
}

// Compares the current application version with the update's application
// version.
static bool
IsOlderVersion(nsILocalFile *versionFile, const char *appVersion)
{
  PRFileDesc *fd = nsnull;
  nsresult rv = versionFile->OpenNSPRFileDesc(PR_RDONLY, 0660, &fd);
  if (NS_FAILED(rv))
    return true;

  char buf[32];
  const PRInt32 n = PR_Read(fd, buf, sizeof(buf));
  PR_Close(fd);

  if (n < 0)
    return false;

  // Trim off the trailing newline
  if (buf[n - 1] == '\n')
    buf[n - 1] = '\0';

  // If the update xml doesn't provide the application version the file will
  // contain the string "null" and it is assumed that the update is not older.
  const char kNull[] = "null";
  if (strncmp(buf, kNull, sizeof(kNull) - 1) == 0)
    return false;

  if (NS_CompareVersions(appVersion, buf) > 0)
    return true;

  return false;
}

static bool
CopyFileIntoUpdateDir(nsIFile *parentDir, const char *leafName, nsIFile *updateDir)
{
  nsDependentCString leaf(leafName);
  nsCOMPtr<nsIFile> file;

  // Make sure there is not an existing file in the target location.
  nsresult rv = updateDir->Clone(getter_AddRefs(file));
  if (NS_FAILED(rv))
    return false;
  rv = file->AppendNative(leaf);
  if (NS_FAILED(rv))
    return false;
  file->Remove(true);

  // Now, copy into the target location.
  rv = parentDir->Clone(getter_AddRefs(file));
  if (NS_FAILED(rv))
    return false;
  rv = file->AppendNative(leaf);
  if (NS_FAILED(rv))
    return false;
  rv = file->CopyToNative(updateDir, EmptyCString());
  if (NS_FAILED(rv))
    return false;

  return true;
}

static bool
CopyUpdaterIntoUpdateDir(nsIFile *greDir, nsIFile *appDir, nsIFile *updateDir,
                         nsCOMPtr<nsIFile> &updater, bool showUI)
{
  // Copy the updater application from the GRE and the updater ini from the app
#if defined(XP_MACOSX)
  if (!CopyFileIntoUpdateDir(greDir, kUpdaterApp, updateDir))
    return false;
#else
  if (!CopyFileIntoUpdateDir(greDir, kUpdaterBin, updateDir))
    return false;
#endif
  if (showUI) {
    // Only copy updater.ini if we need to display the UI
    CopyFileIntoUpdateDir(appDir, kUpdaterINI, updateDir);
  }
#if defined(XP_UNIX) && !defined(XP_MACOSX)
  nsCOMPtr<nsIFile> iconDir;
  appDir->Clone(getter_AddRefs(iconDir));
  iconDir->AppendNative(NS_LITERAL_CSTRING("icons"));
  if (!CopyFileIntoUpdateDir(iconDir, kUpdaterPNG, updateDir))
    return false;
#endif
  // Finally, return the location of the updater binary.
  nsresult rv = updateDir->Clone(getter_AddRefs(updater));
  if (NS_FAILED(rv))
    return false;
#if defined(XP_MACOSX)
  rv  = updater->AppendNative(NS_LITERAL_CSTRING(kUpdaterApp));
  rv |= updater->AppendNative(NS_LITERAL_CSTRING("Contents"));
  rv |= updater->AppendNative(NS_LITERAL_CSTRING("MacOS"));
  if (NS_FAILED(rv))
    return false;
#endif
  rv = updater->AppendNative(NS_LITERAL_CSTRING(kUpdaterBin));
  return NS_SUCCEEDED(rv); 
}

#if defined(XP_WIN)
static bool
CanWriteToOneDirectory(nsIFile* aDir)
{
  nsAutoString path;
  if (NS_FAILED(aDir->GetPath(path)))
    return false;
  path.Append(NS_LITERAL_STRING("/updated.update_in_progress.lock"));
  HANDLE file = CreateFileW(PromiseFlatString(path).get(),
                            GENERIC_READ | GENERIC_WRITE,
                            0,
                            NULL,
                            OPEN_ALWAYS,
                            FILE_FLAG_DELETE_ON_CLOSE,
                            NULL);
  bool success = (file != INVALID_HANDLE_VALUE);
  CloseHandle(file);
  return success;
}

static bool
CanWriteToDirectoryAndParent(nsIFile* aDir)
{
  nsCOMPtr<nsIFile> parent;
  aDir->GetParent(getter_AddRefs(parent));
  return CanWriteToOneDirectory(parent) &&
         CanWriteToOneDirectory(aDir);
}

static bool
OnSameVolume(nsIFile* aDir1, nsIFile* aDir2)
{
  nsAutoString path1, path2;
  wchar_t volume1[MAX_PATH], volume2[MAX_PATH];
  size_t volume1Len, volume2Len;

  nsresult rv = aDir1->GetPath(path1);
  rv |= aDir2->GetPath(path2);
  if (NS_FAILED(rv))
    return false;

  if (!GetVolumePathNameW(PromiseFlatString(path1).get(), volume1,
                          mozilla::ArrayLength(volume1)) ||
      !GetVolumePathNameW(PromiseFlatString(path2).get(), volume2,
                          mozilla::ArrayLength(volume2)))
    return false;

  volume1Len = wcslen(volume1);
  volume2Len = wcslen(volume2);
  return volume1Len == volume2Len &&
         volume1Len == PathCommonPrefixW(volume1, volume2, NULL);
}

static bool
ConstructCompoundApplyToString(nsIFile* aUpdatedDir, nsIFile* aAppDir,
                               nsACString& aApplyToDir)
{
  // Construct the new applyToDir string ("$UPDROOT\updated;$INSTALLDIR").
  nsAutoString applyToDirW;
  nsresult rv = aUpdatedDir->GetPath(applyToDirW);
  if (NS_FAILED(rv))
    return false;
  aApplyToDir.Assign(NS_ConvertUTF16toUTF8(applyToDirW));
  aApplyToDir.AppendASCII(";");

  rv = aAppDir->GetPath(applyToDirW);
  if (NS_FAILED(rv))
    return false;
  aApplyToDir.Append(NS_ConvertUTF16toUTF8(applyToDirW));

  return true;
}
#endif

static void
SwitchToUpdatedApp(nsIFile *greDir, nsIFile *updateDir, nsILocalFile *statusFile,
                   nsIFile *appDir, int appArgc, char **appArgv)
{
  nsresult rv;

  // Steps:
  //  - copy updater into temp dir
  //  - run updater with the correct arguments

  nsCOMPtr<nsILocalFile> tmpDir;
  GetSpecialSystemDirectory(OS_TemporaryDirectory,
                            getter_AddRefs(tmpDir));
  if (!tmpDir) {
    LOG(("failed getting a temp dir\n"));
    return;
  }

  // Try to create our own new temp directory in case there is already an
  // updater binary in the OS temporary location which we cannot write to.
  // Note that we don't check for errors here, as if this directory can't
  // be created, the following CopyUpdaterIntoUpdateDir call will fail.
  tmpDir->Append(NS_LITERAL_STRING("MozUpdater"));
  tmpDir->CreateUnique(nsIFile::DIRECTORY_TYPE, 0755);

  nsCOMPtr<nsIFile> updater;
  if (!CopyUpdaterIntoUpdateDir(greDir, appDir, tmpDir, updater, false)) {
    LOG(("failed copying updater\n"));
    return;
  }

  // We need to use the value returned from XRE_GetBinaryPath when attempting
  // to restart the running application.
  nsCOMPtr<nsILocalFile> appFile;

#if defined(XP_MACOSX)
  // On OS X we need to pass the location of the xulrunner-stub executable
  // rather than xulrunner-bin. See bug 349737.
  GetXULRunnerStubPath(appArgv[0], getter_AddRefs(appFile));
#else
  XRE_GetBinaryPath(appArgv[0], getter_AddRefs(appFile));
#endif

  if (!appFile)
    return;

#ifdef XP_WIN
  nsAutoString appFilePathW;
  rv = appFile->GetPath(appFilePathW);
  if (NS_FAILED(rv))
    return;
  NS_ConvertUTF16toUTF8 appFilePath(appFilePathW);

  nsAutoString updaterPathW;
  rv = updater->GetPath(updaterPathW);
  if (NS_FAILED(rv))
    return;

  NS_ConvertUTF16toUTF8 updaterPath(updaterPathW);

#else
  nsCAutoString appFilePath;
  rv = appFile->GetNativePath(appFilePath);
  if (NS_FAILED(rv))
    return;

  nsCAutoString updaterPath;
  rv = updater->GetNativePath(updaterPath);
  if (NS_FAILED(rv))
    return;

#endif

  // Get the directory to which the update will be applied. On Mac OSX we need
  // to apply the update to the Updated.app directory under the Foo.app
  // directory which is the parent of the parent of the appDir. On other
  // platforms we will just apply to the appDir/updated.
  nsCOMPtr<nsILocalFile> updatedDir;
#if defined(XP_MACOSX)
  nsCAutoString applyToDir;
  {
    nsCOMPtr<nsIFile> parentDir1, parentDir2;
    rv = appDir->GetParent(getter_AddRefs(parentDir1));
    if (NS_FAILED(rv))
      return;
    rv = parentDir1->GetParent(getter_AddRefs(parentDir2));
    if (NS_FAILED(rv))
      return;
    if (!GetFile(parentDir2, NS_LITERAL_CSTRING("Updated.app"), updatedDir))
      return;
    rv = updatedDir->GetNativePath(applyToDir);
  }
#else
  if (!GetFile(appDir, NS_LITERAL_CSTRING("updated"), updatedDir))
    return;
#if defined(XP_WIN)
  nsAutoString applyToDirW;
  rv = updatedDir->GetPath(applyToDirW);

  NS_ConvertUTF16toUTF8 applyToDir(applyToDirW);
#else
  nsCAutoString applyToDir;
  rv = updatedDir->GetNativePath(applyToDir);
#endif
#endif
  if (NS_FAILED(rv))
    return;

  // Make sure that the updated directory exists
  bool updatedDirExists = false;
  updatedDir->Exists(&updatedDirExists);
  if (!updatedDirExists) {
#if defined(XP_WIN)
    // If we don't have write access to the appdir, we might have stored the
    // updated dir inside the update root dir.  Test to see whether that's
    // the case.
    nsCOMPtr<nsILocalFile> alternateUpdatedDir;
    if (GetFile(updateDir, NS_LITERAL_CSTRING("updated"), alternateUpdatedDir)) {
      alternateUpdatedDir->Exists(&updatedDirExists);
      if (!updatedDirExists)
        return;
      if (!ConstructCompoundApplyToString(alternateUpdatedDir, appDir, applyToDir))
        return;
      updatedDir = alternateUpdatedDir;
    } else // continued after #ifdef
#endif
      return;
  }

#if defined(XP_WIN)
  nsAutoString updateDirPathW;
  rv = updateDir->GetPath(updateDirPathW);

  NS_ConvertUTF16toUTF8 updateDirPath(updateDirPathW);
#else
  nsCAutoString updateDirPath;
  rv = updateDir->GetNativePath(updateDirPath);
#endif

  if (NS_FAILED(rv))
    return;

  // Get the current working directory.
  char workingDirPath[MAXPATHLEN];
  rv = GetCurrentWorkingDir(workingDirPath, sizeof(workingDirPath));
  if (NS_FAILED(rv))
    return;

  // Construct the PID argument for this process.  If we are using execv, then
  // we pass "0" which is then ignored by the updater.
#if defined(USE_EXECV)
  nsCAutoString pid("0");
#else
  nsCAutoString pid;
  pid.AppendInt((PRInt32) getpid());
#endif

  // Append a special token to the PID in order to let the updater know that it
  // just needs to replace the update directory.
  pid.AppendASCII("/replace");

  int argc = appArgc + 5;
  char **argv = new char*[argc + 1];
  if (!argv)
    return;
  argv[0] = (char*) updaterPath.get();
  argv[1] = (char*) updateDirPath.get();
  argv[2] = (char*) applyToDir.get();
  argv[3] = (char*) pid.get();
  if (appArgc) {
    argv[4] = workingDirPath;
    argv[5] = (char*) appFilePath.get();
    for (int i = 1; i < appArgc; ++i)
      argv[5 + i] = appArgv[i];
    argc = 5 + appArgc;
    argv[argc] = NULL;
  } else {
    argc = 4;
    argv[4] = NULL;
  }

  if (gSafeMode) {
    PR_SetEnv("MOZ_SAFE_MODE_RESTART=1");
  }

  LOG(("spawning updater process for replacing [%s]\n", updaterPath.get()));

#if defined(USE_EXECV)
  execv(updaterPath.get(), argv);
#elif defined(XP_WIN)
  if (!WinLaunchChild(updaterPathW.get(), argc, argv))
    return;
  _exit(0);
#elif defined(XP_MACOSX)
  CommandLineServiceMac::SetupMacCommandLine(argc, argv, true);
  // LaunchChildMac uses posix_spawnp and prefers the current
  // architecture when launching. It doesn't require a
  // null-terminated string but it doesn't matter if we pass one.
  LaunchChildMac(argc, argv);
  exit(0);
#else
  PR_CreateProcessDetached(updaterPath.get(), argv, NULL, NULL);
  exit(0);
#endif
}

static void
ApplyUpdate(nsIFile *greDir, nsIFile *updateDir, nsILocalFile *statusFile,
            nsIFile *appDir, int appArgc, char **appArgv, bool restart,
            ProcessType *outpid)
{
  nsresult rv;

  // Steps:
  //  - mark update as 'applying'
  //  - copy updater into update dir
  //  - run updater w/ appDir as the current working dir

  nsCOMPtr<nsIFile> updater;
  if (!CopyUpdaterIntoUpdateDir(greDir, appDir, updateDir, updater, restart)) {
    LOG(("failed copying updater\n"));
    return;
  }

  // We need to use the value returned from XRE_GetBinaryPath when attempting
  // to restart the running application.
  nsCOMPtr<nsILocalFile> appFile;

#if defined(XP_MACOSX)
  // On OS X we need to pass the location of the xulrunner-stub executable
  // rather than xulrunner-bin. See bug 349737.
  GetXULRunnerStubPath(appArgv[0], getter_AddRefs(appFile));
#else
  XRE_GetBinaryPath(appArgv[0], getter_AddRefs(appFile));
#endif

  if (!appFile)
    return;

#ifdef XP_WIN
  nsAutoString appFilePathW;
  rv = appFile->GetPath(appFilePathW);
  if (NS_FAILED(rv))
    return;
  NS_ConvertUTF16toUTF8 appFilePath(appFilePathW);

  nsAutoString updaterPathW;
  rv = updater->GetPath(updaterPathW);
  if (NS_FAILED(rv))
    return;

  NS_ConvertUTF16toUTF8 updaterPath(updaterPathW);

#else
  nsCAutoString appFilePath;
  rv = appFile->GetNativePath(appFilePath);
  if (NS_FAILED(rv))
    return;
  
  nsCAutoString updaterPath;
  rv = updater->GetNativePath(updaterPath);
  if (NS_FAILED(rv))
    return;

#endif

  // Get the directory to which the update will be applied. On Mac OSX we need
  // to apply the update to the Updated.app directory under the Foo.app
  // directory which is the parent of the parent of the appDir. On other
  // platforms we will just apply to the appDir/updated.
  nsCOMPtr<nsILocalFile> updatedDir;
#if defined(XP_MACOSX)
  nsCAutoString applyToDir;
  {
    nsCOMPtr<nsIFile> parentDir1, parentDir2;
    rv = appDir->GetParent(getter_AddRefs(parentDir1));
    if (NS_FAILED(rv))
      return;
    rv = parentDir1->GetParent(getter_AddRefs(parentDir2));
    if (NS_FAILED(rv))
      return;
    if (restart) {
      // Use the correct directory if we're not applying the update in the
      // background.
      rv = parentDir2->GetNativePath(applyToDir);
    } else {
      if (!GetFile(parentDir2, NS_LITERAL_CSTRING("Updated.app"), updatedDir))
        return;
      rv = updatedDir->GetNativePath(applyToDir);
    }
  }
#else
  if (restart) {
    // Use the correct directory if we're not applying the update in the
    // background.
    updatedDir = do_QueryInterface(appDir);
  } else if (!GetFile(appDir, NS_LITERAL_CSTRING("updated"), updatedDir)) {
    return;
  }
#if defined(XP_WIN)
  nsAutoString applyToDirW;
  rv = updatedDir->GetPath(applyToDirW);

  NS_ConvertUTF16toUTF8 applyToDir(applyToDirW);
#else
  nsCAutoString applyToDir;
  rv = updatedDir->GetNativePath(applyToDir);
#endif
#endif
  if (NS_FAILED(rv))
    return;

#if defined(XP_WIN)
  // If we're about to try to apply the update in the background, first make
  // sure that we have write access to the directories in question.  If we
  // don't, we just fall back to applying the update at the next startup
  // using a UAC prompt.
  if (!restart && !CanWriteToDirectoryAndParent(appDir)) {
    // Try to see if we can write in the appdir
    nsCOMPtr<nsILocalFile> alternateUpdatedDir;
    if (!GetFile(updateDir, NS_LITERAL_CSTRING("updated"), alternateUpdatedDir))
      return;
    if (OnSameVolume(updatedDir, alternateUpdatedDir)) {
      if (!ConstructCompoundApplyToString(alternateUpdatedDir, appDir, applyToDir))
        return;
      updatedDir = alternateUpdatedDir;
    } else {
      // Fall back to the startup updates
      return;
    }
  }
#endif

#if defined(XP_WIN)
  nsAutoString updateDirPathW;
  rv = updateDir->GetPath(updateDirPathW);

  NS_ConvertUTF16toUTF8 updateDirPath(updateDirPathW);
#else
  nsCAutoString updateDirPath;
  rv = updateDir->GetNativePath(updateDirPath);
#endif

  if (NS_FAILED(rv))
    return;

  // Get the current working directory.
  char workingDirPath[MAXPATHLEN];
  rv = GetCurrentWorkingDir(workingDirPath, sizeof(workingDirPath));
  if (NS_FAILED(rv))
    return;

  if (!SetStatusApplying(statusFile)) {
    LOG(("failed setting status to 'applying'\n"));
    return;
  }

  // Construct the PID argument for this process.  If we are using execv, then
  // we pass "0" which is then ignored by the updater.
#if defined(USE_EXECV)
  nsCAutoString pid("0");
#else
  nsCAutoString pid;
  pid.AppendInt((PRInt32) getpid());
#endif

  if (!restart) {
    // Signal the updater application that it should apply the update in the
    // background.
    // XXX ehsan is there a better way to do this?
    pid.AssignASCII("-1");
  }

  int argc = appArgc + 5;
  char **argv = new char*[argc + 1];
  if (!argv)
    return;
  argv[0] = (char*) updaterPath.get();
  argv[1] = (char*) updateDirPath.get();
  argv[2] = (char*) applyToDir.get();
  argv[3] = (char*) pid.get();
  if (restart && appArgc) {
    argv[4] = workingDirPath;
    argv[5] = (char*) appFilePath.get();
    for (int i = 1; i < appArgc; ++i)
      argv[5 + i] = appArgv[i];
    argc = 5 + appArgc;
    argv[argc] = NULL;
  } else {
    argc = 4;
    argv[4] = NULL;
  }

  if (gSafeMode) {
    PR_SetEnv("MOZ_SAFE_MODE_RESTART=1");
  }

  LOG(("spawning updater process [%s]\n", updaterPath.get()));

#if defined(USE_EXECV)
  // Don't use execv for background updates.
  if (restart) {
    execv(updaterPath.get(), argv);
  } else {
    *outpid = PR_CreateProcess(updaterPath.get(), argv, NULL, NULL);
  }
#elif defined(XP_WIN)
  if (!WinLaunchChild(updaterPathW.get(), argc, argv, outpid))
    return;
  if (restart) {
    _exit(0);
  }
#elif defined(XP_MACOSX)
  CommandLineServiceMac::SetupMacCommandLine(argc, argv, true);
  // LaunchChildMac uses posix_spawnp and prefers the current
  // architecture when launching. It doesn't require a
  // null-terminated string but it doesn't matter if we pass one.
  LaunchChildMac(argc, argv, 0, outpid);
  if (restart) {
    exit(0);
  }
#else
  *outpid = PR_CreateProcess(updaterPath.get(), argv, NULL, NULL);
  if (restart) {
    exit(0);
  }
#endif
}

static void
WaitForProcess(ProcessType pt)
{
#if defined(XP_WIN)
  WaitForSingleObject(pt, INFINITE);
  CloseHandle(pt);
#elif defined(XP_MACOSX)
  waitpid(pt, 0, 0);
#else
  PRInt32 exitCode;
  PR_WaitProcess(pt, &exitCode);
#endif
}

nsresult
ProcessUpdates(nsIFile *greDir, nsIFile *appDir, nsIFile *updRootDir,
               int argc, char **argv, const char *appVersion,
               bool restart, ProcessType *pid)
{
  nsresult rv;

  nsCOMPtr<nsIFile> updatesDir;
  rv = updRootDir->Clone(getter_AddRefs(updatesDir));
  if (NS_FAILED(rv))
    return rv;

  rv = updatesDir->AppendNative(NS_LITERAL_CSTRING("updates"));
  if (NS_FAILED(rv))
    return rv;

  rv = updatesDir->AppendNative(NS_LITERAL_CSTRING("0"));
  if (NS_FAILED(rv))
    return rv;

  nsCOMPtr<nsILocalFile> statusFile;
  switch (GetUpdateStatus(updatesDir, statusFile)) {
  case ePendingUpdate: {
    nsCOMPtr<nsILocalFile> versionFile;
    nsCOMPtr<nsILocalFile> channelChangeFile;
    // Remove the update if the update application version file doesn't exist
    // or if the update's application version is less than the current
    // application version.
    if (!GetChannelChangeFile(updatesDir, channelChangeFile) &&
        (!GetVersionFile(updatesDir, versionFile) ||
         IsOlderVersion(versionFile, appVersion))) {
      updatesDir->Remove(true);
    } else {
      ApplyUpdate(greDir, updatesDir, statusFile, appDir, argc, argv, restart, pid);
    }
    break;
  }
  case eAppliedUpdate:
    // An update was applied in the background, so we need to switch to using
    // it now.
    SwitchToUpdatedApp(greDir, updatesDir, statusFile, appDir, argc, argv);
    break;
  case eNoUpdateAction:
    // We don't need to do any special processing here, we'll just continue to
    // startup the application.
    break;
  }

  return NS_OK;
}

NS_IMPL_THREADSAFE_ISUPPORTS1(nsUpdateProcessor, nsIUpdateProcessor)

nsUpdateProcessor::nsUpdateProcessor()
  : mUpdaterPID(0)
{
}

NS_IMETHODIMP
nsUpdateProcessor::ProcessUpdate(nsIUpdate* aUpdate)
{
  nsCOMPtr<nsIFile> greDir, appDir, updRoot;
  nsCAutoString appVersion;
  int argc;
  char **argv;

  NS_ENSURE_ARG_POINTER(aUpdate);

  nsXREDirProvider* dirProvider = nsXREDirProvider::GetSingleton();
  if (dirProvider) { // Normal code path
    // Check for and process any available updates
    bool persistent;
    nsresult rv = dirProvider->GetFile(XRE_UPDATE_ROOT_DIR, &persistent,
                                       getter_AddRefs(updRoot));
    // XRE_UPDATE_ROOT_DIR may fail. Fallback to appDir if failed
    if (NS_FAILED(rv))
      updRoot = dirProvider->GetAppDir();

    greDir = dirProvider->GetGREDir();
    appDir = dirProvider->GetAppDir();
    appVersion = gAppData->version;
    argc = gRestartArgc;
    argv = gRestartArgv;
  } else {
    // In the xpcshell environment, the usual XRE_main is not run, so things
    // like dirProvider and gAppData do not exist.  This code path accesses
    // XPCOM (which is not available in the previous code path) in order to get
    // the same information.
    nsCOMPtr<nsIProperties> ds =
      do_GetService(NS_DIRECTORY_SERVICE_CONTRACTID);
    if (!ds) {
      NS_ABORT(); // There's nothing which we can do if this fails!
    }

    nsresult rv = ds->Get(NS_GRE_DIR, NS_GET_IID(nsIFile),
                          getter_AddRefs(greDir));
    NS_ASSERTION(NS_SUCCEEDED(rv), "Can't get the GRE dir");
    appDir = greDir;

#ifdef XP_MACOSX
    // It's possible to launch xpcshell from dist/bin as opposed to from the
    // app bundle directory on Mac.  This is how our xpcshell tests run.  In
    // order to launch the updater with the correct arguments, we need to set
    // the appDir to the application bundle directory though.
#ifdef DEBUG
#define BUNDLE_NAME MOZ_APP_DISPLAYNAME "Debug.app"
#else
#define BUNDLE_NAME MOZ_APP_DISPLAYNAME ".app"
#endif
    nsCOMPtr<nsIFile> parent1, parent2;
    rv = appDir->GetParent(getter_AddRefs(parent1));
    NS_ENSURE_SUCCESS(rv, rv);
    rv = parent1->GetParent(getter_AddRefs(parent2));
    NS_ENSURE_SUCCESS(rv, rv);
    nsAutoString bundleLeafName;
    rv = parent2->GetLeafName(bundleLeafName);
    if (!bundleLeafName.Equals(NS_LITERAL_STRING(BUNDLE_NAME))) {
      rv = parent1->Append(NS_LITERAL_STRING(BUNDLE_NAME));
      NS_ENSURE_SUCCESS(rv, rv);
      rv = parent1->Append(NS_LITERAL_STRING("Contents"));
      NS_ENSURE_SUCCESS(rv, rv);
      rv = parent1->Append(NS_LITERAL_STRING("MacOS"));
      NS_ENSURE_SUCCESS(rv, rv);
      bool exists = false;
      parent1->Exists(&exists);
      NS_ENSURE_TRUE(exists, NS_ERROR_FAILURE);
      appDir = parent1;
    }
#undef BUNDLE_NAME
#endif

    rv = ds->Get(XRE_UPDATE_ROOT_DIR, NS_GET_IID(nsIFile),
                 getter_AddRefs(updRoot));
    if (NS_FAILED(rv))
      updRoot = appDir;

    nsCOMPtr<nsIXULAppInfo> appInfo =
      do_GetService("@mozilla.org/xre/app-info;1");
    if (appInfo) {
      rv = appInfo->GetVersion(appVersion);
      NS_ENSURE_SUCCESS(rv, rv);
    } else {
      appVersion = MOZ_APP_VERSION;
    }

    // We need argv[0] to point to the current executable's name.  The rest of
    // the entries in this array will be ignored if argc<2.  Therefore, for
    // xpcshell, we only fill out that item, and leave the rest empty.
    argc = 1;
    nsCAutoString binPath;
    nsCOMPtr<nsIFile> binary;
    rv = ds->Get(XRE_EXECUTABLE_FILE, NS_GET_IID(nsIFile),
                 getter_AddRefs(binary));
    NS_ASSERTION(NS_SUCCEEDED(rv), "Can't get the binary path");
    binary->GetNativePath(binPath);
    char* binPathCString = const_cast<char*> (nsPromiseFlatCString(binPath).get());
    argv = &binPathCString;
  }

  nsresult rv = ProcessUpdates(greDir,
                               appDir,
                               updRoot,
                               argc,
                               argv,
                               PromiseFlatCString(appVersion).get(),
                               false,
                               &mUpdaterPID);
  NS_ENSURE_SUCCESS(rv, rv);

  if (mUpdaterPID) {
    mUpdate = aUpdate;
    // Track the state of the background updater process
    NS_ABORT_IF_FALSE(NS_IsMainThread(), "not main thread");
    rv = NS_NewThread(getter_AddRefs(mProcessWatcher),
                      NS_NewRunnableMethod(this, &nsUpdateProcessor::WaitForProcess));
    NS_ENSURE_SUCCESS(rv, rv);
  }

  return rv;
}

void
nsUpdateProcessor::WaitForProcess()
{
  NS_ABORT_IF_FALSE(!NS_IsMainThread(), "main thread");
  ::WaitForProcess(mUpdaterPID);
  NS_DispatchToMainThread(NS_NewRunnableMethod(this, &nsUpdateProcessor::UpdateDone));
}

void
nsUpdateProcessor::UpdateDone()
{
  NS_ABORT_IF_FALSE(NS_IsMainThread(), "not main thread");
  mProcessWatcher->Shutdown();
  mProcessWatcher = nsnull;

  nsCOMPtr<nsIUpdateManager> um =
    do_GetService("@mozilla.org/updates/update-manager;1");
  if (um) {
    um->RefreshUpdateStatus(mUpdate);
  }
  mUpdate = nsnull;
}

