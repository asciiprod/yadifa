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
/** @defgroup dnsdb
 *  @ingroup dnsdb
 *  @brief journal file & incremental changes
 *
 * @{
 *
 *----------------------------------------------------------------------------*/
#ifndef _ZDB_ICMTL_H
#define	_ZDB_ICMTL_H

#include <dnsdb/zdb_types.h>
#include <dnscore/output_stream.h>
#include <dnscore/input_stream.h>
#include <dnscore/counter_output_stream.h>
#include <dnscore/ptr_vector.h>

#ifdef	__cplusplus
extern "C"
{
#endif

/**
 * The incremental changes are in two files.  This way we don't care about intertwined ADD & REMOVE.
 */

/*
 * ICMTL single-stream extension
 */

#define ICMTL_EXT "ix"
#define ICMTL_EXT_STRLEN 2

/*
 * Contains the removed records
 */
#define ICMTL_REMOVE_FILE_FORMAT "%s/%{dnsname}%08x-%08x.ir"
/*
 * Contains the added records
 */
#define ICMTL_ADD_FILE_FORMAT "%s/%{dnsname}%08x-%08x.ia"
/*
 * Contains the summary (SOA from / SOA to)
 */
#define ICMTL_SUMMARY_FILE_FORMAT "%s/%{dnsname}%08x-%08x.is"

/*
 * Contains the wire ICMTL (almost: not the matching start and end SOA)
 */

#define ICMTL_WIRE_FILE_FORMAT "%s/%{dnsname}%08x-%08x." ICMTL_EXT

#define ZDB_ICMTL_ITEM_ADD	1
#define ZDB_ICMTL_ITEM_REMOVE	2
#define ZDB_ICMTL_ITEM_NOP	(ZDB_ICMTL_ITEM_ADD|ZDB_ICMTL_ITEM_REMOVE)

typedef struct zdb_icmtl_item zdb_icmtl_item;


struct zdb_icmtl_item
{
    u8* name;
    u8* rdata;
    u32 rttl;
    u16 rtype;
    u16 rdata_size;
    u8	flag;	    /* add or remove */
};

typedef struct zdb_icmtl zdb_icmtl;

struct zdb_icmtl
{
    output_stream os_remove_;
    output_stream os_add_;
    
    output_stream os_remove;
    output_stream os_add;
    
    zdb_zone* zone;
    
    u64 file_size_before_append;
    u64 file_size_after_append;
    
    counter_output_stream_data os_remove_stats;
    counter_output_stream_data os_add_stats;
    
    u32 patch_index;
    
    bool modified;                  // @note not aligned
    bool can_ignore_signatures;     // do not complain if no keys are available
    
    u32 soa_ttl;    
    u16 soa_rdata_size;    
    u8  soa_rdata[532];
};

#define ZDB_ICMTL_LISTENER_BEGIN    0
#define ZDB_ICMTL_LISTENER_CANCEL   1
#define ZDB_ICMTL_LISTENER_END      2

typedef void zdb_icmtl_listener_callback(u8 state, const zdb_icmtl* icmtl, void *args);

/**
 * Enables incremental changes recording in the zone
 */

ya_result zdb_icmtl_begin(zdb_icmtl *icmtl, zdb_zone *zone);

/**
 * Disables incremental changes recording in the zone and records them into a file
 */

ya_result zdb_icmtl_end(zdb_icmtl* icmtl);

/**
 * Disables incremental changes recording in the zone and discards recorded changes
 * 
 * @param icmtl
 * @return 
 */

ya_result zdb_icmtl_cancel(zdb_icmtl *icmtl);

/**
 * Replays incremental changes for the zone, looking in the directory for the files (.ix)
 */

#define ZDB_ICMTL_REPLAY_SERIAL_OFFSET 1 // avoids scanning
#define ZDB_ICMTL_REPLAY_SERIAL_LIMIT  2 // don't try to go beyond the set serial

ya_result zdb_icmtl_replay(zdb_zone *zone);

/**
 * Quick-check for the last available serial for an origin and return it. (It's based on file names)
 */

ya_result zdb_icmtl_get_last_serial_from(zdb_zone *zone, u32 *last_serial);

/**
 * Loads the first "DEL" soa matching that serial
 */

ya_result zdb_icmtl_get_soa_with_serial(input_stream *is, u32 serial, u8 *out_dname, struct type_class_ttl_rdlen *out_tctr, u8 *soa_rdata_780);

/**
 * Opens the right incremental stream and reads the soa for the serial
 */

ya_result zdb_icmtl_open_ix_get_soa(const u8 *origin, const char *directory, u32 serial, input_stream *is, struct type_class_ttl_rdlen *tctrp, u8 *rdata_buffer_780, u32 *rdata_size);

/**
 * Adds a listener to the global ICMTL events.
 * Whenever zdb_icmtl_begin/cancel/end is successfully called, all listeners are notified
 * 
 * @param callback the callback function
 * @param callback_args a pointer to pass to the callback function
 */

void zdb_icmtl_listener_add(zdb_icmtl_listener_callback *callback, void *callback_args);

/**
 * Removes a listener from the global ICMTL events.
 * 
 * @param callback the callback function
 */

void zdb_icmtl_listener_remove(zdb_icmtl_listener_callback *callback);

#ifdef	__cplusplus
}
#endif

#endif	/* _ZDB_ICMTL_H */
/** @} */

/*----------------------------------------------------------------------------*/

