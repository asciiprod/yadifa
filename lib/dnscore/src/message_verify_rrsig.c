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
#include "dnscore/sys_types.h"
#include "dnscore/message_verify_rrsig.h"
#include "dnscore/packet_reader.h"
#include "dnscore/format.h"
#include "dnscore/treeset.h"

extern logger_handle *g_system_logger;
#define MODULE_MSG_HANDLE g_system_logger

#define TYPE_FQDN_HAS_RECORDS                   1
#define TYPE_FQDN_HAS_SIGNATURES                2
//#define TYPE_FQDN_HAS_VERIFIED_SIGNATURES       4
//#define TYPE_FQDN_HAS_WRONG_SIGNATURES          8
//#define TYPE_FQDN_HAS_UNKNOWN_SIGNATURES       16

static s32
message_verify_rrsig_compute_digest(const u8 *owner, u16 rtype, u16 rclass,
                                    const u8 *rrsig_rdata, u32 rrsig_rdata_size,
                                    ptr_vector *rrset_canonised_rdata,
                                    u8 *out_digest, u32 out_digest_size)
{
    log_debug6("message_verify_rrsig_compute_digest(%{dnsname},%{dnstype},%{dnsclass},@%p,%u,@%p,@%p)",
                    owner, &rtype, &rclass, rrsig_rdata, rrsig_rdata_size, rrset_canonised_rdata, out_digest);

    u8 rr_header[2 + 2 + 4];
    
    if(rrsig_rdata_size < 18)
    {
        return INCORRECT_RDATA;
    }
    
    if(rtype != GET_U16_AT(rrsig_rdata[0]))
    {
        return RRSIG_COVERED_TYPE_DIFFERS;
    }    

    s32 owner_len = dnsname_len(owner);
        
    u32 rttl = GET_U32_AT(rrsig_rdata[4]);
    
    SET_U16_AT(rr_header[0], rtype);
    SET_U16_AT(rr_header[2], rclass);
    SET_U32_AT(rr_header[4], rttl);
    
    digest_s ctx;
    
    ya_result err;
    if(FAIL(err = dnskey_digest_init(&ctx, rrsig_rdata[2])))
    {
        log_err("message_verify_rrsig_compute_digest: %r", err);
        return err;
    }
    
    if(digest_get_size(&ctx) > out_digest_size)
    {
        return RRSIG_OUTPUT_DIGEST_SIZE_TOO_BIG;
    }

    /*
     * Type covered | algorithm | labels | original_ttl | exp | inception | tag | origin
     *
     */
    
    u32 rrsig_rdata_prefix_size = 18 + dnsname_len(&rrsig_rdata[18]);
        
    digest_update(&ctx, rrsig_rdata, rrsig_rdata_prefix_size);
    
#ifdef DEBUG
    log_memdump(MODULE_MSG_HANDLE, MSG_DEBUG6, rrsig_rdata, rrsig_rdata_prefix_size, 32);
#endif
    
    for(s32 i = 0; i <= rrset_canonised_rdata->offset; i++)
    {
        digest_update(&ctx, owner, owner_len);
        
#ifdef DEBUG
        log_memdump(MODULE_MSG_HANDLE, MSG_DEBUG6, owner, owner_len, 32);
#endif
        
        digest_update(&ctx, rr_header, sizeof(rr_header));
        
#ifdef DEBUG
        log_memdump(MODULE_MSG_HANDLE, MSG_DEBUG6, rr_header, sizeof(rr_header), 32);
#endif
        
        u8 *rdata_size_rdata = (u8*)rrset_canonised_rdata->data[i];
        u16 rdata_size = ntohs(GET_U16_AT(rdata_size_rdata[0]));
        digest_update(&ctx, rdata_size_rdata, rdata_size + 2);
           
#ifdef DEBUG
        log_memdump(MODULE_MSG_HANDLE, MSG_DEBUG6, rdata_size_rdata, rdata_size + 2, 32);
#endif
    }
    
    /*
     * Retrieve the digest
     */

    digest_final(&ctx, out_digest, out_digest_size);
    
#ifdef DEBUG
    log_debug6("digest:");
    log_memdump(MODULE_MSG_HANDLE, MSG_DEBUG6, out_digest, out_digest_size, 32);
#endif

#if RRSIG_DUMP!=0
    log_debug5("rrsig: digest:");
    log_memdump_ex(MODULE_MSG_HANDLE, MSG_DEBUG5, digest_out, out_digest_size, 32, OSPRINT_DUMP_HEX);
#endif
    
    return digest_get_size(&ctx);
}

