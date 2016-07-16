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
/** @defgroup ### #######
 *  @ingroup yadifad
 *  @brief
 *
 * @{
 */
/*------------------------------------------------------------------------------
 *
 * USE INCLUDES */

#include "server-config.h"

#define _GNU_SOURCE 1

#include <pthread.h>

#include "config.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#if defined(__linux__) || defined(__gnu_hurd__)
#define _GNU_SOURCE 1
#include <execinfo.h>
#include <sys/mman.h>
#include <ucontext.h>
#elif defined(__sun)
#include <ucontext.h>
#endif

#include <dnscore/logger.h>
#include <dnscore/format.h>
#include <dnscore/fdtools.h>
#include <dnscore/timems.h>

#include "signals.h"
#include "server_context.h"
#include "server.h"
#if HAS_RRSIG_MANAGEMENT_SUPPORT && HAS_DNSSEC_SUPPORT
#include "database-service-zone-resignature.h"
#endif

#define MODULE_MSG_HANDLE g_server_logger
#define MAXTRACE 128

// Let this to 0
// This prevents the coredump from occurring.
// Let's configure this using runtime flags.
#define SIGNAL_HOOK_COREDUMP 1

#define LOGGER_REOPEN_MIN_PERIOD_US 10000000

ya_result database_save_all_zones_to_disk();

/*------------------------------------------------------------------------------
 * GLOBAL VARIABLES */
//extern server_context_t *server_context;

static pthread_t signal_thread = 0;
static mutex_t signal_mutex = MUTEX_INITIALIZER;
//
static volatile time_t signal_task_logger_handle_reopen_last_active = 0;

static volatile int signal_handler_read_fd = -1;
static volatile int signal_handler_write_fd = -1;

static bool sigsegv_trytrace = TRUE;
static bool sigsegv_tryloggerflush = TRUE;

// signal can be lost, a full pipe will not block and lose even more.
// shutdown is thus given an override so that it cannot be lost after
// the signal handler gets it.

static volatile bool signal_shutdown_received = FALSE;

/*------------------------------------------------------------------------------
 * FUNCTIONS */

/*
 * signals are not supposed to be interrupted by other signals
 * still, it happened once, the pthread_create of another signal : deadlock
 * so, here is another check (mutexes are of course forbidden)
 */

// tool to avoid external function calls during the signal

int
signal_strcat(char *dest, const char* src)
{
    char *p = dest;
    
    while(*p != '\0')
    {
        p++;
    }
    
    while(*src != '\0')
    {
        *p++ = *src++;
    }
    
    *p = '\0';
    
    return p - dest;
}

// tool to avoid external function calls during the signal

int
signal_int2str(char *dest, int src)
{
    char *p = dest;
    int tmp = src;
    do
    {
        p++;
        tmp /= 10;
    }
    while(tmp > 0);
    
    *p = '\0';
    
    do
    {
        char c = '0' + (char)(src % 10);
        src /= 10;
        
        --p;
        
        *p = c;
    }
    while(src > 0);
    
    while(p > dest)
    {
        --p;
        
        *p = ' ';
    }
        
    return p - dest;
}

static const char __HEXA__[16] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

int
signal_longlong2hexstr(char *dest, u64 src)
{
    int shift = 60;
    do
    {
        *dest++ = __HEXA__[(src >> shift) & 15];
        shift -= 4;
    }
    while(shift >= 0);
    *dest = '\0';
    return 16;
}

int
signal_int2hexstr(char *dest, u32 src)
{
    int shift = 28;
    do
    {
        *dest++ = __HEXA__[(src >> shift) & 15];
        shift -= 4;
    }
    while(shift >= 0);
    *dest = '\0';
    return 8;
}

int
signal_char2hexstr(char *dest, u8 src)
{
    int shift = 4;
    do
    {
        *dest++ = __HEXA__[(src >> shift) & 15];
        shift -= 4;
    }
    while(shift >= 0);
    *dest = '\0';
    return 2;
}

// tool to avoid external function calls during the signal

int
signal_ptr2str(char *dest, const void* srcp)
{
#if __SIZEOF_POINTER__ == 8
    return signal_longlong2hexstr(dest,(u64)srcp);
#elif __SIZEOF_POINTER__ == 4
    return signal_int2hexstr(dest,(u32)srcp);
#else
#error "unsupported pointer size"
#endif
}

