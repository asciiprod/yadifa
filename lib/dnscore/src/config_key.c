/*------------------------------------------------------------------------------
*
* Copyright (c) 2011-2017, EURid. All rights reserved.
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
#include "dnscore/dnscore-config.h"
#include "dnscore/tsig.h"
#include "dnscore/base64.h"
#include "dnscore/config_settings.h"

#define CFGSKEY_TAG 0x59454b53474643

struct config_section_key_s
{
    char name[256];
    char algorithm[32];
    char secret[512];
};

typedef struct config_section_key_s config_section_key_s;

#define HMAC_UNKNOWN	  0
#define HMAC_MD5        157
#define HMAC_SHA1       161
#define HMAC_SHA224     162
#define HMAC_SHA256     163
#define HMAC_SHA384     164
#define HMAC_SHA512     165

static value_name_table hmac_digest_enum[]=
{
    {HMAC_MD5   , "hmac-md5"    },
    {HMAC_SHA1  , "hmac-sha1"   },
    {HMAC_SHA224, "hmac-sha224" },
    {HMAC_SHA256, "hmac-sha256" },
    {HMAC_SHA384, "hmac-sha384" },
    {HMAC_SHA512, "hmac-sha512" },
    {0, NULL}
};

#define CONFIG_TYPE config_section_key_s
CONFIG_BEGIN(config_section_key_desc)
CONFIG_STRING_COPY(name, NULL)
CONFIG_STRING_COPY(algorithm, NULL)
CONFIG_STRING_COPY(secret, NULL)
CONFIG_END(config_section_key_desc)
#undef CONFIG_TYPE

static ya_result
config_section_key_init(struct config_section_descriptor_s *csd)
{
    // NOP
    
    if(csd->base != NULL)
    {
        return INVALID_STATE_ERROR; // base SHOULD be NULL at init
    }
    
    return SUCCESS;
}

static ya_result
config_section_key_start(struct config_section_descriptor_s *csd)
{
    // NOP
    //config_section_key_s *csk = (config_section_key_s*)csd->base;
    
    config_section_key_s *csk;
    MALLOC_OR_DIE(config_section_key_s*, csk, sizeof(config_section_key_s), CFGSKEY_TAG);
    csd->base = csk;
    
    csk->name[0] = '\0';
    csk->algorithm[0] = '\0';
    csk->secret[0] = '\0';
    
#if CONFIG_SETTINGS_DEBUG
    formatln("config: section: key: start");
#endif
    
    return SUCCESS;
}

static void
config_section_key_delete(struct config_section_descriptor_s *csd)
{
    if(csd != NULL)
    {
        if(csd->base != NULL)
        {
            free(csd->base);
        }
        csd->base = NULL;
    }
}

static ya_result
config_section_key_stop(struct config_section_descriptor_s *csd)
{
#if CONFIG_SETTINGS_DEBUG
    formatln("config: section: key: stop");
#endif
    
    // NOP
    config_section_key_s *csk = (config_section_key_s*)csd->base;
    
    if( csk->name[0] == '\0'            ||
        csk->algorithm[0] == '\0'       ||
        csk->secret[0] == '\0' )
    {
        if(csk->name[0] == '\0'         &&
           csk->algorithm[0] == '\0'    &&
           csk->secret[0] == '\0' )
        {
            config_section_key_delete(csd);
            
            return SUCCESS; // empty key, ignored
        }
        else
        {
            config_section_key_delete(csd);
            return CONFIG_KEY_INCOMPLETE_KEY;
        }
    }
    
    // check if algorithm is supported
    
    u32 hmac_digest;
    
    anytype table;
    table._voidp = hmac_digest_enum;
    if(FAIL(config_set_enum_value(csk->algorithm, &hmac_digest, table))) // dest must be 32 bits
    {
        config_section_key_delete(csd);
        
        return CONFIG_KEY_UNSUPPORTED_ALGORITHM;
    }
    
    // decode the secret
    
    ya_result return_code;
    u32 secret_len;
    u32 len = strlen(csk->secret);
    
    u8 fqdn[MAX_DOMAIN_LENGTH];
    u8 secret_buffer[512];
    
    if(ISOK(return_code = base64_decode(csk->secret, len, secret_buffer)))
    {
        secret_len = return_code;

        if(ISOK(return_code = cstr_to_dnsname_with_check(fqdn, csk->name)))
        {
            return_code = tsig_register(fqdn, secret_buffer, secret_len, hmac_digest);
        }
    }
    
    config_section_key_delete(csd);
    
#if CONFIG_SETTINGS_DEBUG
    formatln("tsig_register(%s,%s,%s) = %r", csk->name, csk->algorithm, csk->secret, return_code);
#endif
    
    return return_code;
}

static ya_result
config_section_key_postprocess(struct config_section_descriptor_s *csd)
{
    (void)csd;
    return SUCCESS;
}

static ya_result
config_section_key_finalise(struct config_section_descriptor_s *csd)
{
    if(csd != NULL)
    {
        config_section_key_delete(csd);
        free(csd);
    }
    return SUCCESS;
}

static ya_result
config_section_key_set_wild(struct config_section_descriptor_s *csd, const char *key, const char *value)
{
    return CONFIG_UNKNOWN_SETTING;
}

static ya_result
config_section_key_print_wild(struct config_section_descriptor_s *csd, output_stream *os, const char *key)
{
    return CONFIG_UNKNOWN_SETTING;
}

static const config_section_descriptor_vtbl_s config_section_key_descriptor_vtbl =
{
    "key",
    config_section_key_desc,                               // no table
    config_section_key_set_wild,
    config_section_key_print_wild,
    config_section_key_init,
    config_section_key_start,
    config_section_key_stop,
    config_section_key_postprocess,
    config_section_key_finalise
};

ya_result
config_register_key(const char *null_or_key_name, s32 priority)
{
    //null_or_key_name = "key";
    (void)null_or_key_name;
    
    config_section_descriptor_s *desc;
    MALLOC_OR_DIE(config_section_descriptor_s*, desc, sizeof(config_section_descriptor_s), CFGSDESC_TAG);
    desc->base = NULL;
    desc->vtbl = &config_section_key_descriptor_vtbl;
    
    ya_result return_code = config_register(desc, priority);
    
    if(FAIL(return_code))
    {
        free(desc);
    }
    
    return return_code; // no, there is no leak of desc, it is either put in the collection, either freed 
}