static int
message_verify_canonize_sort_rdata_compare(const void *a, const void *b)
{
    u8* ptr_a = *((u8**)a);
    u8* ptr_b = *((u8**)b);
    
    u16 rr_a_size = ntohs(GET_U16_AT(ptr_a[0]));
    u16 rr_b_size = ntohs(GET_U16_AT(ptr_b[0]));

    int ret;

    ptr_a += 2;
    ptr_b += 2;

    int diff_len = rr_a_size - rr_b_size;

    if(diff_len != 0)
    {
        u16 len = MIN(rr_a_size, rr_b_size);

        ret = memcmp(ptr_a, ptr_b, len);

        if(ret == 0)
        {
            ret = diff_len;
        }
    }
    else
    {
        ret = memcmp(ptr_a, ptr_b, rr_a_size);
    }

    return ret;
}

static ya_result
message_verify_rrsig_result_default_handler(const message_data *mesg, const struct dnskey_keyring *keyring, const message_verify_rrsig_result_s *result, void *args)
{
    return MESSAGE_VERIFY_RRSIG_FEEDBACK_CONTINUE;
}

static void
message_verify_rrsig_free_rrset(void *data)
{
    free(data);
}

static int message_verify_rrsig_node_compare(const void *key_a, const void *key_b)
{
    const u8 *fqdn_a = key_a;
    const u8 *fqdn_b = key_b;
    
    s32 type_a = GET_U16_AT_P(fqdn_a);
    s32 type_b = GET_U16_AT_P(fqdn_b);
    
    s32 ret = type_a - type_b;
    
    if(ret == 0)
    {
        fqdn_a += 2;
        fqdn_b += 2;
        
        ret = dnsname_compare(fqdn_a, fqdn_b);
    }
    
    return ret;
}

static void
message_verify_rrsig_init(treeset_tree *section_type_fqdn)
{
    treeset_avl_init(section_type_fqdn);
    section_type_fqdn->compare = message_verify_rrsig_node_compare;
}

static void
message_verify_rrsig_set_flag(treeset_tree *section_type_fqdn, const u8 *type_record_fqdn, u32 type_record_fqdn_len, u8 flag_bits)
{
    // create the type-fqdn entry if needed
    treeset_node *type_fqdn_node = treeset_avl_find(section_type_fqdn, type_record_fqdn);
    if(type_fqdn_node == NULL)
    {
#ifdef DEBUG
        log_debug7("message_verify_rrsig: new node %{dnsname} %{dnstype}", type_record_fqdn + 2, type_record_fqdn);
#endif
        
        u8 *type_record_fqdn_copy;
        MALLOC_OR_DIE(u8*,type_record_fqdn_copy, type_record_fqdn_len, GENERIC_TAG);
        memcpy(type_record_fqdn_copy, type_record_fqdn, type_record_fqdn_len);
        type_fqdn_node = treeset_avl_insert(section_type_fqdn, type_record_fqdn_copy);
        type_fqdn_node->data = NULL; // has records, has verified signatures, has wrong signatures, has unknown signatures

        // the next phase will scan for each of there types instead
    }
    intptr flag = (intptr)type_fqdn_node->data;
    
#ifdef DEBUG
    log_debug7("message_verify_rrsig: set node %{dnsname} %{dnstype} %x => %x", type_record_fqdn + 2, type_record_fqdn, flag, flag | flag_bits);
#endif
    
    flag |= flag_bits;
    type_fqdn_node->data = (void*)flag;
}

static void
message_verify_rrsig_clear_callback(void *node)
{
    treeset_node *type_fqdn_node = (treeset_node*) node;
    free(type_fqdn_node->key);
}

static void
message_verify_rrsig_clear(treeset_tree *section_type_fqdn)
{
    // create the type-fqdn entry if needed
    treeset_avl_callback_and_destroy(section_type_fqdn, message_verify_rrsig_clear_callback);
}

