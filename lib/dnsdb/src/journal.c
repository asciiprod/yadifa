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
/** @defgroup 
 *  @ingroup 
 *  @brief 
 *
 *  
 *
 * @{
 *
 *----------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
 * GLOBAL VARIABLES */

/*------------------------------------------------------------------------------
 * STATIC PROTOTYPES */

/*------------------------------------------------------------------------------
 * FUNCTIONS */

/** @brief Function ...
 *
 *  ...
 *
 *  @param ...
 *
 *  @retval OK
 *  @retval NOK
 */

#define ZDB_JOURNAL_CODE 1

#include "dnsdb/dnsdb-config.h"
#include <dnscore/dns_resource_record.h>
#include <dnscore/fdtools.h>

#include "dnsdb/journal.h"
#include "dnsdb/journal_ix.h"
#include "dnsdb/journal-cjf.h"
#include "dnsdb/zdb_zone.h"
#include "dnsdb/zdb_utils.h"
#include "dnsdb/xfr_copy.h"
#include "dnsdb/zdb-zone-path-provider.h"

extern logger_handle* g_database_logger;
#define MODULE_MSG_HANDLE g_database_logger

//#define journal_default_open journal_ix_open
#define journal_default_open journal_cjf_open

static mutex_t journal_mutex = MUTEX_INITIALIZER;
static journal *journal_mru_first = NULL;
static journal *journal_mru_last = NULL;
static u32     journal_mru_count = 0;
static u32     journal_mru_size = 0;
static u32     journal_count = 0;
static bool    journal_initialised = FALSE;

static bool    journal_mru_remove(journal *jh);

/**
 * 
 * Removes a journal from the MRU (INTERNAL)
 * 
 * A journal removed from the MRU is to be closed
 * 
 * @param jh
 * @return TRUE if the journal was removed, FALSE if it was not in the MRU
 */

static bool
journal_mru_remove(journal *jh)
{
#ifdef DEBUG
    u8 fqdn[MAX_DOMAIN_LENGTH];
    fqdn[0] = 0;
    jh->vtbl->get_domain(jh, fqdn);
    log_debug("journal: MRU: removing journal for %{dnsname}@%p", fqdn, jh);
#endif
    
    if(jh->mru)
    {
        if(jh->prev != NULL)
        {
            jh->prev->next = jh->next;
        }
        else
        {
            // == NULL
            journal_mru_first = (journal*)jh->next;
        }

        if(jh->next != NULL)
        {
            jh->next->prev = jh->prev;
        }
        else
        {
            // == NULL
            journal_mru_last = (journal*)jh->prev;
        }
        
        jh->prev = NULL;
        jh->next = NULL;
        jh->mru = FALSE;
  
        --journal_mru_count;
        
        return TRUE;
    }
#ifdef DEBUG
    else
    {   
        if((jh->next != NULL) || (jh->prev != NULL))
        {
            log_err("journal: MRU: %{dnsname}@%p not in MRU but kept link(s) to it!", fqdn, jh);
            logger_flush(); // then abort()
            abort();
        }
        
        log_debug("journal: MRU: %{dnsname}@%p not in MRU", fqdn, jh);
    }
#endif
    
    // was not in the MRU
    
    return FALSE;
}

/**
 * 
 * Puts the given journal at the head of the MRU list.
 * Clears out the least recently used journal if needed and possible.
 * 
 * @param jh
 */

