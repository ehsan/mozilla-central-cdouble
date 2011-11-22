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
 * The Original Code is Maintenance service certificate check code.
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

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <softpub.h>
#include <wintrust.h>

#include "certificatecheck.h"
#include "servicebase.h"

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

static const int ENCODING = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;

/**
 * Checks to see if a file stored at filePath matches the specified info.
 *
 * @param  filePath    The PE file path to check
 * @param  infoToMatch The acceptable information to match
 * @return ERROR_SUCCESS if successful, ERROR_NOT_FOUND if the info 
 *         does not match, or the last error otherwise.
 */
DWORD
CheckCertificateForPEFile(LPCWSTR filePath, 
                          CertificateCheckInfo &infoToMatch)
{
  HCERTSTORE hStore = NULL;
  HCRYPTMSG hMsg = NULL; 
  PCCERT_CONTEXT pCertContext = NULL;
  PCMSG_SIGNER_INFO pSignerInfo = NULL;
  DWORD lastError = ERROR_SUCCESS;
  PublisherInfo progPubInfo;
  ZeroMemory(&progPubInfo, sizeof(progPubInfo));

  // SEH like exception handling is only used for cleanup of ugly C API calls.
  __try {
    // Get the HCERTSTORE and HCRYPTMSG from the signed file.
    DWORD dwEncoding, dwContentType, dwFormatType;
    BOOL result = CryptQueryObject(CERT_QUERY_OBJECT_FILE,
                                   filePath, 
                                   CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                                   CERT_QUERY_CONTENT_FLAG_ALL, 
                                   0, &dwEncoding, &dwContentType,
                                   &dwFormatType, &hStore, &hMsg, NULL);
    if (!result) {
      lastError = GetLastError();
      PR_LOG(gServiceLog, PR_LOG_ALWAYS, 
             ("CryptQueryObject failed with %d\n", lastError));
      __leave;
    }

    // Pass in NULL to get the needed signer information size.
    DWORD dwSignerInfo;
    result = CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, 
                              NULL, &dwSignerInfo);
    if (!result) {
      lastError = GetLastError();
      PR_LOG(gServiceLog, PR_LOG_ALWAYS, 
             ("CryptMsgGetParam failed with %d\n", lastError));
      __leave;
    }

    // Allocate the needed size for the signer information.
    pSignerInfo = (PCMSG_SIGNER_INFO)LocalAlloc(LPTR, dwSignerInfo);
    if (!pSignerInfo) {
      lastError = GetLastError();
      PR_LOG(gServiceLog, PR_LOG_ALWAYS, 
             ("Unable to allocate memory for Signer Info.\n"));
      __leave;
    }

    // Get the signer information (PCMSG_SIGNER_INFO).
    // In particular we want the issuer and serial number.
    result = CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, 
                              (PVOID)pSignerInfo, &dwSignerInfo);
    if (!result) {
      lastError = GetLastError();
      PR_LOG(gServiceLog, PR_LOG_ALWAYS, 
             ("CryptMsgGetParam failed with %d\n", lastError));
      __leave;
    }

    // Search for the signer certificate in the certificate store.
    CERT_INFO CertInfo;     
    CertInfo.Issuer = pSignerInfo->Issuer;
    CertInfo.SerialNumber = pSignerInfo->SerialNumber;
    pCertContext = CertFindCertificateInStore(hStore, ENCODING, 0, 
                                              CERT_FIND_SUBJECT_CERT,
                                              (PVOID)&CertInfo, NULL);
    if (!pCertContext) {
      lastError = GetLastError();
      PR_LOG(gServiceLog, PR_LOG_ALWAYS, 
             ("CertFindCertificateInStore failed with %d\n", lastError));
      __leave;
    }

    if (!DoCertificateAttributesMatch(pCertContext, infoToMatch)) {
      lastError = ERROR_NOT_FOUND;
      PR_LOG(gServiceLog, PR_LOG_ALWAYS, 
             ("Certificate did not match issuer or name\n"));
      __leave;
    }

    if (infoToMatch.signerInfo.programName || 
        infoToMatch.signerInfo.publisherLink || 
        infoToMatch.signerInfo.moreInfoLink) {
      // Get program name and publisher information from the signer info.
      if (!GetProgramAndPublisherInfo(pSignerInfo, progPubInfo)) {
        lastError = GetLastError();
        PR_LOG(gServiceLog, PR_LOG_ALWAYS, 
               ("GetProgramAndPublisherInfo failed with %d\n", lastError));
        __leave;
      }

      if (infoToMatch.signerInfo.programName && 
          wcslen(infoToMatch.signerInfo.programName) > 0 &&
          ((infoToMatch.signerInfo.programName && !progPubInfo.programName) ||
           (wcscmp(infoToMatch.signerInfo.programName, 
                   progPubInfo.programName)))) {
        lastError = ERROR_NOT_FOUND;
        PR_LOG(gServiceLog, PR_LOG_ALWAYS, 
               ("Program name did not match\n"));
        __leave;
      }

      if (infoToMatch.signerInfo.publisherLink && 
          wcslen(infoToMatch.signerInfo.publisherLink) > 0 &&
          ((infoToMatch.signerInfo.publisherLink && 
           !progPubInfo.publisherLink) ||
           (wcscmp(infoToMatch.signerInfo.publisherLink, 
                   progPubInfo.publisherLink)))) {
        lastError = ERROR_NOT_FOUND;
        PR_LOG(gServiceLog, PR_LOG_ALWAYS, 
               ("Publisher link did not match\n"));
        __leave;
      }

      if (infoToMatch.signerInfo.moreInfoLink && 
          wcslen(infoToMatch.signerInfo.moreInfoLink) > 0 &&
          ((infoToMatch.signerInfo.moreInfoLink && 
            !progPubInfo.moreInfoLink) ||
           (wcscmp(infoToMatch.signerInfo.moreInfoLink, 
                   progPubInfo.moreInfoLink)))) {
        lastError = ERROR_NOT_FOUND;
        PR_LOG(gServiceLog, PR_LOG_ALWAYS, 
               ("MoreInfo link did not match\n"));
        __leave;
      }
    }
  } __finally {
    if (progPubInfo.programName) {
      LocalFree(progPubInfo.programName);
    }
    if (progPubInfo.publisherLink) {
      LocalFree(progPubInfo.publisherLink);
    }
    if (progPubInfo.moreInfoLink) {
      LocalFree(progPubInfo.moreInfoLink);
    }
    if (pSignerInfo) {
      LocalFree(pSignerInfo);
    }
    if (pCertContext) {
      CertFreeCertificateContext(pCertContext);
    }
    if (hStore) { 
      CertCloseStore(hStore, 0);
    }
    if (hMsg) { 
      CryptMsgClose(hMsg);
    }
  }
  return lastError;
}

