/* Copyright 2004 Roger Dingledine */
/* See LICENSE for licensing information */
/* $Id$ */

/**
 * \file rendcommon.c
 * \brief Rendezvous implementation: shared code between
 * introducers, services, clients, and rendezvous points.
 **/

#include "or.h"

/** Free the storage held by the service descriptor 'desc'.
 */
void rend_service_descriptor_free(rend_service_descriptor_t *desc)
{
  int i;
  if (desc->pk)
    crypto_free_pk_env(desc->pk);
  if (desc->intro_points) {
    for (i=0; i < desc->n_intro_points; ++i) {
      tor_free(desc->intro_points[i]);
    }
    tor_free(desc->intro_points);
  }
  tor_free(desc);
}

/** Encode a service descriptor for 'desc', and sign it with 'key'. Store
 * the descriptor in *str_out, and set *len_out to its length.
 */
int
rend_encode_service_descriptor(rend_service_descriptor_t *desc,
                               crypto_pk_env_t *key,
                               char **str_out, int *len_out)
{
  char *buf, *cp, *ipoint;
  int i, keylen, asn1len;
  keylen = crypto_pk_keysize(desc->pk);
  buf = tor_malloc(keylen*2); /* XXXX */
  asn1len = crypto_pk_asn1_encode(desc->pk, buf, keylen*2);
  if (asn1len<0) {
    tor_free(buf);
    return -1;
  }
  *len_out = 2 + asn1len + 4 + 2 + keylen;
  for (i = 0; i < desc->n_intro_points; ++i) {
    *len_out += strlen(desc->intro_points[i]) + 1;
  }
  cp = *str_out = tor_malloc(*len_out);
  set_uint16(cp, htons((uint16_t)asn1len));
  cp += 2;
  memcpy(cp, buf, asn1len);
  tor_free(buf);
  cp += asn1len;
  set_uint32(cp, htonl((uint32_t)desc->timestamp));
  cp += 4;
  set_uint16(cp, htons((uint16_t)desc->n_intro_points));
  cp += 2;
  for (i=0; i < desc->n_intro_points; ++i) {
    ipoint = (char*)desc->intro_points[i];
    strcpy(cp, ipoint);
    cp += strlen(ipoint)+1;
  }
  i = crypto_pk_private_sign_digest(key, *str_out, cp-*str_out, cp);
  if (i<0) {
    tor_free(*str_out);
    return -1;
  }
  cp += i;
  tor_assert(*len_out == (cp-*str_out));
  return 0;
}

/** Parse a service descriptor at 'str' (len bytes).  On success,
 * return a newly alloced service_descriptor_t.  On failure, return
 * NULL.
 */
rend_service_descriptor_t *rend_parse_service_descriptor(
                           const char *str, int len)
{
  rend_service_descriptor_t *result = NULL;
  int keylen, asn1len, i;
  const char *end, *cp, *eos;

  result = tor_malloc_zero(sizeof(rend_service_descriptor_t));
  cp = str;
  end = str+len;
  if (end-cp < 2) goto truncated;
  asn1len = ntohs(get_uint16(cp));
  cp += 2;
  if (end-cp < asn1len) goto truncated;
  result->pk = crypto_pk_asn1_decode(cp, asn1len);
  if (!result->pk) goto truncated;
  cp += asn1len;
  if (end-cp < 4) goto truncated;
  result->timestamp = (time_t) ntohl(get_uint32(cp));
  cp += 4;
  if (end-cp < 2) goto truncated;
  result->n_intro_points = ntohs(get_uint16(cp));
  result->intro_points = tor_malloc_zero(sizeof(char*)*result->n_intro_points);
  cp += 2;
  for (i=0;i<result->n_intro_points;++i) {
    if (end-cp < 2) goto truncated;
    eos = (const char *)memchr(cp,'\0',end-cp);
    if (!eos) goto truncated;
    result->intro_points[i] = tor_strdup(cp);
    cp = eos+1;
  }
  keylen = crypto_pk_keysize(result->pk);
  if (end-cp < keylen) goto truncated;
  if (end-cp > keylen) {
    log_fn(LOG_WARN, "Signature too long on service descriptor");
    goto error;
  }
  if (crypto_pk_public_checksig_digest(result->pk,
                                       (char*)str,cp-str, /* data */
                                       (char*)cp,end-cp  /* signature*/
                                       )<0) {
    log_fn(LOG_WARN, "Bad signature on service descriptor");
    goto error;
  }

  return result;
 truncated:
  log_fn(LOG_WARN, "Truncated service descriptor");
 error:
  rend_service_descriptor_free(result);
  return NULL;
}

