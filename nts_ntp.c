/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2019
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 * 
 **********************************************************************

  =======================================================================

  Implementation of NTS for NTP
  */

#include "config.h"

#include "sysincl.h"

#include "logging.h"
#include "memory.h"
#include "ntp_ext.h"
#include "nts_ke.h"
#include "nts_ntp.h"
#include "util.h"

#include "siv_cmac.h"

#define MAX_COOKIES 8
#define NONCE_LENGTH 16
#define UNIQ_ID_LENGTH 32

#define MAX_SERVER_KEYS 3

struct NTS_ClientInstance_Record {
  IPAddr address;
  int port;
  char *name;
  NKE_Instance nke;
  NKE_Cookie cookies[MAX_COOKIES];
  int num_cookies;
  int cookie_index;
  struct siv_aes128_cmac_ctx siv_c2s;
  struct siv_aes128_cmac_ctx siv_s2c;
  unsigned char nonce[NONCE_LENGTH];
  unsigned char uniq_id[UNIQ_ID_LENGTH];
};

struct ServerKey {
  char key[32];
  uint32_t id;
};

struct NTS_ServerInstance_Record {
  NKE_Instance *nke;
  struct ServerKey keys[MAX_SERVER_KEYS];
  int num_keys;
  int key_index;
};

typedef struct NTS_ServerInstance_Record *NTS_ServerInstance;

struct AuthAndEEF {
  void *nonce;
  void *ciphertext;
  int nonce_length;
  int ciphertext_length;
};

static int
get_padded_length(int length)
{
  if (length % 4U)
    length += 4 - length % 4U;
  return length;
}

static int
parse_auth_and_eef(unsigned char *ef_body, int ef_body_length,
                   struct AuthAndEEF *auth)
{
  if (ef_body_length < 4)
    return 0;

  auth->nonce_length = ntohs(*((uint16_t *)ef_body + 0));
  auth->ciphertext_length = ntohs(*((uint16_t *)ef_body + 1));

  if (get_padded_length(auth->nonce_length) +
      get_padded_length(auth->ciphertext_length) > ef_body_length)
    return 0;

  auth->nonce = ef_body + 4;
  auth->ciphertext = ef_body + 4 + get_padded_length(auth->nonce_length);

  return 1;
}

void
NTS_Initialise(void)
{
}

void
NTS_Finalise(void)
{
}

int
NTS_CheckRequestAuth(NTP_Packet *packet, NTP_PacketInfo *info)
{
  int ef_type, ef_body_length, ef_parsed, parsed, cookie_length;
  void *ef_body, *cookie;
  struct AuthAndEEF auth_and_eef;

  if (info->ext_fields == 0 || info->mode != MODE_CLIENT)
    return 0;

  parsed = 0;
  cookie = NULL;

  while (1) {
    ef_parsed = NEF_ParseField(packet, info->length, parsed,
                               &ef_type, &ef_body, &ef_body_length);
    if (ef_parsed < parsed)
      break;
    parsed = ef_parsed;

    switch (ef_type) {
      case NTP_EF_NTS_COOKIE:
        if (cookie)
          /* Exactly one cookie is expected */
          return 0;
        cookie = ef_body;
        cookie_length = ef_body_length;
        break;
      case NTP_EF_NTS_COOKIE_PLACEHOLDER:
        break;
      case NTP_EF_NTS_AUTH_AND_EEF:
        if (!parse_auth_and_eef(ef_body, ef_body_length, &auth_and_eef))
          return 0;
        break;
      default:
        break;
    }
  }

  if (cookie && cookie_length)
    ;

  return 1;
}

static int
add_response_cookie(NTP_Packet *packet, NTP_PacketInfo *info)
{
  char cookie[100];

  memset(cookie, 0, sizeof (cookie));

  return NEF_AddField(packet, info, NTP_EF_NTS_COOKIE, &cookie, sizeof (cookie));
}

int
NTS_GenerateResponseAuth(NTP_Packet *request, NTP_PacketInfo *req_info,
                         NTP_Packet *response, NTP_PacketInfo *res_info)
{
  int ef_type, ef_body_length, ef_parsed, parsed;
  void *ef_body;

  if (req_info->mode != MODE_CLIENT || res_info->mode != MODE_SERVER)
    return 0;

  parsed = 0;

  while (1) {
    ef_parsed = NEF_ParseField(request, req_info->length, parsed,
                               &ef_type, &ef_body, &ef_body_length);
    if (ef_parsed < parsed)
      break;
    parsed = ef_parsed;

    switch (ef_type) {
      case NTP_EF_NTS_UNIQUE_IDENTIFIER:
        /* Copy the ID from the request */
        if (!NEF_AddField(response, res_info, ef_type, ef_body, ef_body_length))
          return 0;
      case NTP_EF_NTS_COOKIE:
      case NTP_EF_NTS_COOKIE_PLACEHOLDER:
        if (!add_response_cookie(response, res_info))
          return 0;
      default:
        break;
    }
  }

  return 1;
}

