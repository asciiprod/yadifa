/*------------------------------------------------------------------------------
*
* Copyright (c) 2011, EURid. All rights reserved.
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
/** @defgroup dnsdb Zone database
 *  @brief The zone dataBase
 *
 * @{
 */

#define ZDB_JOURNAL_CODE 1

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include <dnscore/logger.h>
#include <dnscore/format.h>

#include <dnscore/sys_error.h>

#include <dnscore/dnscore.h>
#include <dnscore/sys_get_cpu_count.h>

#include <dnscore/thread_pool.h>


extern logger_handle* g_database_logger;
#define MODULE_MSG_HANDLE g_database_logger

#include "dnsdb/zdb.h"

#if ZDB_HAS_DNSSEC_SUPPORT != 0
#include "dnsdb/dnssec_keystore.h"
#endif

#include "dnsdb/zdb_zone.h"
#include "dnsdb/zdb_zone_label.h"
#include "dnsdb/zdb_rr_label.h"
#include "dnsdb/zdb_record.h"
#include "dnsdb/zdb_utils.h"
#include "dnsdb/dictionary.h"
#include "dnsdb/journal.h"
#include "dnsdb/zdb-zone-garbage.h"

#if ZDB_OPENSSL_SUPPORT!=0
#include <openssl/ssl.h>
#include <openssl/engine.h>

/*
 * Required to handle openssl with multiple threads
 */

#define ZDB_SSLMUTEX_TAG 0x584554554d4c5353

static pthread_mutex_t *ssl_mutex = NULL;
static int ssl_mutex_count = 0;

static void
ssl_lock(int mode, int type, const char* file, int line)
{
    if((mode & CRYPTO_LOCK) != 0)
    {
        /* lock */

        pthread_mutex_lock(&ssl_mutex[type]);
    }
    else
    {
        /* unlock */

        pthread_mutex_unlock(&ssl_mutex[type]);
    }
}

static unsigned long
ssl_thread_id()
{
    return (unsigned long)pthread_self();
}

#endif

logger_handle* g_database_logger = NULL;

/** @brief Initializes the database internals.
 *
 *  Checks the architecture settings of the binary.
 *  Initializes the database internals.
 *  Multiple calls is a NOP.
 *
 *  This is not thread safe.
 *
 */

static volatile bool zdb_init_done = FALSE;

dnslib_fingerprint dnsdb_getfingerprint()
{
    dnslib_fingerprint ret = (dnslib_fingerprint)(0
#if ZDB_HAS_TSIG_SUPPORT
    | DNSLIB_TSIG
#endif
#if ZDB_HAS_ACL_SUPPORT != 0
    | DNSLIB_ACL
#endif
#if ZDB_HAS_NSEC_SUPPORT != 0
    | DNSLIB_NSEC
#endif
#if ZDB_HAS_NSEC3_SUPPORT != 0
    | DNSLIB_NSEC3
#endif
    );

    return ret;
}

u32 dnsdb_fingerprint_mask()
{
    return DNSLIB_TSIG|DNSLIB_ACL|DNSLIB_NSEC|DNSLIB_NSEC3;
}

int zalloc_init();

void
zdb_init_ex(u32 thread_pool_count)
{
    (void)thread_pool_count;
    
    if(zdb_init_done)
    {
        return;
    }

    /* DO or DIE */

    if(dnscore_getfingerprint() != (dnsdb_getfingerprint() & dnscore_fingerprint_mask()))
    {
        osformatln(termerr, "mismatched fingerprints: %08x != (%08x = %08x & %08x)",
                dnscore_getfingerprint(),
                dnsdb_getfingerprint() & dnscore_fingerprint_mask(),
                dnsdb_getfingerprint() , dnscore_fingerprint_mask());

        flusherr();
        
        exit(-1);
    }

    zdb_init_done = TRUE;
    
    /* Init the dns core */

    dnscore_init();
    
    zalloc_init();
    
    zdb_zone_garbage_init();

    /* Init the error table */

    zdb_register_errors();

    /* Init the hash tables */

    hash_init();

#if ZDB_OPENSSL_SUPPORT!=0

    /* Init openssl */

    ENGINE_load_openssl();
    ENGINE_load_builtin_engines();
    SSL_load_error_strings();

    ssl_mutex_count = CRYPTO_num_locks();

    MALLOC_OR_DIE(pthread_mutex_t*, ssl_mutex, ssl_mutex_count * sizeof (pthread_mutex_t), ZDB_SSLMUTEX_TAG);

    int i;

    for(i = 0; i < ssl_mutex_count; i++)
    {
        pthread_mutex_init(&ssl_mutex[i], NULL);
    }

    CRYPTO_set_id_callback(ssl_thread_id);
    CRYPTO_set_locking_callback(ssl_lock);
#endif

    journal_init(0);    // uses the default mru size (512)
    
    logger_start();
}