ya_result
message_verify_rrsig(const message_data *mesg, struct dnskey_keyring *keyring, message_verify_rrsig_result *feedback, void *args)
{
    // for the answer, authority and additional sections
    //   count the RRSIG in the section for which we have a key
    //   for all rrset but the RRSIG one
    //     if the rrset has no signature
    //       feedback ...
    //     elseif the rrset has at least one signature that can be verified
    //       for each non-verifiable signature
    //         feedback ...
    //       rof
    //       canonize the rrset
    //       for each verifiable signature   
    //         verify the signature
    //         feedback ...
    //       rof
    //     fi
    //   rof
    // rof
    
    if(keyring == NULL)
    {
        return 0;
    }
    
    message_verify_rrsig_result_s result;
    time_t now;
    u32 total_wrong_signatures = 0;
    ya_result return_code;
    packet_unpack_reader_data pr;
    ya_result feedback_result = MESSAGE_VERIFY_RRSIG_FEEDBACK_CONTINUE;
    u32 type_record_fqdn_len;
    u8 type_record_fqdn[2 + 256];
    u8 record_buffer[8192];
    
    if(feedback == NULL)
    {
        feedback = message_verify_rrsig_result_default_handler;
    }
    
    result.data.any = NULL;
    result.result_type = 0;
    result.section = 0;
    result.ctype = 0;
    
    packet_reader_init(&pr, mesg->buffer, mesg->received);

    pr.offset = DNS_HEADER_LENGTH;
    
    if(FAIL(return_code = packet_reader_skip_fqdn(&pr)))
    {
        return return_code;
    }
    
    if(FAIL(return_code = packet_reader_skip(&pr, 4)))
    {
        return return_code;
    }
    
    now = time(NULL);
    
    // for sections
    
    for(u8 section = 1; section < 3; section++) // addtitionals are not processed
    {
        result.section = section;
        result.ctype = TYPE_NONE;

        u32 section_start = pr.offset;
                
        treeset_tree section_type_fqdn;
        message_verify_rrsig_init(&section_type_fqdn);
        
        for(u16 count = ntohs(MESSAGE_SECTION_COUNT(mesg->buffer, section)); count > 0; --count)
        {
            // count RRSIG / types

            if(FAIL(return_code = packet_reader_read_fqdn(&pr, &type_record_fqdn[2], sizeof(type_record_fqdn) - 2)))
            {
                message_verify_rrsig_clear(&section_type_fqdn);
                        
                return return_code;
            }
            
            type_record_fqdn_len = return_code + 2;

            u16 rtype;

            if(FAIL(return_code = packet_reader_read_u16(&pr, &rtype)))
            {
                message_verify_rrsig_clear(&section_type_fqdn);
                
                return return_code;
            }

            if(FAIL(return_code = packet_reader_skip(&pr, 2 + 4)))
            {
                message_verify_rrsig_clear(&section_type_fqdn);
                
                return return_code;
            }
            
            // FQDN + TYPE ( + CLASS ) = key of what can be signed

            u16 rdata_size;

            if(FAIL(return_code = packet_reader_read_u16(&pr, &rdata_size)))
            {
                message_verify_rrsig_clear(&section_type_fqdn);
                
                return return_code;
            }
            
            rdata_size = ntohs(rdata_size);

            if(rtype == TYPE_RRSIG)
            {
                if(rdata_size < 18)
                {
                    message_verify_rrsig_clear(&section_type_fqdn);
                    
                    return INCORRECT_RDATA;
                }

                u16 ctype;

                if(FAIL(return_code = packet_reader_read(&pr, &ctype, 2)))
                {
                    message_verify_rrsig_clear(&section_type_fqdn);
                    
                    return return_code;
                }
                
                if(ctype == TYPE_RRSIG)
                {
                    message_verify_rrsig_clear(&section_type_fqdn);
                    
                    return RRSIG_UNSUPPORTED_COVERED_TYPE;
                }
                
                if(FAIL(return_code = packet_reader_skip(&pr, rdata_size - 2)))
                {
                    message_verify_rrsig_clear(&section_type_fqdn);

                    return return_code;
                }
                
                SET_U16_AT_P(type_record_fqdn, ctype);
                message_verify_rrsig_set_flag(&section_type_fqdn, type_record_fqdn, type_record_fqdn_len, TYPE_FQDN_HAS_SIGNATURES);
            }
            else
            {
                if(FAIL(return_code = packet_reader_skip(&pr, rdata_size)))
                {
                    message_verify_rrsig_clear(&section_type_fqdn);

                    return return_code;
                }
                
                SET_U16_AT_P(type_record_fqdn, rtype);
                message_verify_rrsig_set_flag(&section_type_fqdn, type_record_fqdn, type_record_fqdn_len, TYPE_FQDN_HAS_RECORDS);
            }
        } // count
        
        // at most 5956 records (beside the query) in a message
        
        // all the records of the section have been parsed (once)
        // signatures have been counted (verifiable & unknown)
        
        // for each type encountered ...
        
        message_verify_rrsig_type_summary_s type_info = {0, 0, 0, 0};
        
        treeset_avl_iterator section_types_fqdn_iter;
        
        treeset_avl_iterator_init(&section_type_fqdn, &section_types_fqdn_iter);
        while(treeset_avl_iterator_hasnext(&section_types_fqdn_iter))
        {
            treeset_node *types_fqdn_node = treeset_avl_iterator_next_node(&section_types_fqdn_iter);

            const u8 * type_fqdn = (u8*)types_fqdn_node->key;
            u16 ctype = GET_U16_AT_P(type_fqdn);
            type_fqdn += 2;
            u8 flags = (u8)(intptr)types_fqdn_node->data; // double cast just to explicitely show what is happening
            
#ifdef DEBUG
            log_debug6("message_verify_rrsig: %{dnsname} %{dnstype} (%x)", type_fqdn, &ctype, flags);
#endif
            
            result.ctype = ctype;
            
            if( (flags & (TYPE_FQDN_HAS_RECORDS | TYPE_FQDN_HAS_SIGNATURES)) != (TYPE_FQDN_HAS_RECORDS | TYPE_FQDN_HAS_SIGNATURES) )
            {
                // no signatures or no records
                
                if(flags & TYPE_FQDN_HAS_SIGNATURES)
                {
                    // not signed
                }
                else
                {
                    // signature without record ?
                }
                
                continue;
            }

            // all/some verifiable : report the verified ones
            // build the RRSET (canonised)
            // verify

            // =>

            // rewind to the beginning of the section

            pr.offset = section_start;

            ptr_vector rrset = EMPTY_PTR_VECTOR;

            for(u16 count = ntohs(MESSAGE_SECTION_COUNT(mesg->buffer, section));count > 0; --count)
            {
                // count RRSIG / types

                if(ISOK(return_code = packet_reader_read_record(&pr, record_buffer, sizeof(record_buffer))))
                {
                    u8 *fqdn = record_buffer;

                    if(dnsname_equals(fqdn, type_fqdn))
                    {                        
                        struct type_class_ttl_rdlen *tctr = (struct type_class_ttl_rdlen*)&fqdn[dnsname_len(fqdn)];

                        if(tctr->qtype == ctype) // and fqdn is good ...
                        {
                            // append the record data to the array

                            u8 *rdata = (u8*)tctr;
                            rdata += 10;

                            u8 *rdata_network_size_rdata;
                            u16 rdata_size = ntohs(tctr->rdlen);

                            MALLOC_OR_DIE(u8*, rdata_network_size_rdata, rdata_size + 2, GENERIC_TAG);
                            SET_U16_AT(rdata_network_size_rdata[0], tctr->rdlen);
                            memcpy(&rdata_network_size_rdata[2], rdata, rdata_size);

                            ptr_vector_append(&rrset, rdata_network_size_rdata);
                        }
                    }
                }
                else
                {
                    ptr_vector_free_empties(&rrset, message_verify_rrsig_free_rrset);
                    ptr_vector_destroy(&rrset);

                    return return_code; // impossible at this point
                }
            }

            // rrset contains all the RDATA for the fqdn/type

            ptr_vector_qsort(&rrset, message_verify_canonize_sort_rdata_compare);

#ifdef DEBUG
            for(int i = 0; i <= rrset.offset; i++)
            {
                u8* rdata = rrset.data[i];
                u16 rdata_size = ntohs(GET_U16_AT(rdata[0]));
                rdata += 2;
                rdata_desc rdatadesc = {ctype, rdata_size, rdata};
                log_debug6(" + %{typerdatadesc}", &rdatadesc);
            }
#endif
            // we know the label & cie
            // we have the rdata on canonized order
            // now ... verify

            u32 saved_offset = pr.offset;

            u8 sha1_digest[DIGEST_BUFFER_SIZE];

            // rewind to the beginning of the section

            pr.offset = section_start;

            for(u16 count = ntohs(MESSAGE_SECTION_COUNT(mesg->buffer, section));count > 0;--count)
            {
                // get RRSIG covering RRSET

                if(ISOK(return_code = packet_reader_read_record(&pr, record_buffer, sizeof(record_buffer))))
                {
                    u8 *fqdn = record_buffer;
                    struct type_class_ttl_rdlen *tctr = (struct type_class_ttl_rdlen*)&fqdn[dnsname_len(fqdn)];

                    if( (tctr->qtype == TYPE_RRSIG) && dnsname_equals(type_fqdn, fqdn) )
                    {
                        // append the record data to the array

                        u16 rdata_size = ntohs(tctr->rdlen);

                        u8 *rdata = (u8*)tctr;
                        rdata += 10;

                        if((GET_U16_AT(rdata[0]) == ctype) && (rdata_size > 18)) // if type covered is the one we are processing ...
                        {
#ifdef DEBUG
                            rdata_desc rdatadesc = {TYPE_RRSIG, rdata_size, rdata};
                            log_debug6("with %{dnsname} %{typerdatadesc}", fqdn, &rdatadesc);
#endif
                            message_verify_rrsig_detail_s rrsig_header;

                            memcpy(&rrsig_header, rdata, 18);
                            rrsig_header.result = 0;
                            rrsig_header.section = section;
                            rrsig_header.signer_name = &rdata[18];
                            rrsig_header.fqdn = fqdn;

                            result.data.detail = &rrsig_header;
                            result.result_type = MESSAGE_VERIFY_RRSIG_RESULT_TYPE_VERIFY;

                            u32 inception = ntohl(rrsig_header.inception);
                            u32 expiration = ntohl(rrsig_header.expiration);

                            if((now >= inception) && (now <= expiration))
                            {
                                u16 tag = ntohs(rrsig_header.tag);

                                dnssec_key*	key = dnskey_keyring_get(keyring, rrsig_header.algorithm, tag, rrsig_header.signer_name);

                                if(key != NULL)
                                {
                                    type_info.verifiable_count++;

                                    s32 digest_size = message_verify_rrsig_compute_digest(fqdn, ctype, tctr->qclass,
                                                                                          rdata, rdata_size,
                                                                                          &rrset,
                                                                                          sha1_digest, sizeof(sha1_digest));
                                    assert(digest_size > 0);

                                    u32 rrsig_signer_name_len = dnsname_len(rrsig_header.signer_name);
                                    u32 rrsig_header_len = 18 + rrsig_signer_name_len;

                                    u8 *signature = &rdata[rrsig_header_len];
                                    u32 signature_len = rdata_size - rrsig_header_len;

                                    if(key->vtbl->dnssec_key_verify_digest(key, sha1_digest, digest_size, signature, signature_len))
                                    {
                                        // verified signature with origin/algorithm/tag

                                        type_info.verified_count++;
                                        rrsig_header.result |= MESSAGE_VERIFY_RRSIG_VERIFIED;
                                    }
                                    else
                                    {
                                        // corrupted/wrong signature with origin/algorithm/tag

                                        total_wrong_signatures++;

                                        type_info.wrong_count++;
                                        rrsig_header.result |= MESSAGE_VERIFY_RRSIG_WRONG;
                                    }
                                }
                                else
                                {
                                    type_info.unverifiable_count++;

                                    rrsig_header.result |= MESSAGE_VERIFY_RRSIG_NOKEY;
                                }
                            }
                            else
                            {
                                // wrong time frame

                                type_info.wrong_count++;

                                rrsig_header.result |= MESSAGE_VERIFY_RRSIG_TIMEFRAME;
                            }

                            if((feedback_result = feedback(mesg, keyring, &result, args)) != MESSAGE_VERIFY_RRSIG_FEEDBACK_CONTINUE)
                            {
                                break;
                            }
                        }
                    }
                }
            } // for each signature covering the type

            // break goes here

            pr.offset = saved_offset;

            ptr_vector_free_empties(&rrset, message_verify_rrsig_free_rrset);
            ptr_vector_destroy(&rrset);
            
        } // for all types/fqdn

        // summary of the results for the type

        result.data.summary = &type_info;
        result.result_type = MESSAGE_VERIFY_RRSIG_RESULT_TYPE_SUMMARY;

        if((feedback_result = feedback(mesg, keyring, &result, args)) != MESSAGE_VERIFY_RRSIG_FEEDBACK_CONTINUE)
        {
            // ends the section loop
            //section = 4;
            // breaks the current loop
            break;
        }
        
        // clear the types of the section
        
        message_verify_rrsig_clear(&section_type_fqdn);
        
    } // for all sections
    
    // done
    
    if((total_wrong_signatures == 0) && (feedback_result != MESSAGE_VERIFY_RRSIG_FEEDBACK_ERROR))
    {
        return SUCCESS;
    }
    else
    {
        return RRSIG_VERIFICATION_FAILED;
    }
}