/**
 * Checks to see if a file stored at filePath matches the specified info.
 *
 * @param  pCertContext The certificate context of the file
 * @param  infoToMatch  The acceptable information to match
 * @return FALSE if the info does not match or if any error occurs in the check
 */
BOOL 
DoCertificateAttributesMatch(PCCERT_CONTEXT pCertContext, 
                             CertificateCheckInfo &infoToMatch)
{
  DWORD dwData;
  LPTSTR szName = NULL;

  if (infoToMatch.issuer) {
    // Pass in NULL to get the needed size of the issuer buffer.
    dwData = CertGetNameString(pCertContext, 
                               CERT_NAME_SIMPLE_DISPLAY_TYPE,
                               CERT_NAME_ISSUER_FLAG, NULL,
                               NULL, 0);
    if (!dwData) {
      PR_LOG(gServiceLog, PR_LOG_ALWAYS, 
             ("CertGetNameString failed.\n"));
      return FALSE;
    }

    // Allocate memory for Issuer name buffer.
    LPTSTR szName = (LPTSTR)LocalAlloc(LPTR, dwData * sizeof(WCHAR));
    if (!szName) {
      PR_LOG(gServiceLog, PR_LOG_ALWAYS, 
             ("Unable to allocate memory for issuer name.\n"));
      return FALSE;
    }

    // Get Issuer name.
    if (!CertGetNameString(pCertContext, CERT_NAME_SIMPLE_DISPLAY_TYPE,
                            CERT_NAME_ISSUER_FLAG, NULL, szName, dwData)) {
      PR_LOG(gServiceLog, PR_LOG_ALWAYS, 
             ("CertGetNameString failed.\n"));
      LocalFree(szName);
      return FALSE;
    }

    // If the issuer does not match, return a failure.
    if (infoToMatch.issuer &&
        wcslen(infoToMatch.issuer) &&
        wcscmp(szName, infoToMatch.issuer)) {
      LocalFree(szName);
      return FALSE;
    }

    LocalFree(szName);
    szName = NULL;
  }

  if (infoToMatch.name) {
    // Pass in NULL to get the needed size of the name buffer.
    dwData = CertGetNameString(pCertContext, CERT_NAME_SIMPLE_DISPLAY_TYPE,
                               0, NULL, NULL, 0);
    if (!dwData) {
      PR_LOG(gServiceLog, PR_LOG_ALWAYS, 
             ("CertGetNameString failed.\n"));
      return FALSE;
    }

    // Allocate memory for the name buffer.
    szName = (LPTSTR)LocalAlloc(LPTR, dwData * sizeof(WCHAR));
    if (!szName) {
      PR_LOG(gServiceLog, PR_LOG_ALWAYS, 
             ("Unable to allocate memory for subject name.\n"));
      return FALSE;
    }

    // Obtain the name.
    if (!(CertGetNameString(pCertContext, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0,
                            NULL, szName, dwData))) {
      PR_LOG(gServiceLog, PR_LOG_ALWAYS, 
             ("CertGetNameString failed.\n"));
      LocalFree(szName);
      return FALSE;
    }

    // If the issuer does not match, return a failure.
    if (infoToMatch.name && 
        wcslen(infoToMatch.name) > 0 && 
        wcscmp(szName, infoToMatch.name)) {
      LocalFree(szName);
      return FALSE;
    }

    // We have a match!
    LocalFree(szName);
  }

  // If there were any errors we would have aborted by now.
  return TRUE;
}

