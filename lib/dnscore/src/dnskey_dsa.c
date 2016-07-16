/*------------------------------------------------------------------------------
 *
 * Copyright (c) 2011-2016, EURid. All rights reserved.
 * The YADIFA TM software product is provided under the BSD 3-clause license:
 * 
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *        * Redistributions of source code must retain the above copyright 
 *          notice, this list of conditions and the following disclaimer.
 *        * Redistributions in binary form must reproduce the above copyright 
 *          notice, this list of conditions and the following disclaimer in the 
 *          documentation and/or other materials provided with the distribution.
 *        * Neither the name of EURid nor the names of its contributors may be 
 *          used to endorse or promote products derived from this software 
 *          without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) 
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 *------------------------------------------------------------------------------
 *
 */
/** @defgroup dnskey DNSSEC keys functions
 *  @ingroup dnsdbdnssec
 *  @brief
 *
 * @{
 */
/*------------------------------------------------------------------------------
 *
 * USE INCLUDES */
#include "dnscore/dnscore-config.h"
#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>

#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/dsa.h>
#include <openssl/ssl.h>
#include <openssl/engine.h>

#include "dnscore/dnscore.h"

#include "dnscore/sys_types.h"
#include "dnscore/base64.h"

#include "dnscore/logger.h"
#include "dnscore/dnskey.h"
#include "dnscore/dnskey_dsa.h"
#include "dnscore/dnssec_errors.h"

#include "dnscore/dnskey.h"

#define MODULE_MSG_HANDLE g_system_logger

/// @note 20151118 edf -- names MUST end with ':'

static const struct structdescriptor struct_DSA[] ={
    {"Prime(p)", offsetof(DSA, p), STRUCTDESCRIPTOR_BN},
    {"Subprime(q)", offsetof(DSA, q), STRUCTDESCRIPTOR_BN},
    {"Base(g)", offsetof(DSA, g), STRUCTDESCRIPTOR_BN},
    {"Private_value(x)", offsetof(DSA, priv_key), STRUCTDESCRIPTOR_BN},
    {"Public_value(y)", offsetof(DSA, pub_key), STRUCTDESCRIPTOR_BN},
    {NULL, 0, 0}
};

static int
dsa_getnid(u8 algorithm)
{
    switch(algorithm)
    {
        case DNSKEY_ALGORITHM_DSASHA1_NSEC3:
        case DNSKEY_ALGORITHM_DSASHA1:
        {
            return NID_sha1;
        }
        default:
        {
            return DNSSEC_ERROR_UNSUPPORTEDKEYALGORITHM;
        }
    }
}

static DSA*
dsa_genkey(u32 size)
{
    yassert(size >= DNSSEC_MINIMUM_KEY_SIZE && size <= DNSSEC_MAXIMUM_KEY_SIZE);

    int err;
    DSA* dsa;

    dsa = DSA_generate_parameters(size, NULL,0, NULL, NULL, NULL, NULL);
    
    yassert(dsa != NULL);
    
    err = DSA_generate_key(dsa); /* no callback */

    if(err == 0)
    {
        // error
        
        DSA_free(dsa);
        dsa = NULL;
    }
    
    return dsa;
}

static ya_result
dsa_signdigest(const dnssec_key *key, const u8 *digest, u32 digest_len, u8 *output)
{
    DSA_SIG *sig = DSA_do_sign(digest, digest_len, key->key.dsa);

    if(sig != NULL)
    {
        u32 t = BN_num_bytes(key->key.dsa->pub_key) >> 3;
        u32 rn = BN_num_bytes(sig->r);        
        
        *output++ = t;
        BN_bn2bin(sig->r, output);
        output += rn;
        BN_bn2bin(sig->s, output);
        
        DSA_SIG_free(sig);
        
        return (rn << 1) + 1;
    }
    else
    {
        unsigned long ssl_err;

        while((ssl_err = ERR_get_error()) != 0)
        {
            char buffer[256];
            ERR_error_string_n(ssl_err, buffer, sizeof(buffer));
            log_err("digest signature returned an ssl error %08x %s", ssl_err, buffer);
        }

        ERR_clear_error();
        
        return DNSSEC_ERROR_DSASIGNATUREFAILED;
    }
}

