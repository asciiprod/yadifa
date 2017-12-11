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
/** @defgroup dnsdbzone Zone related functions
 *  @ingroup dnsdb
 *  @brief Functions used to manipulate a zone
 *
 *  Functions used to manipulate a zone
 *
 * @{
 */

#define ZDB_JOURNAL_CODE 1

#include "dnsdb/dnsdb-config.h"
#include <unistd.h>
#include <arpa/inet.h>

#ifdef DEBUG
#include <dnscore/format.h>
#endif

#include <dnscore/dnscore.h>
#include <dnscore/logger.h>
#include <dnscore/threaded_dll_cw.h>

#include "dnsdb/dnsdb-config.h"
#include "dnsdb/dnssec-keystore.h"
#include "dnsdb/zdb_icmtl.h"
#include "dnsdb/zdb.h"

#include "dnsdb/zdb_zone.h"
#include "dnsdb/zdb_zone_label.h"
#include "dnsdb/zdb_rr_label.h"
#include "dnsdb/zdb_record.h"

#include "dnsdb/zdb_utils.h"
#include "dnsdb/zdb_error.h"

#include "dnsdb/dnsrdata.h"

#include "dnsdb/journal.h"
#include "dnsdb/dynupdate-diff.h"

#if HAS_DNSSEC_SUPPORT
#include "dnsdb/rrsig.h"
#if ZDB_HAS_NSEC_SUPPORT != 0
#include "dnsdb/nsec.h"
#endif
#if ZDB_HAS_NSEC3_SUPPORT != 0
#include "dnsdb/nsec3.h"
#endif
#endif

#ifndef HAS_DYNUPDATE_DIFF_ENABLED
#error "HAS_DYNUPDATE_DIFF_ENABLED not defined"
#endif

#ifdef DEBUG
#define ZONE_MUTEX_LOG 0    // set this to 0 to disable in DEBUG
#else
#define ZONE_MUTEX_LOG 0
#endif

extern logger_handle* g_database_logger;
#define MODULE_MSG_HANDLE g_database_logger

#define TMPRDATA_TAG 0x4154414452504d54

#if HAS_TRACK_ZONES_DEBUG_SUPPORT
smp_int g_zone_instanciated_count = SMP_INT_INITIALIZER;
ptr_set g_zone_instanciated_set = PTR_SET_PTR_EMPTY;
#endif

static void zdb_zone_record_or_flags_to_subdomains(zdb_rr_label *rr_label, u16 orflags)
{
    dictionary_iterator iter;
    dictionary_iterator_init(&rr_label->sub, &iter);
    while(dictionary_iterator_hasnext(&iter))
    {
        zdb_rr_label** sub_labelp = (zdb_rr_label**)dictionary_iterator_next(&iter);

        (*sub_labelp)->flags |= orflags;
        
        zdb_zone_record_or_flags_to_subdomains(*sub_labelp, orflags);
    }
}

/**
 * @brief Adds a record to a zone
 *
 * Adds a record to a zone.
 * 
 * @note Expects the full fqdn in the labels parameter. "." has a labels_top at -1
 * 
 *
 * @param[in] zone the zone
 * @param[in] labels the stack of labels of the dns name
 * @param[in] labels_top the index of the top of the stack (the level)
 * @param[in] type the type of the record
 * @param[in] ttlrdata the ttl and rdata of the record.  NOTE: the zone becomes its new owner !!!
 */