static void
journal_mru_add(journal *jh)
{
    // if jh is first already, stop
    
#ifdef DEBUG
    u8 fqdn[MAX_DOMAIN_LENGTH];
    fqdn[0] = 0;
    jh->vtbl->get_domain(jh, fqdn);
#endif
    
    if(journal_mru_first == jh)
    {
#ifdef DEBUG
        log_debug("journal: MRU: adding %{dnsname}@%p : already first", fqdn, jh);
#endif
        return;
    }
    
    // detach from the MRU
    
    bool was_in_mru = journal_mru_remove(jh);
    
    // insert at the head of the MRU
    
    jh->prev = NULL;
    jh->next = journal_mru_first;
    
    // if there is a first, link it to the new first
    
    if(journal_mru_count != 0)
    {
        journal_mru_first->prev = jh;
    }
    else
    {
        journal_mru_last = jh;
    }
    journal_mru_first = jh;
    
    // mark as being in the MRU
    
    jh->mru = TRUE;
            
    //
    
    ++journal_mru_count;

    if(!was_in_mru)
    {
        // new reference, so increase the count
        
        // slots available ?
        
        if(journal_mru_count < journal_mru_size)
        {
            // yes
           
#ifdef DEBUG
            log_debug("journal: MRU: %{dnsname}@%p : added at the head (count = %u/%u)",
                      fqdn, jh,
                      journal_mru_count, journal_mru_size);
#endif
        }
        else
        {
#ifdef DEBUG
            u8 lru_fqdn[MAX_DOMAIN_LENGTH];
            lru_fqdn[0] = 0;
            journal_mru_last->vtbl->get_domain(journal_mru_last, lru_fqdn);
    
            log_debug("journal: MRU: %{dnsname}@%p : added at the head (count = %u/%u), removing %{dnsname}@%p",
                      fqdn, jh,
                      journal_mru_count, journal_mru_size,
                      lru_fqdn, journal_mru_last);
#endif
            // no, remove the last one of the MRU
            
            journal_mru_remove(journal_mru_last);
        }
    }
#ifdef DEBUG
    else
    {
        log_debug("journal: MRU: %{dnsname}@%p moved at the head of the MRU (count = %u/%u)", fqdn, jh, journal_mru_count, journal_mru_size);
    }
#endif
}

ya_result
journal_init(u32 mru_size)
{
    log_debug("journal: initialising with an MRU of %i slots", mru_size);
    
    if(!journal_initialised)
    {
        // initialises journal open/close access mutex (avoid creation/destruction races)
        // will be responsible for the journal file-descriptor resources allocation (closes least recently used journal when no more FDs are available)

        mutex_init_recursive(&journal_mutex);

        if(mru_size == 0)
        {
            mru_size = ZDB_JOURNAL_FD_DEFAULT;
        }

        journal_mru_size = MAX(MIN(mru_size, ZDB_JOURNAL_FD_MAX), ZDB_JOURNAL_FD_MIN);
        
        log_debug("journal: intialised with an MRU of %i slots", journal_mru_size);

        journal_initialised = TRUE;
    }
    else
    {
        log_debug("journal: already initialised with an MRU of %i slots", journal_mru_size);
        
        if(mru_size == 0)
        {
            mru_size = ZDB_JOURNAL_FD_DEFAULT;
        }
        else
        {
            mru_size = MAX(MIN(mru_size, ZDB_JOURNAL_FD_MAX), ZDB_JOURNAL_FD_MIN);
        }
        
        if(journal_mru_size != mru_size)
        {
            log_err("journal: the journal MRU size was already set to %u but %u is now requested", journal_mru_size, mru_size);
            return ERROR;
        }
    }
    
    

    return SUCCESS;
}

void
journal_finalise()
{
    log_debug("journal: finalising");
            
    if(journal_initialised)
    {
        mutex_lock(&journal_mutex);
        
        if(journal_mru_count > 0)
        {
            log_warn("journal: there are still %u journals in the MRU", journal_mru_count);
        }
        
        int coundown = 30;  // 3 seconds (30 * 0.1s)
        
        while((journal_mru_count > 0) && (--coundown > 0))
        {
            mutex_unlock(&journal_mutex);
            usleep(100000); // 0.1s
            mutex_lock(&journal_mutex);
        }
        
        mutex_unlock(&journal_mutex);
        
        if(journal_mru_count == 0)
        {
            mutex_destroy(&journal_mutex);
            journal_initialised = FALSE;
            
            log_debug("journal: finalised");
        }
        else
        {
            log_err("journal: giving up on waiting for MRU");
#if DEBUG
            logger_flush();
#endif
        }
    }
}

/**
 * 
 * Opens the journal for a zone
 * 
 * Binds the journal to the zone (?)
 * 
 * Increments the reference count to the journal
 * 
 * @param jhp
 * @param zone
 * @param workingdir
 * @param create
 * @return 
 */

