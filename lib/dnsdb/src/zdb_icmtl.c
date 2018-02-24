/*------------------------------------------------------------------------------
*
* Copyright (c) 2011-2018, EURid vzw. All rights reserved.
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
 *  @ingroup dnsdb
 *  @brief
 *
 * ICMTL is actually INCREMENTAL.
 *
 *
 * @{
 */
/*------------------------------------------------------------------------------
 *
 * USE INCLUDES */
#include "dnsdb/dnsdb-config.h"
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

#include <dnscore/format.h>

#include <dnscore/input_stream.h>
#include <dnscore/output_stream.h>
#include <dnscore/file_output_stream.h>
#include <dnscore/file_input_stream.h>
#include <dnscore/buffer_input_stream.h>
#include <dnscore/buffer_output_stream.h>
#include <dnscore/bytearray_input_stream.h>
#include <dnscore/bytearray_output_stream.h>
#include <dnscore/concat_input_stream.h>
#include <dnscore/clone_input_output_stream.h>
#include <dnscore/ptr_set.h>
#include <dnscore/dns_resource_record.h>
#include <dnscore/serial.h>

#include "dnsdb/zdb_icmtl.h"
#include "dnsdb/zdb_zone.h"
#include "dnsdb/zdb-zone-journal.h"
#include "dnsdb/nsec3.h"
#include "dnsdb/nsec.h"
#include "dnsdb/rrsig.h"
#include "dnsdb/dnssec.h"
#include "dnsdb/zdb_utils.h"

#include "dnsdb/nsec-chain-replay.h"
#include "dnsdb/nsec3-chain-replay.h"

#ifndef HAS_DYNUPDATE_DIFF_ENABLED
#error "HAS_DYNUPDATE_DIFF_ENABLED not defined"
#endif

extern logger_handle* g_database_logger;
#define MODULE_MSG_HANDLE g_database_logger

#define ZDB_ICMTL_REPLAY_GATHER 0
#define ZDB_ICMTL_REPLAY_COMMIT 1
#define ZDB_ICMTL_REPLAY_COMMIT_AND_STOP 3
#define ZDB_ICMTL_REPLAY_STOP 4
#define ZDB_ICMTL_REPLAY_SHUTDOWN_POLL_PERIOD 1000
#define ZDB_ICMTL_REPLAY_BUFFER_SIZE 4096

#define ICMTL_DUMP_JOURNAL_RECORDS 0

static ya_result
zdb_icmtl_replay_commit_label_forall_nsec3_del_cb(zdb_rr_label *rr_label, const u8 *rr_label_fqdn, void *data)
{
    (void)rr_label;
    chain_replay *nsec3replayp = (chain_replay*)data;
    ya_result ret = nsec3replayp->vtbl->record_del(nsec3replayp, rr_label_fqdn, TYPE_NONE, NULL);
    return ret;
}

static ya_result
zdb_icmtl_replay_commit_label_forall_nsec3_add_cb(zdb_rr_label *rr_label, const u8 *rr_label_fqdn, void *data)
{
    (void)rr_label;
    chain_replay *nsec3replayp = (chain_replay*)data;
    ya_result ret = nsec3replayp->vtbl->record_add(nsec3replayp, rr_label_fqdn, TYPE_NONE, NULL);
    return ret;
}