void
zdb_zone_record_add(zdb_zone *zone, dnslabel_vector_reference labels, s32 labels_top, u16 type, zdb_packed_ttlrdata *ttlrdata)
{
    zdb_rr_label *rr_label = zdb_rr_label_add(zone, labels, labels_top - zone->origin_vector.size - 1); // flow verified
    /* This record will be put as it is in the DB */

#if ZDB_HAS_NSEC_SUPPORT
    /*
     * At this point I could add empty nsec3 records, or schedule the nsec3 signature
     */
#endif

    u16 flag_mask = 0;

    switch(type)
    {
        case TYPE_CNAME:
        {
            if((rr_label->flags & ZDB_RR_LABEL_DROPCNAME) != 0)
            {
                log_err("zone %{dnsname}: ignoring CNAME add on non-CNAME", zone->origin);
                ZDB_RECORD_ZFREE(ttlrdata);
                return;
            }
            flag_mask = ZDB_RR_LABEL_HASCNAME;
            break;
        }
        case TYPE_RRSIG:
        {
            if(!zdb_record_insert_checked_keep_ttl(&rr_label->resource_record_set, type, ttlrdata)) /* FB done */
            {
                ZDB_RECORD_ZFREE(ttlrdata);
                return;
            }

            rr_label->flags |= flag_mask;

#if ZDB_CHANGE_FEEDBACK_SUPPORT != 0

            /*
             * Update ICMTL.
             *
             * NOTE: the zdb_rr_label set of functions are zdb_listener-aware but the zdb_record ones are not.
             * That's why this one needs a call to the listener.
             *
             */

            zdb_ttlrdata unpacked_ttlrdata;
            unpacked_ttlrdata.rdata_pointer = &ttlrdata->rdata_start[0];
            unpacked_ttlrdata.rdata_size = ttlrdata->rdata_size;
            unpacked_ttlrdata.ttl = ttlrdata->ttl;
            zdb_listener_notify_add_record(zone, labels, labels_top, type, &unpacked_ttlrdata);
#endif
            return;
        }
        case TYPE_NSEC:
            break;
        case TYPE_NS:
        {
            if( (rr_label->flags & ZDB_RR_LABEL_HASCNAME) != 0)
            {
                log_err("zone %{dnsname}: ignoring NS add on CNAME", zone->origin);
                ZDB_RECORD_ZFREE(ttlrdata);
                return;
            }

            if( (rr_label->flags & ZDB_RR_LABEL_APEX) == 0)
            {
                flag_mask = ZDB_RR_LABEL_DELEGATION;

                /* all labels under are "under delegation" */
                
                zdb_zone_record_or_flags_to_subdomains(rr_label, ZDB_RR_LABEL_UNDERDELEGATION);
            }
            
            flag_mask |= ZDB_RR_LABEL_DROPCNAME;

            break;
        }
        default:
        {
            if( (rr_label->flags & ZDB_RR_LABEL_HASCNAME) != 0)
            {
                log_err("zone %{dnsname}: ignoring non-CNAME add on CNAME", zone->origin);
                ZDB_RECORD_ZFREE(ttlrdata);
                return;
            }
            flag_mask = ZDB_RR_LABEL_DROPCNAME;
            break;
        }
    }

    if(!zdb_record_insert_checked(&rr_label->resource_record_set, type, ttlrdata)) /* FB done */
    {
        ZDB_RECORD_ZFREE(ttlrdata);
        return;
    }

    rr_label->flags |= flag_mask;

#if ZDB_CHANGE_FEEDBACK_SUPPORT != 0

    /*
     * Update ICMTL.
     *
     * NOTE: the zdb_rr_label set of functions are zdb_listener-aware but the zdb_record ones are not.
     * That's why this one needs a call to the listener.
     *
     */

    zdb_ttlrdata unpacked_ttlrdata;
    unpacked_ttlrdata.rdata_pointer = &ttlrdata->rdata_start[0];
    unpacked_ttlrdata.rdata_size = ttlrdata->rdata_size;
    unpacked_ttlrdata.ttl = ttlrdata->ttl;
    zdb_listener_notify_add_record(zone, labels, labels_top, type, &unpacked_ttlrdata);
#endif
}

/**
 * @brief Removes a record from a zone
 *
 * Removes a record from a zone and frees its memory.
 * 
 * In the current version (20160513) if the record itself should not be used as
 * the parameter is still used after removal for feedback to the journal.
 *
 * @param[in] zone the zone
 * @param[in] labels the stack of labels of the dns name
 * @param[in] labels_top the index of the top of the stack (the level)
 * @param[in] type the type of the record
 * @param[in] ttlrdata the ttl and rdata of the record.  NOTE: the caller stays the owner
 */


ya_result
zdb_zone_record_delete(zdb_zone *zone, dnslabel_vector_reference labels, s32 labels_top, u16 type, zdb_packed_ttlrdata* packed_ttlrdata)
{
    zdb_ttlrdata ttlrdata;

    ttlrdata.next = NULL;
    ttlrdata.rdata_size = ZDB_PACKEDRECORD_PTR_RDATASIZE(packed_ttlrdata);
    ttlrdata.rdata_pointer = ZDB_PACKEDRECORD_PTR_RDATAPTR(packed_ttlrdata);
    ttlrdata.ttl = packed_ttlrdata->ttl;

    return zdb_rr_label_delete_record_exact(zone, labels, labels_top, type, &ttlrdata); // in zdb_zone_record_delete
}

/**
 * @brief Removes a record from a zone
 *
 * Copies the record then removes it from the zone and frees its memory.
 * This allows removing safely a record using itself as a parameter.
 *
 * @param[in] zone the zone
 * @param[in] labels the stack of labels of the dns name
 * @param[in] labels_top the index of the top of the stack (the level)
 * @param[in] type the type of the record
 * @param[in] ttlrdata the ttl and rdata of the record.  NOTE: the caller stays the owner
 */