//

static void
signal_task_reconfigure_reopen_log()
{  
    log_debug1("signal_task_reconfigure_reopen_log()");
    
    u64 now = timeus();
        
    if(now - signal_task_logger_handle_reopen_last_active > LOGGER_REOPEN_MIN_PERIOD_US)
    {        
        signal_task_logger_handle_reopen_last_active = now;
        
        if(g_config->reloadable)
        {
            yadifad_config_update(g_config->config_file);
        }
        else
        {
            log_err("cannot reopen configuration file(s): '%s' is outside of jail", g_config->config_file);
        }
        
        logger_reopen();
        
#if HAS_DNSSEC_SUPPORT && HAS_RRSIG_MANAGEMENT_SUPPORT
        database_service_zone_dnskey_set_alarms_on_all_zones();
#endif
    }
#ifdef DEBUG
    else
    {
        double dt = LOGGER_REOPEN_MIN_PERIOD_US - (now - signal_task_logger_handle_reopen_last_active);
        dt /= 1000000.0;
        log_debug1("signal_task_reconfigure_reopen_log(): ignore for %.3fs", dt);
    }
#endif
    
    log_debug1("signal_task_reconfigure_reopen_log(): end");
}

/***/

static void
signal_task_database_save_all_zones_to_disk()
{
    log_debug("signal_task_database_save_all_zones_to_disk()");
    
    if(g_config->database != NULL)
    {
        database_save_all_zones_to_disk();
    }
    
    log_debug("signal_task_database_save_all_zones_to_disk(): end");
}

/**/

static void
signal_task_shutdown()
{
    log_debug("signal_task_shutdown()");
    
    program_mode = SA_SHUTDOWN;
    
    if(!dnscore_shuttingdown())
    {
        program_mode = SA_SHUTDOWN;
        
        dnscore_shutdown();
    }
    
    log_debug("signal_task_shutdown(): end");
}

static void*
signal_handler_thread(void* parms)
{
    (void)parms;

#if HAS_PTHREAD_SETNAME_NP
#ifdef DEBUG
#if __APPLE__
    pthread_setname_np("signal-handler");
#else
    pthread_setname_np(pthread_self(), "signal-handler");
#endif // __APPLE__
#endif
#endif
    
    log_info("signal: thread started");
       
    while(signal_handler_read_fd >= 0)
    {
        u8 signum;
        ya_result return_code;
        
#ifdef DEBUG
        log_debug7("signal: waiting for next signal");
#endif 
        
        if(FAIL(return_code = readfully(signal_handler_read_fd, &signum, sizeof(signum))))
        {
            log_err("signal: error reading the next signal: %r", return_code);
            break;
        }
        
#ifdef DEBUG
        log_debug7("signal: handling signal %i", signum);
#endif
        
        if(signal_shutdown_received)
        {
#ifdef DEBUG
            if(signum != SIGINT && signum != SIGTERM)
            {
                log_debug7("signal: check that, handling a SIGINT instead");
            }
#endif
            signum = SIGINT;
        }
        
        switch(signum)
        {
            case SIGHUP:
            {
                if(!dnscore_shuttingdown())
                {
                    signal_task_reconfigure_reopen_log();
                }
                break;
            }
            
            case SIGUSR1:
            {
                if(!dnscore_shuttingdown())
                {
                    signal_task_database_save_all_zones_to_disk();
                }
                break;
            }
            case SIGINT:
            case SIGTERM:
            {
                signal_task_shutdown();
            }
            
            case MAX_U8:
            {
                log_info("signal: thread stopping");
                
                close_ex(signal_handler_read_fd);
                mutex_lock(&signal_mutex);
                signal_handler_read_fd = -1;
                mutex_unlock(&signal_mutex);
                break;
            }
            
            default:
            {
                break;
            }
        }
    }
    
    log_info("signal: thread stopped");
    
    mutex_lock(&signal_mutex);
    signal_thread = 0;
    mutex_unlock(&signal_mutex);
    
    return NULL;
}