ya_result
zdb_icmtl_replay_commit(zdb_zone *zone, input_stream *is, u32 *current_serialp)
{
    ya_result ret;
    /*
     * 0: DELETE, 1: ADD
     * The mode is switched every time an SOA is found.
     */

    yassert(zdb_zone_islocked(zone));
    
#if ZDB_HAS_NSEC3_SUPPORT
    bool has_nsec3 = zdb_zone_is_nsec3(zone);
#endif
#if ZDB_HAS_NSEC_SUPPORT
    bool has_nsec = zdb_zone_is_nsec(zone);
#endif
    
#if ZDB_HAS_NSEC3_SUPPORT && ZDB_HAS_NSEC_SUPPORT
    if(has_nsec3 && has_nsec)
    {
        log_err("journal: %{dnsname}: zone has both NSEC and NSEC3 status, which is not supported by YADIFA", zone->origin);
        return ERROR;
    }
#endif
    
    u8 mode = 1; // the first SOA will switch the mode to delete
    s32 changes = 0;
    
    zdb_ttlrdata ttlrdata;
    dns_resource_record rr;
    dns_resource_record_init(&rr);
    const u8 *fqdn = rr.name;
    dnslabel_vector labels;

    ttlrdata.next = NULL;
    


    /*
     * The plan for NSEC3 :
     * Store the fqdn + type class ttl rdata in collections
     * => the delete collection
     * => the add collection
     * Then there is the NSEC3 covered labels: keep a reference to them for later
     *
     * When a pass of SOA-/SOA+ has finished:
     * _ replace the NSEC3 in both collections (reading from delete)
     * _ delete NSEC3 to delete
     * _ add NSEC3 to add
     *
     * _ and finally update the NSEC3 for the labels kept above
     */

#if ZDB_HAS_NSEC3_SUPPORT
    chain_replay nsec3replay;
    nsec3_chain_replay_init(&nsec3replay, zone);
#endif
    
#if ZDB_HAS_NSEC_SUPPORT
    chain_replay nsecreplay;
    nsec_chain_replay_init(&nsecreplay, zone);
#endif
    
#if ZDB_HAS_NSEC3_SUPPORT
    ptr_set downed_fqdn = PTR_SET_DNSNAME_EMPTY;
#endif

    /* 
     * At this point : the next record, if it exists AND is not an SOA , has to be deleted
     * 
     */
    
    bool did_remove_soa = FALSE;

    // something has to be committed

    for(;;)
    {
        /*
         * read the full record
         * 
         * == 0 : no record (EOF)
         *  < 0 : failed
         */

        if((ret = dns_resource_record_read(&rr, is)) <= 0)
        {
            if(ISOK(ret))
            {
                log_info("journal: %{dnsname}: reached the end of the journal page", zone->origin);
            }
            else
            {
                log_err("journal: %{dnsname}: broken journal: %r", zone->origin, ret);
                logger_flush(); // broken journal (bad, keep me)
            }

            break;
        }
        
        ttlrdata.ttl = ntohl(rr.tctr.ttl);
        ttlrdata.rdata_pointer = rr.rdata;
        ttlrdata.rdata_size = rr.rdata_size;

        /*
         * Stop at the SOA
         */

        if(rr.tctr.qtype == TYPE_SOA)
        {
            mode ^= 1;

            if(mode == 0)
            {
                /* ADD */

#if ZDB_HAS_NSEC3_SUPPORT
                // NSEC3
                {
                    ret = nsec3replay.vtbl->execute(&nsec3replay);

                    if(FAIL(ret))
                    {
                        dns_resource_record_clear(&rr);
                        // DO NOT: input_stream_close(is);

                        nsec3replay.vtbl->finalise(&nsec3replay);
                        
#if ZDB_HAS_NSEC_SUPPORT
                        nsecreplay.vtbl->finalise(&nsecreplay);
#endif // ZDB_HAS_NSEC_SUPPORT
                        return ret;
                    }
                }
#endif
                
#if ZDB_HAS_NSEC_SUPPORT
                // NSEC
                ret = nsecreplay.vtbl->execute(&nsecreplay);
#endif //ZDB_HAS_NSEC_SUPPORT
            }
        }

        if(!did_remove_soa)
        {
            log_info("journal: %{dnsname}: removing obsolete SOA", zone->origin);

            if(FAIL(ret = zdb_record_delete(&zone->apex->resource_record_set, TYPE_SOA)))
            {
                /**
                * complain
                */

                log_err("journal: %{dnsname}: removing current SOA gave an error: %r", zone->origin, ret);

                /* That's VERY bad ... */

                changes = ret;

                break;
            }

            did_remove_soa = TRUE;
        }

        s32 top = dnsname_to_dnslabel_vector(fqdn, labels);

        if(mode == 0)
        {
            /*
             * "TO DEL" record
             */

#if ICMTL_DUMP_JOURNAL_RECORDS
            rdata_desc type_len_rdata = {rr.tctr.qtype, rr.rdata_size, rr.rdata };
            log_debug("journal: del %{dnsname} %{typerdatadesc}", fqdn, &type_len_rdata);
            logger_flush();
#endif
            bool added_in_chain = FALSE;

#if ZDB_HAS_NSEC3_SUPPORT
            
            // 0 : proceed
            // 1 : ignore
            // ? : error
            
            if((added_in_chain = (nsec3replay.vtbl->record_del(&nsec3replay, fqdn, rr.tctr.qtype, &ttlrdata) != 0)))
            {
                // add everything up until a non-empty terminal is found (the apex will thus be automatically avoided)
                // if the record is a delegation, add everything down too
                /*
                if((top > zone->origin_vector.size) &&
                        (rr.tctr.qtype != TYPE_NSEC3) &&
                        (   (rr.tctr.qtype != TYPE_RRSIG) ||
                            ((rr.tctr.qtype == TYPE_RRSIG) && (GET_U16_AT_P(ZDB_RECORD_PTR_RDATAPTR(&ttlrdata)) != TYPE_NSEC3))
                        )
                    )
                {
                    
                }
                */
                ++changes;
            }
            else
            {
                if(top > zone->origin_vector.size)
                {
                    const u8 *above_fqdn = fqdn;
                    for(int i = 1; i < top - zone->origin_vector.size; ++i)
                    {
                        zdb_rr_label *above = zdb_rr_label_find_exact(zone->apex, &labels[i], top - zone->origin_vector.size - 1 - i);
                        if(above != NULL)
                        {
                            if(btree_notempty(above->resource_record_set))
                            {
                                break;
                            }
                        }

                        above_fqdn += above_fqdn[0] + 1;
                        nsec3replay.vtbl->record_del(&nsec3replay, above_fqdn, TYPE_NONE, NULL);
                    }

                    zdb_rr_label *rr_label = zdb_rr_label_find_exact(zone->apex, labels, (top - zone->origin_vector.size) - 1);
                    if(rr_label != NULL)
                    {
                        zdb_rr_label_forall_children_of_fqdn(rr_label, fqdn, zdb_icmtl_replay_commit_label_forall_nsec3_del_cb, &nsec3replay);
                    }
                }
            }
#endif
#if ZDB_HAS_NSEC_SUPPORT
            
            // 0 : proceed
            // 1 : ignore
            // ? : error
            
            if(!added_in_chain && (added_in_chain = nsecreplay.vtbl->record_del(&nsecreplay, fqdn, rr.tctr.qtype, &ttlrdata) != 0))
            {
                ++changes;
            }
            //else
#endif
            if(!added_in_chain)
            switch(rr.tctr.qtype)
            {
                case TYPE_SOA:
                {
                    rdata_desc rdata = {TYPE_SOA, ttlrdata.rdata_size, ttlrdata.rdata_pointer};
                    log_info("journal: %{dnsname}: SOA: del %{dnsname} %{typerdatadesc}", zone->origin, fqdn, &rdata);

                    s32 m1 = (top - zone->origin_vector.size) - 1;

                    if(m1 == -1)
                    {
                        if(FAIL(ret = zdb_record_delete_exact(&zone->apex->resource_record_set, TYPE_SOA, &ttlrdata))) /* FB done, APEX : no delegation, source is the journal */
                        {
                            if(!did_remove_soa)
                            {
                                log_err("journal: %{dnsname}: SOA: %r", zone->origin, ret);
                            }
                        }
                    }
                    else
                    {
                        if(FAIL(ret = zdb_rr_label_delete_record_exact(zone, labels, (top - zone->origin_vector.size) - 1, rr.tctr.qtype, &ttlrdata))) // source is journal
                        {
                            if(!did_remove_soa)
                            {
                                log_err("journal: %{dnsname}: SOA: (2) %r", zone->origin, ret);
                            }
                        }
                    }
                    break;
                }

                default:
                {
#if ZDB_HAS_NSEC3_SUPPORT
                    // NSEC3
                    if(rr.tctr.qtype != TYPE_NSEC3)
                    {
                        if((rr.tctr.qtype != TYPE_RRSIG) && (rrsig_get_type_covered_from_rdata(rr.rdata, rr.rdata_size) != TYPE_NSEC3))
                        {
                            if(ptr_set_avl_find(&downed_fqdn, fqdn) == NULL)
                            {
                                ptr_set_avl_insert(&downed_fqdn, dnsname_dup(fqdn));
                            }
                        }
                    }
                    else
                    {
                        if(!NSEC3_RDATA_IS_OPTOUT(rr.rdata))
                        {
                            zone->_flags &= ~ZDB_ZONE_HAS_OPTOUT_COVERAGE;
                        }
                    }
#endif
                    if(FAIL(ret = zdb_rr_label_delete_record_exact(zone, labels, (top - zone->origin_vector.size) - 1, rr.tctr.qtype, &ttlrdata))) // source is journal
                    {
                        // signatures can be removed automatically by maintenance
                        
                        if((rr.tctr.qtype != TYPE_RRSIG) && (ret != ZDB_ERROR_KEY_NOTFOUND))
                        {
                            log_err("journal: %{dnsname}: del %{dnsrr}", zone->origin, &rr);
                            log_err("journal: %{dnsname}: %{dnstype}: %r", zone->origin, &rr.tctr.qtype, ret);
                        }
                        else
                        {
                            log_debug("journal: %{dnsname}: del %{dnsrr}", zone->origin, &rr);
                            log_debug("journal: %{dnsname}: %{dnstype}: %r", zone->origin, &rr.tctr.qtype, ret);
                        }
                    }
                }
            }
        }
        else
        {
            /*
             * "TO ADD" record
             */
            
            bool added_in_chain = FALSE;
            
#if ZDB_HAS_NSEC3_SUPPORT

            // returns the number of changes taken into account (0 or 1)
            // 0 : proceed
            // 1 : ignore
            // ? : error
            
            if((added_in_chain = (nsec3replay.vtbl->record_add(&nsec3replay, fqdn, rr.tctr.qtype, &ttlrdata) != 0)))
            {
                ++changes;
            }
            else
            {
                if( (top > zone->origin_vector.size) &&
                    (rr.tctr.qtype != TYPE_NSEC3) &&
                    (   (rr.tctr.qtype != TYPE_RRSIG) ||
                        ((rr.tctr.qtype == TYPE_RRSIG) && (GET_U16_AT_P(ZDB_RECORD_PTR_RDATAPTR(&ttlrdata)) != TYPE_NSEC3))
                        )
                    )
                {
                    const u8 *above_fqdn = fqdn;
                    for(int i = 1; i < top - zone->origin_vector.size; ++i)
                    {
                        zdb_rr_label *above = zdb_rr_label_find_exact(zone->apex, &labels[i], top - zone->origin_vector.size - 1 - i);
                        if(above != NULL)
                        {
                            if(btree_notempty(above->resource_record_set))
                            {
                                break;
                            }
                        }

                        above_fqdn += above_fqdn[0] + 1;
                        nsec3replay.vtbl->record_add(&nsec3replay, above_fqdn, TYPE_NONE, NULL);
                    }

                    zdb_rr_label *rr_label = zdb_rr_label_find_exact(zone->apex, labels, (top - zone->origin_vector.size) - 1);
                    if(rr_label != NULL)
                    {
                        zdb_rr_label_forall_children_of_fqdn(rr_label, fqdn, zdb_icmtl_replay_commit_label_forall_nsec3_add_cb, &nsec3replay);
                    }
                }
            }
#endif
#if ZDB_HAS_NSEC_SUPPORT
            
            // returns the number of changes taken into account (0 or 1)
            // 0 : proceed
            // 1 : ignore
            // ? : error
            
            if(!added_in_chain && (added_in_chain = (nsecreplay.vtbl->record_add(&nsecreplay, fqdn, rr.tctr.qtype, &ttlrdata) != 0)))
            {
                ++changes;
            }
            //else
#endif
            if(!added_in_chain)
            switch(rr.tctr.qtype)
            {
#if ZDB_HAS_NSEC3_SUPPORT
                case TYPE_NSEC3CHAINSTATE:
                {
                    // create chain if missing ...
                    
                    nsec3_zone_add_from_rdata(zone, rr.rdata_size, rr.rdata);
                    
                    // add the record
                    
                    zdb_packed_ttlrdata *packed_ttlrdata;

                    ZDB_RECORD_ZALLOC_EMPTY(packed_ttlrdata, ttlrdata.ttl, rr.rdata_size);
                    packed_ttlrdata->next = NULL;
                    MEMCOPY(ZDB_PACKEDRECORD_PTR_RDATAPTR(packed_ttlrdata), rr.rdata, rr.rdata_size);
                    zdb_zone_record_add(zone, labels, top, rr.tctr.qtype, packed_ttlrdata); // class is implicit, flow verified
                    
                    break;
                }
#endif // ZDB_HAS_NSEC3_SUPPORT
#
                default:
                {
                    zdb_packed_ttlrdata *packed_ttlrdata;

                    ZDB_RECORD_ZALLOC_EMPTY(packed_ttlrdata, ttlrdata.ttl, rr.rdata_size);
                    packed_ttlrdata->next = NULL;
                    MEMCOPY(ZDB_PACKEDRECORD_PTR_RDATAPTR(packed_ttlrdata), rr.rdata, rr.rdata_size);

#if ICMTL_DUMP_JOURNAL_RECORDS
                    rdata_desc type_len_rdata = {rr.tctr.qtype, rr.rdata_size, ZDB_PACKEDRECORD_PTR_RDATAPTR(packed_ttlrdata) };
                    log_debug("journal: add %{dnsname} %{typerdatadesc}", fqdn, &type_len_rdata);
                    logger_flush();
#endif
                    if(rr.tctr.qtype == TYPE_SOA)
                    {
                        rr_soa_get_serial(ZDB_PACKEDRECORD_PTR_RDATAPTR(packed_ttlrdata), ZDB_PACKEDRECORD_PTR_RDATASIZE(packed_ttlrdata), current_serialp);
                        rdata_desc rdata = {TYPE_SOA, ZDB_PACKEDRECORD_PTR_RDATASIZE(packed_ttlrdata), ZDB_PACKEDRECORD_PTR_RDATAPTR(packed_ttlrdata)};
                        log_info("journal: %{dnsname}: SOA: add %{dnsname} %{typerdatadesc}", zone->origin, fqdn, &rdata);
                    }
                    
                    zdb_zone_record_add(zone, labels, top, rr.tctr.qtype, packed_ttlrdata); // class is implicit, flow verified
                }
            }            
        } // end if ADD

        changes++;
    }

    /*
     * Yes, I know.  If 2^32 changes (add batch + del batch) occurs then it will be seen as an error ...
     */

    if(ISOK(changes))
    {
#if ZDB_HAS_NSEC3_SUPPORT && ZDB_HAS_NSEC_SUPPORT
        
        if(has_nsec3 && has_nsec)
        {
            log_warn("journal: %{dnsname}: both NSEC3 and NSEC operations happened, which is not supported by YADIFA. Keeping the original one.", zone->origin);
            
            has_nsec3 = zdb_zone_is_nsec3(zone);
            has_nsec = zdb_zone_is_nsec(zone);
        }        
#endif
        
#if ZDB_HAS_NSEC3_SUPPORT
        nsec3replay.vtbl->execute(&nsec3replay);
#endif

#if ZDB_HAS_NSEC_SUPPORT
        nsecreplay.vtbl->execute(&nsecreplay);
#endif
    }
    
#if ZDB_HAS_NSEC3_SUPPORT
    // has_nsec3 = zdb_zone_is_nsec3(zone);
    nsec3replay.vtbl->finalise(&nsec3replay);
#endif
#if ZDB_HAS_NSEC_SUPPORT
    // has_nsec = zdb_zone_is_nsec(zone);
    nsecreplay.vtbl->finalise(&nsecreplay);
#endif
    
    dns_resource_record_clear(&rr);


    
    return changes;
}