ya_result
zdb_zone_record_delete_self(zdb_zone *zone, dnslabel_vector_reference labels, s32 labels_top, u16 type, zdb_packed_ttlrdata* packed_ttlrdata)
{
    u8 *tmp;
    ya_result ret;
    u8 tmp_[512];
    zdb_ttlrdata ttlrdata;
    ttlrdata.next = NULL;
    ttlrdata.rdata_size = ZDB_PACKEDRECORD_PTR_RDATASIZE(packed_ttlrdata);
    if(ttlrdata.rdata_size <= sizeof(tmp_))
    {
        tmp = &tmp_[0];
    }
    else // ttlrdata.rdata_size > sizeof(tmp_) // scan build does not seem to get this
    {
        MALLOC_OR_DIE(u8*,tmp,ttlrdata.rdata_size,TMPRDATA_TAG);
    }
    memcpy(tmp, ZDB_PACKEDRECORD_PTR_RDATAPTR(packed_ttlrdata), ttlrdata.rdata_size);
    ttlrdata.rdata_pointer = tmp;
    ttlrdata.ttl = packed_ttlrdata->ttl;

    ret = zdb_rr_label_delete_record_exact(zone, labels, labels_top, type, &ttlrdata); // safe
    
    if(ttlrdata.rdata_size > sizeof(tmp_))
    {
        free(tmp); // scan-build false positive : this is only called because tmp has been mallocated
    }
    
    return ret;
}

/**
 * @brief Search for a record in a zone
 *
 * Search for a record in a zone
 *
 * @param[in] zone the zone
 * @param[in] labels the stack of labels of the dns name
 * @param[in] labels_top the index of the top of the stack (the level)
 * @param[in] type the type of the record
 */

zdb_packed_ttlrdata*
zdb_zone_record_find(zdb_zone *zone, dnslabel_vector_reference labels, s32 labels_top, u16 type)
{
    zdb_rr_label* rr_label = zdb_rr_label_find_exact(zone->apex, labels, labels_top);

    if(rr_label != NULL)
    {
        return zdb_record_find(&rr_label->resource_record_set, type);
    }

    return NULL;
}

static ya_result
zdb_default_query_access_filter(const message_data *mesg, const void *extension)
{
    return SUCCESS;
}

static
u32 zdb_zone_get_struct_size(const u8 *origin)
{
    u32 zone_footprint = sizeof(zdb_zone) - sizeof(dnsname_vector) + sizeof(u8*) * (dnsname_getdepth(origin) + 1);
    
    return zone_footprint;
}

zdb_zone*
zdb_zone_create(const u8* origin)
{
    zdb_zone *zone;
    u32 zone_footprint = zdb_zone_get_struct_size(origin);
    ZALLOC_ARRAY_OR_DIE(zdb_zone*, zone, zone_footprint, ZDB_ZONETAG);
    
#ifdef DEBUG
    memset(zone, 0xac, zone_footprint);
#endif
    
#if HAS_TRACK_ZONES_DEBUG_SUPPORT
    smp_int_inc(&g_zone_instanciated_count);
    pthread_mutex_lock(&g_zone_instanciated_count.mutex);
    ptr_node *node = ptr_set_avl_insert(&g_zone_instanciated_set, zone);
    pthread_mutex_unlock(&g_zone_instanciated_count.mutex);
    node->value = NULL;
#endif
   
    log_debug7("zdb_zone_create %{dnsname}@%p", origin, zone);
            
    zone->origin = dnsname_zdup(origin);

    dnsname_to_dnsname_vector(zone->origin, &zone->origin_vector);

#if ZDB_RECORDS_MAX_CLASS != 1
    zone->zclass = CLASS_IN;
#endif
    
    zone->axfr_timestamp = 1;
    /* zone->axfr_serial = 0; implicit */

#if ZDB_HAS_DNSSEC_SUPPORT != 0
    ZEROMEMORY(&zone->nsec, sizeof(dnssec_zone_extension));
    zone->sig_validity_interval_seconds = 30*24*3600;       /* 1 month */
    zone->sig_validity_regeneration_seconds = 7*24*3600;    /* 1 week */
    zone->sig_validity_jitter_seconds = 86400;              /* 1 day */
    zone->sig_quota = 100;

#endif

    zone->alarm_handle = alarm_open(zone->origin);

    zone->apex = zdb_rr_label_new_instance(ROOT_LABEL);
    zone->apex->flags = ZDB_RR_LABEL_APEX;

    zone->query_access_filter = zdb_default_query_access_filter;
    zone->extension = NULL;
#if ZDB_HAS_DNSSEC_SUPPORT
    zone->progressive_signature_update.current_fqdn = NULL;
#endif
    mutex_init(&zone->lock_mutex);
    cond_init(&zone->lock_cond);
    zone->rc = 1;
    zone->lock_owner = ZDB_ZONE_MUTEX_NOBODY;
    zone->lock_count = 0;
    zone->lock_reserved_owner = ZDB_ZONE_MUTEX_NOBODY;
    zone->_status = 0;
    zone->_flags = 0;
#if ZDB_HAS_OLD_MUTEX_DEBUG_SUPPORT
    zone->lock_trace = NULL;
    zone->lock_id = 0;
    zone->lock_timestamp = 0;
#endif
#if ZDB_ZONE_HAS_JNL_REFERENCE
    zone->journal = NULL;
#endif
    
    return zone;
}