void
zdb_init()
{
    u32 thread_pool_count = sys_get_cpu_count() + 2;
    
    zdb_init_ex(thread_pool_count);
}

void
zdb_finalize()
{
    if(!zdb_init_done)
    {
        return;
    }

    zdb_init_done = FALSE;

    zdb_zone_garbage_finalize();
    
    journal_finalise();
    
#if ZDB_HAS_DNSSEC_SUPPORT != 0
    dnssec_keystore_destroy();
    dnssec_keystore_resetpath();
#endif

#if ZDB_OPENSSL_SUPPORT!=0

    ERR_remove_state(0);

    /* Init openssl */

    CRYPTO_set_locking_callback(NULL);
    CRYPTO_set_id_callback(NULL);

    int i;

    for(i = 0; i < ssl_mutex_count; i++)
    {
        pthread_mutex_destroy(&ssl_mutex[i]);
    }
    
    ssl_mutex_count = 0;

    free(ssl_mutex);

    ENGINE_cleanup();

#endif
}

/** @brief Initializes a database.
 *
 *  Initializes a database.
 *
 *  @param[in]  db a pointer to the zdb structure that will be initialized.
 *
 */

void
zdb_create(zdb* db)
{
    zdb_zone_label* zone_label;

    ZALLOC_OR_DIE(zdb_zone_label*, zone_label, zdb_zone_label, ZDB_ZONELABEL_TAG);
    ZEROMEMORY(zone_label, sizeof (zdb_zone_label));
    zone_label->name = dnslabel_zdup(ROOT_LABEL); /* . */
    dictionary_init(&zone_label->sub);


    db->root = zone_label; /* native order */
        
    db->alarm_handle = alarm_open((const u8*)"\010database"); /** @todo change this (uppercase?) */
    
    group_mutex_init(&db->mutex);
}

zdb_zone *
zdb_set_zone(zdb *db, zdb_zone* zone)
{
    yassert(zone != NULL);
    
    zdb_lock(db, ZDB_MUTEX_WRITER);
    zdb_zone_label *label = zdb_zone_label_add_nolock(db, &zone->origin_vector); // zdb_set_zone
    zdb_zone *old_zone = label->zone;
    zdb_zone_acquire(zone);
    label->zone = zone;
#ifdef DEBUG
    log_debug("zdb: added zone %{dnsname}@%p", zone->origin, zone);
#endif
    zdb_unlock(db, ZDB_MUTEX_WRITER);
    return old_zone;
}

zdb_zone *
zdb_remove_zone(zdb *db, dnsname_vector *name)
{
    yassert(db != NULL && name != NULL);
    
    zdb_lock(db, ZDB_MUTEX_WRITER);    
    zdb_zone_label *label = zdb_zone_label_find(db, name); // zdb_detach_zone
    zdb_zone *old_zone = NULL;
    if(label != NULL)
    {
        old_zone = label->zone;        
        label->zone = NULL;
#ifdef DEBUG
        log_debug("zdb: removed zone %{dnsnamevector}@%p", name, old_zone);
#endif
        if(ZONE_LABEL_IRRELEVANT(label))
        {
            // removes the label from the database
            // if the label had been relevant (sub domains)
            // the zones below would be released and destroyed in due time
            
            zdb_zone_label_delete(db, name);
        }
    }
    zdb_unlock(db, ZDB_MUTEX_WRITER);
    return old_zone;
}

zdb_zone *
zdb_remove_zone_from_dnsname(zdb *db, const u8 *fqdn)
{
    dnsname_vector origin;

    dnsname_to_dnsname_vector(fqdn, &origin);
    
    zdb_zone *zone = zdb_remove_zone(db, &origin);
    
    return zone;
}