/** \brief handle the signals received
 *
 *  @param[in] signo
 *
 *  @note The signal handler CANNOT use the loggers or it has to use its own channels + handle. (ie: not the ones of the logger)
 *        The reason being mutexes are not reentrant.  So if a signal occurs while the log mutex is on
 *        the signal will deadlock as soon as it tries to log.
 *
 *  return NONE
 */

static void
signal_handler(int signo, siginfo_t* info, void* context)
{   
    switch(signo)
    {
        case SIGINT:
        case SIGTERM:
        {
            /*
             * We are shutting down : ignore other "command" signals
             * Also, in order to avoid handling an hammering of signals,
             * (and risking missing the shutdown if the pipe is already full)
             * we set a volatile that will be sync "soon" (no mutexes here please)
             */
            
            int errno_value = errno;
            signal(SIGHUP, SIG_IGN);
            signal(SIGUSR1, SIG_IGN);
            signal(SIGINT, SIG_IGN);
            signal(SIGTERM, SIG_IGN);
            errno = errno_value;
            
            signal_shutdown_received = TRUE;
            
            // fall-through
        }
        case SIGHUP:
        case SIGUSR1:

        {
            int errno_value = errno;
            u8 signum = (u8)signo;
            write(signal_handler_write_fd, &signum, sizeof(signum));
            errno = errno_value;
            break;
        }
        
#if SIGNAL_HOOK_COREDUMP
        case SIGABRT:
        case SIGBUS:
        case SIGFPE:
        case SIGILL:
        case SIGSEGV:
        {
            // reactivate the default handler

            int errno_value = errno;
            
            signal(signo, SIG_DFL);
            
            if(sigsegv_trytrace)
            {
                char filepath[PATH_MAX];
                char number[32];
                    
                sigsegv_trytrace = FALSE;
                
                for(int source = 0; source <= 1; source++)
                {
                    char *eol = (source == 0)?"\n":"";
                    int fd = -1; /// @note edf: set to -1 to shut-up false positive "uninitialised"
                    int len;
                    
                    filepath[0] = '\0';
                    signal_strcat(filepath, g_config->log_path);
                    signal_strcat(filepath, "/");
                    signal_strcat(filepath, "sig-");
                    signal_int2str(number, signo);
                    signal_strcat(filepath, number);
                    signal_strcat(filepath, "-");
                    signal_int2str(number, getpid());
                    signal_strcat(filepath, number);                   
                    
                    if(source == 0)
                    {
                        fd = open_create_ex(filepath, O_WRONLY|O_CREAT|O_APPEND, 0644);
                        if(fd < 0)
                        {
                            continue;
                        }
                    }
                    else
                    {
                        if(!logger_is_running())
                        {
                            continue;
                        }
                    }
                    
                    len = signal_strcat(filepath, eol);  
                    
                    if(source == 0)
                    {
                        writefully(fd, filepath, len);
                        fsync(fd);
                    }
                    else
                    {
                        log_err(filepath);
                    }

#if defined(__linux__) || defined(__gnu_hurd__)
                    void* buffer[MAXTRACE];
                    char** strings;
                    int n = backtrace(buffer, MAXTRACE);
                    int i;
                    time_t now = time(NULL);

                    filepath[0] = '\0';
                    signal_strcat(filepath, "got signal ");
                    signal_int2str(number, signo);
                    signal_strcat(filepath, number);
                    signal_strcat(filepath, " at time=");
                    signal_int2str(number, now);
                    signal_strcat(filepath, number);
                    signal_strcat(filepath, " for address ");
                    
                    signal_ptr2str(number, info->si_addr);
                    signal_strcat(filepath, number);
                    len = signal_strcat(filepath, eol);
                    
                    if(source == 0)
                    {
                        writefully(fd, filepath, len);
                        fsync(fd);
                    }
                    else
                    {
                        log_err(filepath);
                    }

#if __linux__
                    ucontext_t* ucontext = (ucontext_t*)context;
                    /*
                    filepath[0] = '\0';
                    signal_strcat(filepath, "uc_flags=");
                    signal_int2hexstr(number, ucontext->uc_flags);
                    signal_strcat(filepath, number);
                    len = signal_strcat(filepath, eol);
                    
                    if(source == 0)
                    {
                        writefully(fd, filepath, len);
                        fsync(fd);
                    }
                    else
                    {
                        log_err(filepath);
                    }
                    */
                    /*
                     * cpu registers dump, if supported
                     * this helps a lot if there is no core dump available
                     */
                    
#ifdef __x86_64__   
                    // specific x86_64 information
                    
                    struct text_idx
                    {
                        const char* name;
                        int index;
                    };
                    
                    static struct text_idx text_idx[18] =
                    {
                        {"rax", REG_RAX},
                        {"rcx", REG_RCX},
                        {"rdx", REG_RDX},
                        {"rbx", REG_RBX},
                        {"rsi", REG_RSI},
                        {"rdi", REG_RDI},
                        {"rsp", REG_RSP},
                        {"rbp", REG_RBP},
                        {"r8 ", REG_R8},
                        {"r9 ", REG_R9},
                        {"r10", REG_R10},
                        {"r11", REG_R11},
                        {"r12", REG_R12},
                        {"r13", REG_R13},
                        {"r14", REG_R14},
                        {"r15", REG_R15},
                        {"rip", REG_RIP},
                        {"efl", REG_EFL}
                    };
                    
                    for(int i = 0; i < 18; ++i)
                    {   
                        filepath[0] = '\0';
                        signal_strcat(filepath, text_idx[i].name);
                        signal_strcat(filepath, "=");
                        signal_longlong2hexstr(number, ucontext->uc_mcontext.gregs[text_idx[i].index]);
                        signal_strcat(filepath, number);
                        len = signal_strcat(filepath, eol);
                                                
                        if(source == 0)
                        {
                            writefully(fd, filepath, len);
                            fsync(fd);
                        }
                        else
                        {
                            log_err(filepath);
                        }
                    }
#elif defined(__i386__)
                    struct text_idx
                    {
                        const char* name;
                        int index;
                    };
                    
                    static struct text_idx text_idx[10] =
                    {
                        {"eax", REG_EAX},
                        {"ecx", REG_ECX},
                        {"edx", REG_EDX},
                        {"ebx", REG_EBX},
                        {"esi", REG_ESI},
                        {"edi", REG_EDI},
                        {"esp", REG_ESP},
                        {"ebp", REG_EBP},
                        {"rip", REG_EIP},
                        {"efl", REG_EFL}
                    };
                    
                    for(int i = 0; i < 10; ++i)
                    {   
                        filepath[0] = '\0';
                        signal_strcat(filepath, text_idx[i].name);
                        signal_strcat(filepath, "=");
                        signal_int2hexstr(number, ucontext->uc_mcontext.gregs[text_idx[i].index]);
                        signal_strcat(filepath, number);
                        len = signal_strcat(filepath, eol);
                                                
                        if(source == 0)
                        {
                            writefully(fd, filepath, len);
                            fsync(fd);
                        }
                        else
                        {
                            log_err(filepath);
                        }
                    }
#else // not x86_64 nor i386
                    // cpu registers dump not supported
#endif
                    
#endif // linux
                    strings = backtrace_symbols(buffer, n);

                    if(strings != NULL)
                    {
                        for(i = 0; i < n; i++)
                        {
                            filepath[0] = '\0';
                            signal_strcat(filepath, "\t[");
                            signal_int2str(number, i);
                            signal_strcat(filepath, number);
                            signal_strcat(filepath, "]: ");
                            signal_strcat(filepath, strings[i]);
                            len = signal_strcat(filepath, eol);
                            
                            if(source == 0)
                            {
                                writefully(fd, filepath, len);
                                fsync(fd);
                            }
                            else
                            {
                                log_err(filepath);
                            }
                        }
                    }
                    else
                    {
                        filepath[0] = '\0';
                        signal_strcat(filepath, "no backtrace available: ");
                        signal_int2str(number, errno);
                        signal_strcat(filepath, number);
                        len = signal_strcat(filepath, eol);
                        
                        if(source == 0)
                        {
                            writefully(fd, filepath, len);
                            fsync(fd);
                        }
                        else
                        {
                            log_err(filepath);
                        }
                    }

                    filepath[0] = '\0';
                    signal_strcat(filepath, "pid: ");
                    signal_int2str(number, getpid());
                    signal_strcat(filepath, number);
                    signal_strcat(filepath, " ");
                    signal_strcat(filepath, "thread id: ");
                    signal_int2str(number, (u32)pthread_self());
                    signal_strcat(filepath, number);
                    len = signal_strcat(filepath, eol);
                    
                    if(source == 0)
                    {
                        writefully(fd, filepath, len);
                        fsync(fd); // fd IS initialised : (source == 0) => fd set
                    }
                    else
                    {
                        log_err(filepath);
                    }
                    
#if __linux__ && (defined(__x86_64__) || defined(__i386__)) && (_BSD_SOURCE || _SVID_SOURCE || _DEFAULT_SOURCE)
                    // dump more information about the memory address of the error
#define PAGESIZE 4096
#define LINESIZE 32
                    const u8 *addr = (u8*)info->si_addr;
                    for(;;)
                    {
                        u8 *page_addr = (u8*) (((intptr)addr + PAGESIZE-1) & ~(PAGESIZE - 1));
                        unsigned char vec[1];

                        if(mincore(page_addr, PAGESIZE, vec) == 0)
                        {
                            // memory is resident

                            for(const u8* p = page_addr; p < &page_addr[PAGESIZE]; p += 32)
                            {
                                filepath[0] = '\0';
                                signal_ptr2str(number, p);
                                signal_strcat(filepath, number);
                                signal_strcat(filepath, " | ");
                                for(int i = 0; i < LINESIZE; ++i)
                                {
                                    signal_char2hexstr(number, p[i]);
                                    signal_strcat(filepath, number);
                                    signal_strcat(filepath, " ");
                                }
                                len = signal_strcat(filepath, "| ");
                                for(int i = 0; i < LINESIZE; ++i)
                                {
                                    u8 c = p[i];
                                    if(c < 32 || c >= 127)
                                    {
                                        c = '.';
                                    }
                                    filepath[len + i] = c;
                                }
                                len += LINESIZE;

                                if(source == 0)
                                {
                                    writefully(fd, filepath, len);
                                    fsync(fd); // fd IS initialised : (source == 0) => fd set
                                }
                                else
                                {
                                    log_err(filepath);
                                }
                            }

                            // dump enough memory to make sense

                            if(&page_addr[PAGESIZE] >= &addr[32])
                            {
                                break;
                            }
                        }
                        else
                        {
                            int err = errno;

                            if(err == ENOMEM)
                            {
                                // memory is not mapped
                                filepath[0] = '\0';
                                signal_strcat(filepath, "page at ");
                                signal_ptr2str(number, page_addr);
                                signal_strcat(filepath, number);
                                len = signal_strcat(filepath, " is not mapped.");
                            }
                            else
                            {
                                //
                                filepath[0] = '\0';
                                signal_strcat(filepath, "could not get information for page at ");
                                signal_ptr2str(number, page_addr);
                                signal_strcat(filepath, number);
                                signal_strcat(filepath, " : errno = ");
                                signal_int2str(number, err);
                                len = signal_strcat(filepath, number);
                            }

                            if(source == 0)
                            {
                                writefully(fd, filepath, len);
                                fsync(fd); // fd IS initialised : (source == 0) => fd set
                            }
                            else
                            {
                                log_err(filepath);
                            }
                            
                            break;
                        }
                    }
                    
#endif // linux && mincore supported
                  
#elif defined(__sun)
                    filepath[0] = '\0';
                    signal_strcat(filepath, "got signal ");
                    signal_int2str(number, signo);
                    signal_strcat(filepath, number);
                    
                    signal_strcat(filepath, eol);
                    if(source == 0) // 0 -> output to file, else to the logger if it's on
                    {
                        writefully(fd, filepath, len);
                        printstack(fd);
                        fsync(fd);
                    }
                    else
                    {
                        signal_strcat(filepath, "stack trace dumped in ");
                        signal_strcat(filepath, g_config->log_path);
                        signal_strcat(filepath, "/");
                        signal_strcat(filepath, "sig-");
                        signal_int2str(number, signo);
                        signal_strcat(filepath, number);
                        signal_strcat(filepath, "-");
                        signal_int2str(number, getpid());
                        signal_strcat(filepath, number);
                    
                        log_err(filepath);
                    }
#else
                    filepath[0] = '\0';
                    signal_strcat(filepath, "got signal ");
                    signal_int2str(number, signo);
                    signal_strcat(filepath, number);
                    len = signal_strcat(filepath, "\nno backtrace available\n");
                    
                    if(source == 0)
                    {
                        writefully(fd, filepath, len);
                        fsync(fd);
                    }
                    else
                    {
                        log_err(filepath);
                    }
#endif
                    if(source == 0)
                    {
                        close_ex(fd);
                    }
                    
                } // for both sources

                    /**
                     * Do NOT free(strings) :
                     * If the memory is corrupted, this is one more chance to crash
                     *
                     */
            } // if sigsegv_trytrace
            
            /* There COULD be some relevant information in the logger */
            /* try to flush it */

            if(sigsegv_tryloggerflush)
            {
                sigsegv_tryloggerflush = FALSE;
                logger_flush();
                log_err("CRITICAL ERROR");
                logger_flush();
            }

            errno = errno_value;
            
            /* trigger the original signal (to dump a core if possible ) */

            raise(signo);

            /* should never be reached : Exit without disabling stuff (no atexit registered function called) */

            _exit(EXIT_CODE_SELFCHECK_ERROR);

            break;
        }
#endif

        /*
        case SIGUSR2:
        case SIGCHLD:
        */
        default:
        {
            break;
        }    
    }
}