void
zdb_zone_invalidate(zdb_zone *zone)
{
    yassert(zone != NULL && zone->apex != NULL);
    
    zone->apex->flags |= ZDB_RR_LABEL_INVALID_ZONE;
}

/**
 * @brief Destroys a zone and all its content
 *
 * Destroys a zone and all its content
 *
 * @param[in] zone a pointer to the zone
 */

void
zdb_zone_truncate_invalidate(zdb_zone *zone)
{
    if(zone != NULL)
    {
        // remove all alarms linked to the zone
        alarm_close(zone->alarm_handle);
        zone->alarm_handle = ALARM_HANDLE_INVALID;
        
        // empty the zone records
        if(zone->apex != NULL)
        {
#if ZDB_HAS_NSEC_SUPPORT
            nsec_destroy_zone(zone);
#endif

#if ZDB_HAS_NSEC3_SUPPORT
            nsec3_destroy_zone(zone);
#endif
            
            // zdb_rr_label_destroy(zone, &zone->apex);
            
            /*
             * Destroy ALL the content of the apex but not the apex itself.
             */
            
            zdb_rr_label_truncate(zone, zone->apex);
            
            zone->apex->flags |= ZDB_RR_LABEL_INVALID_ZONE;
        }
    }
}

/**
 * @brief Destroys a zone and all its content
 *
 * Destroys a zone and all its content
 *
 * @param[in] zone a pointer to the zone
 */

void
zdb_zone_destroy(zdb_zone *zone)
{     
    if(zone != NULL)
    {
        zdb_zone_lock(zone, ZDB_ZONE_MUTEX_DESTROY);
        int rc = zone->rc;
        zdb_zone_unlock(zone, ZDB_ZONE_MUTEX_DESTROY);
        
        if(rc != 0)
        {
            logger_flush();
            abort();
        }
        
        log_debug5("zdb_zone_destroy zone@%p", zone);
        
#if HAS_TRACK_ZONES_DEBUG_SUPPORT
        pthread_mutex_lock(&g_zone_instanciated_count.mutex);
        bool known_zone = (ptr_set_avl_find(&g_zone_instanciated_set, zone) != NULL);
        yassert(known_zone);
        ptr_set_avl_delete(&g_zone_instanciated_set, zone);
        pthread_mutex_unlock(&g_zone_instanciated_count.mutex);
        smp_int_dec(&g_zone_instanciated_count);
        yassert(smp_int_get(&g_zone_instanciated_count) >= 0);
#endif
        
        zdb_zone_lock(zone, ZDB_ZONE_MUTEX_DESTROY);
        if(zone->alarm_handle != ALARM_HANDLE_INVALID)
        {
            alarm_close(zone->alarm_handle);
            zone->alarm_handle = ALARM_HANDLE_INVALID;
        }
#if ZDB_ZONE_HAS_JNL_REFERENCE        
        if(zone->journal != NULL)
        {
            journal *jh = zone->journal; // pointed for closing/releasing
            zdb_zone_unlock(zone, ZDB_ZONE_MUTEX_DESTROY);
            journal_close(jh); // only authorised usage of this call
            zone->journal = NULL;
        }
        else
#endif
        {
            zdb_zone_unlock(zone, ZDB_ZONE_MUTEX_DESTROY);
        }
        
                
#ifndef DEBUG
        // do not bother clearing the memory if it's for a shutdown (faster)
        if(!dnscore_shuttingdown())
#endif
        {
            if(zone->apex != NULL)
            {

#if ZDB_HAS_NSEC_SUPPORT
                nsec_destroy_zone(zone);
#endif

#if ZDB_HAS_NSEC3_SUPPORT
                nsec3_destroy_zone(zone);
#endif
                zdb_rr_label_destroy(zone, &zone->apex);
                zone->apex = NULL;
            }
        }
        
        u32 zone_footprint = zdb_zone_get_struct_size(zone->origin);
        
        dnsname_zfree(zone->origin);
        
#if HAS_DNSSEC_SUPPORT
        if(zone->progressive_signature_update.current_fqdn != NULL)
        {
            dnsname_zfree(zone->progressive_signature_update.current_fqdn);
            zone->progressive_signature_update.current_fqdn = NULL;
        }
#endif

#ifdef DEBUG
        zone->origin = NULL;
        zone->min_ttl= 0xbadbad01;
        zone->extension = NULL;
        zone->axfr_serial = 0xbadbad00;
#endif

        cond_finalize(&zone->lock_cond);
        mutex_destroy(&zone->lock_mutex);
                
        ZFREE_ARRAY(zone, zone_footprint);
    }
}

