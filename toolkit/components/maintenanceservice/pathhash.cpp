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
 * The Original Code is for Maintenance service path hashing 
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
 * decision by deleting the provisions /PGM and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include <windows.h>
#include <wincrypt.h>
#include "pathhash.h"


/**
 * Converts a binary sequence into a hex string
 *
 * @param hash      The binary data sequence
 * @param hashSize  The size of the binary data sequence
 * @param hexString A buffer to store the hex string, must be of 
 *        size 2 * @hashSize
*/
static void
BinaryDataToHexString(const BYTE *hash, DWORD &hashSize, 
                      LPWSTR hexString)
{
  WCHAR *p = hexString;
  for (DWORD i = 0; i < hashSize; ++i) {
    wsprintfW(p, L"%.2x", hash[i]);
    p += 2;
  }
}

/**
 * Calculates an MD5 hash for the given input binary data
 *
 * @param  data     Any sequence of bytes
 * @param  dataSize The number of bytes inside @data
 * @param  hash     Output buffer to store hash, must be freed by the caller
 * @param  hashSize The number of bytes in the output buffer
 * @return TRUE on success
*/
static BOOL
CalculateMD5(const char *data, DWORD dataSize, 
             BYTE **hash, DWORD &hashSize)
{
  HCRYPTPROV hProv = 0;
  HCRYPTHASH hHash = 0;

  if(!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, 0)) {
    if (NTE_BAD_KEYSET == GetLastError()) {
      // Maybe it doesn't exist, try to create it.
      if(!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, 
                              CRYPT_NEWKEYSET)) {
        return FALSE;
      }
    } else {
      return FALSE;
    }
  }

  if(!CryptCreateHash(hProv, CALG_MD5, 0, 0, &hHash)) {
    return FALSE;
  }

  if(!CryptHashData(hHash, reinterpret_cast<const BYTE*>(data), 
                    dataSize, 0)) {
    return FALSE;
  }

  DWORD dwCount = sizeof(DWORD);
  if(!CryptGetHashParam(hHash, HP_HASHSIZE, (BYTE *)&hashSize, 
                        &dwCount, 0)) {
    return FALSE;
  }
  
  *hash = new BYTE[hashSize];
  ZeroMemory(*hash, hashSize);
  if(!CryptGetHashParam(hHash, HP_HASHVAL, *hash, &hashSize, 0)) {
    return FALSE;
  }

  if(hHash) {
    CryptDestroyHash(hHash);
  }

  if(hProv) {
    CryptReleaseContext(hProv,0);
  }

  return TRUE;
}

/**
 * Converts a file path into a unique registry location for cert storage
 *
 * @param  filePath     The input file path to get a registry path from
 * @param  registryPath A buffer to write the registry path to, must 
 *         be of size in WCHARs MAX_PATH + 1
 * @return TRUE if successful
*/
BOOL
CalculateRegistryPathFromFilePath(const LPCWSTR filePath, 
                                  LPWSTR registryPath)
{
  size_t filePathLen = wcslen(filePath); 
  if (!filePathLen)
    return FALSE;

  // If the file path ends in a slash, ignore that character
  if (filePath[filePathLen -1] == L'\\' || 
      filePath[filePathLen - 1] == L'/') {
    filePathLen--;
  }

  WCHAR *lowercasePath = new WCHAR[filePathLen + 1];
  wcscpy(lowercasePath, filePath);
  _wcslwr(lowercasePath);

  BYTE *hash;
  DWORD hashSize = 0;
  if (!CalculateMD5(reinterpret_cast<const char*>(lowercasePath), 
                    filePathLen * 2, 
                    &hash, hashSize)) {
    delete[] lowercasePath;
    return FALSE;
  }
  delete[] lowercasePath;

  LPCWSTR baseRegPath = L"SOFTWARE\\Mozilla\\"
    L"MaintenanceService\\";
  wcsncpy(registryPath, baseRegPath, MAX_PATH);
  BinaryDataToHexString(hash, hashSize, 
                        registryPath + wcslen(baseRegPath));
  delete[] hash;
  return TRUE;
}