/*
 * Replay the incremental stream
 */

ya_result
zdb_icmtl_replay(zdb_zone *zone)
{
    ya_result return_value;
    u32 serial;
    
    zdb_zone_double_lock(zone, ZDB_ZONE_MUTEX_SIMPLEREADER, ZDB_ZONE_MUTEX_LOAD);

    return_value = zdb_zone_getserial(zone, &serial); // zone is locked
    
    if(FAIL(return_value))
    {
        zdb_zone_double_unlock(zone, ZDB_ZONE_MUTEX_SIMPLEREADER, ZDB_ZONE_MUTEX_LOAD);
        
        log_err("journal: %{dnsname}: error reading serial for zone: %r", zone->origin, return_value);
        
        return return_value;
    }

    input_stream is;
    
#if ICMTL_DUMP_JOURNAL_RECORDS
    log_debug("journal: zdb_icmtl_replay(%{dnsname})", zone->origin);
    logger_flush();
#endif
    
    u32 first_serial;
    u32 last_serial;
        
    if(FAIL(return_value = zdb_zone_journal_get_serial_range(zone, &first_serial, &last_serial)))
    {
        zdb_zone_double_unlock(zone, ZDB_ZONE_MUTEX_SIMPLEREADER, ZDB_ZONE_MUTEX_LOAD);
        
        if(return_value == ZDB_ERROR_ICMTL_NOTFOUND)
        {
            return_value = SUCCESS;
        }
        else
        {
            log_err("journal: %{dnsname}: error opening journal for zone: %r", zone->origin, return_value);
        }
                
        return return_value;
    }
    
    log_debug("journal: %{dnsname}: zone serial is %i, journal covers serials from %i to %i", zone->origin, serial, first_serial, last_serial);
    
    if(last_serial == serial)
    {
        zdb_zone_double_unlock(zone, ZDB_ZONE_MUTEX_SIMPLEREADER, ZDB_ZONE_MUTEX_LOAD);
        
        log_debug("journal: %{dnsname}: nothing to read from the journal", zone->origin);
        return 0;
    }
    
    if(serial_lt(serial, first_serial))
    {
        zdb_zone_double_unlock(zone, ZDB_ZONE_MUTEX_SIMPLEREADER, ZDB_ZONE_MUTEX_LOAD);
        
        log_warn("journal: %{dnsname}: first serial from the journal is after the zone", zone->origin);
        // should invalidate the journal
        zdb_zone_journal_delete(zone);
        return 0;
    }
    
    if(serial_gt(serial, last_serial))
    {
        zdb_zone_double_unlock(zone, ZDB_ZONE_MUTEX_SIMPLEREADER, ZDB_ZONE_MUTEX_LOAD);
        
        log_warn("journal: %{dnsname}: last serial from the journal is before the zone", zone->origin);
        // should invalidate the journal
        zdb_zone_journal_delete(zone);
        return 0;
    }
    
    if(FAIL(return_value = zdb_zone_journal_get_ixfr_stream_at_serial(zone, serial, &is, NULL)))
    {
        zdb_zone_double_unlock(zone, ZDB_ZONE_MUTEX_SIMPLEREADER, ZDB_ZONE_MUTEX_LOAD);
        
        log_err("journal: %{dnsname}: error reading journal from serial %d: %r",zone->origin, serial, return_value);

        return return_value;
    }
    
    log_info("journal: %{dnsname}: replaying from serial %u",zone->origin, serial);

    buffer_input_stream_init(&is, &is, ZDB_ICMTL_REPLAY_BUFFER_SIZE);    

    u16 shutdown_test_countdown = ZDB_ICMTL_REPLAY_SHUTDOWN_POLL_PERIOD;
    
    u32 current_serial = serial;
    
    /*
     * Read all records from [ SOA ... SOA ... [ SOA in memory
     */
    
    output_stream baos;
    input_stream bais;
    dns_resource_record rr;
    
    int baos_rr_count = 0;
    int baos_soa_count = 0;
    
    bool was_nsec3 = zdb_zone_is_nsec3(zone);

    bytearray_output_stream_init_ex(&baos, NULL, ZDB_ICMTL_REPLAY_BUFFER_SIZE, BYTEARRAY_DYNAMIC);
    dns_resource_record_init(&rr);    
           
    // 0: gather, 1: commit, 2: commit & stop
    
    for(int replay_state = ZDB_ICMTL_REPLAY_GATHER; replay_state != ZDB_ICMTL_REPLAY_COMMIT_AND_STOP;)
    {
        // ensure it's not supposed to shutdown (every few iterations)
        
        if(--shutdown_test_countdown <= 0)
        {
            if(dnscore_shuttingdown())
            {
                return_value = STOPPED_BY_APPLICATION_SHUTDOWN;
                break;
            }
            
            shutdown_test_countdown = ZDB_ICMTL_REPLAY_SHUTDOWN_POLL_PERIOD;
        }
        
        // read the next record
        
        if((return_value = dns_resource_record_read(&rr, &is)) <= 0)
        {
            if(ISOK(return_value))
            {
                log_info("journal: %{dnsname}: reached the end of the journal file", zone->origin);
                replay_state = ZDB_ICMTL_REPLAY_COMMIT_AND_STOP;
            }
            else
            {
                log_err("journal: broken journal: %r", return_value);
                logger_flush(); // broken journal (flush is slow, but this is bad, so : keep it)
                replay_state = ZDB_ICMTL_REPLAY_STOP;
            }
        }
        else // first record must be an SOA (or it's wrong)        
        if(baos_rr_count == 0) // first record ?
        {
            if(rr.tctr.qtype != TYPE_SOA) // must be SOA
            {
                // expected an SOA
                return_value = ERROR;
                break;
            }
            
            ++baos_soa_count; // 0 -> 1 // this is not mandatory but clearer to read
        }
        else // the page ends with an SOA or end of stream
        if(rr.tctr.qtype == TYPE_SOA)
        {
            if(baos_soa_count == 2)
            {
                // this record is the start of the next stream, keep it for the next iteration
                replay_state = ZDB_ICMTL_REPLAY_COMMIT;
            }
            
            ++baos_soa_count;
        }
        
        ++baos_rr_count;
        
        if((replay_state & ZDB_ICMTL_REPLAY_COMMIT) != 0)
        {
            log_info("journal: %{dnsname}: committing changes", zone->origin);
            u64 ts_start = timeus();
            zdb_zone_exchange_locks(zone, ZDB_ZONE_MUTEX_SIMPLEREADER, ZDB_ZONE_MUTEX_LOAD);
            bytearray_input_stream_init_const(&bais, bytearray_output_stream_buffer(&baos), bytearray_output_stream_size(&baos));
            
            return_value = zdb_icmtl_replay_commit(zone, &bais, &current_serial);
            
            zdb_zone_exchange_locks(zone, ZDB_ZONE_MUTEX_LOAD, ZDB_ZONE_MUTEX_SIMPLEREADER);
            input_stream_close(&bais);
            u64 ts_stop = timeus();
            if(ts_stop < ts_start) // time change
            {
                ts_stop = ts_start;
            }
            
            u64 ts_delta = ts_stop - ts_start;
            
            if(ISOK(return_value))
            {
                if(ts_delta < 1000)
                {            
                    log_info("journal: %{dnsname}: committed changes (%lluus)", zone->origin, ts_delta);
                }
                else if(ts_delta < 1000000)
                {
                    double ts_delta_s = ts_delta;
                    ts_delta_s /= 1000.0;
                    log_info("journal: %{dnsname}: committed changes (%5.2fms)", zone->origin, ts_delta_s);
                }
                else
                {
                    double ts_delta_s = ts_delta;
                    ts_delta_s /= 1000000.0;
                    log_info("journal: %{dnsname}: committed changes (%5.2fs)", zone->origin, ts_delta_s);
                }
            }
            else
            {
                log_err("journal: %{dnsname}: failed to committed changes", zone->origin);
                break;
            }
                    
            // the current page has been processed
            
            if(replay_state == ZDB_ICMTL_REPLAY_COMMIT_AND_STOP)
            {
                // no more page to read
                break;
            }
            
            // reset the state for the next page
            // note: the next written record will be the last read SOA
                        
            baos_rr_count = 1;
            baos_soa_count = 1;
            replay_state = ZDB_ICMTL_REPLAY_GATHER;
            bytearray_output_stream_reset(&baos);
                       
        } // end if replay_state is ZDB_ICMTL_REPLAY_COMMIT (mask)
                
        dns_resource_record_write(&rr, &baos);
    }
   
    input_stream_close(&is);
    output_stream_close(&baos);
    dns_resource_record_clear(&rr);
    
    // cleanup destroyed nsec3 chains
    
    if(ISOK(return_value))
    {
        bool is_nsec3 = zdb_zone_is_nsec3(zone);

        if(is_nsec3 && !was_nsec3)
        {
            // the chain has just been created, but is probably missing internal links
            log_debug("journal: %{dnsname}: zone switched to NSEC3 by reading the journal: updating links", zone->origin);

            zdb_zone_exchange_locks(zone, ZDB_ZONE_MUTEX_SIMPLEREADER, ZDB_ZONE_MUTEX_LOAD);
            nsec3_zone_update_chain0_links(zone);
            zdb_zone_exchange_locks(zone, ZDB_ZONE_MUTEX_LOAD, ZDB_ZONE_MUTEX_SIMPLEREADER);

            log_debug("journal: %{dnsname}: zone switched to NSEC3 by reading the journal: links updated", zone->origin);
        }

        if(FAIL(return_value = zdb_zone_getserial(zone, &serial))) // zone is locked
        {
            zdb_zone_double_unlock(zone, ZDB_ZONE_MUTEX_SIMPLEREADER, ZDB_ZONE_MUTEX_LOAD);

            log_err("journal: %{dnsname}: error reading confirmation serial for zone: %r",zone->origin, return_value);

            return return_value;
        }

        if(serial != last_serial)
        {
            log_warn("journal: %{dnsname}: expected serial to be %i but it is %i instead",zone->origin, last_serial, serial);
        }

#if 0 // ICMTL_DUMP_JOURNAL_RECORDS
        if(is_nsec)
        {
            nsec_logdump_tree(zone);
            logger_flush();
        }
#endif
    }
    
    zdb_zone_double_unlock(zone, ZDB_ZONE_MUTEX_SIMPLEREADER, ZDB_ZONE_MUTEX_LOAD);
    
    log_info("journal: %{dnsname}: done", zone->origin);

    return return_value;
}

/** @} */