/**
 * Duplicates the specified string
 *
 * @param  inputString The string to duplicate
 * @return The duplicated string which should be freed by the caller.
 */
LPWSTR 
AllocateAndCopyWideString(LPCWSTR inputString)
{
  LPWSTR outputString = 
    (LPWSTR)LocalAlloc(LPTR, (wcslen(inputString) + 1) * sizeof(WCHAR));
  if (outputString) {
    lstrcpyW(outputString, inputString);
  }
  return outputString;
}

/**
 * Fills the publisher info from the signer info pointer
 *
 * @param  pSignerInfo  The signer info to fill from
 * @param  info         The publisher info to fill to
 * @return TRUE if successful.
 */
BOOL 
GetProgramAndPublisherInfo(PCMSG_SIGNER_INFO pSignerInfo,
                           PublisherInfo &info)
{
  BOOL result = FALSE;
  PSPC_SP_OPUS_INFO OpusInfo = NULL;  
  DWORD dwData;

  __try {
    // Loop through authenticated attributes and 
    // find SPC_SP_OPUS_INFO_OBJID OID.
    for (DWORD n = 0; n < pSignerInfo->AuthAttrs.cAttr; n++) {
      if (lstrcmpA(SPC_SP_OPUS_INFO_OBJID, 
                   pSignerInfo->AuthAttrs.rgAttr[n].pszObjId) == 0) {
        // Get Size of SPC_SP_OPUS_INFO structure.
        _CRYPTOAPI_BLOB &attr1 = pSignerInfo->AuthAttrs.rgAttr[n].rgValue[0];
        result = CryptDecodeObject(ENCODING,
                                   SPC_SP_OPUS_INFO_OBJID,
                                   attr1.pbData,
                                   attr1.cbData,
                                   0, NULL, &dwData);
        if (!result) {
          PR_LOG(gServiceLog, PR_LOG_ALWAYS, 
                 ("CryptDecodeObject failed with %d\n", GetLastError()));
          __leave;
        }

        // Allocate memory for SPC_SP_OPUS_INFO structure.
        OpusInfo = (PSPC_SP_OPUS_INFO)LocalAlloc(LPTR, dwData);
        if (!OpusInfo) {
          PR_LOG(gServiceLog, PR_LOG_ALWAYS, 
                 ("Unable to allocate memory for Publisher Info.\n"));
          __leave;
        }

        // Decode and get SPC_SP_OPUS_INFO structure.
        _CRYPTOAPI_BLOB &attr2 = pSignerInfo->AuthAttrs.rgAttr[n].rgValue[0];
        result = CryptDecodeObject(ENCODING,
                                   SPC_SP_OPUS_INFO_OBJID,
                                   attr2.pbData,
                                   attr2.cbData,
                                   0, OpusInfo, &dwData);
        if (!result) {
          PR_LOG(gServiceLog, PR_LOG_ALWAYS, 
                 ("CryptDecodeObject failed with %d\n", GetLastError()));
          __leave;
        }

        // Fill in Program Name if present.
        if (OpusInfo->pwszProgramName) {
          info.programName =
            AllocateAndCopyWideString(OpusInfo->pwszProgramName);
        } else {
          info.programName = NULL;
        }

        // Fill in Publisher Information if present.
        if (OpusInfo->pPublisherInfo) {
          switch (OpusInfo->pPublisherInfo->dwLinkChoice) {
            case SPC_URL_LINK_CHOICE:
              info.publisherLink =
                AllocateAndCopyWideString(OpusInfo->pPublisherInfo->pwszUrl);
              break;
            case SPC_FILE_LINK_CHOICE:
              info.publisherLink =
                AllocateAndCopyWideString(OpusInfo->pPublisherInfo->pwszFile);
              break;
            default:
              info.publisherLink = NULL;
              break;
          }
        } else {
          info.publisherLink = NULL;
        }

        // Fill in More Info if present.
        if (OpusInfo->pMoreInfo) {
          switch (OpusInfo->pMoreInfo->dwLinkChoice) {
          case SPC_URL_LINK_CHOICE:
            info.moreInfoLink =
              AllocateAndCopyWideString(OpusInfo->pMoreInfo->pwszUrl);
            break;
          case SPC_FILE_LINK_CHOICE:
            info.moreInfoLink =
              AllocateAndCopyWideString(OpusInfo->pMoreInfo->pwszFile);
            break;
          default:
            info.moreInfoLink = NULL;
            break;
          }
        } else {
          info.moreInfoLink = NULL;
        }
        result = TRUE;
        break;
      }
    }
  } __finally {
    if (OpusInfo != NULL) {
      LocalFree(OpusInfo);
    }
  }
  return result;
}