NTS_ClientInstance
NTS_CreateClientInstance(IPAddr *address, int port, const char *name)
{
  NTS_ClientInstance inst;

  inst = MallocNew(struct NTS_ClientInstance_Record);

  memset(inst, 0, sizeof (*inst));
  inst->address = *address;
  inst->port = port;
  inst->name = strdup(name);
  inst->num_cookies = 0;
  memset(inst->uniq_id, 0, sizeof (inst->uniq_id));

  inst->nke = NULL;

  return inst;
}

void
NTS_DestroyClientInstance(NTS_ClientInstance inst)
{
  if (inst->nke)
    NKE_DestroyInstance(inst->nke);

  Free(inst->name);
  Free(inst);
}

static int
needs_nke(NTS_ClientInstance inst)
{
  return inst->num_cookies == 0;
}

static void
get_nke_data(NTS_ClientInstance inst)
{
  NKE_Key c2s, s2c;

  assert(needs_nke(inst));

  if (!inst->nke) {
    inst->nke = NKE_CreateInstance();

    if (!NKE_OpenClientConnection(inst->nke, &inst->address, inst->port, inst->name)) {
      NKE_DestroyInstance(inst->nke);
      inst->nke = NULL;
      return;
    }
  }

  inst->cookie_index = 0;
  inst->num_cookies = NKE_GetCookies(inst->nke, inst->cookies, MAX_COOKIES);

  if (inst->num_cookies == 0)
    return;

  if (!NKE_GetKeys(inst->nke, &c2s, &s2c)) {
    inst->num_cookies = 0;
    return;
  }

  assert(c2s.length == 2 * AES128_KEY_SIZE);
  assert(s2c.length == 2 * AES128_KEY_SIZE);

  DEBUG_LOG("c2s key: %x s2c key: %x", *(unsigned int *)c2s.key, *(unsigned int *)s2c.key);
  siv_aes128_cmac_set_key(&inst->siv_c2s, (uint8_t *)c2s.key);
  siv_aes128_cmac_set_key(&inst->siv_s2c, (uint8_t *)s2c.key);

  NKE_DestroyInstance(inst->nke);
  inst->nke = NULL;
}

int
NTS_PrepareForAuth(NTS_ClientInstance inst)
{
  if (!needs_nke(inst))
    return 1;

  get_nke_data(inst);

  if (needs_nke(inst))
    return 0;

  UTI_GetRandomBytes(&inst->uniq_id, sizeof (inst->uniq_id)); 
  UTI_GetRandomBytes(&inst->nonce, sizeof (inst->nonce)); 

  return 1;
}

int
NTS_GenerateRequestAuth(NTS_ClientInstance inst, NTP_Packet *packet,
                        NTP_PacketInfo *info)
{
  NKE_Cookie *cookie;
  int i;
  struct {
    uint16_t nonce_length;
    uint16_t ciphertext_length;
    uint8_t nonce[NONCE_LENGTH];
    uint8_t ciphertext[SIV_DIGEST_SIZE];
  } auth;
  
  if (needs_nke(inst))
    return 0;

  cookie = &inst->cookies[inst->cookie_index];

  if (!NEF_AddField(packet, info, NTP_EF_NTS_UNIQUE_IDENTIFIER,
                    &inst->uniq_id, sizeof (inst->uniq_id)))
    return 0;

  if (!NEF_AddField(packet, info, NTP_EF_NTS_COOKIE,
                    cookie->cookie, cookie->length))
    return 0;

  for (i = 0; i < MAX_COOKIES - inst->num_cookies; i++) {
    if (!NEF_AddField(packet, info, NTP_EF_NTS_COOKIE_PLACEHOLDER,
                      cookie->cookie, cookie->length))
      return 0;
  }

  auth.nonce_length = htons(NONCE_LENGTH);
  auth.ciphertext_length = htons(sizeof (auth.ciphertext));
  memcpy(auth.nonce, inst->nonce, sizeof (auth.nonce));
  siv_aes128_cmac_encrypt_message(&inst->siv_c2s, sizeof (inst->nonce), inst->nonce,
                                  info->length, (uint8_t *)packet,
                                  SIV_DIGEST_SIZE, 0, auth.ciphertext, (uint8_t *)"");

#if 0
  unsigned char x[100];
  printf("decrypt: %d\n",
         siv_aes128_cmac_decrypt_message(&inst->siv_c2s, sizeof (inst->nonce), inst->nonce,
                                  info->length, (uint8_t *)packet,
                                  SIV_DIGEST_SIZE, sizeof (auth.ciphertext),
                                  x, auth.ciphertext));
#endif
  if (!NEF_AddField(packet, info, NTP_EF_NTS_AUTH_AND_EEF,
                    &auth, sizeof (auth)))
    return 0;

  inst->num_cookies--;
  inst->cookie_index = (inst->cookie_index + 1) % MAX_COOKIES;

  return 1;
}

int
NTS_CheckResponseAuth(NTS_ClientInstance inst, NTP_Packet *packet,
                      NTP_PacketInfo *info)
{
  if (info->ext_fields == 0 || info->mode != MODE_SERVER)
    return 0;

  return 0;
}
