# Version: MPL 1.1/GPL 2.0/LGPL 2.1
#
# The contents of this file are subject to the Mozilla Public License Version
# 1.1 (the "License"); you may not use this file except in compliance with
# the License. You may obtain a copy of the License at
# http://www.mozilla.org/MPL/
#
# Software distributed under the License is distributed on an "AS IS" basis,
# WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
# for the specific language governing rights and limitations under the
# License.
#
# The Original Code is an NSIS installer for the maintenance service
#
# The Initial Developer of the Original Code is
# Mozilla Foundation.
# Portions created by the Initial Developer are Copyright (C) 2011
# the Initial Developer. All Rights Reserved.
#
# Contributor(s):
#  Brian R. Bondy <netzen@gmail.com>
#
# Alternatively, the contents of this file may be used under the terms of
# either the GNU General Public License Version 2 or later (the "GPL"), or
# the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
# in which case the provisions of the GPL or the LGPL are applicable instead
# of those above. If you wish to allow use of your version of this file only
# under the terms of either the GPL or the LGPL, and not to allow others to
# use your version of this file under the terms of the MPL, indicate your
# decision by deleting the provisions above and replace them with the notice
# and other provisions required by the GPL or the LGPL. If you do not delete
# the provisions above, a recipient may use your version of this file under
# the terms of any one of the MPL, the GPL or the LGPL.
#
# ***** END LICENSE BLOCK *****

# Required Plugins:
# ServicesHelper

; Set verbosity to 3 (e.g. no script) to lessen the noise in the build logs
!verbose 3

; 7-Zip provides better compression than the lzma from NSIS so we add the files
; uncompressed and use 7-Zip to create a SFX archive of it
SetDatablockOptimize on
SetCompress off
CRCCheck on

RequestExecutionLevel admin
!addplugindir ./

; Variables
Var PageName
Var TempMaintServiceName

; Modenr UI
!include "MUI2.nsh"


; Other included files may depend upon these includes!
; The following includes are provided by NSIS.
!include FileFunc.nsh
!include LogicLib.nsh
!include MUI.nsh
!include WinMessages.nsh
!include WinVer.nsh
!include WordFunc.nsh

!insertmacro GetOptions
!insertmacro GetParameters
!insertmacro GetSize

!define CompanyName "Mozilla Corporation"

; The following includes are custom.
!include defines.nsi
; We keep defines.nsi defined so that we get other things like 
; the version number, but we redefine BrandFullName
!define MaintFullName "Mozilla Maintenance Service"
!undef BrandFullName
!define BrandFullName "${MaintFullName}"

!include common.nsh
!include locales.nsi

; Must be inserted before other macros that use logging
!insertmacro _LoggingCommon
!insertmacro SetBrandNameVars
!insertmacro UpdateShortcutAppModelIDs

Name "${MaintFullName}"
OutFile "maintenanceservice_installer.exe"
InstallDir "$PROGRAMFILES\${MaintFullName}"

; Get installation folder from registry if available
InstallDirRegKey HKCU "Software\Mozilla\MaintenanceService" ""

SetOverwrite on

!define MaintUninstallKey \
 "Software\Microsoft\Windows\CurrentVersion\Uninstall\MozillaMaintenanceService"

!ifdef HAVE_64BIT_OS
  InstallDir "$PROGRAMFILES64\${MaintFullName}\"
!else
  InstallDir "$PROGRAMFILES32\${MaintFullName}\"
!endif
ShowInstDetails nevershow

################################################################################
# Modern User Interface - MUI

!define MUI_ICON setup.ico
!define MUI_UNICON setup.ico
!define MUI_WELCOMEPAGE_TITLE_3LINES
!define MUI_UNWELCOMEFINISHPAGE_BITMAP wizWatermark.bmp

;Interface Settings
!define MUI_ABORTWARNING

; Uninstaller Pages
!define MUI_PAGE_CUSTOMFUNCTION_PRE un.preWelcome
!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

################################################################################
# Language

!insertmacro MOZ_MUI_LANGUAGE 'baseLocale'
!verbose push
!verbose 3
!include "overrideLocale.nsh"
!include "customLocale.nsh"
!verbose pop

Function .onInit
  SetSilent silent
FunctionEnd

Function un.onInit
  StrCpy $BrandFullNameDA "${MaintFullName}"
FunctionEnd