/** \brief initialize the signals
 *
 *  @param NONE
 *
 *  @return NONE
 *
 */
int
signal_handler_init()
{
    int fds[2];
    
    log_debug("signal_handler_init()");
    
    if((signal_handler_read_fd >= 0) || (signal_handler_write_fd >= 0))
    {
        log_debug("signal_handler_init() : already initialised");
        
        return INVALID_STATE_ERROR; // invalid status
    }
    
    if(pipe(fds) < 0)
    {
        int pipe_err = ERRNO_ERROR;
        
        log_debug("signal_handler_init(): %r", pipe_err);
        
        return pipe_err;
    }
    
    signal_handler_read_fd = fds[0];
    signal_handler_write_fd = fds[1];
    
    int write_fd_flags = fcntl(signal_handler_write_fd, F_GETFL, 0);
    write_fd_flags |= O_NONBLOCK;
    if(fcntl(signal_handler_write_fd, F_SETFL, write_fd_flags) < 0)
    {
        int fcntl_err = ERRNO_ERROR;
        
        log_debug("signal_handler_init(): %r", fcntl_err);
        
        return fcntl_err;
    }

    int pthread_errcode;
    
    if((pthread_errcode = pthread_create(&signal_thread, NULL, signal_handler_thread, NULL)) != 0)
    {
        close_ex(signal_handler_read_fd);
        close_ex(signal_handler_write_fd);
        
        signal_handler_read_fd = -1;
        signal_handler_write_fd = -1;
        
        pthread_errcode = MAKE_ERRNO_ERROR(pthread_errcode);
        
        log_debug("signal_handler_init(): %r", pthread_errcode);
        
        return pthread_errcode;
    }
    
    u8 handled_signals[] =
    {
        SIGHUP,		/* Hangup (POSIX).  */
        SIGINT,		/* Interrupt (ANSI).  */
        SIGQUIT,	/* Quit (POSIX).  */
        SIGIOT,		/* IOT trap (4.2 BSD).  */
        SIGUSR1,	/* User-defined signal 1 (POSIX).  */
#if SIGNAL_HOOK_COREDUMP
        SIGABRT,	/* Abort (ANSI).  */
        SIGILL,		/* Illegal instruction (ANSI).  */	    /* ERROR/EXIT */
        SIGBUS,		/* BUS error (4.2 BSD).  */
        SIGFPE,		/* Floating-point exception (ANSI).  */	    /* ERROR/EXIT */
        SIGSEGV,	/* Segmentation violation (ANSI).  */	    /* ERROR/EXIT */
#endif
        SIGUSR2,	/* User-defined signal 2 (POSIX).  */
        SIGALRM,	/* Alarm clock (POSIX).  */
        SIGTERM,	/* Termination (ANSI).  */
    /*	SIGSTKFLT,*/	/* Stack fault.  */
        SIGCHLD,	/* Child status has changed (POSIX).  */
        SIGCONT,	/* Continue (POSIX).  */
        SIGTSTP,	/* Keyboard stop (POSIX).  */
        SIGTTIN,	/* Background read from tty (POSIX).  */
        SIGTTOU,	/* Background write to tty (POSIX).  */
        SIGURG,		/* Urgent condition on socket (4.2 BSD).  */
        SIGXCPU,	/* CPU limit exceeded (4.2 BSD).  */
        SIGXFSZ,	/* File size limit exceeded (4.2 BSD).  */
        0
    };

    u8 ignored_signals[] =
    {
        SIGPIPE,	/* Broken pipe (POSIX).  */
        0
    };

    struct sigaction action;
    int signal_idx;
    
    ZEROMEMORY(&action,sizeof(action));

    action.sa_sigaction = signal_handler;
    
    for(signal_idx = 0; handled_signals[signal_idx] != 0; signal_idx++)
    {
#ifdef SA_NOCLDWAIT
        action.sa_flags = SA_SIGINFO | SA_NOCLDSTOP | SA_NOCLDWAIT;
#else /// @note 20151119 edf -- quick fix for Debian Hurd i386, and any other system missing SA_NOCLDWAIT
        action.sa_flags = SA_SIGINFO | SA_NOCLDSTOP;
#endif
        
        switch(signal_idx)
        {
            case SIGBUS:
            case SIGFPE:
            case SIGILL:
            case SIGSEGV:
            {
                sigemptyset(&action.sa_mask);    /* can interrupt the interrupt */
                
                break;
            }
            default:
            {
                sigfillset(&action.sa_mask);    /* don't interrupt the interrupt */
                break;
            }
        }
        sigaction(handled_signals[signal_idx], &action, NULL);
    }
    
    action.sa_handler = SIG_IGN;

    for(signal_idx = 0; ignored_signals[signal_idx] != 0; signal_idx++)
    {
        sigaction(ignored_signals[signal_idx], &action, NULL);
    }
    
    log_debug("signal_handler_init() done");
    
    return SUCCESS;
}

