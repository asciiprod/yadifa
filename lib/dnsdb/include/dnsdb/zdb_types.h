/*------------------------------------------------------------------------------
*
* Copyright (c) 2011-2019, EURid vzw. All rights reserved.
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
/** @defgroup types The types used in the database
 *  @ingroup dnsdb
 *  @brief The types used in the database
 *
 * The types used in the database
 *
 * @{
 */
#ifndef _ZDB_TYPES_H
#define	_ZDB_TYPES_H

#include <dnsdb/zdb-config-features.h>

#include <dnscore/dnsname.h>
#include <dnscore/zalloc.h>
#include <dnscore/rfc.h>
#include <dnscore/message.h>
#include <dnscore/alarm.h>
#include <dnscore/mutex.h>

#include <dnsdb/zdb_config.h>
#include <dnsdb/dictionary.h>
//#include <dnsdb/journal.h>

#include <dnsdb/zdb_error.h>

#ifdef	__cplusplus
extern "C"
{
#endif

struct journal;
    
#define ROOT_LABEL                  ((u8*)"")

typedef struct zdb_packed_ttlrdata zdb_packed_ttlrdata;

struct zdb_packed_ttlrdata
{ /* DO NOT CHANGE THE ORDER OF THE FIELDS !!! */
    zdb_packed_ttlrdata* next; /*  4  8 */
    s32 ttl; /*  4  4 */
    u16 rdata_size; /*  2  2 */
    u8 rdata_start[1];
};

// a zdb_packed_ttlrdata ready to store a valid SOA

struct zdb_packed_ttlrdata_soa
{ /* DO NOT CHANGE THE ORDER OF THE FIELDS !!! */
    zdb_packed_ttlrdata* next; /*  4  8 */
    u32 ttl; /*  4  4 */
    u16 rdata_size; /*  2  2 */
    u8 rdata_start[MAX_SOA_RDATA_LENGTH];
};

#define ZDB_RDATABUF_TAG    0x4655424154414452
#define ZDB_RECORD_TAG      0x4443455242445a    /** "ZDBRECD" */

#define ZDB_RECORD_SIZE_FROM_RDATASIZE(rdata_size_) (sizeof(zdb_packed_ttlrdata)-1+(rdata_size_))
#define ZDB_RECORD_SIZE(record_)                    ZDB_RECORD_SIZE_FROM_RDATASIZE((record_)->rdata_size)

#if !DNSCORE_HAS_ZALLOC

#define ZDB_RECORD_ZALLOC(record,ttl_,len_,rdata_)                   \
    {                                                                \
        MALLOC_OR_DIE(zdb_packed_ttlrdata*,(record),sizeof(zdb_packed_ttlrdata)-1+len_,ZDB_RECORD_TAG); /* ZALLOC IMPOSSIBLE */ \
        (record)->ttl=ttl_;                                          \
        (record)->rdata_size=len_;                                   \
        MEMCOPY(&(record)->rdata_start[0],rdata_,len_);               \
    }

#define ZDB_RECORD_ZALLOC_EMPTY(record,ttl_,len_)                    \
    {                                                                \
        MALLOC_OR_DIE(zdb_packed_ttlrdata*,(record),sizeof(zdb_packed_ttlrdata)-1+len_,ZDB_RECORD_TAG); /* ZALLOC IMPOSSIBLE */ \
        (record)->ttl=ttl_;                                          \
        (record)->rdata_size=len_;                                   \
    }

#define ZDB_RECORD_CLONE(record_s_,record_d_)                           \
    {                                                                   \
        u32 size=sizeof(zdb_packed_ttlrdata)-1+(record_s_)->rdata_size; \
        MALLOC_OR_DIE(zdb_packed_ttlrdata*,(record_d_),size,ZDB_RECORD_TAG); /* ZALLOC IMPOSSIBLE */ \
        record_d_->ttl=record_s_->ttl;                                  \
        record_d_->rdata_size=record_s_->rdata_size;                    \
        MEMCOPY(&(record_d_)->rdata_start[0],&(record_s_)->rdata_start[0],record_s_->rdata_size); \
    }

#define ZDB_RECORD_ZFREE(record) free(record)

#define ZDB_RECORD_SAFE_ZFREE(record) free(record)

#else

#define ZDB_RECORD_ZALLOC(record_,ttl_,len_,rdata_)                     \
    {                                                                   \
        u32 size=ZDB_RECORD_SIZE_FROM_RDATASIZE(len_);                  \
        if(size<=ZALLOC_PG_PAGEABLE_MAXSIZE)                         \
        {                                                               \
            record_=(zdb_packed_ttlrdata*)zalloc_line((size-1)>>3);      \
        }                                                               \
        else                                                            \
        {                                                               \
            MALLOC_OR_DIE(zdb_packed_ttlrdata*,(record_),sizeof(zdb_packed_ttlrdata)-1+len_,ZDB_RECORD_TAG); /* ZALLOC IMPOSSIBLE */ \
        }                                                               \
                                                                        \
        (record_)->ttl=ttl_;                                            \
        (record_)->rdata_size=len_;                                     \
        MEMCOPY(&(record_)->rdata_start[0],rdata_,len_);                \
    }

#define ZDB_RECORD_ZALLOC_EMPTY(record_,ttl_,len_)                      \
    {                                                                   \
        u32 size=ZDB_RECORD_SIZE_FROM_RDATASIZE(len_);                  \
        if(size<=ZALLOC_PG_PAGEABLE_MAXSIZE)                         \
        {                                                               \
            record_=(zdb_packed_ttlrdata*)zalloc_line((size-1)>>3);      \
        }                                                               \
        else                                                            \
        {                                                               \
            MALLOC_OR_DIE(zdb_packed_ttlrdata*,(record_),sizeof(zdb_packed_ttlrdata)-1+len_,ZDB_RECORD_TAG); /* ZALLOC IMPOSSIBLE */ \
        }                                                               \
                                                                        \
        (record_)->ttl=ttl_;                                            \
        (record_)->rdata_size=len_;                                     \
    }

#define ZDB_RECORD_CLONE(record_s_,record_d_)                           \
    {                                                                   \
        u32 size=ZDB_RECORD_SIZE_FROM_RDATASIZE((record_s_)->rdata_size);\
        if(size<=ZALLOC_PG_PAGEABLE_MAXSIZE)                         \
        {                                                               \
            record_d_=(zdb_packed_ttlrdata*)zalloc_line((size-1)>>3); \
        }                                                               \
        else                                                            \
        {                                                               \
            MALLOC_OR_DIE(zdb_packed_ttlrdata*,(record_d_),size,ZDB_RECORD_TAG); /* ZALLOC IMPOSSIBLE */ \
        }                                                               \
        record_d_->ttl=record_s_->ttl;                                  \
        record_d_->rdata_size=record_s_->rdata_size;                    \
        MEMCOPY(&(record_d_)->rdata_start[0],&(record_s_)->rdata_start[0],record_s_->rdata_size); \
    }

/* DOES NOT CHECKS FOR NULL */
#define ZDB_RECORD_ZFREE(record_)                                       \
    {                                                                   \
        u32 size=ZDB_RECORD_SIZE_FROM_RDATASIZE((record_)->rdata_size); \
        if(size<=ZALLOC_PG_PAGEABLE_MAXSIZE)                         \
        {                                                               \
            zfree_line(record_,(size-1)>>3);                             \
        }                                                               \
        else                                                            \
        {                                                               \
            free(record_);                                              \
        }                                                               \
    }

/* DOES CHECKS FOR NULL */
#define ZDB_RECORD_SAFE_ZFREE(record_)                                  \
    if(record_ != NULL)                                                 \
    {                                                                   \
        u32 size=ZDB_RECORD_SIZE_FROM_RDATASIZE((record_)->rdata_size); \
        if(size<=ZALLOC_PG_PAGEABLE_MAXSIZE)                         \
        {                                                               \
            zfree_line(record_,(size-1)>>3);                             \
        }                                                               \
        else                                                            \
        {                                                               \
            free(record_);                                              \
        }                                                               \
    }
#endif

#define ZDB_RECORD_MALLOC_EMPTY(record_,ttl_,rdata_size_)               \
    {                                                                   \
        u32 size=ZDB_RECORD_SIZE_FROM_RDATASIZE(rdata_size_);           \
        MALLOC_OR_DIE(zdb_packed_ttlrdata*,(record_),size,ZDB_RECORD_TAG); /* ZALLOC IMPOSSIBLE */ \
        (record_)->ttl=ttl_;                                            \
        (record_)->rdata_size=rdata_size_;                              \
    }

/*
 * These macros existed when 2 different ways for storing the record were
 * available at compile time.
 *
 * The zdb_packed_ttlrdata having proved to be the best (by far),
 * the other one has been removed.
 *
 */

#define ZDB_PACKEDRECORD_PTR_RDATAPTR(record_)  (&(record_)->rdata_start[0])
#define ZDB_PACKEDRECORD_PTR_RDATASIZE(record_) ((record_)->rdata_size)

#define ZDB_RECORD_PTR_RDATASIZE(record_)       ((record_)->rdata_size)
#define ZDB_RECORD_PTR_RDATAPTR(record_)        ((record_)->rdata_pointer)

typedef struct zdb_ttlrdata zdb_ttlrdata;

struct zdb_ttlrdata
{
    zdb_ttlrdata* next; /*  4  8 */
    u32 ttl; /*  4  4 */
    u16 rdata_size; /*  2  2 */
    u16 padding_reserved;
    void* rdata_pointer; /*  4  8 */
};

#define ZDB_PACKEDRECORD_RDATADESC(type_,ttlrdata_) { (type_), ZDB_PACKEDRECORD_PTR_RDATASIZE(ttlrdata_), ZDB_PACKEDRECORD_PTR_RDATAPTR(ttlrdata_)}
#define ZDB_RECORD_RDATADESC(type_,ttlrdata_) { (type_), ZDB_RECORD_PTR_RDATASIZE(ttlrdata_), ZDB_RECORD_PTR_RDATAPTR(ttlrdata_)}

#define TTLRDATA_INLINESIZE         THIS_SHOULD_NOT_BE_USED_IN_PACKED_MODE

#define ZDB_RECORD_TTLRDATA_SET(record,ttl_,len_,rdata_)            \
    {                                                               \
        (record).ttl=ttl_;                                          \
        (record).rdata_size=len_;                                   \
        (record).rdata_pointer=rdata_;                              \
    }

typedef btree zdb_rr_collection;


#define ZDB_RESOURCERECORD_TAG 0x444345524c4c5546   /** "FULLRECD" */

/* zdb_zone */

typedef struct zdb_zone zdb_zone;


#define LABEL_HAS_RECORDS(label_) ((label_)->resource_record_set != NULL)

typedef struct zdb_resourcerecord zdb_resourcerecord;

struct zdb_resourcerecord
{
    zdb_resourcerecord* next; /*  4  8 */
    zdb_packed_ttlrdata* ttl_rdata; /*  4  8 */
    const u8* name; /*  4  8 */
    u16 zclass; /*  2  2 */
    u16 rtype; /*  2  2 */
    u32 ttl;
}; /* 16 28 => 16 32 */

/* zdb_rr_label */

typedef dictionary zdb_rr_label_set;

typedef struct zdb_rr_label zdb_rr_label;

#if ZDB_HAS_DNSSEC_SUPPORT != 0

#if ZDB_HAS_NSEC_SUPPORT != 0

typedef struct nsec_label_extension nsec_label;
typedef struct nsec_label_extension nsec_label_extension;
typedef struct nsec_node nsec_zone;


#endif

#if ZDB_HAS_NSEC3_SUPPORT != 0

typedef struct nsec3_zone nsec3_zone;


#endif

typedef struct dnssec_zone_extension dnssec_zone_extension;

struct dnssec_zone_extension
{
#if ZDB_HAS_NSEC_SUPPORT
    /*
     * A pointer to an array of nsec_label (nsec_label buffer[])
     * The size is the same as the dictionnary
     */
    nsec_zone* nsec;
#endif
    
#if ZDB_HAS_NSEC3_SUPPORT
    nsec3_zone* nsec3;
#endif
};

typedef union nsec_label_union nsec_label_union;

struct nsec_label_extension
{
    struct nsec_node* node;
};

union nsec_label_union
{
    /* NSEC */
#if ZDB_HAS_NSEC_SUPPORT
    nsec_label_extension nsec;
#endif

    /* NSEC3 */
#if ZDB_HAS_NSEC3_SUPPORT
    struct nsec3_label_extension* nsec3;
#endif

    /* Placeholder */
    void* dnssec;
};

#endif

/*
 * RR_LABEL flags
 */

/*
 * Zone security is managed
 */
#define ZDB_RR_LABEL_MAINTAINED     0x8000 // should be moved

/*
 * Forbid updates to the apex
 */
#define ZDB_RR_APEX_LABEL_FROZEN    0x4000  // should be moved

/*
 * When a zone has expired, the apex is marked with this flag
 */

#define ZDB_RR_LABEL_INVALID_ZONE   0x2000  // should be moved

/*
 * For the apex, marks a label as being the apex
 */

#define ZDB_RR_LABEL_APEX           0x0001

/*
 * For any label but the apex : marks it as being a delegation (contains an NS record)
 */

#define ZDB_RR_LABEL_DELEGATION     0x0002

/*
 * For any label, means there is a delegation (somewhere) above
 */

#define ZDB_RR_LABEL_UNDERDELEGATION 0x0004

/*
 * For any label : marks that one owns a '*' label
 */

#define ZDB_RR_LABEL_GOT_WILD       0x0008

/*
 * Explicitly mark a label as owner of a (single) CNAME
 */

#define ZDB_RR_LABEL_HASCNAME       0x0010

/*
 * Explicitly mark a label as owner of a something that is not a CNAME nor RRSIG nor NSEC
 */

#define ZDB_RR_LABEL_DROPCNAME      0x0020

#define ZDB_RR_LABEL_N3COVERED      0x0040
#define ZDB_RR_LABEL_N3OCOVERED     0x0080


#if ZDB_HAS_DNSSEC_SUPPORT
/*
 * This flag means that the label has a valid NSEC structure
 *
 * IT IS NOT VALID TO CHECK THIS TO SEE IF A ZONE IS NSEC
 */
#define ZDB_RR_LABEL_NSEC           0x0100

/*
 * This flag means that the label has a valid NSEC3 structure
 *
 * IT IS NOT VALID TO CHECK THIS TO SEE IF A ZONE IS NSEC3
 */
#define ZDB_RR_LABEL_NSEC3          0x0200

/*
 * The zone is (NSEC3) + OPTOUT (NSEC3 should also be set)
 * 
 * IT IS NOT VALID TO CHECK THIS TO SEE IF A ZONE IS NSEC3
 */

#define ZDB_RR_LABEL_NSEC3_OPTOUT   0x0400

#endif

#define ZDB_ZONE_VALID(__z__) ((((__z__)->apex->flags)&ZDB_RR_LABEL_INVALID_ZONE) == 0)
#define ZDB_ZONE_INVALID(__z__) ((((__z__)->apex->flags)&ZDB_RR_LABEL_INVALID_ZONE) != 0)

#define ZDB_LABEL_UNDERDELEGATION(__l__) ((((__l__)->flags)&ZDB_RR_LABEL_UNDERDELEGATION)!=0)
#define ZDB_LABEL_ATDELEGATION(__l__) ((((__l__)->flags)&ZDB_RR_LABEL_DELEGATION)!=0)
#define ZDB_LABEL_ATORUNDERDELEGATION(__l__) ((((__l__)->flags)&(ZDB_RR_LABEL_DELEGATION|ZDB_RR_LABEL_UNDERDELEGATION))!=0)

#define ZDB_LABEL_ISAPEX(__l__) ((((__l__)->flags)&ZDB_RR_LABEL_APEX)!=0)

struct zdb_rr_label
{
    zdb_rr_label* next; /* dictionnary_node* next */ /*  4  8 */
    zdb_rr_label_set sub; /* dictionnary of N children labels */ /* 16 24 */

    zdb_rr_collection resource_record_set; /* resource records for the label (a btree)*/ /*  4  4 */

#if ZDB_HAS_DNSSEC_SUPPORT != 0
    nsec_label_union nsec;
#endif

    u16 flags;	/* NSEC, NSEC3, and 6 for future usage ... */

    u8 name[1]; /* label */ /*  4  8 */
    /* No zone ptr */
}; /* 28 44 => 32 48 */

#define ZDB_ZONE_MUTEX_EXCLUSIVE_FLAG   0x80
#define ZDB_ZONE_MUTEX_LOCKMASK_FLAG    0x7f

#define ZDB_ZONE_MUTEX_UNLOCKMASK_FLAG  0x7f

#define ZDB_ZONE_MUTEX_NOBODY           GROUP_MUTEX_NOBODY
#define ZDB_ZONE_MUTEX_SIMPLEREADER     0x01 /* non-conflicting */
#define ZDB_ZONE_MUTEX_RRSIG_UPDATER    0x82 /* conflicting */
#define ZDB_ZONE_MUTEX_XFR              0x84 /* conflicting, cannot by nature be launched more than once in parallel.  new ones have to be discarded */
#define ZDB_ZONE_MUTEX_REFRESH          0x85 /* conflicting, can never be launched more than once.  new ones have to be discarded */
#define ZDB_ZONE_MUTEX_DYNUPDATE        0x86 /* conflicting */
#define ZDB_ZONE_MUTEX_UNFREEZE         0x87 /* conflicting, needs to be sure nobody else (ie: the freeze) is acting at the same time */
#define ZDB_ZONE_MUTEX_INVALIDATE       0x88 /* conflicting */
#define ZDB_ZONE_MUTEX_REPLACE          0x89 /* conflicting */
#define ZDB_ZONE_MUTEX_LOAD             0x8a /* conflicting but this case is impossible */
#define ZDB_ZONE_MUTEX_NSEC3            0x8b /* conflicting, marks an hard operation to be done */
#define ZDB_ZONE_MUTEX_DESTROY          0xff /* conflicting, can never be launched more than once.  The zone will be destroyed before unlock. */

typedef ya_result zdb_zone_access_filter(const message_data* /*mesg*/, const void* /*zone_extension*/);

#define ALARM_KEY_ZONE_SIGNATURE_UPDATE 1
#define ALARM_KEY_ZONE_AXFR_QUERY       2
#define ALARM_KEY_ZONE_REFRESH          3

#define ALARM_KEY_ZONE_DNSKEY_PUBLISH   4
#define ALARM_KEY_ZONE_DNSKEY_UNPUBLISH 5
#define ALARM_KEY_ZONE_DNSKEY_ACTIVATE  6
#define ALARM_KEY_ZONE_DNSKEY_DEACTIVATE 7

#define ALARM_KEY_ZONE_NOTIFY_SLAVES    8

#define ZDB_ZONE_KEEP_RAW_SIZE          1

#define ZDB_ZONE_STATUS_ICMTL_ENABLED   1
#define ZDB_ZONE_STATUS_DUMPING_AXFR    2
#define ZDB_ZONE_STATUS_WILL_NOTIFY     4 // in the queue

#define ZDB_ZONE_HAS_OPTOUT_COVERAGE    1   // assumed true, until something else is found
#define ZDB_ZONE_MAINTAIN_NSEC          2
#define ZDB_ZONE_MAINTAIN_NSEC3         4
#define ZDB_ZONE_MAINTAIN_NSEC3_OPTOUT  5
#define ZDB_ZONE_MAINTAIN_MASK          7

#define ZDB_ZONE_RRSIG_PUSH_ALLOWED     8   // feature requested internally



struct dnskey_keyring;
/*
#define UNSIGNED_TYPE_VALUE_MAX(__type__)    ((__type__)~0)
#define SIGNED_TYPE_VALUE_MAX(__type__)      (((__type__)~0)>>1)
#define SIGNED_TYPE_VALUE_MIN(__type__)      (((__type__)~0) - (((__type__)~0)>>1))

#define UNSIGNED_VAR_VALUE_MAX(__var__)     ((~0ULL)>>((sizeof(~0ULL) - sizeof(__var__)) * 8LL))
#define SIGNED_VAR_VALUE_MAX(__var__)      (UNSIGNED_VAR_VALUE_MAX(__var__)>>1)
#define SIGNED_VAR_VALUE_MIN(__var__)      (UNSIGNED_VAR_VALUE_MAX(__var__) - SIGNED_VAR_VALUE_MAX(__var__))

#define UNSIGNED_VAR_VALUE_IS_MAX(__var__) (__var__ == UNSIGNED_VAR_VALUE_MAX(__var__))
#define SIGNED_VAR_VALUE_IS_MAX(__var__) (__var__ == SIGNED_VAR_VALUE_MAX(__var__))
*/
#define ZDB_ZONE_HAS_JNL_REFERENCE 0

struct zdb_zone_update_signatures_ctx
{
    u8 *current_fqdn;
    u32 earliest_signature_expiration;
    u16 labels_at_once;

};

typedef struct zdb_zone_update_signatures_ctx zdb_zone_update_signatures_ctx;

struct zdb_zone
{
    u8 *origin; /* dnsname, origin */
    zdb_rr_label *apex; /* pointer to the zone cut, 1 name for : SOA, NS, ... */
    
#if ZDB_HAS_DNSSEC_SUPPORT
    dnssec_zone_extension nsec;
#endif

    zdb_zone_access_filter* query_access_filter;
    void *extension;    /**
                         * This pointer is meant to be used by the server so it can associate data with the zone
                         * without having to do the match on its side too.
                         *
                         */
    
#if ZDB_HAS_DNSSEC_SUPPORT
    zdb_zone_update_signatures_ctx progressive_signature_update;
#endif
    
    s32 min_ttl;        /* a copy of the min-ttl from the SOA */

    /* 
     * AXFR handling.
     * 
     * In order to be nicer with the resources of the machine and more reactive we are adding a pace mechanism.
     * MASTER:
     * init: ts=1, serial = real serial - 1
     * axfr(1): ts=0, serial = real serial, writing on disk, streaming to client until axfr_timestamp>1 OR axfr_serial has changed
     *         (both meaning the file has fully been written on disk)
     * axfr(2): ts=0, serial = real serial, reading from the file being written
     * axfr(3): ts=T, serial = real serial, reading from the written file
     * axfr(4): now - ts > too_much, do axfr(1)
     * SLAVE:
     *        : ts=last time the axfr has been fully done, serial = serial in the axfr on file
     * 
     */
    
    volatile u32 axfr_timestamp;        // The last time when an AXFR has ENDED to be written on disk, if 0, an AXFR is being written right now
    volatile u32 axfr_serial;           // The serial number of the AXFR (being written) on disk
    
#if ZDB_HAS_DNSSEC_SUPPORT
    u32 sig_validity_regeneration_seconds;
    u32 sig_validity_interval_seconds;
    u32 sig_validity_jitter_seconds;
    u32 sig_quota;                      // starts at 100, updated so a batch does not takes more than a fraction of a second
#endif
        
    alarm_t alarm_handle;               // 32 bits
    volatile s32 rc;                    // reference counter when it reaches 0, the zone and its content should be destroyed asap
    volatile s32 lock_count;            // the number of owners with the current lock ID
    volatile u8 lock_owner;             // the ID of who can manipulate the zone
    volatile u8 lock_reserved_owner;    // to the next-owner mechanism (reserve an ownership change)
    
    volatile u8 _status;                // extended status flags for background tasks not part of the normal operations        
    volatile u8 _flags;                 // extended flags (optout coverage)
#if ZDB_RECORDS_MAX_CLASS != 1
    u16 zclass;
#endif
    
    mutex_t lock_mutex;
    cond_t  lock_cond;
    
#if DNSCORE_HAS_MUTEX_DEBUG_SUPPORT
    stacktrace lock_trace;
    pthread_t lock_id;
    u64 lock_timestamp;
#endif

#if ZDB_ZONE_KEEP_RAW_SIZE
    volatile s64 wire_size;             // approximation of the size of a zone. updated on load and store of the zone on disk
    volatile u64 write_time_elapsed;    // the time that was spent writing the zone in a file (ie: axfr)
#endif
    
#if ZDB_ZONE_HAS_JNL_REFERENCE
    /** journal is only to be accessed trough the journal_* functions */
    struct journal *_journal;
#endif
        
    dnsname_vector origin_vector;       // note: the origin vector is truncated to it's used length (sparing quite a lot of memory)

}; /* 18 34 => 20 40 */

/* zdb_zone_label */

typedef dictionary zdb_zone_label_set;

typedef struct zdb_zone_label zdb_zone_label;

struct zdb_zone_label
{
    zdb_zone_label* next;   /* used to link labels with the same hash into a SLL */
    zdb_zone_label_set sub; /* labels of the sub-level                           */
    u8* name;               /* label name                                        */

    zdb_zone *zone; /* zone cut starting at this level                   */
}; /* 32 56 */

typedef zdb_zone_label* zdb_zone_label_pointer_array[DNSNAME_MAX_SECTIONS];

/* zdb */

#define ZDB_MUTEX_NOBODY GROUP_MUTEX_NOBODY
#define ZDB_MUTEX_READER 0x01
#define ZDB_MUTEX_WRITER 0x82                   // only one allowed at once
typedef struct zdb zdb;

#define ZDBCLASS_TAG 0x5353414c4342445a

struct zdb
{
    zdb_zone_label* root;
    alarm_t alarm_handle;
    group_mutex_t mutex;
    u16 zclass;
};

typedef zdb_ttlrdata** zdb_ttlrdata_pointer_array;

/*
 *
 */

typedef struct zdb_query_ex_answer zdb_query_ex_answer;

struct zdb_query_ex_answer
{
    zdb_resourcerecord *answer;
    zdb_resourcerecord *authority;
    zdb_resourcerecord *additional;
    u8 depth;           // CNAME
    u8 delegation;      // set as an integer to avoid testing for it
};

/**
 * Iterator through the (rr) labels in a zone
 */

typedef struct zdb_zone_label_iterator zdb_zone_label_iterator;

#define ZDB_ZONE_LABEL_ITERATOR_CAN_SKIP_CHILDREN 0

struct zdb_zone_label_iterator /// 47136 bytes on a 64 bits architecture
{
    const zdb_zone* zone;
    zdb_rr_label* current_label;
    s32 top;
    s32 current_top;    /* "top" of the label pointer by current_label  */
#if ZDB_ZONE_LABEL_ITERATOR_CAN_SKIP_CHILDREN
    s32 prev_top;       /* "top" of the label returned with "_next"     */
    s32 __reserved__;
#endif
    dnslabel_stack dnslabels;
    dictionary_iterator stack[DNSNAME_MAX_SECTIONS];
};

#define RRL_PROCEED         0
#define RRL_SLIP            1
#define RRL_DROP            2

typedef ya_result rrl_process_callback(message_data *mesg, zdb_query_ex_answer *ans_auth_add);

u8 zdb_zone_get_status(zdb_zone *zone);
u8 zdb_zone_set_status(zdb_zone *zone, u8 status);
u8 zdb_zone_clear_status(zdb_zone *zone, u8 status);

#ifdef	__cplusplus
}
#endif

#endif	/* _ZDB_TYPES_H */

/** @} */
