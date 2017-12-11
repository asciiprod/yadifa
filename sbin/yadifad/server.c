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

/**
 *  @defgroup server Server
 *  @ingroup yadifad
 *  @brief Server initialisation and launch
 *
 *  Starts server
 *
 * @{
 */
/*----------------------------------------------------------------------------*/

#define SERVER_C_

/** @note: here we define the variable that is holding the default logger handle for the current source file
 *         Such a handle should NEVER been set in an include file.
 */

#include "server-config.h"
#include "config.h"

#include <dnscore/logger.h>
#include <dnscore/fdtools.h>
#include <dnscore/tcp_io_stream.h>
#include <dnscore/thread_pool.h>
#include <dnscore/ctrl-rfc.h>
#include <dnscore/service.h>

logger_handle *g_server_logger;
#define MODULE_MSG_HANDLE g_server_logger

#include "signals.h"
#include "database-service.h"
#include "log_query.h"
#include "poll-util.h"
#include "server-mt.h"
#include "server-rw.h"
#include "notify.h"
#include "server_context.h"
#include "axfr.h"
#include "ixfr.h"
#include "process_class_ch.h"
#if HAS_DYNUPDATE_SUPPORT
#include "dynupdate_query_service.h"
#endif
#if HAS_CTRL
#include "ctrl.h"
#include "ctrl_query.h"

#endif
#if HAS_RRL_SUPPORT
#include "rrl.h"
#endif


// DEBUG build: log debug 5 of incoming wire
#define DUMP_TCP_RECEIVED_WIRE 0

// DEBUG build: log debug 5 of outgoing wire
#define DUMP_TCP_OUTPUT_WIRE 0

static struct thread_pool_s *server_tcp_thread_pool = NULL;
struct thread_pool_s *server_disk_thread_pool = NULL;

#include "server.h"

server_statistics_t server_statistics;
static bool server_statistics_initialised = FALSE;

volatile int program_mode = SA_CONT; /** @note must be volatile */

static struct server_desc_s server_type[2] =
{
    {
        server_mt_context_init,
        server_mt_query_loop,
        "multithreaded resolve"
    },
    {
        server_rw_context_init,
        server_rw_query_loop,
        "multithreaded deferred resolve"
    }
};





/*******************************************************************************************************************
 *
 * TCP protocol
 *
 ******************************************************************************************************************/

void
tcp_send_message_data(message_data* mesg)
{
    mesg->buffer_tcp_len[0]       = (mesg->send_length >> 8);
    mesg->buffer_tcp_len[1]       = (mesg->send_length);

    /*
     * Message status cannot be used here to set the rcode.
     * The main reason being : it is better done when the message is built
     * The other reason being : OPT contains extended codes. A pain to parse and handle here.
     */

    //yassert(((mesg->status < 15) && ((MESSAGE_LOFLAGS(mesg->buffer) & RCODE_BITS) == mesg->status)) || (mesg->status >= 15) );

#ifdef DEBUG
    log_debug("tcp: answering %d bytes @%p to socket %d", mesg->send_length + 2, mesg->buffer_tcp_len, mesg->sockfd);
#if DUMP_TCP_OUTPUT_WIRE
    log_memdump_ex(g_server_logger, MSG_DEBUG5, mesg->buffer, mesg->send_length, 16, OSPRINT_DUMP_HEXTEXT);
#endif
#endif
    
    /**
     * SAME AS READ : THERE HAS TO BE A RATE !
     */
#if !HAS_DROPALL_SUPPORT
    ya_result sent;
    
    if(FAIL(sent = writefully_limited(mesg->sockfd, mesg->buffer_tcp_len, mesg->send_length + 2, g_config->tcp_query_min_rate_us)))
    {
        log_err("tcp write error: %r", sent);

        tcp_set_abortive_close(mesg->sockfd);
    }
#endif
}

