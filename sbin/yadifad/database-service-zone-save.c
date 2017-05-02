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
/** @defgroup database Routines for database manipulations
 *  @ingroup yadifad
 *  @brief database functions
 *
 *  Implementation of routines for the database
 *   - add zone file(s)
 *   - clear zone file(s)
 *   - print zone files(s)
 *   - load db
 *   - unload db
 *   - lookup database result of a message
 *
 * @{
 */
/*------------------------------------------------------------------------------
 *
 * USE INCLUDES */

#include "server-config.h"
#include "config.h"

#include <dnscore/logger.h>
#include <dnsdb/zdb_zone.h>
#include <dnsdb/zdb_zone_label.h>
#include <dnsdb/zdb_zone_write.h>
#include <dnsdb/zdb-lock.h>
#define ZDB_JOURNAL_CODE 1
#include <dnsdb/journal.h>

#include "zone.h"
#include "server.h"
#include "database-service.h"

#define MODULE_MSG_HANDLE g_server_logger

/**
 * Saves a zone in the current thread using the provided locks (0 meaning: do not try to lock)
 * Not locking puts the responsibility of the lock to the caller as having this code running
 * without any lock whatsoever on the descriptor/zone will give undefined results, a.k.a : crash.
 * 
 * @param zone_desc
 * @param desclockowner
 * @param zonelockowner
 * @return 
 */

ya_result
database_service_zone_save_ex(zone_desc_s *zone_desc, u8 desclockowner, u8 zonelockowner, bool save_unmodified)
{
    // not implemented yet
    log_debug("zone save: %{dnsname}@%p#%i", zone_desc->origin, zone_desc, zone_desc->rc);
    
    // for all modified zones descriptors
    //   if the file source is defined AND the source is not a template
    //     if the zone is valid
    //       save the text representation of the zone to the disk

    //bool must_be_on = ZONE_STATUS_READONLY|ZONE_STATUS_MODIFIED;
    
    bool must_be_off = ZONE_STATUS_TEMPLATE_SOURCE_FILE | ZONE_STATUS_STARTING_UP |
                       ZONE_STATUS_LOADING | ZONE_STATUS_MOUNTING | ZONE_STATUS_UNMOUNTING |
                       ZONE_STATUS_DROPPING | ZONE_STATUS_SAVING_ZONE_FILE |
                       ZONE_STATUS_SAVING_AXFR_FILE | ZONE_STATUS_SIGNATURES_UPDATING |
                       ZONE_STATUS_DYNAMIC_UPDATING | ZONE_STATUS_DOWNLOADING_XFR_FILE |
                       ZONE_STATUS_UNREGISTERING;
    
    if(desclockowner != 0)
    {
        zone_lock(zone_desc, desclockowner);
    }
    
    if(zone_desc->status_flags & ZONE_STATUS_MUST_CLEAR_JOURNAL)
    {
        zone_desc->status_flags |= ZONE_STATUS_MODIFIED;
    }
    
    if(!save_unmodified && ((zone_desc->status_flags & ZONE_STATUS_MODIFIED) == 0)) // a "journal" should set modified, or should it be tested here ?
    {
        log_debug("zone save: %{dnsname} hasn't been modified", zone_desc->origin);
        zone_desc->status_flags &= ~(ZONE_STATUS_SAVETO_ZONE_FILE|ZONE_STATUS_SAVING_ZONE_FILE|ZONE_STATUS_PROCESSING);
        
        if(desclockowner != 0)
        {
            zone_unlock(zone_desc, desclockowner);
        }
        zone_release(zone_desc);
        return SUCCESS;
    }
    
    if(zone_desc->file_name == NULL)
    {
        log_debug("zone save: %{dnsname} has no source file set", zone_desc->origin);
        zone_desc->status_flags &= ~(ZONE_STATUS_SAVETO_ZONE_FILE|ZONE_STATUS_SAVING_ZONE_FILE|ZONE_STATUS_PROCESSING);
        if(desclockowner != 0)
        {
            zone_unlock(zone_desc, desclockowner);
        }
        zone_release(zone_desc);
        return ERROR;
    }
    
    if((zone_desc->status_flags & ZONE_STATUS_TEMPLATE_SOURCE_FILE) != 0)
    {
        log_debug("zone save: %{dnsname} source is a template", zone_desc->origin);
        zone_desc->status_flags &= ~(ZONE_STATUS_SAVETO_ZONE_FILE|ZONE_STATUS_SAVING_ZONE_FILE|ZONE_STATUS_PROCESSING);
        if(desclockowner != 0)
        {
            zone_unlock(zone_desc, desclockowner);
        }
        zone_release(zone_desc);
        return ERROR;
    }
    
    if((zone_desc->status_flags & must_be_off) != 0)
    {
        log_debug("zone save: %{dnsname} can't be saved at this time (%08x & %08x = %08x)", zone_desc->origin, zone_desc->status_flags, must_be_off, zone_desc->status_flags & must_be_off);
        zone_desc->status_flags &= ~(ZONE_STATUS_SAVETO_ZONE_FILE|ZONE_STATUS_SAVING_ZONE_FILE|ZONE_STATUS_PROCESSING);
        if(desclockowner != 0)
        {
            zone_unlock(zone_desc, desclockowner);
        }
        zone_release(zone_desc);
        return ERROR;
    }
    
    zone_desc->status_flags |= ZONE_STATUS_SAVING_ZONE_FILE;
    
    zdb *db = g_config->database;

    zdb_zone *zone;
    
    if(zonelockowner != 0)
    {
        zone = zdb_acquire_zone_read_lock_from_fqdn(db, zone_desc->origin, zonelockowner); // ACQUIRES & LOCKS
    }
    else
    {
        zone = zdb_acquire_zone_read_from_fqdn(db, zone_desc->origin); // ACQUIRES
    }
    
    ya_result ret = ERROR;
    
    if(zone != NULL)
    {
        if(zdb_zone_isvalid(zone))
        {
            char file_name[PATH_MAX];    
            snformat(file_name, sizeof(file_name), "%s/%s", g_config->data_path, zone_desc->file_name);
    
            log_debug("zone save: %{dnsname} saving zone to file '%s'", zone_desc->origin, file_name);
            
            ret = zdb_zone_write_text_file(zone, file_name, FALSE);
            
            if(ISOK(ret))
            {
                zdb_zone_getserial(zone, &zone_desc->stored_serial);
                zone_desc->status_flags &= ~ZONE_STATUS_MODIFIED;
                
                bool clear_journal = zone_desc->status_flags & ZONE_STATUS_MUST_CLEAR_JOURNAL;
                
                if(clear_journal)
                {
                    journal_truncate(zone_desc->origin);
                    zone_desc->status_flags &= ~ZONE_STATUS_MUST_CLEAR_JOURNAL;
                }
                
                log_debug("zone save: %{dnsname} saved zone to file '%s'", zone_desc->origin, file_name);
            }
            else
            {
                log_err("zone save: %{dnsname} failed to save as '%s': %r", zone_desc->origin, file_name, ret);
            }
        }
        else
        {
            log_err("zone save: %{dnsname} cannot be saved because its current instance in the database is marked as invalid", zone_desc->origin);
        }
        
        if(zonelockowner != 0)
        {
            zdb_zone_release_unlock(zone, zonelockowner);
        }
        else
        {
            zdb_zone_release(zone);
        }
    }
    
    // zdb_unlock(db, ZDB_MUTEX_READER);
    
    zone_desc->status_flags &= ~(ZONE_STATUS_SAVETO_ZONE_FILE|ZONE_STATUS_SAVING_ZONE_FILE|ZONE_STATUS_PROCESSING);
    
    if(desclockowner != 0)
    {
        zone_unlock(zone_desc, desclockowner);
    }
    
    zone_release(zone_desc);
    
    return ret;
}