static bool
dsa_verifydigest(const dnssec_key *key, const u8 *digest, u32 digest_len, const u8 *signature, u32 signature_len)
{
    yassert(signature_len <= DNSSEC_MAXIMUM_KEY_SIZE_BYTES);
    
#ifdef DEBUG
    log_debug6("dsa_verifydigest(K%{dnsname}-%03d-%05d, @%p, @%p)", key->owner_name, key->algorithm, key->tag, digest, signature);
    log_memdump(MODULE_MSG_HANDLE, MSG_DEBUG6, digest, digest_len, 32);
    log_memdump(MODULE_MSG_HANDLE, MSG_DEBUG6, signature, signature_len, 32);
#endif

    DSA_SIG sig;
    
    if(signature_len != 41)
    {
        log_warn("DSA signature expected to be 41 bytes long");
    }
    
    if((signature_len & 1) == 0)
    {
        log_err("DSA signature size expected to be an odd number");
        
        return FALSE;
    }
    
    u8 t = *signature++;
    
    if(t != 8)
    {
        log_warn("DSA T!=8");
    }
    
    signature_len--;        
    signature_len >>= 1;
    
    sig.r = BN_bin2bn(signature, signature_len, NULL);
    signature += signature_len;
    sig.s = BN_bin2bn(signature, signature_len, NULL);

    int err = DSA_do_verify(digest, digest_len, &sig, key->key.dsa);
    
    BN_free(sig.r);
    BN_free(sig.s);
    
    if(err != 1)
    {
        unsigned long ssl_err;

        while((ssl_err = ERR_get_error()) != 0)
        {
            char buffer[256];
            ERR_error_string_n(ssl_err, buffer, sizeof(buffer));
            log_err("digest verification returned an ssl error %08x %s", ssl_err, buffer);
        }

        ERR_clear_error();

        return FALSE;
    }

    return TRUE;
}

static DSA*
dsa_public_load(const u8* rdata, u16 rdata_size)
{
    if(rdata == NULL)
    {
        return NULL;
    }
    
    const u8 *inptr = rdata;
    u32 t;
    t = *inptr;
    
    u32 pgy_len = 64 + (t << 3);
    
    if(rdata_size != 1 + 20 + 3 * pgy_len)
    {
        return NULL;
    }
    
    inptr++;

    BIGNUM* q;
    BIGNUM* p;
    BIGNUM* g;
    BIGNUM* y;

    q = BN_bin2bn(inptr, 20, NULL);
    if(q == NULL)
    {
        log_err("dsa_public_load: NULL q");
        
        return NULL;
    }
    inptr += 20;
    p = BN_bin2bn(inptr, pgy_len, NULL);
    if(p == NULL)
    {
        log_err("dsa_public_load: NULL p");
        BN_free(q);
        
        return NULL;
    }
    inptr += pgy_len;
    g = BN_bin2bn(inptr, pgy_len, NULL);
    if(g == NULL)
    {
        log_err("dsa_public_load: NULL g");
        BN_free(q);
        BN_free(p);
        
        return NULL;
    }
    inptr += pgy_len;
    y = BN_bin2bn(inptr, pgy_len, NULL);
    if(y == NULL)
    {
        log_err("dsa_public_load: NULL y");
        BN_free(q);
        BN_free(p);
        BN_free(g);
        
        return NULL;
    }

    DSA* dsa;
    dsa = DSA_new();

    yassert(dsa != NULL);

    dsa->q = q;
    dsa->p = p;
    dsa->g = g;
    dsa->pub_key = y;

    return dsa;
 }