#if 1

//#error obsolete

/** @brief Search for a match in the database
 *
 *  Search for a match in the database.
 *  Only the most relevant match will be returned.
 *
 *  @param[in]  db the database
 *  @param[in]  dnsname_name the name dnsname to search for
 *  @param[in]  class the class to match
 *  @param[in]  type the type to match
 *  @param[out] ttl_rdara_out a pointer to a pointer set of results (single linked list)
 *
 *  @return SUCCESS in case of success.
 */

ya_result
zdb_query(zdb* db, u8* name_, u16 zclass, u16 type, zdb_packed_ttlrdata** ttlrdata_out)
{
    yassert(ttlrdata_out != NULL);

    dnsname_vector name;
    DEBUG_RESET_dnsname(name);

    dnsname_to_dnsname_vector(name_, &name);


    /* Find closest matching label
     * Should return a stack of zones
     */

    zdb_zone_label_pointer_array zone_label_stack;

    s32 top = zdb_zone_label_match(db, &name, zone_label_stack);
    s32 sp = top;

    /* Got a stack of zone labels with and without zone cuts */
    /* Search the label on the zone files */

    while(sp >= 0)
    {
        zdb_zone_label* zone_label = zone_label_stack[sp];

        if(zone_label->zone != NULL)
        {
            if((*ttlrdata_out = zdb_zone_record_find(zone_label->zone, name.labels, name.size - sp, type)) != NULL)
            {
                /* *ttlrdata_out for the answer */
                /* zone_label->zone for the authority section */
                /* subsequent searchs for the additional */

                return SUCCESS;
            }
        }

        sp--;
    }



    return ZDB_ERROR_KEY_NOTFOUND;
}

#endif

/** @brief Search for a match in the database
 *
 *  Search for a match in the database.
 *  Only the most relevant match will be returned.
 *
 *  @param[in]  db the database
 *  @param[in]  dnsname_name the name dnsname to search for
 *  @param[in]  type the type to match
 *  @param[out] ttl_rdara_out a pointer to a pointer set of results (single linked list)
 *
 *  @return SUCCESS in case of success.
 */

ya_result
zdb_query_ip_records(zdb* db, const u8* name_, zdb_packed_ttlrdata* * restrict ttlrdata_out_a, zdb_packed_ttlrdata* * restrict ttlrdata_out_aaaa) // mutex checked
{
    yassert(ttlrdata_out_a != NULL && ttlrdata_out_aaaa != NULL);

    dnsname_vector name;

    dnsname_to_dnsname_vector(name_, &name);

    /* Find closest matching label
     * Should return a stack of zones
     */

#ifdef HAS_DYNAMIC_PROVISIONING
    zdb_lock(db, ZDB_MUTEX_READER); // zdb_query_ip_records
#endif
    
    zdb_zone_label_pointer_array zone_label_stack;

    s32 top = zdb_zone_label_match(db, &name, zone_label_stack);
    s32 sp = top;

    /* Got a stack of zone labels with and without zone cuts */
    /* Search the label on the zone files */

    while(sp >= 0)
    {
        zdb_zone_label* zone_label = zone_label_stack[sp];

        if(zone_label->zone != NULL)
        {
            /* Get the label, instead of the type in the label */
            zdb_rr_label* rr_label = zdb_rr_label_find_exact(zone_label->zone->apex, name.labels, name.size - sp);

            if(rr_label != NULL)
            {
                zdb_packed_ttlrdata* a = zdb_record_find(&rr_label->resource_record_set, TYPE_A);
                zdb_packed_ttlrdata* aaaa = zdb_record_find(&rr_label->resource_record_set, TYPE_AAAA);

                if(a != NULL || aaaa != NULL)
                {
                    *ttlrdata_out_a = a;
                    *ttlrdata_out_aaaa = aaaa;
                    
#ifdef HAS_DYNAMIC_PROVISIONING
                    zdb_unlock(db, ZDB_MUTEX_READER); // zdb_query_ip_records (success)
#endif
                    
                    return SUCCESS;
                }
            }
        }

        sp--;
    }



#ifdef HAS_DYNAMIC_PROVISIONING
    zdb_unlock(db, ZDB_MUTEX_READER); // zdb_query_ip_records (failure)
#endif
    
    return ZDB_ERROR_KEY_NOTFOUND;
}