/**
 * @brief Copies the soa of a zone to an soa_rdata structure.
 *
 * Copies the soa of a zone to an soa_rdata structure.
 * No memory is allocated for the soa_rdata.  If the zone is destroyed,
 * the soa_rdata becomes invalid.
 *
 * @param[in] zone a pointer to the zone
 * @param[out] soa_out a pointer to an soa_rdata structure
 */

ya_result
zdb_zone_getsoa(const zdb_zone *zone, soa_rdata* soa_out)
{
#ifdef DEBUG
    if(zone->lock_owner == ZDB_ZONE_MUTEX_NOBODY)
    {
        log_err("zdb_zone_getsoa called on an unlocked zone: %{dnsname}", zone->origin);
        debug_log_stacktrace(MODULE_MSG_HANDLE, LOG_ERR, "zdb_zone_getsoa");
        //logger_flush();
    }
    else
    {
        log_debug("zdb_zone_getsoa called on a zone locked by %02hhx (%{dnsname})", zone->lock_owner, zone->origin);
    }
#endif
    
    const zdb_rr_label *apex = zone->apex;
    const zdb_packed_ttlrdata *soa = zdb_record_find(&apex->resource_record_set, TYPE_SOA); // zone is locked
    ya_result return_code;

    if(soa != NULL)
    {
        return_code = zdb_record_getsoa(soa, soa_out);
    }
    else
    {
        return_code = ZDB_ERROR_NOSOAATAPEX;
    }
    
    return return_code;
}

ya_result
zdb_zone_getsoa_ttl_rdata(const zdb_zone *zone, u32 *ttl, u16 *rdata_size, const u8 **rdata)
{
#ifdef DEBUG
    if(zone->lock_owner == ZDB_ZONE_MUTEX_NOBODY)
    {
        log_err("zdb_zone_getsoa_ttl_rdata called on an unlocked zone: %{dnsname}", zone->origin);
        debug_log_stacktrace(MODULE_MSG_HANDLE, LOG_ERR, "zdb_zone_getsoa_ttl_rdata");
        logger_flush();
    }
    else
    {
        log_debug("zdb_zone_getsoa_ttl_rdata called on a zone locked by %02hhx (%{dnsname})", zone->lock_owner, zone->origin);
    }
#endif
    
    const zdb_rr_label *apex = zone->apex;
    const zdb_packed_ttlrdata *soa = zdb_record_find(&apex->resource_record_set, TYPE_SOA); // zone is locked

    if(soa == NULL)
    {
        return ZDB_ERROR_NOSOAATAPEX;
    }

    if(ttl != NULL)
    {
        *ttl = soa->ttl;
    }

    if(rdata_size != NULL && rdata != NULL)
    {
        *rdata_size = soa->rdata_size;
        *rdata = &soa->rdata_start[0];
    }

    return SUCCESS;
}

/**
 * @brief Retrieve the serial of a zone
 *
 * Retrieve the serial of a zone
 *
 * @param[in] zone a pointer to the zone
 * @param[out] soa_out a pointer to an soa_rdata structure
 */

ya_result
zdb_zone_getserial(const zdb_zone *zone, u32 *serial)
{
#ifdef DEBUG
    if(zone->lock_owner == ZDB_ZONE_MUTEX_NOBODY)
    {
        log_err("zdb_zone_getserial called on an unlocked zone (%{dnsname})", zone->origin);
        debug_log_stacktrace(MODULE_MSG_HANDLE, LOG_ERR, "zdb_zone_getserial");
        logger_flush();
    }
    else
    {
        log_debug1("zdb_zone_getserial called on a zone locked by %02hhx (%{dnsname})", zone->lock_owner, zone->origin);
    }
#endif
    
    yassert(serial != NULL);

    zdb_rr_label *apex = zone->apex;
    zdb_packed_ttlrdata *soa = zdb_record_find(&apex->resource_record_set, TYPE_SOA); // zone is locked

    if(soa != NULL)
    {
        return rr_soa_get_serial(soa->rdata_start, soa->rdata_size, serial);
    }

    return ZDB_ERROR_NOSOAATAPEX;
}

const zdb_packed_ttlrdata*
zdb_zone_get_dnskey_rrset(zdb_zone *zone)
{
    return zdb_record_find(&zone->apex->resource_record_set, TYPE_DNSKEY); // zone is locked
}

bool
zdb_zone_isinvalid(zdb_zone *zone)
{
    bool invalid = TRUE;
    
    if((zone != NULL) && (zone->apex != NULL))
    {
        invalid = (zone->apex->flags & ZDB_RR_LABEL_INVALID_ZONE) != 0;
    }
    
    return invalid;
}

