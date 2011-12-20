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
 * The Original Code is Mozilla Archive verify code.
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


#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "mar.h"
#include "mar_private.h"
#include "cryptox.h"
#ifdef XP_WIN
#include <winsock2.h>
#else
#include <netinet/in.h>
#include <unistd.h>
#endif

int mar_verify_signature_fp(FILE *fp, 
                            CryptoX_LibraryHandle provider, 
                            CryptoX_PublicKey key);
int mar_verify_signature_for_id_fp(FILE *fp, 
                                   CryptoX_LibraryHandle provider, 
                                   CryptoX_PublicKey key, 
                                   char *extractedSignature,
                                   size_t extractedSignatureLen,
                                   PRUint32 signatureAlgorithmIDToVerify);
int IsOldMAR(FILE *fp, int *oldMar);


/**
 * Reads the specified number of bytes from the file pointer and
 * stores them in the passed buffer.
 *
 * @param  fp     The file pointer to read from.
 * @param  buffer The buffer to store the read results.
 * @param  size   The number of bytes to read, buffer must be 
 *         at least of this size.
 * @param  ctx    The verify context.
 * @param  err    The name of what is being written to in case of error.
 * @return  0 on success
 *         -1 on read error
 *         -2 on verify update error
*/
int ReadAndUpdateVerifyContext(FILE *fp, void *buffer,
                               PRUint32 size, CryptoX_SignatureHandle *ctx,
                               const char *err) 
{
  if (!size) { 
    return 0;
  }
  if (fread(buffer, size, 1, fp) != 1) {
    fprintf(stderr, "ERROR: Could not read %s\n", err);
    return -1;
  }
  if (CryptoX_Failed(CryptoX_VerifyUpdate(ctx, (char*)buffer, size))) {
    fprintf(stderr, "ERROR: Could not update verify context for %s\n", err);
    return -2;
  }
  return 0;
}


/**
 * Verifies a MAR file's signature.
 * 
 * @param  pathToMARFile The path of the MAR file who's signature 
           should be calculated
 * @param  certData      The certificate data
 * @param sizeOfCertData The size of the data stored in certData
 * @param configDir      The NSS config DIR if using NSS
 * @param certName       The cert name in the NSS store if using NSS
 * @return 0 on success
*/
int mar_verify_signature(const char *pathToMARFile, 
                         const char *certData,
                         size_t sizeOfCertData,
                         const char *configDir,
                         const char *certName) {
  int rv;
  CryptoX_LibraryHandle provider;
  CryptoX_Certificate cert;
  CryptoX_PublicKey key;
  FILE *fp;
  
  fp = fopen(pathToMARFile, "rb");
  if (!fp) {
    fclose(fp);
    fprintf(stderr, "ERROR: Could not open MAR file.\n");
    return -1;
  }

  if (CryptoX_Failed(CryptoX_InitCryptoLibrary(&provider, 
                                               configDir))) {
    fclose(fp);
    fprintf(stderr, "ERROR: Could not init crytpo library.\n");
    return -1;
  }

  if (CryptoX_Failed(CryptoX_LoadPublicKey(provider, certData, sizeOfCertData,
                                           &key, certName, &cert))) {
    fclose(fp);
    fprintf(stderr, "ERROR: Could not load public key.\n");
    return -1;
  }

  rv = mar_verify_signature_fp(fp, provider, key);
  fclose(fp);
  if (key) {
    CryptoX_FreePublicKey(&key);
  }
  if (cert) {
    CryptoX_FreeCertificate(&cert);
  }
  return rv;
}