/** \brief Does the tcp processing
 *
 *  When pselect has an TCP request, this function reads the tcp packet,
 *  processes dns packet and send reply
 *
 *  @param[in,out] mesg
 *
 *  @retval OK
 *  @return status of message is written in mesg->status
 */

static int
server_process_tcp_task(zdb *database, message_data *mesg, u16 svr_sockfd)
{
    ya_result                                   return_code = SUCCESS;

    u16                                                 dns_query_len;
    ssize_t                                                  received;
    ssize_t                                         next_message_size;

#ifdef DEBUG
    log_info("tcp: processing socket %i (%{sockaddr})", mesg->sockfd, &mesg->other.sa);
#endif
    
    int loop_count = 0;
    
    tcp_set_recvtimeout(mesg->sockfd, 1, 0);
    tcp_set_nodelay(mesg->sockfd, TRUE);
    tcp_set_cork(mesg->sockfd, FALSE);
    
    s64 tstart = (s64)timeus();
    
    /** @note do a full read, not one that can be interrupted or deliver only a part of what we need (readfully) */
    while((next_message_size = readfully_limited(mesg->sockfd, &dns_query_len, 2, g_config->tcp_query_min_rate_us)) == 2)
    {
        ++loop_count;
        
#ifdef DEBUG
        log_debug("tcp: loop count = %d", loop_count);
#endif
        
        u16 native_dns_query_len = ntohs(dns_query_len);

        if(native_dns_query_len == 0)
        {
            log_err("tcp: message size is 0");

            /** @todo 20120706 edf -- no linger, check the best place to do it */

            return_code = UNPROCESSABLE_MESSAGE;

            break;
        }

        /** 
         *  read with an expected rate
         */

        if((received = readfully_limited(mesg->sockfd, mesg->buffer, native_dns_query_len, g_config->tcp_query_min_rate_us)) != native_dns_query_len)
        {
            if(ISOK(received))
            {
                log_err("tcp: message read: received %d bytes but %hd were expected", received, native_dns_query_len);
            }
            else
            {
                log_err("tcp: message read: %r", ERRNO_ERROR);
            }
            
            mesg->received = 0;

            tcp_set_abortive_close(mesg->sockfd);

            return_code = UNPROCESSABLE_MESSAGE;

            break;
        }
        
        mesg->received = received;
        
#ifdef DEBUG
#if DUMP_TCP_RECEIVED_WIRE
        log_memdump_ex(g_server_logger, MSG_DEBUG5, mesg->buffer, mesg->received, 16, OSPRINT_DUMP_HEXTEXT);
#endif
#endif

        mesg->protocol = IPPROTO_TCP;
        
        switch(MESSAGE_OP(mesg->buffer))
        {
            case OPCODE_QUERY:
            {
                if(ISOK(return_code = message_process_query(mesg)))
                {
                    mesg->size_limit = DNSPACKET_MAX_LENGTH;

                    switch(mesg->qclass)
                    {
                        case CLASS_IN:
                        {
                            log_query(svr_sockfd, mesg);                                                        

                            if(mesg->qtype == TYPE_AXFR)
                            {
                                /*
                                 * Start an AXFR "writer" thread
                                 * Give it the tcp fd
                                 * It will store the current AXFR on the disk if it does not exist yet (writers blocked)
                                 * It will then open the stored file and stream it back to the tcp fd (writers freed)
                                 * ACL/TSIG is not taken in account yet.
                                 */

                                TCPSTATS(tcp_axfr_count++);

                                return_code = axfr_process(mesg);

    #ifdef DEBUG
                                log_debug("server_process_tcp scheduled : %r", return_code);
    #endif

                                return return_code; /* AXFR PROCESSING: process then closes: all in background */
                            }

                            if(mesg->qtype == TYPE_IXFR)
                            {
                                /*
                                 * Start an IXFR "writer" thread
                                 * Give it the tcp fd
                                 * It will either send the incremental changes (stored on the disk), either answer with an AXFR
                                 * ACL/TSIG is not taken in account yet.
                                 */

                                TCPSTATS(tcp_ixfr_count++);
                                return_code = ixfr_process(mesg);

    #ifdef DEBUG
                                log_debug("server_process_tcp scheduled : %r", return_code);
    #endif

                                return return_code; /* IXFR PROCESSING: process then closes: all in background */
                            }

    #ifdef DEBUG
                            log_debug("server_process_tcp query");
    #endif

                            TCPSTATS(tcp_queries_count++);

                            /*
                             * This query must go through the task channel.
                             */

                            database_query(database, mesg);

    #ifdef DEBUG
                            log_debug("server_process_tcp write");
    #endif

                            tcp_send_message_data(mesg);

                            TCPSTATS(tcp_referrals_count += mesg->referral);
                            TCPSTATS(tcp_fp[mesg->status]++);
                            TCPSTATS(tcp_output_size_total += mesg->send_length);

                            break;
                        } // case query IN
                        case CLASS_CH:
                        {
                            log_query(svr_sockfd, mesg);
                            class_ch_process(mesg);
                            TCPSTATS(tcp_fp[mesg->status]++);
                            tcp_send_message_data(mesg);

                            break;
                        }
                        default:
                        {

                            message_make_error(mesg, FP_NOT_SUPP_CLASS);
                            TCPSTATS(tcp_fp[FP_NOT_SUPP_CLASS]++);
                            break;
                        }
                    } // query class
                } // if message process succeeded
                else // an error occurred : no query to be done at all
                {
                    log_warn("query [%04hx] error %i : %r", ntohs(MESSAGE_ID(mesg->buffer)), mesg->status, return_code);

                    TCPSTATS(tcp_fp[mesg->status]++);
                    
                    if(return_code == UNPROCESSABLE_MESSAGE && (g_config->server_flags & SERVER_FL_LOG_UNPROCESSABLE))
                    {
                        log_memdump_ex(MODULE_MSG_HANDLE, MSG_DEBUG, mesg->buffer, mesg->received, 16, OSPRINT_DUMP_ALL);
                    }
                    
                    if( (return_code != INVALID_MESSAGE) && (((g_config->server_flags & SERVER_FL_ANSWER_FORMERR) != 0) || mesg->status != RCODE_FORMERR) && (MESSAGE_QR(mesg->buffer) == 0) )
                    {
                        if(!MESSAGEP_HAS_TSIG(mesg))
                        {
                            message_transform_to_error(mesg);
                        }

                        tcp_send_message_data(mesg);
                    }
                    else
                    {
                        TCPSTATS(tcp_dropped_count++);
                        tcp_set_agressive_close(mesg->sockfd, 1);
                    }
                }

                break;
            } // case query

            case OPCODE_NOTIFY:
            {
                if(ISOK(return_code = message_process(mesg)))
                {
                    mesg->size_limit = DNSPACKET_MAX_LENGTH;

                    switch(mesg->qclass)
                    {
                        case CLASS_IN:
                        {
                            /// @todo 20140521 edf -- notify on TCP

                            TCPSTATS(tcp_notify_input_count++);
                            break;
                        }
                        default:
                        {

                            message_make_error(mesg, FP_NOT_SUPP_CLASS);
                            TCPSTATS(tcp_fp[FP_NOT_SUPP_CLASS]++);
                            break;
                        }
                    } // notify class
                } // if message process succeeded
                else // an error occurred : no query to be done at all
                {
                    log_warn("notify [%04hx] error %i : %r", ntohs(MESSAGE_ID(mesg->buffer)), mesg->status, return_code);

                    TCPSTATS(tcp_fp[mesg->status]++);
#ifdef DEBUG
                    log_memdump_ex(MODULE_MSG_HANDLE, MSG_DEBUG5, mesg->buffer, mesg->received, 16, OSPRINT_DUMP_ALL);
#endif
                    if( (return_code != INVALID_MESSAGE) && (((g_config->server_flags & SERVER_FL_ANSWER_FORMERR) != 0) || mesg->status != RCODE_FORMERR) && (MESSAGE_QR(mesg->buffer) == 0) )
                    {
                        if(!MESSAGEP_HAS_TSIG(mesg))
                        {
                            message_transform_to_error(mesg);
                        }

                        tcp_send_message_data(mesg);
                    }
                    else
                    {
                        TCPSTATS(tcp_dropped_count++);
                        tcp_set_agressive_close(mesg->sockfd, 1);
                    }
                }
                break;
            } // case notify
            case OPCODE_UPDATE:
            {
                if(ISOK(return_code = message_process(mesg)))
                {
                    switch(mesg->qclass)
                    {
                        case CLASS_IN:
                        {
                            /*
                             * _ Post an update on the scheduler
                             * _ wait for the end of the update
                             * _ proceed
                             */

                            /**
                             * @note It's the responsibility of the called function (or one of its callees) to ensure
                             *       this does not take much time and thus to trigger a background task with the
                             *       scheduler if needed.
                             */

#if HAS_DYNUPDATE_SUPPORT
                            
                            TCPSTATS(tcp_updates_count++);

                            log_info("update (%04hx) %{dnsname} %{dnstype} (%{sockaddr})",
                                    ntohs(MESSAGE_ID(mesg->buffer)),
                                    mesg->qname,
                                    &mesg->qtype,
                                    &mesg->other.sa);

                            if(ISOK(database_update(database, mesg)))
                            {
                                tcp_send_message_data(mesg);
                                TCPSTATS(tcp_fp[mesg->status]++);
                            }
#else
                            message_make_error(mesg, FP_FEATURE_DISABLED);
                            tcp_send_message_data(mesg);
                            TCPSTATS(tcp_fp[FP_FEATURE_DISABLED]++);
#endif

                            break;
                        } // update class IN
                        default:
                        {

                            message_make_error(mesg, FP_NOT_SUPP_CLASS);
                            TCPSTATS(tcp_fp[FP_NOT_SUPP_CLASS]++);
                            break;
                        }
                    } // update class
                } // if message process succeeded
                else // an error occurred : no query to be done at all
                {
                    log_warn("update [%04hx] error %i : %r", ntohs(MESSAGE_ID(mesg->buffer)), mesg->status, return_code);

                    TCPSTATS(tcp_fp[mesg->status]++);
#ifdef DEBUG
                    log_memdump_ex(MODULE_MSG_HANDLE, MSG_DEBUG5, mesg->buffer, mesg->received, 16, OSPRINT_DUMP_ALL);
#endif
                    if( (return_code != INVALID_MESSAGE) && (((g_config->server_flags & SERVER_FL_ANSWER_FORMERR) != 0) || mesg->status != RCODE_FORMERR) && (MESSAGE_QR(mesg->buffer) == 0) )
                    {
                        if(!MESSAGEP_HAS_TSIG(mesg))
                        {
                            message_transform_to_error(mesg);
                        }

                        tcp_send_message_data(mesg);
                    }
                    else
                    {
                        TCPSTATS(tcp_dropped_count++);
                        tcp_set_agressive_close(mesg->sockfd, 1);
                    }
                }
                break;
            } // case update
#if HAS_CTRL
            case OPCODE_CTRL:
            {
                if(ISOK(return_code = message_process(mesg)))
                {
                    switch(mesg->qclass)
                    {
                        case CLASS_CTRL:
                        {
                            ctrl_query_process(mesg);
                            break;
                        } // ctrl class CTRL


                        default:
                        {
                            /**
                             * @todo 20140521 edf -- Handle unknown classes better
                             */
                            log_warn("query [%04hx] %{dnsname} %{dnstype} %{dnsclass} (%{sockaddrip}) : unsupported class",
                                            ntohs(MESSAGE_ID(mesg->buffer)),
                                            mesg->qname, &mesg->qtype, &mesg->qclass,
                                            &mesg->other.sa);

                            mesg->status = FP_CLASS_NOTFOUND;
                            message_transform_to_error(mesg);

                            break;
                        }
                    } /* switch class */
                    
                    if(mesg->status != FP_PACKET_DROPPED)
                    {
                        TCPSTATS(tcp_fp[mesg->status]++);
                        tcp_send_message_data(mesg);
                    }
                    else
                    {
                        TCPSTATS(tcp_dropped_count++);
                        tcp_set_agressive_close(mesg->sockfd, 1);
                    }
                }
                else // an error occurred : no query to be done at all
                {
                    log_warn("ctrl [%04hx] error %i : %r", ntohs(MESSAGE_ID(mesg->buffer)), mesg->status, return_code);

                    TCPSTATS(tcp_fp[mesg->status]++);
#ifdef DEBUG
                   log_memdump_ex(MODULE_MSG_HANDLE, MSG_DEBUG5, mesg->buffer, mesg->received, 16, OSPRINT_DUMP_ALL);
#endif
                    if( (return_code != INVALID_MESSAGE) &&
                        (((g_config->server_flags & SERVER_FL_ANSWER_FORMERR) != 0) || mesg->status != RCODE_FORMERR) &&
                        (MESSAGE_QR(mesg->buffer) == 0) )
                    {
                        if(mesg->tsig.tsig == NULL)
                        {
                            message_transform_to_error(mesg);
                            tcp_send_message_data(mesg);
                        }
                        else
                        {
                            /// @todo 20150428 edf -- handle this more nicely
                            
                            TCPSTATS(tcp_dropped_count++);
                            tcp_set_agressive_close(mesg->sockfd, 1);
                        }
                    }
                    else
                    {
                        TCPSTATS(tcp_dropped_count++);
                        tcp_set_agressive_close(mesg->sockfd, 1);
                    }
                }

                break;
            } // case ctrl
#endif // HAS_CTRL
            default:
            {
                log_warn("unknown [%04hx] error: %r", ntohs(MESSAGE_ID(mesg->buffer)), MAKE_DNSMSG_ERROR(mesg->status));
                
                if( (return_code != INVALID_MESSAGE) && (((g_config->server_flags & SERVER_FL_ANSWER_FORMERR) != 0) || mesg->status != RCODE_FORMERR) && (MESSAGE_QR(mesg->buffer) == 0) )
                {
                    if(!MESSAGEP_HAS_TSIG(mesg))
                    {
                        message_transform_to_error(mesg);
                    }

                    tcp_send_message_data(mesg);
                }
                else
                {
                    TCPSTATS(tcp_dropped_count++);
                    tcp_set_agressive_close(mesg->sockfd, 1);
                }
            }
        } // switch operation code
    } // while received bytes

    if(loop_count > 0)
    {
        // If the next message size is not 2, then we didn't had to expect a message     
        
        if(next_message_size == 2)
        {
            // If we have got an error while receiving (tcp too slow), then abort the connection

            if(ISOK(received))
            {
                tcp_set_agressive_close(mesg->sockfd, 1);
            }
            else
            {
                log_err("tcp: %{sockaddr} message #%i processing failed: %r", &mesg->other.sa, loop_count, received);
                tcp_set_abortive_close(mesg->sockfd);
            }
        }
        else
        {
            // We processed at least one message but this last one was either
            // non-existent, or truncated, or too slow :
            //
            // We give it a second and we close.
            
            tcp_set_agressive_close(mesg->sockfd, 1);
        }
    }
    else
    {
        s64 d = MAX((s64)timeus() - tstart, 0);
        double s = d / 1000000.;
        
        if(next_message_size < 0)
        {
            log_err("tcp: %{sockaddr} connection didn't sent the message size after %5.3fs: %r", &mesg->other.sa, s, next_message_size);
        }
        else if(next_message_size > 0) // a.k.a : 1 
        {
            log_err("tcp: %{sockaddr} connection didn't sent the message size after %5.3fs", &mesg->other.sa, s);
        }
        else
        {
            log_err("tcp: %{sockaddr} connection closed after %5.3fs", &mesg->other.sa, s);
        }
        
        tcp_set_abortive_close(mesg->sockfd);
    }

#ifdef DEBUG
	log_info("tcp: closing socket %i, loop count = %d", mesg->sockfd, loop_count);
#endif

    close_ex(mesg->sockfd);
    
    mesg->sockfd = -1;

    return return_code;
}