/**
 * 
 * Appends all A and AAAA records found in the database for the given fqdn
 * Given the nature of the list, what is returned is a copy.
 * The call locks the database for reading, then each involved zone for reading.
 * Locks are released before the function returns.
 * 
 * @param db database
 * @param name_ fqdn
 * @param target_list list
 * @return 
 */

ya_result
zdb_append_ip_records(zdb* db, const u8* name_, host_address *target_list)
{
    yassert(target_list != NULL);
    
    dnsname_vector name;

    dnsname_to_dnsname_vector(name_, &name);

    /* Find closest matching label
     * Should return a stack of zones
     */

#ifdef HAS_DYNAMIC_PROVISIONING
    zdb_lock(db, ZDB_MUTEX_READER); // zdb_query_ip_records
#endif
    
    zdb_zone_label_pointer_array zone_label_stack;

    s32 top = zdb_zone_label_match(db, &name, zone_label_stack);
    s32 sp = top;

    /* Got a stack of zone labels with and without zone cuts */
    /* Search the label on the zone files */

    while(sp >= 0)
    {
        zdb_zone_label* zone_label = zone_label_stack[sp];

        if(zone_label->zone != NULL)
        {
            zdb_zone_lock(zone_label->zone, ZDB_ZONE_MUTEX_SIMPLEREADER);
            /* Get the label, instead of the type in the label */
            zdb_rr_label* rr_label = zdb_rr_label_find_exact(zone_label->zone->apex, name.labels, name.size - sp);

            if(rr_label != NULL)
            {
                ya_result ret = 0;
                
                zdb_packed_ttlrdata* rrset;
                
                rrset = zdb_record_find(&rr_label->resource_record_set, TYPE_A);
                while(rrset != NULL)
                {
                    host_address_append_ipv4(target_list, ZDB_PACKEDRECORD_PTR_RDATAPTR(rrset), NU16(DNS_DEFAULT_PORT));
                    ++ret;
                    rrset = rrset->next;
                }
                
                rrset = zdb_record_find(&rr_label->resource_record_set, TYPE_AAAA);
                while(rrset != NULL)
                {
                    host_address_append_ipv6(target_list, ZDB_PACKEDRECORD_PTR_RDATAPTR(rrset), NU16(DNS_DEFAULT_PORT));
                    ++ret;
                    rrset = rrset->next;
                }
                
                zdb_zone_unlock(zone_label->zone, ZDB_ZONE_MUTEX_SIMPLEREADER);

                zdb_unlock(db, ZDB_MUTEX_READER); // zdb_query_ip_records (success)
                
                return ret;
            }
            
            zdb_zone_unlock(zone_label->zone, ZDB_ZONE_MUTEX_SIMPLEREADER);
        }

        sp--;
    }



#ifdef HAS_DYNAMIC_PROVISIONING
    zdb_unlock(db, ZDB_MUTEX_READER); // zdb_query_ip_records (failure)
#endif
    
    return ZDB_ERROR_KEY_NOTFOUND;
}

zdb_rr_label *
zdb_get_rr_label(zdb* db, const u8* name_, zdb_zone **zonep, u8 owner)
{
    yassert((owner == ZDB_ZONE_MUTEX_NOBODY) || (zonep != NULL));
    
    zdb_rr_label* rr_label = NULL;
    s32 top;
    s32 sp;
    dnsname_vector name;
    dnsname_to_dnsname_vector(name_, &name);

    /* Find closest matching label
     * Should return a stack of zones
     */

    zdb_lock(db, ZDB_MUTEX_READER); // zdb_query_ip_records
    
    zdb_zone_label_pointer_array zone_label_stack;

    top = zdb_zone_label_match(db, &name, zone_label_stack);
    sp = top;

    /* Got a stack of zone labels with and without zone cuts */
    /* Search the label on the zone files */

    while(sp >= 0)
    {
        zdb_zone_label *zone_label = zone_label_stack[sp];
        zdb_zone *zone;
        if((zone = zone_label->zone) != NULL)
        {
            zdb_zone_acquire(zone);
            if(owner != ZDB_ZONE_MUTEX_NOBODY);
            {
                zdb_zone_lock(zone, owner);
            }
            /* Get the label, instead of the type in the label */
            rr_label = zdb_rr_label_find_exact(zone->apex, name.labels, name.size - sp);

            if(rr_label != NULL)
            {
                if(zonep != NULL)
                {
                    *zonep = zone;
                }
                else
                {
                    if(owner != ZDB_ZONE_MUTEX_NOBODY);
                    {
                        zdb_zone_unlock(zone, owner);
                    }
                    zdb_zone_release(zone);
                }
                
                break;
            }
            
            if(owner != ZDB_ZONE_MUTEX_NOBODY);
            {
                zdb_zone_unlock(zone, owner);
            }
            zdb_zone_release(zone);
        }

        sp--;
    }
    
    zdb_unlock(db, ZDB_MUTEX_READER); // zdb_query_ip_records
    
    return rr_label;
}