ya_result
journal_open(journal **jhp, zdb_zone *zone, bool create)
{
    journal *jh = NULL;
    ya_result return_value = SUCCESS;
    char data_path[PATH_MAX];
    
    yassert((jhp != NULL) && (zone != NULL));
    
    // DO NOT zdb_zone_acquire(zone);
    
    log_debug("journal: opening journal for zone %{dnsname}@%p", zone->origin, zone);
    
    mutex_lock(&journal_mutex);

    // get the journal
    
    jh = zone->journal;
    
    if(jh == NULL)
    {
        // The zone has no journal linked yet
        
#ifdef DEBUG
        log_debug("journal_open(%p,%p,%i) getting journal path (%{dnsname})", jhp, zone, create, zone->origin);
#endif
        
        // it does not exist, so create a new one (using the default format)
        
        /// @todo 20141128 edf -- get rid of the hashing function here : this has to be pushed to the journal file format
        ///                       but first, the journal must be able to know where the zone file is (so it could put the journal next to it)
        
        u32 path_flags = ZDB_ZONE_PATH_PROVIDER_ZONE_PATH;
        if(create)
        {
            path_flags |= ZDB_ZONE_PATH_PROVIDER_MKDIR;
        }
        
        if(FAIL(return_value = zdb_zone_path_get_provider()(zone->origin, data_path, sizeof(data_path), path_flags)))
        {
            mutex_unlock(&journal_mutex);
            *jhp = NULL;
            
            return return_value;
        }
        
#ifdef DEBUG
        log_debug("journal_open(%p,%p,%i) opening journal (%{dnsname}) in '%s'", jhp, zone, create, zone->origin, data_path);
#endif
        
        // open the journal
        
        if(FAIL(return_value = journal_default_open(&jh, zone->origin, data_path, create))) // followed by a link zone
        {
            mutex_unlock(&journal_mutex);
            *jhp = NULL;
            
            // DO NOT zdb_zone_release(zone);
            
            return return_value;
        }
        
        // if the journal was successfully opened, link it to the zone
        /// @note this link is weak, there is no reference count increase for it
        
        ++journal_count;
        
        journal_link_zone(jh, zone);
        
    }
#ifdef DEBUG
    else
    {
        log_debug("journal_open(%p,%p,%i) referencing journal (%{dnsname})", jhp, zone, create, zone->origin);
        
        // DO NOT zdb_zone_release(zone);
    }
#endif
    
    // from here jh is not NULL  
            
    mutex_unlock(&journal_mutex);
    *jhp = jh;
    
    return return_value;
}

/**
 * Closes the journal
 * 
 * @param jh
 */

void
journal_close(journal *jh)
{
    if(jh != NULL)
    {
        if(jh->zone != NULL)
        {
            log_debug("journal: closing journal %p for zone %{dnsname}@%p", jh, jh->zone->origin, jh->zone);
        }
        else
        {
            log_debug("journal: closing journal %p", jh);
        }
        
        mutex_lock(&journal_mutex);
        journal_mru_remove(jh);
        jh->vtbl->close(jh); // allowed close
        //jh->vtbl->destroy(jh);
        mutex_unlock(&journal_mutex);
    }
    else
    {
        //log_debug("journal: close called on a NULL journal");
    }
}

/**
 * 
 * Returns the last serial stored in a file.
 * 
 * Opens the journal file based on the fqdn.
 * Reads the serial.
 * Closes the journal.
 * 
 * @param origin
 * @param workingdir
 * @param serialp
 * @return 
 */

ya_result
journal_last_serial(const u8 *origin, const char *workingdir, u32 *serialp)
{
    journal *jh = NULL;
    ya_result return_value;
    char data_path[PATH_MAX];
    
    if(origin == NULL)
    {
        return ZDB_JOURNAL_WRONG_PARAMETERS;
    }
    
    if(FAIL(return_value = zdb_zone_path_get_provider()(origin, data_path, sizeof(data_path), ZDB_ZONE_PATH_PROVIDER_ZONE_PATH)))
    {
        return return_value;
    }
    
    workingdir = data_path;
    
    if(ISOK(return_value = journal_default_open(&jh, origin, workingdir, FALSE))) // open/close
    {
        bool close_it = (jh->zone == NULL);
        
        return_value = journal_get_last_serial(jh, serialp);
        
        if(close_it)
        {
            journal_close(jh);
        }
    }
    
    return return_value;
}

/**
 * 
 * Reduces the size of the journal to 0.
 * 
 * Opens the journal file based on the fqdn.
 * Truncates the journal
 * Closes the journal.
 * 
 * Opens the journal file based on the fqdn.
 * Reads the serial.
 * Closes the journal.
 * 
 * @param origin
 * @param workingdir
 * @return 
 */