#if HAS_DNSSEC_SUPPORT

/**
 * 
 * Returns TRUE iff the key is present as a record in the zone
 * 
 * @param zone
 * @param key
 * @return 
 */

bool
zdb_zone_contains_dnskey_record_for_key(zdb_zone *zone, const dnssec_key *key)
{
    yassert(zdb_zone_islocked(zone));
    
    const zdb_packed_ttlrdata *dnskey_rrset = zdb_record_find(&zone->apex->resource_record_set, TYPE_DNSKEY); // zone is locked
    
    const zdb_packed_ttlrdata *dnskey_record = dnskey_rrset;
    
    while(dnskey_record != NULL)
    {
        if(dnskey_matches_rdata(key, ZDB_PACKEDRECORD_PTR_RDATAPTR(dnskey_record), ZDB_PACKEDRECORD_PTR_RDATASIZE(dnskey_record)))
        {
            return TRUE;
        }

        dnskey_record = dnskey_record->next;
    }
    
    return FALSE;
}

/**
 * Returns TRUE iff there is at least one RRSIG record with the tag and algorithm of the key
 * 
 * @param zone
 * @param key
 * @return 
 */

bool
zdb_zone_apex_contains_rrsig_record_by_key(zdb_zone *zone, const dnssec_key *key)
{
    yassert(zdb_zone_islocked(zone));
    
    const zdb_packed_ttlrdata *rrsig_rrset = zdb_record_find(&zone->apex->resource_record_set, TYPE_RRSIG); // zone is locked
    
    if(rrsig_rrset != NULL)
    {
        const zdb_packed_ttlrdata *rrsig_record = rrsig_rrset;
        u16 tag = dnssec_key_get_tag_const(key);
        u8 algorithm = dnssec_key_get_algorithm(key);
        
        while(rrsig_record != NULL)
        {
            if((RRSIG_ALGORITHM(rrsig_record) == algorithm) && (RRSIG_KEY_TAG(rrsig_record) == tag))
            {
                return TRUE;
            }

            rrsig_record = rrsig_record->next;
        }
    }
    
    return FALSE;
}

#if HAS_MASTER_SUPPORT

/**
 * Adds a DNSKEY record in a zone from the dnssec_key object.
 * 
 * @param key
 * @return TRUE iff the record has been added
 */

bool
zdb_zone_add_dnskey_from_key(zdb_zone *zone, const dnssec_key *key)
{
    yassert(zdb_zone_islocked(zone));
    
    zdb_packed_ttlrdata *dnskey_record;
    u32 rdata_size = key->vtbl->dnskey_key_rdatasize(key);
    ZDB_RECORD_ZALLOC_EMPTY(dnskey_record, 86400, rdata_size);
    key->vtbl->dnskey_key_writerdata(key, ZDB_PACKEDRECORD_PTR_RDATAPTR(dnskey_record));

    // store the record

    if(zdb_record_insert_checked(&zone->apex->resource_record_set, TYPE_DNSKEY, dnskey_record)) /* FB done */
    {
#if ZDB_CHANGE_FEEDBACK_SUPPORT
        zdb_ttlrdata unpacked_dnskey_record;
        unpacked_dnskey_record.rdata_pointer = ZDB_PACKEDRECORD_PTR_RDATAPTR(dnskey_record);
        unpacked_dnskey_record.rdata_size = ZDB_PACKEDRECORD_PTR_RDATASIZE(dnskey_record);
        unpacked_dnskey_record.ttl = dnskey_record->ttl;
        zdb_listener_notify_add_record(zone, zone->origin_vector.labels, zone->origin_vector.size, TYPE_DNSKEY, &unpacked_dnskey_record);
#endif
        return TRUE;
    }
    else
    {
        ZDB_RECORD_ZFREE(dnskey_record);
        
        return FALSE;
    }
}

/**
 * Removes a DNSKEY record in a zone from the dnssec_key object.
 * 
 * @param key
 * @return TRUE iff the record has been found and removed
 */