#ifdef XP_WIN
/**
 * Verifies a MAR file's signature by making sure at least one 
 * signature verifies.
 * 
 * @param  pathToMARFile The path of the MAR file who's signature 
 *         should be calculated
 * @param  certData      The certificate data
 * @param sizeOfCertData The size of the data stored in certData
 * @param configDir      The NSS config DIR if using NSS
 * @param certName       The cert name in the NSS store if using NSS
 * @return 0 on success
*/
int mar_verify_signatureW(const PRUnichar *pathToMARFile, 
                          const char *certData,
                          size_t sizeOfCertData,
                          const char *configDir,
                          const char *certName) {
  int rv;
  CryptoX_LibraryHandle provider;
  CryptoX_Certificate cert;
  CryptoX_PublicKey key;
  FILE *fp;
  
  fp = _wfopen(pathToMARFile, L"rb");
  if (!fp) {
    fclose(fp);
    fprintf(stderr, "ERROR: Could not open MAR file.\n");
    return -1;
  }

  if (CryptoX_Failed(CryptoX_InitCryptoLibrary(&provider, 
                                               configDir))) {
    fclose(fp);
    fprintf(stderr, "ERROR: Could not init crytpo library.\n");
    return -1;
  }

  if (CryptoX_Failed(CryptoX_LoadPublicKey(provider, certData, sizeOfCertData,
                                           &key, certName, &cert))) {
    fclose(fp);
    fprintf(stderr, "ERROR: Could not load public key.\n");
    return -1;
  }

  rv = mar_verify_signature_fp(fp, provider, key);
  fclose(fp);
  if (key) {
    CryptoX_FreePublicKey(&key);
  }
  if (cert) {
    CryptoX_FreeCertificate(&cert);
  }
  return rv;
}
#endif

/**
 * Verifies a MAR file's signature by making sure at least one 
 * signature verifies.
 * 
 * @param  fp       An opened MAR file handle
 * @param  provider A library provider
 * @param  key      The public key to use to verify the MAR
 * @return 0 on success
*/
int mar_verify_signature_fp(FILE *fp,
                            CryptoX_LibraryHandle provider, 
                            CryptoX_PublicKey key) {
  char buf[5] = {0};
  PRUint32 signatureAlgorithmID, signatureCount, signatureLen, numVerified = 0;
  int rv = -1, oldMar = 0;
  long curPos;
  char *extractedSignature;
  size_t i;

  if (!fp) {
    fprintf(stderr, "ERROR: Invalid file pointer passed.\n");
    return -1;
  }

  /* Determine if the source MAR file has the new fields for signing or not */
  if (IsOldMAR(fp, &oldMar)) {
    fprintf(stderr, "ERROR: could not determine if MAR is old or new.\n");
    return -1;
  }

  if (oldMar) {
    fprintf(stderr, "ERROR: The MAR file is in the old format so has"
                    " no signature to verify.\n");
    return -1;
  }

  /* Skip to the start of the signature block */
  if (fseek(fp, SIGNATURE_BLOCK_OFFSET, SEEK_SET)) {
    fprintf(stderr, "ERROR: Could not seek past signature block.\n");
    return -1;
  }

  /* Get the number of signatures */
  if (fread(&signatureCount, sizeof(signatureCount), 1, fp) != 1) {
    fprintf(stderr, "ERROR: Could not read number of signatures.\n");
    return -1;
  }
  signatureCount = ntohl(signatureCount);
  for (i = 0; i < signatureCount; i++) {
    /* Get the signature algorithm ID */
    if (fread(&signatureAlgorithmID, sizeof(PRUint32), 1, fp) != 1) {
      fprintf(stderr, "ERROR: Could not read signatures algorithm ID.\n");
      return -1;
    }
    signatureAlgorithmID = ntohl(signatureAlgorithmID);
  
    if (fread(&signatureLen, sizeof(PRUint32), 1, fp) != 1) {
      fprintf(stderr, "ERROR: Could not read signatures length.\n");
      return -1;
    }
    signatureLen = ntohl(signatureLen);

    extractedSignature = malloc(signatureLen);
    if (!extractedSignature) {
      fprintf(stderr, "ERROR: Could allocate buffer for signature.\n");
      return -1;
    }
    if (fread(extractedSignature, signatureLen, 1, fp) != 1) {
      fprintf(stderr, "ERROR: Could not read extracted signature.\n");
      free(extractedSignature);
      return -1;
    }

    /* We don't try to verify signatures we don't know about */
    if (1 == signatureAlgorithmID) {
      curPos = ftell(fp);
      rv = mar_verify_signature_for_id_fp(fp, provider, key,
                                          extractedSignature,
                                          signatureLen, 
                                          signatureAlgorithmID);
      if (!rv) {
        numVerified++;
      }
      free(extractedSignature);
      if (fseek(fp, curPos, SEEK_SET)) {
        fprintf(stderr, "ERROR: Could not seek back to last signature.\n");
        return -1;
      }
    } else {
      free(extractedSignature);
    }
  }

  // If we reached here and we verified at least one signature, return success.
  if (numVerified > 0) {
    return 0;
  } else {
    fprintf(stderr, "ERROR: No signatures were verified.\n");
    return -1;
  }
}