static void*
database_service_zone_save_thread(void *params)
{
    zone_desc_s *zone_desc = (zone_desc_s*)params;
    database_service_zone_save_ex(zone_desc, ZONE_LOCK_SAVE, ZDB_ZONE_MUTEX_SIMPLEREADER, FALSE);
    return NULL;
}

/**
 * 
 * Triggers the standard background save of a zone
 * 
 * @param zone_desc
 * @return 
 */

ya_result
database_service_zone_save(zone_desc_s *zone_desc)
{
    if(zone_desc == NULL)
    {
        log_err("database_service_zone_save(NULL)");
        return ERROR;
    }
    
    log_debug1("database_service_zone_save(%{dnsname}@%p=%i)", zone_desc->origin, zone_desc, zone_desc->rc);
    
    log_debug1("database_service_zone_save: locking zone '%{dnsname}' for saving", zone_desc->origin);
    
    if(FAIL(zone_lock(zone_desc, ZONE_LOCK_SAVE)))
    {
        log_err("database_service_zone_save: failed to lock zone settings for '%{dnsname}'", zone_desc->origin);
        return ERROR;
    }
    
    const u8 *origin = zone_desc->origin;
    
    log_info("zone save: %{dnsname}", origin);
                    
    /*
     * Invalidate the zone
     * Empty the current zone if any
     */

    /*
     * If the zone descriptor (config) exists and it can be locked by the saveer ...
     */
    
    // locks the descriptor with the saveer identity
    
    if(zone_desc->status_flags & (ZONE_STATUS_SAVETO_ZONE_FILE|ZONE_STATUS_SAVING_ZONE_FILE))
    {
        // already saving
        
        zone_desc_log(MODULE_MSG_HANDLE, MSG_DEBUG1, zone_desc, "database_service_zone_save");
        
        log_err("database_service_zone_save: '%{dnsname}' already saving", origin);
        
        zone_unlock(zone_desc, ZONE_LOCK_SAVE);
                        
        return ERROR;
    }
    
    zone_desc->status_flags |= ZONE_STATUS_SAVETO_ZONE_FILE;

    zone_acquire(zone_desc);
    database_service_zone_save_queue_thread(database_service_zone_save_thread, zone_desc, NULL, "database_zone_save_thread");
    
    log_debug1("database_service_zone_save: unlocking zone '%{dnsname}' for saving", origin);
    
    zone_unlock(zone_desc, ZONE_LOCK_SAVE);
    
    return SUCCESS;
}

/**
 * @}
 */
