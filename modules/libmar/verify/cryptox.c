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
 * The Original Code is cryptographic wrappers for Mozilla archive code.
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

#include <stdlib.h>
#include "cryptox.h"

#if defined(MAR_NSS)

CryptoX_Result NSS_Init(const char *configDir)
{
  SECStatus status;
  if (!configDir) {
    return CryptoX_Error;
  }

  status = NSS_Initialize(configDir, 
                          "", "", 
                          SECMOD_DB, NSS_INIT_READONLY);
  if (SECSuccess != status) {
    return status;
  }

  return SECSuccess == status ? CryptoX_Success : CryptoX_Error;
}

CryptoX_Result NSS_LoadPublicKey(const char *certNickname, 
                                 SECKEYPublicKey **publicKey, 
                                 CERTCertificate **cert)
{
  secuPWData pwdata = { PW_NONE, 0 };
  if (!cert || !publicKey) {
    return CryptoX_Error;
  }

  /* Get the cert and embedded public key out of the database */
  *cert = PK11_FindCertFromNickname(certNickname, &pwdata);
  if (!*cert) {
    return CryptoX_Error;
  }
  *publicKey = CERT_ExtractPublicKey(*cert);
  if (!*publicKey) {
    return CryptoX_Error;
  }
  return CryptoX_Success;
}

CryptoX_Result NSS_VerifyBegin(VFYContext **ctx, SECKEYPublicKey **publicKey)
{
  SECStatus status;
  *ctx = VFY_CreateContext(*publicKey, NULL, 
                           SEC_OID_ISO_SHA1_WITH_RSA_SIGNATURE, NULL);
  status = VFY_Begin(*ctx);
  return SECSuccess == status ? CryptoX_Success : CryptoX_Error;
}

CryptoX_Result NSS_VerifySignature(VFYContext **ctx, 
                                   char *signature, 
                                   size_t signatureLen)
{
  SECItem signedItem;
  SECStatus status;
  signedItem.len = signatureLen;
  signedItem.data = signature;
  status = VFY_EndWithSignature(*ctx, &signedItem);
  return SECSuccess == status ? CryptoX_Success : CryptoX_Error;
}

#elif defined(XP_WIN)
CryptoX_Result CyprtoAPI_VerifySignature(HCRYPTHASH *hash, 
                                         HCRYPTKEY *pubKey,
                                         char *signature, 
                                         DWORD signatureLen)
{
  size_t i;
  BOOL result;
  char *signatureReversed = malloc(signatureLen);
  for (i = 0; i < signatureLen; i++) {
    signatureReversed[i] = signature[signatureLen - 1 - i]; 
  }
  result = CryptVerifySignature(*hash, (BYTE*)signatureReversed,
                                signatureLen, *pubKey, NULL, 0);
  free(signatureReversed);
  return result ? CryptoX_Success : CryptoX_Error;
}

CryptoX_Result CryptoAPI_LoadPublicKey(HCRYPTPROV hProv, 
                                       const char *certData,
                                       size_t sizeOfCertData,
                                       HCRYPTKEY *publicKey,
                                       HCERTSTORE *hCertStore)
{
  CRYPT_DATA_BLOB blob;
  CERT_CONTEXT *context;

  blob.cbData = sizeOfCertData;
  blob.pbData = CryptMemAlloc(sizeOfCertData);
  memcpy(blob.pbData, certData, sizeOfCertData);
  if (!CryptQueryObject(CERT_QUERY_OBJECT_BLOB, &blob, 
                        CERT_QUERY_CONTENT_FLAG_CERT, 
                        CERT_QUERY_FORMAT_FLAG_BINARY, 
                        0, NULL, NULL, NULL, 
                        hCertStore, NULL, (const void **)&context)) {
    CryptMemFree(blob.pbData);
    return CryptoX_Error;
  }

  CryptMemFree(blob.pbData);
  if (!CryptImportPublicKeyInfo(hProv, 
                                PKCS_7_ASN_ENCODING | X509_ASN_ENCODING,
                                &context->pCertInfo->SubjectPublicKeyInfo,
                                publicKey)) {
    CertFreeCertificateContext(context);
    return CryptoX_Error;
  }

  CertFreeCertificateContext(context);
  return CryptoX_Success;
}

CryptoX_Result CryptoAPI_InitCryptoContext(HCRYPTPROV *provider)
{
  BOOL result = TRUE;
  *provider = (HCRYPTPROV)NULL;
  if(!CryptAcquireContext(provider, NULL, NULL, PROV_RSA_FULL, 0)) {
    if (NTE_BAD_KEYSET == GetLastError()) {
      /* Maybe it doesn't exist, try to create it. */
      result = CryptAcquireContext(provider, NULL, NULL, PROV_RSA_FULL,
                                   CRYPT_NEWKEYSET);
    }
  }
  return result ? CryptoX_Success : CryptoX_Error;
}

CryptoX_Result CryptoAPI_VerifyBegin(HCRYPTPROV provider, HCRYPTHASH* hash) 
{
  *hash = (HCRYPTHASH)NULL;
  return CryptCreateHash(provider, CALG_SHA1,
                         0, 0, hash) ? CryptoX_Success : CryptoX_Error;
}

CryptoX_Result CryptoAPI_VerifyUpdate(HCRYPTHASH* hash, char *buf, size_t len)
{
  return CryptHashData(*hash, (BYTE*)buf, len, 0) ? 
         CryptoX_Success : CryptoX_Error;
}

#endif