/** Sets out to the first 10 bytes of the digest of 'pk', base32
 * encoded.  NUL-terminates out.  (We use this string to identify
 * services in directory requests and .onion URLs.)
 */
int rend_get_service_id(crypto_pk_env_t *pk, char *out)
{
  char buf[DIGEST_LEN];
  tor_assert(pk);
  if (crypto_pk_get_digest(pk, buf) < 0)
    return -1;
  if (base32_encode(out, REND_SERVICE_ID_LEN+1, buf, 10) < 0)
    return -1;
  return 0;
}

/* ==== Rendezvous service descriptor cache. */

#define REND_CACHE_MAX_AGE (24*60*60)
#define REND_CACHE_MAX_SKEW (90*60)

/** Map from service id (as generated by rend_get_service_id) to
 * rend_cache_entry_t. */
static strmap_t *rend_cache = NULL;

/** Initializes the service descriptor cache.
 */
void rend_cache_init(void)
{
  rend_cache = strmap_new();
}

/** Removes all old entries from the service descriptor cache.
 */
void rend_cache_clean(void)
{
  strmap_iter_t *iter;
  const char *key;
  void *val;
  rend_cache_entry_t *ent;
  time_t cutoff;
  cutoff = time(NULL) - REND_CACHE_MAX_AGE;
  for (iter = strmap_iter_init(rend_cache); !strmap_iter_done(iter); ) {
    strmap_iter_get(iter, &key, &val);
    ent = (rend_cache_entry_t*)val;
    if (ent->parsed->timestamp < cutoff) {
      iter = strmap_iter_next_rmv(rend_cache, iter);
      rend_service_descriptor_free(ent->parsed);
      tor_free(ent->desc);
      tor_free(ent);
    } else {
      iter = strmap_iter_next(rend_cache, iter);
    }
  }
}

/** Return true iff 'query' is a syntactically valid service ID (as
 * generated by rend_get_service_id).  */
int rend_valid_service_id(const char *query) {
  if(strlen(query) != REND_SERVICE_ID_LEN)
    return 0;

  if (strspn(query, BASE32_CHARS) != REND_SERVICE_ID_LEN)
    return 0;

  return 1;
}

/** If we have a cached rend_cache_entry_t for the service ID 'query', set
 * *e to that entry and return 1.  Else return 0.
 */
int rend_cache_lookup_entry(const char *query, rend_cache_entry_t **e)
{
  tor_assert(rend_cache);
  if (!rend_valid_service_id(query))
    return -1;
  *e = strmap_get_lc(rend_cache, query);
  if (!*e)
    return 0;
  return 1;
}

/** 'query' is a base-32'ed service id. If it's malformed, return -1.
 * Else look it up.
 *   If it is found, point *desc to it, and write its length into
 *   *desc_len, and return 1.
 *   If it is not found, return 0.
 * Note: calls to rend_cache_clean or rend_cache_store may invalidate
 * *desc.
 */
int rend_cache_lookup_desc(const char *query, const char **desc, int *desc_len)
{
  rend_cache_entry_t *e;
  int r;
  r = rend_cache_lookup_entry(query,&e);
  if (r <= 0) return r;
  *desc = e->desc;
  *desc_len = e->len;
  return 1;
}

/** Parse *desc, calculate its service id, and store it in the cache.
 * If we have a newer descriptor with the same ID, ignore this one.
 * If we have an older descriptor with the same ID, replace it.
 * Returns -1 if it's malformed or otherwise rejected, else return 0.
 */