static u32
dsa_public_store(DSA* dsa, u8* output_buffer)
{
    unsigned char* outptr = output_buffer;

    BIGNUM* q = dsa->q;
    BIGNUM* p = dsa->p;
    BIGNUM* g = dsa->g;
    BIGNUM* y = dsa->pub_key;

    u32 q_n = BN_num_bytes(q);
    
    if(q_n != 20)
    {
        return 0;
    }
    
    u32 p_n = BN_num_bytes(p);
    u32 g_n = BN_num_bytes(g);
    u32 y_n = BN_num_bytes(y);

    if((p_n != g_n) || (p_n != y_n))
    {
        return 0;
    }
    
    s32 t = p_n;
    t -= 64;
    
    if(t < 0)
    {
        return 0;
    }
    
    if((t & 7) != 0)
    {
        return 0;
    }
    
    t >>= 3;
    
    *outptr++ = t;

    BN_bn2bin(q, outptr);
    outptr += q_n;
    
    BN_bn2bin(p, outptr);
    outptr += p_n;
    
    BN_bn2bin(g, outptr);
    outptr += g_n;
    
    BN_bn2bin(y, outptr);
    outptr += y_n;
    
    return outptr - output_buffer;
}

static u32
dsa_dnskey_public_store(const dnssec_key* key, u8 *rdata)
{
    u32 len;
    
    SET_U16_AT(rdata[0], key->flags);
    rdata[2] = DNSKEY_PROTOCOL_FIELD;
    rdata[3] = key->algorithm;
    
    len = dsa_public_store(key->key.dsa, &rdata[4]) + 4;
    
    return len;
}

static u32
dsa_public_getsize(const DSA* dsa)
{
    BIGNUM* q = dsa->q;
    BIGNUM* p = dsa->p;
    BIGNUM* g = dsa->g;
    BIGNUM* y = dsa->pub_key;

    u32 q_n = BN_num_bytes(q);
    u32 p_n = BN_num_bytes(p);
    u32 g_n = BN_num_bytes(g);
    u32 y_n = BN_num_bytes(y);

    return 1 + q_n + p_n + g_n + y_n;
}

static u32
dsa_dnskey_public_getsize(const dnssec_key* key)
{
    return dsa_public_getsize(key->key.dsa) + 4;
}

static void
dsa_free(dnssec_key* key)
{
    DSA* dsa = key->key.dsa;
    DSA_free(dsa);

    key->key.dsa = NULL;
}

static bool
dsa_equals(const dnssec_key* key_a, const dnssec_key* key_b)
{
    /* RSA, compare modulus and exponent, exponent first (it's the smallest) */

    if(key_a == key_b)
    {
        return TRUE;
    }
    
    if(dnssec_key_tag_field_set(key_a) && dnssec_key_tag_field_set(key_b))
    {
       if(key_a->tag != key_b->tag)
       {
           return FALSE;
       }
    }
    
    if((key_a->flags == key_b->flags) && (key_a->algorithm == key_b->algorithm))
    {
        if(strcmp(key_a->origin, key_b->origin) == 0)
        {
            DSA* a_dsa = key_a->key.dsa;
            DSA* b_dsa = key_b->key.dsa;

            if(BN_cmp(a_dsa->q, b_dsa->q) == 0)
            {
                if(BN_cmp(a_dsa->p, b_dsa->p) == 0)
                {
                    if(BN_cmp(a_dsa->g, b_dsa->g) == 0)
                    {
                        if(BN_cmp(a_dsa->pub_key, b_dsa->pub_key) == 0)
                        {
                            if(a_dsa->priv_key != NULL)
                            {
                                if(b_dsa->priv_key != NULL)
                                {
                                    return BN_cmp(a_dsa->priv_key, b_dsa->priv_key) == 0;
                                }
                            }
                            else
                            {
                                return b_dsa->priv_key == NULL;
                            }
                        }
                    }
                }
            }
        }
    }

    return FALSE;
}

const struct structdescriptor *
dsa_get_fields_descriptor(dnssec_key* key)
{
    return struct_DSA;
}

static ya_result
dsa_private_print_fields(dnssec_key *key, output_stream *os)
{
    ya_result ret; // static analyser false positive: the loop will run at least once

#ifdef DEBUG
    ret = ERROR; // just to shut-up the false positive
#endif
    
    const DSA* dsa = key->key.dsa;
    
    for(const struct structdescriptor *sd = struct_DSA; sd->name != NULL; sd++)
    {
        osformat(os, "%s: ", sd->name);
        const BIGNUM **valuep = (const BIGNUM**)&(((const u8*)dsa)[sd->address]);
        
        // WRITE_BIGNUM_AS_BASE64(private, *valuep, tmp_in, tmp_out);
        
        if(FAIL(ret = dnskey_write_bignum_as_base64_to_stream(*valuep, os)))
        {
            break;
        }
        
        osprintln(os, "");
    }
    
    return ret;
}

