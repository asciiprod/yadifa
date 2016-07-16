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
/** @defgroup rrsig RRSIG functions
 *  @ingroup dnsdbdnssec
 *  @brief 
 *
 *  
 *
 * @{
 *
 * lock for readers
 * 
 * label iterator -> Q -> sign -> Q
 *                   U -> sign -> U
 *                   E -> sign -> E
 *                   U -> sign -> U
 *                   E -> sign -> E -> get results -> set lock to writer -> store
 * 
 *----------------------------------------------------------------------------*/

#pragma once

struct rrsig_updater_result_process_item_s;

#define RRSUPRMS_TAG 0x534d525055535252

#define RRSIG_UPDATER_PREPARE_KEYS_ZSK 1
#define RRSIG_UPDATER_PREPARE_KEYS_KSK 2

struct rrsig_updater_parms
{
    dnssec_task_s task;
    smp_int remaining_quota;
        
    struct rrsig_updater_result_process_item_s *to_commit;
    u32_set ksk_tag_set;
    u32_set zsk_tag_set;

    s32 quota;  /// maximum number of signatures allowed
    
    // output
    
    u32 good_signatures;
    u32 expired_signatures;
    u32 wrong_signatures;
    
    bool signatures_are_verified;
};

typedef struct rrsig_updater_parms rrsig_updater_parms;

rrsig_updater_parms *rrsig_updater_parms_alloc();
void rrsig_updater_parms_free(rrsig_updater_parms *parms);

void rrsig_updater_init(rrsig_updater_parms *parms, zdb_zone *zone);

/**
 * Prepares the DNSKEYs and the parameters.
 * Loads the (missing) private keys.
 * Marks the tags of the keys in the parms
 * Returns a mask of ZSK(1) or KSK(2) found, or an error code
 * 
 * @param parms
 * @param zone
 * @return 
 */

ya_result rrsig_updater_prepare_keys(rrsig_updater_parms *parms, zdb_zone *zone);

ya_result rrsig_updater_process_zone(rrsig_updater_parms *parms);

void rrsig_updater_commit(rrsig_updater_parms *parms);

void rrsig_updater_finalize(rrsig_updater_parms *parms);

/**
 * Clears all the usage marks of the tags.
 * Returns the number of missing marks.
 * 
 * @param tag set
 * @return the number of missing marks
 */

u32 rrsig_updater_clear_tags(u32_set *set);

/**
 * Adds a tag in the set
 * 
 * @param set
 * @param tag
 */

void rrsig_updater_add_tag(u32_set *set, u32 tag);

/**
 * Marks a tag in the set.
 * Returns 1 if the tag was not found, 0 otherwise
 *  * 
 * @param set
 * @param tag
 * 
 * @return 1 if the tag was not found, 0 otherwise
 */

int rrsig_updater_mark_tag(u32_set *set, u32 tag);

/** @} */