bool
zdb_zone_remove_dnskey_from_key(zdb_zone *zone, const dnssec_key *key)
{
    yassert(zdb_zone_islocked(zone));
    
    zdb_packed_ttlrdata *dnskey_record;
    u32 rdata_size = key->vtbl->dnskey_key_rdatasize(key);
    ZDB_RECORD_ZALLOC_EMPTY(dnskey_record, 86400, rdata_size);
    key->vtbl->dnskey_key_writerdata(key, ZDB_PACKEDRECORD_PTR_RDATAPTR(dnskey_record));

    zdb_ttlrdata unpacked_dnskey_record;
    unpacked_dnskey_record.rdata_pointer = ZDB_PACKEDRECORD_PTR_RDATAPTR(dnskey_record);
    unpacked_dnskey_record.rdata_size = ZDB_PACKEDRECORD_PTR_RDATASIZE(dnskey_record);
    unpacked_dnskey_record.ttl = dnskey_record->ttl;
    
    // remove the record

    if(zdb_record_delete_self_exact(&zone->apex->resource_record_set, TYPE_DNSKEY, &unpacked_dnskey_record) >= 0)
    {
#if ZDB_CHANGE_FEEDBACK_SUPPORT
        zdb_listener_notify_remove_record(zone, zone->origin, TYPE_DNSKEY, &unpacked_dnskey_record);
#endif
        // remove all RRSIG on DNSKEY
        rrsig_delete(zone, zone->origin,zone->apex, TYPE_DNSKEY);
        
        rrsig_delete_by_tag(zone, dnssec_key_get_tag_const(key));
                
        // zdb_listener_notify_remove_type(zone, zone->origin, &zone->apex->resource_record_set, TYPE_RRSIG);

        ZDB_RECORD_ZFREE(dnskey_record);
        
        return TRUE;
    }
    else
    {    
        ZDB_RECORD_ZFREE(dnskey_record);
        return FALSE;
    }
}

static ya_result
zdb_zone_update_zone_remove_add_dnskeys(zdb_zone *zone, ptr_vector *removed_keys, ptr_vector *added_keys, u8 secondary_lock)
{
    dynupdate_message dmsg;
    packet_unpack_reader_data reader;
    const u8 *fqdn = NULL;
    
    if(!ptr_vector_isempty(removed_keys))
    {
        dnssec_key *key = (dnssec_key*)ptr_vector_get(removed_keys, 0);
        yassert(key != NULL);
        fqdn = dnssec_key_get_domain(key);
    }
    else if(!ptr_vector_isempty(added_keys))
    {
        dnssec_key *key = (dnssec_key*)ptr_vector_get(added_keys, 0);
        yassert(key != NULL);
        fqdn = dnssec_key_get_domain(key);
    }
    else
    {
        return 0;   // EMPTY
    }
    
    ya_result ret;
    int add_index = 0;
    int del_index = 0;
    bool work_to_do = FALSE;
    
    do
    {
        dynupdate_message_init(&dmsg, fqdn, CLASS_IN);
        
        for(; add_index <= ptr_vector_last_index(added_keys); ++add_index)
        {
            dnssec_key *key = (dnssec_key*)ptr_vector_get(added_keys, add_index);
            if(FAIL(ret = dynupdate_message_add_dnskey(&dmsg, zone->min_ttl, key)))
            {
                log_debug("dnskey: %{dnsname}: +%03d+%05d/%d key cannot be sent with this update, postponing", dnssec_key_get_domain(key), dnssec_key_get_algorithm(key), dnssec_key_get_tag_const(key), ntohs(dnssec_key_get_flags(key)), ret);
                work_to_do = TRUE;
                break;
            }

            log_info("dnskey: %{dnsname}: +%03d+%05d/%d key will be added", dnssec_key_get_domain(key), dnssec_key_get_algorithm(key), dnssec_key_get_tag_const(key), ntohs(dnssec_key_get_flags(key)));
        }

        if(!work_to_do)
        {
            for(; del_index <= ptr_vector_last_index(removed_keys); ++del_index)
            {
                dnssec_key *key = (dnssec_key*)ptr_vector_get(removed_keys, del_index);
                if(FAIL(ret = dynupdate_message_del_dnskey(&dmsg, key)))
                {
                    log_debug("dnskey: %{dnsname}: +%03d+%05d/%d key cannot be sent with this update, postponing", dnssec_key_get_domain(key), dnssec_key_get_algorithm(key), dnssec_key_get_tag_const(key), ntohs(dnssec_key_get_flags(key)), ret);
                    work_to_do = TRUE;
                    break;
                }

                log_info("dnskey: %{dnsname}: +%03d+%05d/%d key will be removed", dnssec_key_get_domain(key), dnssec_key_get_algorithm(key), dnssec_key_get_tag_const(key), ntohs(dnssec_key_get_flags(key)));
            }
        }
    
        dynupdate_message_set_reader(&dmsg, &reader);
        u16 count = dynupdate_message_get_count(&dmsg);

        packet_reader_skip(&reader, DNS_HEADER_LENGTH);
        packet_reader_skip_fqdn(&reader);
        packet_reader_skip(&reader, 4);

        // the update is ready : push it

        if(ISOK(ret = dynupdate_diff(zone, &reader, count, secondary_lock, DYNUPDATE_UPDATE_RUN)))
        {
            // done
            log_info("dnskey: %{dnsname}: keys update successfull", fqdn);
        }

        if(FAIL(ret))
        {
            log_err("dnskey: %{dnsname}: keys update failed", fqdn);
            break;
        }

        dynupdate_message_finalise(&dmsg);
    }
    while(work_to_do);
    
    return ret;
}