typedef struct server_process_tcp_thread_parm server_process_tcp_thread_parm;

struct server_process_tcp_thread_parm
{
    zdb *database;
    socketaddress sa;
    socklen_t addr_len;
    int sockfd;
    int svr_sockfd;
};

static void*
server_process_tcp_thread(void* parm)
{
#ifdef DEBUG
    log_debug("tcp: begin");
#endif

    server_process_tcp_thread_parm* tcp_parm = (server_process_tcp_thread_parm*)parm;
    message_data mesg;

#ifdef DEBUG
    memset(&mesg, 0xff, sizeof(message_data));
#endif

    mesg.sockfd = tcp_parm->sockfd;

    mesg.process_flags = ~0; /** @todo 20120706 edf -- FIX ME */

    memcpy(&mesg.other, &tcp_parm->sa, tcp_parm->addr_len);
    mesg.addr_len = tcp_parm->addr_len;

    server_process_tcp_task(tcp_parm->database, &mesg, tcp_parm->svr_sockfd);

    free(parm);

#ifdef DEBUG
    log_debug("tcp: end");
#endif

    return NULL;
}

void
server_process_tcp(zdb *database, int sockfd)
{
    server_process_tcp_thread_parm* parm;
    socklen_t addr_len;
    socketaddress addr;
    addr_len = sizeof(addr);
    

    /*
     * AFAIK there are two relevant fields in mesg at this point: addr & sockfd
     * After the accept only the sockfd is relevant
     */

    /* I know I'm already in an #if with the same condition but I want to mark
     * the code I've c&p from the original do_tcp_process
     */

#ifdef DEBUG
    log_debug("server_process_tcp_thread_start begin");
#endif

    int current_tcp = poll_update();

    /**
     * @note we MAY want to accept & close before rejecting.  But in case of a DOS we lose.
     *       here we will just ignore until it's possible to do something about it (or it's cancelled)
     *
     */

    if(current_tcp >= g_config->max_tcp_queries)
    {
        log_info("tcp: rejecting: already %d/%d handled", current_tcp, g_config->max_tcp_queries);
        
        int rejected_fd = accept(sockfd, &addr.sa, &addr_len);
        
        tcp_set_abortive_close(rejected_fd);
        close_ex(rejected_fd);

        TCPSTATS(tcp_overflow_count++);
        
        return;
    }

    TCPSTATS(tcp_input_count++);

    MALLOC_OR_DIE(server_process_tcp_thread_parm*, parm, sizeof(server_process_tcp_thread_parm), TPROCPRM_TAG);
    parm->database = database;

    /** @todo 20120706 edf -- test: timeout */

    /* don't test -1, test < 0 instead (test + js instead of add + stall + jz */
    while((parm->sockfd = accept(sockfd, &addr.sa, &addr_len)) < 0)
    {
        int err = errno;

        if(err != EINTR)
        {
            log_err("tcp: accept returned %r", MAKE_ERRNO_ERROR(err));
            free(parm);
            return;
        }
    }

    if(addr_len > MAX(sizeof(struct sockaddr_in),sizeof(struct sockaddr_in6)))
    {
        log_err("tcp: addr_len = %i, max allowed is %i", addr_len, MAX(sizeof(struct sockaddr_in),sizeof(struct sockaddr_in6)));

        close_ex(parm->sockfd);
        
        free(parm);

        return;
    }

    memcpy(&parm->sa, &addr, addr_len);
    parm->addr_len = addr_len;
    parm->svr_sockfd = sockfd;
    
    if(poll_add(parm->sockfd))
    {
        log_debug("tcp: using slot %d/%d", current_tcp + 1 , g_config->max_tcp_queries);

        /*
         * And here is the AXFR change: if it's an AXFR, then we need to ensure that
         * _ we are allowed (TSIG, time limit between two AXFR "milestones", ...)
         * _ we have the AXFR file ready and if not, fork to generate it
         *
         * The thread is launched anyway and waits for the file with the right serial to be generated.
         * When the file is finally available, it is sent to the caller.
         *
         * If it's not an AXFR, then we do as ever.
         */

#ifdef DEBUG
        log_debug("server_process_tcp_thread_start scheduling job");
#endif

        thread_pool_enqueue_call(server_tcp_thread_pool, server_process_tcp_thread, parm, NULL, "server_process_tcp_thread_start");
    }
    else
    {
        log_debug("server_process_tcp_thread_start tcp overflow");
        
        close_ex(parm->sockfd);
        free(parm);
    }

#ifdef DEBUG
    log_debug("server_process_tcp_thread_start end");
#endif
}