Function un.preWelcome
  StrCpy $PageName "Welcome"
  ${If} ${FileExists} "$EXEDIR\core\distribution\modern-wizard.bmp"
    Delete "$PLUGINSDIR\modern-wizard.bmp"
    CopyFiles /SILENT "$EXEDIR\core\distribution\modern-wizard.bmp" \
              "$PLUGINSDIR\modern-wizard.bmp"
  ${EndIf}
FunctionEnd

Section "MaintenanceService"
  AllowSkipFiles off

  ${LogHeader} "Installing Main Files"

  CreateDirectory $INSTDIR
  SetOutPath $INSTDIR

  ; Stop the maintenance service so we can overwrite any
  ; binaries that it uses.
  ; 1 for wait for file release, and 30 second timeout
  ServicesHelper::Stop "MozillaMaintenance"

  ; If we don't have maintenanceservice.exe already installed
  ; then keep that name.  If we do use maintenanceservice_tmp.exe
  ; which will auto install itself when you call it with the install parameter.
  StrCpy $TempMaintServiceName "maintenanceservice.exe"
  IfFileExists "$INSTDIR\maintenanceservice.exe" 0 skipAlreadyExists
    StrCpy $TempMaintServiceName "maintenanceservice_tmp.exe"
  skipAlreadyExists:

  ; We always write out a copy and then decide whether to install it or 
  ; not via calling its 'install' cmdline which works by version comparison.
  CopyFiles "$EXEDIR\maintenanceservice.exe" "$INSTDIR\$TempMaintServiceName"

  ; Install the application updater service.
  ; If a service already exists, the command line parameter will stop the
  ; service and only install itself if it is newer than the already installed
  ; service.  If successful it will remove the old maintenanceservice.exe
  ; and replace it with maintenanceservice_tmp.exe.
  ClearErrors
  ${GetParameters} $0
  ${GetOptions} "$0" "/Upgrade" $0
  ${Unless} ${Errors}
    ; The upgrade cmdline is the same as install except
    ; It will fail if the service isn't already installed.
    nsExec::Exec '"$INSTDIR\$TempMaintServiceName" upgrade'
  ${Else}
    nsExec::Exec '"$INSTDIR\$TempMaintServiceName" install'
  ${EndIf}

  ${GetLongPath} "$INSTDIR" $8
  WriteRegStr HKLM "${MaintUninstallKey}" "DisplayName" "${MaintFullName}"
  WriteRegStr HKLM "${MaintUninstallKey}" "UninstallString" \
                   '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${MaintUninstallKey}" "DisplayVersion" "${AppVersion}"
  WriteRegStr HKLM "${MaintUninstallKey}" "Publisher" "Mozilla"
  WriteRegStr HKLM "${MaintUninstallKey}" "Comments" \
                   "${BrandFullName} ${AppVersion} (${ARCH} ${AB_CD})"
  ${GetSize} "$8" "/S=0K" $R2 $R3 $R4
  WriteRegDWORD HKLM "${MaintUninstallKey}" "EstimatedSize" $R2 

  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ; Write out that a maintenance service was attempted.
  ; We do this because on upgrades we will check this value and we only
  ; want to install once on the first upgrade to maintenance service.
  ; Also write out that we are currently installed, preferences will check
  ; this value to determine if we should show the service update pref.
  ; Since the Maintenance service can be installed either x86 or x64,
  ; always use the 64-bit registry for checking if an attempt was made.
  ; *Nothing* should be added under here that modifies the registry
  ; unless it restores the registry view.
  SetRegView 64
  WriteRegDWORD HKLM "Software\Mozilla\MaintenanceService" "Attempted" 1
  WriteRegDWORD HKLM "Software\Mozilla\MaintenanceService" "Installed" 1

  # The Mozilla/updates directory will have an inherited permission
  # which allows any user to write to it.  Work items are written there.
  SetShellVarContext all
  CreateDirectory "$APPDATA\Mozilla\updates"
SectionEnd

Section "Uninstall"
  ; Delete the service so that no updates will be attempted
  nsExec::Exec '"$INSTDIR\maintenanceservice.exe" uninstall'

  Delete /REBOOTOK "$INSTDIR\maintenanceservice.exe"
  Delete /REBOOTOK "$INSTDIR\maintenanceservice_tmp.exe"
  Delete /REBOOTOK "$INSTDIR\Uninstall.exe"
  RMDir /REBOOTOK "$INSTDIR"

  DeleteRegKey HKLM "${MaintUninstallKey}"

  # Keep this last since it modifies the registry key view
  SetRegView 64
  DeleteRegValue HKLM "Software\Mozilla\MaintenanceService" "Installed"
SectionEnd