/**
 * From the keystore (files/pkcs12) for that zone
 * 
 * Remove the keys that should not be in the zone anymore.
 * Add the keys that should be in the zone.
 * 
 * @param zone
 */

void
zdb_zone_update_keystore_keys_from_zone(zdb_zone *zone, u8 secondary_lock)
{
    // keystore keys with a publish time that did not expire yet have to be added
    // keystore keys with an unpublish time that passed have to be removed
    //
    // after (and only after) the signature is done, set alarms at all the (relevant) timings of the keys (publish, activate, inactivate, unpublish)
    
    yassert(zdb_zone_islocked(zone));

    ptr_vector dnskey_add = EMPTY_PTR_VECTOR;
    ptr_vector dnskey_del = EMPTY_PTR_VECTOR;

    for(int i = 0; ; ++i)
    {
        dnssec_key *key = dnssec_keystore_acquire_key(zone->origin, i);
        if(key == NULL)
        {
            break;
        }

        if(dnskey_is_published(key, time(NULL)))
        {
            if(!zdb_zone_contains_dnskey_record_for_key(zone, key))
            {
                dnskey_acquire(key);
                ptr_vector_append(&dnskey_add, key);
            }
        }

        dnskey_release(key);
    }


    zdb_packed_ttlrdata *dnskey_rrset = zdb_record_find(&zone->apex->resource_record_set, TYPE_DNSKEY); // zone is locked
    zdb_packed_ttlrdata *dnskey_record = dnskey_rrset;
    while(dnskey_record != NULL)
    {
        dnssec_key *key;

        if(ISOK(dnssec_keystore_load_private_key_from_rdata(
                ZDB_PACKEDRECORD_PTR_RDATAPTR(dnskey_record),
                ZDB_PACKEDRECORD_PTR_RDATASIZE(dnskey_record),
                zone->origin,
                &key)))
        {
            if(dnskey_is_unpublished(key, time(NULL)))
            {
                // need to unpublish
                ptr_vector_append(&dnskey_del, key);
            }

            dnskey_release(key);
        }

        dnskey_record = dnskey_record->next;
    }

    if(ptr_vector_size(&dnskey_add) + ptr_vector_size(&dnskey_del) > 0)
    {
        if(ISOK(zdb_zone_update_zone_remove_add_dnskeys(zone, &dnskey_del, &dnskey_add, secondary_lock)))
        {
            log_info("zone: %{dnsname}: keys added: %i, keys deleted: %i", zone->origin, ptr_vector_size(&dnskey_add), ptr_vector_size(&dnskey_del));
        }
        else
        {
            log_warn("zone: %{dnsname}: failed to update keys", zone->origin, ptr_vector_size(&dnskey_add), ptr_vector_size(&dnskey_del));
        }
    }
}

#endif // HAS_MASTER_SUPPORT

#endif

#ifdef DEBUG

/**
 * DEBUG
 */

void
zdb_zone_print_indented(zdb_zone *zone, output_stream *os, int indent)
{
    if(zone == NULL)
    {
        osformatln(os, "%tz: NULL", indent);
        return;
    }
    
    u16 zclass = zdb_zone_getclass(zone);

    osformatln(os, "%tzone@%p(CLASS=%{dnsclass},ORIGIN='%{dnsname}'", indent, (void*)zone, &zclass, zone->origin);
    zdb_rr_label_print_indented(zone->apex, os, indent + 1);
    osformatln(os, "%t+:", indent);
}

void
zdb_zone_print(zdb_zone *zone, output_stream *os)
{
    zdb_zone_print_indented(zone, os, 0);
}

#endif

u8
zdb_zone_get_status(zdb_zone *zone)
{
    mutex_lock(&zone->lock_mutex);
    u8 ret = zone->_status;
    mutex_unlock(&zone->lock_mutex);
    return ret;
}

u8
zdb_zone_set_status(zdb_zone *zone, u8 status)
{
#ifdef DEBUG
    log_debug4("zdb_zone_set_status(%{dnsname},%02x)", zone->origin, status);
#endif
    mutex_lock(&zone->lock_mutex);
    u8 ret = zone->_status;
    zone->_status |= status;
    mutex_unlock(&zone->lock_mutex);
    return ret;
}

u8
zdb_zone_clear_status(zdb_zone *zone, u8 status)
{
#ifdef DEBUG
    log_debug4("zdb_zone_clear_status(%{dnsname},%02x)", zone->origin, status);
#endif
    mutex_lock(&zone->lock_mutex);
    u8 ret = zone->_status;
    zone->_status &= ~status;
    mutex_unlock(&zone->lock_mutex);
    return ret;
}

/** @} */