static const dnssec_key_vtbl dsa_vtbl =
{
    dsa_signdigest,
    dsa_verifydigest,
    dsa_dnskey_public_getsize,
    dsa_dnskey_public_store,
    dsa_free,
    dsa_equals,
    dsa_private_print_fields,
    "DSA"
};

static ya_result
dsa_initinstance(DSA* dsa, u8 algorithm, u16 flags, const char* origin, dnssec_key** out_key)
{
    int nid;
    
    u8 rdata[DNSSEC_MAXIMUM_KEY_SIZE_BYTES]; /* 4096 bits -> 1KB */
    
    *out_key = NULL;
    
    if(FAIL(nid = dsa_getnid(algorithm)))
    {
        return nid;
    }

#ifdef DEBUG
    memset(rdata, 0xff, sizeof(rdata));
#endif

    u32 rdata_size = dsa_public_getsize(dsa);

    if(rdata_size > DNSSEC_MAXIMUM_KEY_SIZE_BYTES)
    {
        return DNSSEC_ERROR_KEYISTOOBIG;
    }

    SET_U16_AT(rdata[0], flags); // NATIVEFLAGS
    rdata[2] = DNSKEY_PROTOCOL_FIELD;
    rdata[3] = algorithm;

    if(dsa_public_store(dsa, &rdata[4]) != rdata_size)
    {
        return DNSSEC_ERROR_UNEXPECTEDKEYSIZE; /* Computed size != real size */
    }

    /* Note : + 4 because of the flags,protocol & algorithm bytes
     *        are not taken in account
     */

    u16 tag = dnskey_get_key_tag_from_rdata(rdata, rdata_size + 4);

    dnssec_key* key = dnskey_newemptyinstance(algorithm, flags, origin); // RC

    key->key.dsa = dsa;
    key->vtbl = &dsa_vtbl;
    key->tag = tag;
    key->nid = nid;
    if(dsa->priv_key != NULL)
    {
        key->status |= DNSKEY_KEY_IS_PRIVATE;
    }
    
    *out_key = key;
    
    return SUCCESS;
}