int rend_cache_store(const char *desc, int desc_len)
{
  rend_cache_entry_t *e;
  rend_service_descriptor_t *parsed;
  char query[REND_SERVICE_ID_LEN+1];
  time_t now;
  tor_assert(rend_cache);
  parsed = rend_parse_service_descriptor(desc,desc_len);
  if (!parsed) {
    log_fn(LOG_WARN,"Couldn't parse service descriptor");
    return -1;
  }
  if (rend_get_service_id(parsed->pk, query)<0) {
    log_fn(LOG_WARN,"Couldn't compute service ID");
    rend_service_descriptor_free(parsed);
    return -1;
  }
  now = time(NULL);
  if (parsed->timestamp < now-REND_CACHE_MAX_AGE) {
    log_fn(LOG_WARN,"Service descriptor %s is too old", query);
    rend_service_descriptor_free(parsed);
    return -1;
  }
  if (parsed->timestamp > now+REND_CACHE_MAX_SKEW) {
    log_fn(LOG_WARN,"Service descriptor %s is too far in the future", query);
    rend_service_descriptor_free(parsed);
    return -1;
  }
  e = (rend_cache_entry_t*) strmap_get_lc(rend_cache, query);
  if (e && e->parsed->timestamp > parsed->timestamp) {
    log_fn(LOG_INFO,"We already have a newer service descriptor %s with the same ID", query);
    rend_service_descriptor_free(parsed);
    return 0;
  }
  if (e && e->len == desc_len && !memcmp(desc,e->desc,desc_len)) {
    log_fn(LOG_INFO,"We already have this service descriptor %s", query);
    e->received = time(NULL);
    rend_service_descriptor_free(parsed);
    return 0;
  }
  if (!e) {
    e = tor_malloc_zero(sizeof(rend_cache_entry_t));
    strmap_set_lc(rend_cache, query, e);
  } else {
    rend_service_descriptor_free(e->parsed);
    tor_free(e->desc);
  }
  e->received = time(NULL);
  e->parsed = parsed;
  e->len = desc_len;
  e->desc = tor_malloc(desc_len);
  memcpy(e->desc, desc, desc_len);

  log_fn(LOG_INFO,"Successfully stored rend desc '%s', len %d", query, desc_len);
  return 0;
}

/** Called when we get a rendezvous-related relay cell on circuit
 * *circ.  Dispatch on rendezvous relay command. */
void rend_process_relay_cell(circuit_t *circ, int command, int length,
                             const char *payload)
{
  int r;
  switch(command) {
    case RELAY_COMMAND_ESTABLISH_INTRO:
      r = rend_mid_establish_intro(circ,payload,length);
      break;
    case RELAY_COMMAND_ESTABLISH_RENDEZVOUS:
      r = rend_mid_establish_rendezvous(circ,payload,length);
      break;
    case RELAY_COMMAND_INTRODUCE1:
      r = rend_mid_introduce(circ,payload,length);
      break;
    case RELAY_COMMAND_INTRODUCE2:
      r = rend_service_introduce(circ,payload,length);
      break;
    case RELAY_COMMAND_INTRODUCE_ACK:
      r = rend_client_introduction_acked(circ,payload,length);
      break;
    case RELAY_COMMAND_RENDEZVOUS1:
      r = rend_mid_rendezvous(circ,payload,length);
      break;
    case RELAY_COMMAND_RENDEZVOUS2:
      r = rend_client_receive_rendezvous(circ,payload,length);
      break;
    case RELAY_COMMAND_INTRO_ESTABLISHED:
      r = rend_service_intro_established(circ,payload,length);
      break;
    case RELAY_COMMAND_RENDEZVOUS_ESTABLISHED:
      r = rend_client_rendezvous_acked(circ,payload,length);
      break;
    default:
      tor_assert(0);
  }
}

/*
  Local Variables:
  mode:c
  indent-tabs-mode:nil
  c-basic-offset:2
  End:
*/