const zdb_packed_ttlrdata*
zdb_get_rr_set(zdb* db, const u8* name, u16 rtype, zdb_zone **zonep, u8 owner)
{
    zdb_rr_label *rr_label;
    if((rr_label = zdb_get_rr_label(db, name, zonep, owner)) != NULL)
    {
        zdb_packed_ttlrdata *ds_record = zdb_record_find(&rr_label->resource_record_set, rtype);
        
        if(ds_record == NULL)
        {
            if(owner != ZDB_ZONE_MUTEX_NOBODY);
            {
                zdb_zone_unlock(*zonep, owner);
            }
            zdb_zone_release(*zonep);
        }
        
        return ds_record;
    }
    
    return NULL;
}

#if OBSOLETE

/** @brief Adds an entry in a zone of the database
 *
 *  Adds an entry in a zone of the database
 *
 *  @param[in]  db the database
 *  @param[in]  origin_ the zone where to add the record
 *  @param[in]  name_ the full name of the record (dns form)
 *  @param[in]  type the type of the record
 *  @param[in]  ttl the ttl of the record
 *  @param[in]  rdata_size the size of the rdata of the record
 *  @param[in]  rdata a pointer to the rdata of the record
 *
 *  @return SUCCESS in case of success.
 */

ya_result
zdb_add(zdb* db, u8* origin_, u8* name_, u16 type, u32 ttl, u16 rdata_size, void* rdata) /* 4 match, add 1 */ // mutex checked
{
    yassert(db != NULL && origin_ != NULL && name_ != NULL && (rdata_size == 0 || rdata != NULL));

    dnsname_vector origin;
    DEBUG_RESET_dnsname(origin);

    dnsname_to_dnsname_vector(origin_, &origin);

    dnsname_vector name;
    DEBUG_RESET_dnsname(name);

    dnsname_to_dnsname_vector(name_, &name);

    zdb_zone* zone = zdb_zone_find(db, &origin); // IN OBSOLETE

    if(zone != NULL)
    {
        zdb_packed_ttlrdata* ttlrdata;

        ZDB_RECORD_ZALLOC(ttlrdata, ttl, rdata_size, rdata); // rdata can be null if rdata_size is 0, no problem here
        /* The record will be cloned in this call */
        zdb_zone_record_add(zone, name.labels, (name.size - origin.size) - 1, type, ttlrdata);

        /**
         * @todo: If the zone is NSEC/NSEC3 :
         *
         *  NOTE: Remember that all the intermediary labels have to be added
         *  if they do not exist yet.
         *
         *  (order an) udpate of the NSECx record
         *  (order an) update of both signatures (NSECx + record type)
         *
         */

        /**
         * @todo: If the record added is NS, update glue record status of name in the rdata
         */

        return SUCCESS;
    }

    return ZDB_READER_ZONENOTLOADED;
}

/** @brief Deletes an entry from a zone in the database
 *
 *  Matches and deletes an entry from a zone in the database
 *
 *  @param[in]  db the database
 *  @param[in]  origin_ the zone from which to remove the record
 *  @param[in]  name_ the name of the record
 *  @param[in]  type the type of the record
 *  @param[in]  ttl the ttl of the record
 *  @param[in]  rdata_size the size of the rdata of the record
 *  @param[in]  rdata a pointer to the rdata of the record
 *
 *  @return SUCCESS in case of success.
 */