/**
 * Verifies if a specific signature ID matches the extracted signature.
 * 
 * @param  fp                   An opened MAR file handle
 * @param  provider             A library provider
 * @param  key                  The public key to use to verify the MAR
 * @param extractedSignature    The signature that should be verified
 * @param extractedSignatureLen The signature length
 * @param signatureAlgorithmIDToVerify
 *        The signature algorithm ID to verify. 
 *
 * @return 0 on success
*/
int mar_verify_signature_for_id_fp(FILE *fp, 
                                   CryptoX_LibraryHandle provider, 
                                   CryptoX_PublicKey key, 
                                   char *extractedSignature,
                                   size_t extractedSignatureLen,
                                   PRUint32 signatureAlgorithmIDToVerify) {
  CryptoX_SignatureHandle signatureHandle;
  char buf[BLOCKSIZE];
  PRUint32 signatureCount, signatureLen, signatureAlgorithmID;
  size_t i;

  CryptoX_VerifyBegin(provider, &signatureHandle, &key);

  /* Skip to the start of the file */
  if (fseek(fp, 0, SEEK_SET)) {
    fprintf(stderr, "ERROR: Could not seek to start of the file\n");
    return -1;
  }

  /* Bytes 0-3: MAR1
     Bytes 4-7: index offset 
     Bytes 8-15: size of entire MAR
   */
  if (ReadAndUpdateVerifyContext(fp, buf, SIGNATURE_BLOCK_OFFSET, 
                                 &signatureHandle, "signature block")) {
    return -1;
  }

  /* Bytes 8-11: number of signatures */
  if (ReadAndUpdateVerifyContext(fp, &signatureCount, sizeof(PRUint32), 
                                 &signatureHandle, "signature count")) {
    return -1;
  }
  signatureCount = ntohl(signatureCount);

  for (i = 0; i < signatureCount; i++) {
    /* Get the signature algorithm ID */
    if (ReadAndUpdateVerifyContext(fp, &signatureAlgorithmID, sizeof(PRUint32),
                                   &signatureHandle, 
                                   "signature algorithm ID")) {
        return -1;
    }
    signatureAlgorithmID = ntohl(signatureAlgorithmID);

    if (ReadAndUpdateVerifyContext(fp, &signatureLen, sizeof(PRUint32), 
                                   &signatureHandle, "signature length")) {
      return -1;
    }
    signatureLen = ntohl(signatureLen);

    if (signatureAlgorithmIDToVerify == signatureAlgorithmID && 
        signatureLen != extractedSignatureLen) {
      fprintf(stderr, "ERROR: Signature length is not correct.\n");
      return -1;
    }
    
    /* Skip past the signature itself as those are not included */
    if (fseek(fp, signatureLen, SEEK_CUR)) {
      fprintf(stderr, "ERROR: Could not seek past signature.\n");
      return -1;
    }
  }

  while (!feof(fp)) {
    int numRead = fread(buf, 1, BLOCKSIZE , fp);
    if (ferror(fp)) {
      fprintf(stderr, "ERROR: Error reading data block.\n");
      return -1;
    }
    if (CryptoX_Failed(CryptoX_VerifyUpdate(&signatureHandle, 
                                            buf, numRead))) {
      fprintf(stderr, "ERROR: Error updating verify context with"
                      " data block.\n");
      return -1;
    }
  }

  if (CryptoX_Failed(CryptoX_VerifySignature(&signatureHandle, 
                                             &key,
                                             extractedSignature, 
                                             signatureLen))) {
    fprintf(stderr, "ERROR: Error verifying signature.\n");
    return -1;
  }

  // If we reached here and we verified at least one signature, we have success
  if (signatureCount > 0) {
    return 0;
  } else {
    fprintf(stderr, "ERROR: Signature count is 0.\n");
    return -1;
  }
}