/*******************************************************************************************************************
 *
 * Server init, load, start, stop and exit
 *
 ******************************************************************************************************************/

static struct service_s server_service_handler = UNINITIALIZED_SERVICE;
static bool server_handler_initialised = FALSE;

static ya_result
server_init()
{
    ya_result ret;
    
    server_type[g_config->network_model].context_init(g_config->thread_count_by_address);
    
    log_info("using %i working modules per UDP interface (%i threads per UDP module)", server_context.udp_unit_per_interface, server_context.thread_per_udp_worker_count);
    log_info("using %i working modules per TCP interface (%i threads per TCP module)", server_context.tcp_unit_per_interface, server_context.thread_per_tcp_worker_count);
    
    ret = server_context_start(g_config->listen);
        
    return ret;
}

static ya_result
server_run_loop()
{
    ya_result ret = server_type[g_config->network_model].loop();
    return ret;
}

/** @brief Startup server with all its processes
 *
 *  Never returns. Ends with the program.
 */

static ya_result
server_run()
{
    log_info("server starting with pid %lu", getpid());
    
#if HAS_RRL_SUPPORT
    // Sets the RRL
    
    rrl_init();
#endif
    
    // Resets the statistics

    if(!server_statistics_initialised)
    {
        server_statistics_initialised = TRUE;
        
        ZEROMEMORY(&server_statistics, sizeof(server_statistics_t));
        mutex_init(&server_statistics.mtx);
    }

    // Initialises the TCP thread pool (used to answer to TCP queries)
    
    if((server_tcp_thread_pool != NULL) && (thread_pool_get_size(server_tcp_thread_pool) != g_config->max_tcp_queries))
    {
        // the thread-pool size is wrong
        ya_result return_code;
        
        if(FAIL(return_code = thread_pool_resize(server_tcp_thread_pool, g_config->max_tcp_queries)))
        {
            return return_code;
        }
        
        if(return_code != g_config->max_tcp_queries)
        {
            log_err("could not properly set the TCP handlers");
            return INVALID_STATE_ERROR;
        }
    }
    
    if(server_tcp_thread_pool == NULL && g_config->max_tcp_queries > 0)
    {
        server_tcp_thread_pool = thread_pool_init_ex(g_config->max_tcp_queries, g_config->max_tcp_queries * 2, "svrtcp");
        
        if(server_tcp_thread_pool == NULL)
        {
            log_err("tcp thread pool init failed");
            
            return THREAD_CREATION_ERROR;
        }
    }
    
    if(server_disk_thread_pool == NULL)
    {
        server_disk_thread_pool = thread_pool_init_ex(4, 64, "diskio");
        
        if(server_disk_thread_pool == NULL)
        {
            log_warn("disk thread pool init failed");
            
            return THREAD_CREATION_ERROR;
        }
    }

    OSDEBUG(termout, "I come to serve ...\n");

    log_info("I come to serve ..."); /** I could not resist ... */
    
    /* Initialises the TCP usage limit structure (It's global and defined at the beginning of server.c */

    poll_alloc(g_config->max_tcp_queries);
    


    /* Go to work */
        
    log_debug("thread count by address: %i", g_config->thread_count_by_address);
    
    server_run_loop();
    

    
    /* Proper shutdown. All this could be simply dropped since it takes time for "nothing".
     * But it's good to check that nothing is broken.
     */

    poll_free();
    
    log_info("clearing context");
    
    if((server_tcp_thread_pool != NULL) && (g_config->max_tcp_queries > 0))
    {
        thread_pool_destroy(server_tcp_thread_pool);
        server_tcp_thread_pool = NULL;
    }
    
    if(server_disk_thread_pool != NULL)
    {
        thread_pool_destroy(server_disk_thread_pool);
        server_disk_thread_pool = NULL;
    }
    
    /* Clear config struct and close all fd's */
    server_context_stop(g_config);
    
#if HAS_RRL_SUPPORT
    rrl_finalize();
#endif
    
    return SUCCESS;
}

