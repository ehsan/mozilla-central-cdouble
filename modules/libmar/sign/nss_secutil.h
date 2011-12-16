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
 * The Original Code is code copied from secutil and secpwd.
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

#ifndef NSS_SECUTIL_H_
#define NSS_SECUTIL_H_

#include "nss.h"
#include "cryptohi.h"
#include "hasht.h"

struct VFYContextStr {
    SECOidTag hashAlg;  /* the hash algorithm */
    SECKEYPublicKey *key;
    /*
     * This buffer holds either the digest or the full signature
     * depending on the type of the signature (key->keyType).  It is
     * defined as a union to make sure it always has enough space.
     *
     * Use the "buffer" union member to reference the buffer.
     * Note: do not take the size of the "buffer" union member.  Take
     * the size of the union or some other union member instead.
     */
    union {
  unsigned char buffer[1];

  /* the digest in the decrypted RSA signature */
  unsigned char rsadigest[HASH_LENGTH_MAX];
  /* the full DSA signature... 40 bytes */
  unsigned char dsasig[DSA_SIGNATURE_LEN];
  /* the full ECDSA signature */
  unsigned char ecdsasig[2 * MAX_ECKEY_LEN];
    } u;
    unsigned int rsadigestlen;
    void * wincx;
    void *hashcx;
    const SECHashObject *hashobj;
    SECOidTag encAlg;  /* enc alg */
    PRBool hasSignature;  /* true if the signature was provided in the
                           * VFY_CreateContext call.  If false, the
                           * signature must be provided with a
                           * VFY_EndWithSignature call. */
};

typedef struct {
  enum {
    PW_NONE = 0,
    PW_FROMFILE = 1,
    PW_PLAINTEXT = 2,
    PW_EXTERNAL = 3
  } source;
  char *data;
} secuPWData;

#if( defined(_WINDOWS) && !defined(_WIN32_WCE))
#include <conio.h>
#include <io.h>
#define QUIET_FGETS quiet_fgets
static char * quiet_fgets (char *buf, int length, FILE *input);
#else
#define QUIET_FGETS fgets
#endif

/* Get a password from the input terminal, without echoing */

#if defined(_WINDOWS)
static char * quiet_fgets (char *buf, int length, FILE *input);
#endif

char *SEC_GetPassword(FILE *input, FILE *output, char *prompt,
                      PRBool (*ok)(char *));
PRBool SEC_BlindCheckPassword(char *cp);
char *
SECU_GetPasswordString(void *arg, char *prompt);
char *
SECU_FilePasswd(PK11SlotInfo *slot, PRBool retry, void *arg);
char *
SECU_GetModulePassword(PK11SlotInfo *slot, PRBool retry, void *arg);

#endif