ya_result
dsa_private_parse_field(dnssec_key *key, parser_s *p)
{
    if(key == NULL)
    {
        return UNEXPECTED_NULL_ARGUMENT_ERROR;
    }

    switch(key->algorithm)
    {
        case DNSKEY_ALGORITHM_DSASHA1_NSEC3:
        case DNSKEY_ALGORITHM_DSASHA1:
            break;
        default:
            return DNSSEC_ERROR_UNSUPPORTEDKEYALGORITHM;
            break;
    }
    
    ya_result ret = ERROR;
    
    if(key->key.dsa == NULL)
    {
        key->key.dsa = DSA_new();
        key->vtbl = &dsa_vtbl;
    }
    
    DSA *dsa = key->key.dsa;
    u32 label_len = parser_text_length(p);
    const char *label = parser_text(p);
    bool parsed_it = FALSE;
    u8 tmp_out[DNSSEC_MAXIMUM_KEY_SIZE_BYTES];
    
    for(const struct structdescriptor *sd = struct_DSA; sd->name != NULL; sd++)
    {
        if(memcmp(label, sd->name, label_len) == 0)
        {
            BIGNUM **valuep = (BIGNUM**)&(((u8*)dsa)[sd->address]);

            ret = parser_next_word(p);
            
            if((*valuep != NULL) || FAIL(ret))
            {
                return ret;
            }

            u32 word_len = parser_text_length(p);
            const char *word = parser_text(p);
            
            ya_result n = base64_decode(word, word_len, tmp_out);

            if(FAIL(n))
            {
                log_err("unable to decode field %s", sd->name);
                return n;
            }

            *valuep = BN_bin2bn(tmp_out, n, NULL);
            
            if(*valuep == NULL)
            {
                log_err("unable to get big number from field %s", sd->name);
                return DNSSEC_ERROR_BNISNULL;
            }
            
            parsed_it = TRUE;
            
            break;
        }
    } /* for each possible field */
    
    if(!parsed_it)
    {
        return SUCCESS; // unknown keyword (currently : ignore)
    }
    
    if((dsa->p != NULL)    &&
       (dsa->q != NULL)    &&
       (dsa->g != NULL)    &&
       /*(dsa->priv_key != NULL)    &&*/
       (dsa->pub_key != NULL))
    {
        yassert(key->nid == 0);
        
        int nid;
        
        if(FAIL(nid = dsa_getnid(key->algorithm)))
        {
            return nid;
        }
        
        u32 rdata_size = dsa_public_getsize(dsa);
        u8 *rdata = tmp_out;
        if(rdata_size > DNSSEC_MAXIMUM_KEY_SIZE_BYTES)
        {
            return DNSSEC_ERROR_KEYISTOOBIG;
        }

        SET_U16_AT(rdata[0], key->flags);
        rdata[2] = DNSKEY_PROTOCOL_FIELD;
        rdata[3] = key->algorithm;

        if(dsa_public_store(dsa, &rdata[4]) != rdata_size)
        {
            return DNSSEC_ERROR_UNEXPECTEDKEYSIZE; /* Computed size != real size */
        }

        /* Note : + 4 because of the flags,protocol & algorithm bytes
         *        are not taken in account
         */

        u16 tag = dnskey_get_key_tag_from_rdata(rdata, rdata_size + 4);

        key->tag = tag;
        key->nid = nid;
        
        key->status |= DNSKEY_KEY_IS_VALID;
    }
    
    if(((key->status & DNSKEY_KEY_IS_VALID) != 0) && (dsa->priv_key != NULL))
    {
        key->status |= DNSKEY_KEY_IS_PRIVATE;
    }
        
    return ret;
}


ya_result
dsa_loadpublic(const u8 *rdata, u16 rdata_size, const char *origin, dnssec_key** out_key)
{
    *out_key = NULL;
            
    if(rdata == NULL || rdata_size <= 6 || origin == NULL)
    {
        /* bad */
        
        return UNEXPECTED_NULL_ARGUMENT_ERROR;
    }

    u16 flags = GET_U16_AT(rdata[0]);
    u8 algorithm = rdata[3];
    
    if((algorithm != DNSKEY_ALGORITHM_DSASHA1_NSEC3) && (algorithm != DNSKEY_ALGORITHM_DSASHA1))
    {
        return DNSSEC_ERROR_UNSUPPORTEDKEYALGORITHM;
    }

    rdata += 4;
    rdata_size -= 4;
    
    ya_result return_value = ERROR;

    DSA *dsa = dsa_public_load(rdata, rdata_size);
    
    if(dsa != NULL)
    {
        dnssec_key *key;
        
        if(ISOK(return_value = dsa_initinstance(dsa, algorithm, flags, origin, &key)))
        {
            *out_key = key;

            return return_value;
        }
        
        DSA_free(dsa);
    }
    
    return return_value;
}

ya_result
dsa_newinstance(u32 size, u8 algorithm, u16 flags, const char* origin, dnssec_key** out_key)
{
    *out_key = NULL;
    
    if(size > DNSSEC_MAXIMUM_KEY_SIZE)
    {
        return DNSSEC_ERROR_KEYISTOOBIG;
    }
    
    if((algorithm != DNSKEY_ALGORITHM_DSASHA1_NSEC3) && (algorithm != DNSKEY_ALGORITHM_DSASHA1))
    {
        return DNSSEC_ERROR_UNSUPPORTEDKEYALGORITHM;
    }
    
    ya_result return_value = ERROR;

    DSA *dsa = dsa_genkey(size);
    
    if(dsa != NULL)
    {
        dnssec_key *key;
        
        if(ISOK(return_value = dsa_initinstance(dsa, algorithm, flags, origin, &key)))
        {
            *out_key = key;
            
            return return_value;
        }
        
        DSA_free(dsa);
    }

    return return_value;
}

/*    ------------------------------------------------------------    */

/** @} */