/**
 * Verifies the trust of the specified file path.
 *
 * @param  filePath  The file path to check.
 * @return ERROR_SUCCESS if successful, or the last error code otherwise.
 */
DWORD
VerifyCertificateTrustForFile(LPCWSTR filePath)
{
  // Initialize the WINTRUST_FILE_INFO structure.
  WINTRUST_FILE_INFO FileData;
  memset(&FileData, 0, sizeof(FileData));
  FileData.cbStruct = sizeof(WINTRUST_FILE_INFO);
  FileData.pcwszFilePath = filePath;
  FileData.hFile = NULL;
  FileData.pgKnownSubject = NULL;

  // WVTPolicyGUID specifies the policy to apply on the file
  // WINTRUST_ACTION_GENERIC_VERIFY_V2 policy checks:
  //   
  // 1) The certificate used to sign the file chains up to a root 
  // certificate located in the trusted root certificate store. This 
  // implies that the identity of the publisher has been verified by 
  // a certification authority.
  // 
  // 2) In cases where user interface is displayed (which this example
  // does not do), WinVerifyTrust will check for whether the  
  // end entity certificate is stored in the trusted publisher store,  
  // implying that the user trusts content from this publisher.
  // 
  // 3) The end entity certificate has sufficient permission to sign 
  // code, as indicated by the presence of a code signing EKU or no EKU.
  GUID WVTPolicyGUID = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  WINTRUST_DATA WinTrustData;

  // Initialize the WinVerifyTrust input data structure.
  // Default all fields to 0.
  memset(&WinTrustData, 0, sizeof(WinTrustData));
  WinTrustData.cbStruct = sizeof(WinTrustData);
  // Use default code signing EKU.
  WinTrustData.pPolicyCallbackData = NULL;
  // No data to pass to SIP.
  WinTrustData.pSIPClientData = NULL;
  // Disable WVT UI.
  WinTrustData.dwUIChoice = WTD_UI_NONE;
  // No revocation checking.
  WinTrustData.fdwRevocationChecks = WTD_REVOKE_NONE; 
  // Verify an embedded signature on a file.
  WinTrustData.dwUnionChoice = WTD_CHOICE_FILE;
  // Default verification.
  WinTrustData.dwStateAction = 0;
  // Not applicable for default verification of embedded signature.
  WinTrustData.hWVTStateData = NULL;
  // Not used.
  WinTrustData.pwszURLReference = NULL;
  // This is not applicable if there is no UI because it changes the UI
  // to accommodate running applications instead of installing applications.
  WinTrustData.dwUIContext = 0;
  // Set pFile.
  WinTrustData.pFile = &FileData;

  // WinVerifyTrust verifies signatures as specified by the GUID 
  // and Wintrust_Data.
  LONG lStatus = WinVerifyTrust(NULL, &WVTPolicyGUID, &WinTrustData);
  DWORD dwLastError = GetLastError();
  BOOL validated = FALSE;
  switch (lStatus) {
    case ERROR_SUCCESS:
      // The hash that represents the subject is trusted and there were no
      // verification errors.  No publisher nor time stamp chain errors.
      PR_LOG(gServiceLog, PR_LOG_ALWAYS, 
             ("The file \"%ls\" is signed and the signature was verified.\n",
              filePath));
      validated = TRUE;
      break;
    case TRUST_E_NOSIGNATURE:
      // The file was not signed or had a signature that was not valid.
      // Get the reason for no signature.
      if (TRUST_E_TIME_STAMP == dwLastError) {
        // The file was not signed.
        PR_LOG(gServiceLog, PR_LOG_ALWAYS, 
               ("The file \"%ls\" has a timestamp error.\n", filePath));
      } else if (TRUST_E_NOSIGNATURE == dwLastError ||
                TRUST_E_SUBJECT_FORM_UNKNOWN == dwLastError ||
                TRUST_E_PROVIDER_UNKNOWN == dwLastError) {
        // The file was not signed.
        PR_LOG(gServiceLog, PR_LOG_ALWAYS, 
               ("The file \"%ls\" is not signed.\n", filePath));
      } else {
        // The signature was not valid or there was an error 
        // opening the file.
        PR_LOG(gServiceLog, PR_LOG_ALWAYS, 
               ("An unknown error occurred trying to verify the signature of "
                "the \"%ls\" file.\n", filePath));
      }
      break;
    case TRUST_E_EXPLICIT_DISTRUST:
      // The hash that represents the subject or the publisher 
      // is not allowed by the admin or user.
      PR_LOG(gServiceLog, PR_LOG_ALWAYS, 
             ("The signature is present, but specifically disallowed.\n"));
      break;
    case TRUST_E_SUBJECT_NOT_TRUSTED:
      // The user clicked "No" when asked to install and run.
      // Since the UI is disabled I'm not sure if this happens
      // some time in the past, or if the error will simply never happen.
      PR_LOG(gServiceLog, PR_LOG_ALWAYS, 
             ("The signature is present, but not trusted.\n"));
      break;
    case CRYPT_E_SECURITY_SETTINGS:
      // The hash that represents the subject or the publisher was not 
      // explicitly trusted by the admin and the admin policy has disabled 
      // user trust. No signature, publisher or time stamp errors.
      PR_LOG(gServiceLog, PR_LOG_ALWAYS, 
             ("CRYPT_E_SECURITY_SETTINGS - The hash "
              "representing the subject or the publisher wasn't "
              "explicitly trusted by the admin and admin policy "
              "has disabled user trust. No signature, publisher "
              "or timestamp errors.\n"));
      break;
    default:
      // The UI was disabled in dwUIChoice or the admin policy has disabled
      // user trust. lStatus contains the publisher or time stamp chain error.
      PR_LOG(gServiceLog, PR_LOG_ALWAYS, 
             ("Error is: %d\n", lStatus));
      break;
  }

  return validated ? ERROR_SUCCESS : dwLastError;
}