ya_result
zdb_delete(zdb* db, u8* origin_, u8* name_, u16 type, u32 ttl, u16 rdata_size, void* rdata) /* 5 match, delete 1 */ // mutex checked
{
    yassert(db != NULL && origin_ != NULL && name_ != NULL && (rdata_size == 0 || rdata != NULL));

    dnsname_vector origin;
    DEBUG_RESET_dnsname(origin);

    dnsname_to_dnsname_vector(origin_, &origin);

    dnsname_vector name;
    DEBUG_RESET_dnsname(name);

    dnsname_to_dnsname_vector(name_, &name);

    zdb_zone* zone = zdb_zone_find(db, &origin); // IN OBSOLETE

    if(zone != NULL)
    {
        zdb_ttlrdata ttlrdata;

        ZDB_RECORD_TTLRDATA_SET(ttlrdata, ttl, rdata_size, rdata);

        /* I do not really require a record set here ... */
        return zdb_rr_label_delete_record_exact(zone, name.labels, (name.size - origin.size) - 1, type, &ttlrdata);

        /**
         * @todo: If the zone is NSEC/NSEC3 :
         *
         *  NOTE: Remember that all the intermediary labels have to be deleted
         *  if they are not relevant anymore
         *
         *  (order an) udpate of the NSECx record
         *  (order an) update of both signatures (NSECx + record type)
         *
         */

    }

    return ZDB_READER_ZONENOTLOADED;
}

#endif

#if NOTUSEDATALL

/**
 * Looks for a zone and tells if zone is marked as invalid.
 * The zone can only be invalid if it exists.
 * 
 * @param db
 * @param origin
 * @return 
 */

bool
zdb_is_zone_invalid(zdb *db, const u8 *origin)
{
    zdb_zone_label* label = zdb_zone_label_find_from_dnsname(db, origin); // in an unused function
    bool invalid = FALSE;
    
    if(label != NULL)
    {
        invalid = zdb_zone_isinvalid(label->zone);
    }
    
    return invalid;
}

#endif

/** @brief Destroys the database
 *
 *  Destroys a database. (Empties it)
 *
 *  @param[in]  db the database to destroy
 *
 */

void
zdb_destroy(zdb* db) // mutex checked
{
    zdb_lock(db, ZDB_MUTEX_WRITER); // zdb_destroy

    alarm_close(db->alarm_handle);
    db->alarm_handle = ALARM_HANDLE_INVALID;
    
    zdb_zone_label_destroy(&db->root);  /* native order */
    
    zdb_unlock(db, ZDB_MUTEX_WRITER); // zdb_destroy
    
    group_mutex_destroy(&db->mutex);
}

static void
zdb_signature_check_one(const char* name, int should, int is)
{
    if(is != should)
    {
        printf("critical: zdb: '%s' should be of size %i but is of size %i\n", name, is, should);
        fflush(stdout);
        abort();
    }
}

void zdb_signature_check(int so_zdb, int so_zdb_zone, int so_zdb_zone_label, int so_zdb_rr_label, int so_mutex_t)
{
    DNSCORE_API_CHECK();
    
    zdb_signature_check_one("zdb", sizeof(zdb), so_zdb);
    zdb_signature_check_one("zdb_zone", sizeof(zdb_zone), so_zdb_zone);
    zdb_signature_check_one("zdb_zone_label", sizeof(zdb_zone_label), so_zdb_zone_label);
    zdb_signature_check_one("zdb_rr_label", sizeof(zdb_rr_label), so_zdb_rr_label);
    zdb_signature_check_one("mutex_t", sizeof(mutex_t), so_mutex_t);
}

#ifdef DEBUG

/** @brief DEBUG: Prints the content of the database.
 *
 *  DEBUG: Prints the content of the database.
 *
 *  @param[in]  db the database to print
 *
 */

void
zdb_print(zdb* db, output_stream *os) // mutex checked
{
    osformatln(os, "zdb@%p\n", (void*)db);

    if(db != NULL)
    {
        zdb_lock(db, ZDB_MUTEX_READER); // for print db

        osformatln(os, "zdb@%p class=%{dnsclass}\n", (void*)db, &db->zclass);

        zdb_zone_label_print_indented(db->root, os, 1); /* native order */

        zdb_unlock(db, ZDB_MUTEX_READER); // for print db
    }
}


#endif

/** @} */