static int
server_service_main(struct service_worker_s *worker)
{
    ya_result ret;
    
    service_set_servicing(worker);
    
    if(ISOK(ret = server_init()))
    {
        ret = server_run();        
    }
    
    service_set_stopping(worker);
    
    return ret;
}

/**
 * Initialises the DNS server service.
 * 
 * @return 
 */

ya_result
server_service_init()
{
    ya_result ret = ERROR;
    
    if(!server_handler_initialised && ISOK(ret = service_init_ex(&server_service_handler, server_service_main, "Server", 1)))
    {
        if(ISOK(ret = server_init()))
        {
            server_handler_initialised = TRUE;
        }
    }
    
    return ret;
}

bool
server_service_started()
{
    return server_handler_initialised && !service_stopped(&server_service_handler);
}

ya_result
server_service_start()
{
    int err = ERROR;

    if(server_handler_initialised)
    {
        if(service_stopped(&server_service_handler))
        {
            err = service_start(&server_service_handler);
        }
    }

    return err;
}

ya_result
server_service_start_and_wait()
{
    int ret = ERROR;
    
    if(server_handler_initialised)
    {
        if(service_stopped(&server_service_handler))
        {
            ret = service_start_and_wait(&server_service_handler);
        }
    }
    
    return ret;
}

ya_result
server_service_wait()
{
    int ret = ERROR;
    if(server_handler_initialised)
    {
        if(ISOK(ret = service_wait_servicing(&server_service_handler)))
        {
            ret = ERROR;
            if(service_servicing(&server_service_handler))
            {
                ret = service_wait(&server_service_handler);
            }
        }
    }
    return ret;
}

ya_result
server_service_stop()
{
    int err = ERROR;
    
    if(server_handler_initialised)
    {
        if(!service_stopped(&server_service_handler))
        {
            err = service_stop(&server_service_handler);
            service_wait(&server_service_handler);
        }
    }
    
    return err;
}

ya_result
server_service_finalise()
{
    int err = SUCCESS;
    
    if(server_handler_initialised)
    {
        err = server_service_stop();
        
        service_finalize(&server_service_handler);
        
        server_handler_initialised = FALSE;
    }

    return err;
}

/** @} */