void
signal_handler_finalise()
{
    log_debug("signal_handler_finalise()");
    
    if(signal_handler_write_fd >= 0)
    {
        log_debug1("signal: pipe not closed yet");
        
        mutex_lock(&signal_mutex);
        pthread_t signal_thread_local = signal_thread;
        mutex_unlock(&signal_mutex);
            
        if(signal_handler_read_fd >= 0)
        {
            u8 stop_value = MAX_U8;
            
            if(signal_thread_local != 0)
            {
                log_debug1("signal: handler is still running");
                
                writefully(signal_handler_write_fd, &stop_value, sizeof(stop_value));
                pthread_join(signal_thread_local, NULL);
                mutex_lock(&signal_mutex);
                signal_thread = 0;
                mutex_unlock(&signal_mutex);
                
                log_debug1("signal: handler has stopped");
            }
            else
            {
                log_debug1("signal: handler is not running anymore");
            }

            int signal_handler_read_fd_local = signal_handler_read_fd;

            if(signal_handler_read_fd_local >= 0)
            {
                close_ex(signal_handler_read_fd_local);
                mutex_lock(&signal_mutex);
                signal_handler_read_fd = -1;
                mutex_unlock(&signal_mutex);
            }
        }
        else
        {
            if(signal_thread_local != 0)
            {
                log_debug1("signal: waiting for the handler to stop");

                pthread_join(signal_thread_local, NULL);
                mutex_lock(&signal_mutex);
                signal_thread = 0;
                mutex_unlock(&signal_mutex);
            }
            
            log_debug1("signal: handler has stopped");
        }
        
        close_ex(signal_handler_write_fd);
        signal_handler_write_fd = -1;
    }
    else
    {
        if(signal_handler_read_fd >= 0)
        {
            log_err("signal: invalid pipe status r:%i>=0 && w:%i>=0",
                    signal_handler_read_fd,
                    signal_handler_write_fd);
        }
        
        if(signal_thread != 0)
        {
            log_err("signal: handler is unexpectedly still running");
        }        
    }
  
    
    
    log_debug("signal_handler_finalise() done");
}

/** @} */