ya_result
journal_truncate(const u8 *origin)
{
    journal *jh = NULL;
    ya_result return_value;
    char data_path[PATH_MAX];
    
    if(origin == NULL)
    {
        return ZDB_JOURNAL_WRONG_PARAMETERS;
    }
    
    if(FAIL(return_value = zdb_zone_path_get_provider()(origin, data_path, sizeof(data_path), ZDB_ZONE_PATH_PROVIDER_ZONE_PATH)))
    {
        return return_value;
    }
    
    if(ISOK(return_value = journal_default_open(&jh, origin, data_path, TRUE))) // no link !
    {
        return_value = journal_truncate_to_size(jh, 0);
    }
    
    return return_value;
}

/**
 * 
 * Returns the last SOA TTL + RDATA 
 * 
 * Opens the journal file based on the fqdn.
 * Reads the SOA.
 * Closes the journal.
 * 
 * @param origin
 * @param workingdir
 * @param serial
 * @param ttl
 * @param last_soa_rdata
 * @param last_soa_rdata_size
 * @return 
 */

ya_result
journal_last_soa(const u8 *origin, const char *workingdir, u32 *serial, u32 *ttl, u8 *last_soa_rdata, u16 *last_soa_rdata_size)
{
    journal *jh = NULL;
    ya_result return_value;
    char data_path[PATH_MAX];
    
    /* check preconditions */

    if((origin == NULL)     ||  /* mandatory */
       (workingdir == NULL)    /* mandatory */
      )
    {
        return ZDB_JOURNAL_WRONG_PARAMETERS;
    }
    
    /* translate path */
    
    if(FAIL(return_value = zdb_zone_path_get_provider()(origin, data_path, sizeof(data_path), ZDB_ZONE_PATH_PROVIDER_ZONE_PATH)))
    {
        return return_value;
    }
    
    workingdir = data_path;
    
    /* open a new instance of the journal */
    
    if(ISOK(return_value = journal_default_open(&jh, origin, workingdir, FALSE))) // no link ?
    {
        input_stream is;
        u32 first_serial = 0;
        u32 last_serial = 0;
        u16 last_soa_rdata_size_store;
        
        bool close_it = (jh->zone == NULL);

        dns_resource_record rr;

        if(last_soa_rdata_size == NULL)
        {
            last_soa_rdata_size = &last_soa_rdata_size_store;
        }
        
        journal_get_first_serial(jh, &first_serial);
        journal_get_last_serial(jh, &last_serial);
        
        if(first_serial != last_serial)
        {
            dns_resource_record_init(&rr);
            
            if(ISOK(return_value = journal_get_ixfr_stream_at_serial(jh, first_serial, &is, &rr)))
            {
                journal_mru_add(jh);
                
                if(last_soa_rdata_size == NULL)
                {
                    *last_soa_rdata_size = rr.rdata_size;
                    
                    if((last_soa_rdata != NULL) && (*last_soa_rdata_size >= rr.rdata_size))
                    {
                        MEMCOPY(last_soa_rdata, rr.rdata, rr.rdata_size);
                    }
                }

                if(serial != NULL)
                {
                    if(rr.rdata_size > 0)
                    {
                        return_value = rr_soa_get_serial(rr.rdata, rr.rdata_size, serial);
                    }
                    else
                    {
                        log_err("jnl: %{dnsname}: empty last SOA in journal [%u;%u]", origin, first_serial, last_serial);
                    }
                }

                if(ttl != NULL)
                {
                    *ttl = htonl(rr.tctr.ttl);
                }
                
                input_stream_close(&is);
            }
            
            dns_resource_record_clear(&rr);
        }
        
        if(close_it)
        {
            journal_close(jh);
        }
    }
    
    return return_value;
}

/**
 * 
 * Prints the status of the journal (mostly the ones in the MRU) to the logger.
 * 
 */

void
journal_log_status()
{
    mutex_lock(&journal_mutex);

    log_debug("journal: instances: %u, mru: %u/%u", journal_count, journal_mru_count, journal_mru_size);
    
    journal *jh = journal_mru_first;
    
    while(jh != NULL)
    {
        jh->vtbl->log_dump(jh);
        jh = (journal*)jh->next;
    }
    
    mutex_unlock(&journal_mutex);
}

/** @} */
