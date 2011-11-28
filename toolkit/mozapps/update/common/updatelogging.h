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
 * The Original Code is common code between maintenanceservice and updater
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

#ifndef UPDATELOGGING_H
#define UPDATELOGGING_H

#include "updatedefines.h"
#include <stdio.h>

#ifndef MAXPATHLEN
# ifdef PATH_MAX
#  define MAXPATHLEN PATH_MAX
# elif defined(MAX_PATH)
#  define MAXPATHLEN MAX_PATH
# elif defined(_MAX_PATH)
#  define MAXPATHLEN _MAX_PATH
# elif defined(CCHMAXPATH)
#  define MAXPATHLEN CCHMAXPATH
# else
#  define MAXPATHLEN 1024
# endif
#endif

#if defined(XP_WIN)
#define NS_tsnprintf(dest, count, fmt, ...) \
  PR_BEGIN_MACRO \
  int _count = count - 1; \
  _snwprintf(dest, _count, fmt, ##__VA_ARGS__); \
  dest[_count] = L'\0'; \
  PR_END_MACRO
#else
#define NS_tsnprintf snprintf
#endif

class UpdateLog
{
public:
  static UpdateLog & GetPrimaryLog() 
  {
    if (!primaryLog) {
      primaryLog = new UpdateLog();
    }
    return *primaryLog;
  }

  void Init(NS_tchar* sourcePath, NS_tchar* fileName);
  void Finish();
  void Printf(const char *fmt, ... );

  ~UpdateLog()
  {
    delete primaryLog;
  }

protected:
  UpdateLog();
  FILE *logFP;
  NS_tchar* sourcePath;

  static UpdateLog* primaryLog;
};

#define LOG(args) UpdateLog::GetPrimaryLog().Printf args
#define LogInit(PATHNAME_, FILENAME_) \
  UpdateLog::GetPrimaryLog().Init(PATHNAME_, FILENAME_)
#define LogFinish() UpdateLog::GetPrimaryLog().Finish()

#endif
