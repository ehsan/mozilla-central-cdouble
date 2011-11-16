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
 * The Original Code is Maintenance service UAC helper functions.
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
#include "uachelper.h"

typedef BOOL (WINAPI *LPWTSQueryUserToken)(ULONG, PHANDLE);

/**
 * Determines if the OS is vista or later
 *
 * @return TRUE if the OS is vista or later.
 */
BOOL
UACHelper::IsVistaOrLater() 
{
  // Check if we are running Vista or later.
  OSVERSIONINFO osInfo;
  osInfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
  return GetVersionEx(&osInfo) && osInfo.dwMajorVersion >= 6;
}

/**
 * Opens a user token for the given session ID
 *
 * @param  sessionID  The session ID for the token to obtain
 * @return A handle to the token to obtain which will be primary if enough
 *         permissions exist.  Caller should close the handle.
 */
HANDLE
UACHelper::OpenUserToken(DWORD sessionID)
{
  HMODULE module = LoadLibraryW(L"wtsapi32.dll");
  HANDLE token = NULL;
  LPWTSQueryUserToken wtsQueryUserToken = 
    (LPWTSQueryUserToken)GetProcAddress(module, "WTSQueryUserToken");
  if (wtsQueryUserToken) {
    wtsQueryUserToken(sessionID, &token);
  }
  FreeModule(module);
  return token;
}

/**
 * Opens a linked token for the specified token.
 *
 * @param  token The token to get the linked token from
 * @return A linked token or NULL if one does not exist.
 *         Caller should close the handle.
 */
HANDLE
UACHelper::OpenLinkedToken(HANDLE token) 
{
  // Magic below...
  // UAC creates 2 tokens.  One is the restricted token which we have.
  // the other is the UAC elevated one. Since we are running as a service
  // as the system account we have access to both.
  TOKEN_LINKED_TOKEN tlt;
  HANDLE hNewLinkedToken = NULL;
  DWORD len;
  if(GetTokenInformation(token, (TOKEN_INFORMATION_CLASS)TokenLinkedToken, 
                         &tlt, sizeof(TOKEN_LINKED_TOKEN), &len)) {
    token = tlt.LinkedToken;
    hNewLinkedToken = token;
  }
  return hNewLinkedToken;
}

/**
 * Determines if the user that owns the token is an administrator or not.
 *
 * @param  token   The token of the user to check for admin access.
 * @param  isAdmin returns TRUE if the user is an administrator on success.
 * @return TRUE if there are no errors.
 */
BOOL 
UACHelper::IsUserAdmin(HANDLE token, BOOL &isAdmin)
{
  BOOL success = FALSE;
  SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
  PSID administratorsGroup; 
  if (AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                               DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                               &administratorsGroup)) {
    if (CheckTokenMembership(token, administratorsGroup, &isAdmin)) {
      success = TRUE;
    } 
    FreeSid(administratorsGroup); 
  }
  return success;
}

/**
 * Obtains the elevation type for the given user
 *
 * @param  token The token of the user to check the elevation type for.
 * @param  userType returns the elevation type.
 * @return TRUE if there are no errors.
 */
BOOL 
UACHelper::GetElevationType(HANDLE token, UserType &userType)
{
  if (!IsVistaOrLater()) {
    BOOL isAdmin;
    if (!IsUserAdmin(token, isAdmin)) {
      return FALSE;
    }

    if (isAdmin) {
      userType = AdministratorUACIsOff;
    } else {
      userType = LimitedUser;
    }
    return TRUE;
  }

  // TokenElevationTypeDefault: The token does not have a linked token 
  // This happens when the user is a limited account or UAC is off.
  // TokenElevationTypeFull: The token is an elevated token.
  // TokenElevationTypeLimited: The token is a limited token.
  DWORD returnLength = 0;
  TOKEN_ELEVATION_TYPE elevationType;
  if (!GetTokenInformation(token, TokenElevationType,
                           &elevationType, 
                           sizeof(elevationType), 
                           &returnLength)) {
      return FALSE;
  }

  switch (elevationType) {
    case TokenElevationTypeFull:
      userType = AdministratorElevated;
      break;
    case TokenElevationTypeLimited:
      userType = AdministratorUnelevated;
      break;
    case TokenElevationTypeDefault: {
      // TokenElevationTypeDefault is returned when either the user is a 
      // limited account or the user has UAC off.
      BOOL isAdmin;
      if (!IsUserAdmin(token, isAdmin)) {
        return FALSE;
      }

      if (isAdmin) {
        userType = AdministratorUACIsOff;
      } else {
        userType = LimitedUser;
      }
      break;
    }
    default:
      return FALSE;
  }

  // If we had an error we would have returned already
  return TRUE;
}
