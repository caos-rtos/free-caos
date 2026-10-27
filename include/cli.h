// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : cli.h
 * @brief          : Command Line Interface System interface
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Command Line Interface system for the caOS operating system.
 * Provides a way to interact with the system via text commands.
 * Includes functionality for command processing, buffer management
 * and integration with the system's logging and communication systems.
 *
 * Typical sequence:
 * 1) CLI system initialization -> thread creation -> command processing
 ******************************************************************************
 */

#ifndef CLI_H
#define CLI_H

#include "common.h"

#include "utils.h"


#if ENABLE_CLI

/* Command history depth (overridable from os_config.h before including caos.h) */
#ifndef CLI_HISTORY_SIZE
#define CLI_HISTORY_SIZE  16U
#endif

/* ============================================================================
 * OUTPUT MACROS — use in all CLI handlers of the chain
 * ============================================================================
 * ENABLE_CLI_STDOUT=1 (default): output via ca_stdout (always available)
 * ENABLE_CLI_STDOUT=0           : output via g_sender (set with ca_cli_set_sender)
 * ========================================================================== */
#define ENABLE_CLI_STDOUT    1

sender ca_cli_get_sender(void);

#if ENABLE_CLI_STDOUT
#define CLI_PRINTF_N(...)    ca_stdout_printf_n(__VA_ARGS__) 
#define CLI_PUT_N(MSG)       ca_stdout_put_n(MSG)           
#else
#define CLI_PRINTF_N(...)  do { static char _b[DUMP_BUFFER_SIZE]; ca_printf_n(ca_cli_get_sender(),_b,DUMP_BUFFER_SIZE,__VA_ARGS__);  } while(0)
#define CLI_PUT_N(MSG)    do { static char _b[DUMP_BUFFER_SIZE]; ca_put_n(ca_cli_get_sender(),_b,DUMP_BUFFER_SIZE,MSG);              } while(0)
#endif

/* ============================================================================
 * FUNCTION PROTOTYPES
 * ========================================================================== */


typedef struct ca_cli_node {
    bool (*handler)(c_str *cmd);  /* true = command handled */
    struct ca_cli_node *next;
} ca_cli_node_t;

/**
 * @brief Registers a node in the CLI chain (prepend)
 * @param[in] node Static node with handler and next=NULL
 */
void ca_cli_register(ca_cli_node_t *node);

/**
 * @brief Initializes the CLI system
 * @param[in] buf_size Buffer size (will be clamped to CLI_BUFFER_MAX)
 */
void ca_cli_init(u16 buf_size);

/**
 * @brief Creates the CLI thread
 * @return Handle of the created task (ca_task_handle_t), 0xFFFFFFFF if it fails
 */
u32 ca_cli_create_thread(void);

/**
 * @brief Sets the output function for the CLI
 * @param[in] out Output function
 */
void ca_cli_set_sender(sender out);

/**
 * @brief Checks if the CLI system is enabled
 * @return true if enabled, false otherwise
 */
bool ca_cli_is_enabled(void);

/**
 * @brief Gets statistics on the CLI system
 * @param[out] queued Number of messages in queue
 * @param[out] buffered Number of messages buffered
 * @param[out] dropped Number of messages lost
 */
void ca_cli_stats(u32 *queued, u32 *buffered, u32 *dropped);

/**
 * @brief Number of input bytes rejected by the RX filter since boot
 *        (anything outside printable ASCII 0x20..0x7E; CR/LF, backspace and
 *        VT100 escapes are handled before the filter and are not counted).
 * @return Running count of dropped bytes, 0 if the console is clean
 */
u32 ca_cli_filtered_count(void);

/* ---- Input character classes -----------------------------------------------
 * One class per ASCII code, from a 128-byte const table in cli.c. It exists so
 * the console and the SSH shell classify input the SAME way: the two used to
 * carry their own copy of the rule, which is exactly how the arrow keys ended
 * up working on one and printing "[A" on the other.
 *
 * The table is filter-by-exclusion, never a letters+digits whitelist: commands
 * legitimately carry '.', '/', '-', '_', ':' and case matters (base64 keys, the
 * B|K|M unit). CR/LF sit BELOW the printable range, so they are their own class
 * rather than an exception someone has to remember. */
#define CH_NONE    0x00U   /* not usable: control byte, or non-ASCII        */
#define CH_CMD     0x01U   /* goes into the command line                    */
#define CH_SUBMIT  0x02U   /* CR / LF: run the line                         */
#define CH_ERASE   0x04U   /* BS / DEL                                      */
#define CH_ESC     0x08U   /* 0x1B: start of a VT100 escape sequence        */
#define CH_TYPO    0x10U   /* printable, but no command uses it: a mistyped
                            * key. Answered with a bell instead of entering
                            * the buffer, so the command still runs and the
                            * user still hears that a key was rejected.     */

/**
 * @brief Class of one input byte (CH_* above)
 * @param[in] byte Raw input byte; anything >= 0x80 is CH_NONE
 * @return The CH_* class, CH_NONE if the byte has no use in a command line
 */
u8 ca_cli_char_class(u8 byte);

/**
 * @brief Number of mistyped-but-printable keys rejected since boot
 *
 * Counted apart from ca_cli_filtered_count(): that one counts bytes nobody
 * typed (line noise, unrecognised escapes), this one counts keys a person
 * actually pressed. Telling them apart is what says whether the link is
 * dropping characters or the operator is just missing keys.
 */
u32 ca_cli_typo_count(void);

/**
 * @brief Read one entry of the shared command history
 *
 * The console CLI navigates this with the arrow keys on its own. A shell that
 * assembles its line elsewhere - SSH does, it never passes through
 * ca_cli_rx_byte() - uses these three to share THIS history rather than carry
 * a second copy per session.
 *
 * @param[in] pos 0 = most recent, up to ca_cli_hist_count()-1
 * @return NUL-terminated entry, or NULL if pos is past the end
 */
const char *ca_cli_hist_get(u8 pos);

/**
 * @brief Number of entries currently held in the command history
 */
u8 ca_cli_hist_count(void);

/**
 * @brief Append a command line to the shared history
 *
 * For callers that did not build the line in the console RX buffer. Empty
 * lines are ignored, and the text is truncated to the CLI buffer size.
 */
void ca_cli_hist_add(const char *line);

/**
 * @brief Wipe every history entry that starts with prefix
 *
 * For a command that carries a secret (e.g. app_3 'link pass <password>'):
 * call it from the handler, after the secret has been copied, so the arrow
 * keys cannot bring it back. Also wipes the idle line buffer.
 */
void ca_cli_hist_forget(const char *prefix);

/**
 * @brief Declare a command prefix as secret (e.g. "link pass ")
 *
 * Once the line being typed starts with prefix, the following characters
 * are echoed as '*', and the line never enters the history. NULL disables.
 * The string must stay valid (a literal).
 */
void ca_cli_set_secret_prefix(const char *prefix);

/**
 * @brief Sends data to the CLI system from an ISR context
 * @param[in] data Received data
 * @param[in] len Data length
 */
void ca_cli_data_from_isr(const u8  *data, u32 len);

/**
 * @brief Receives a byte from the UART and accumulates it in the circular buffer.
 *        On '\r' or '\n' sends the complete line to the CLI queue.
 *        Called by ISR (not reentrant).
 * @param[in] ch The received byte (u32 for ca_uart_rx_cb_t compatibility)
 */
void ca_cli_rx_byte(u32 ch);


typedef u16 (*ca_cli_hook_t)(c_str cmd);
/**
 * @brief sets a hook function on the cli for user-defined
 * commands in addition to system ones
 * 
 */
void ca_cli_set_hook(ca_cli_hook_t hook);

/**
 * @brief returns the hook function on the cli for user-defined
 * commands in addition to system ones
 * 
 */
ca_cli_hook_t ca_cli_get_hook(void);

/**
 * @brief Executes a CLI command and captures the output in a buffer.
 * @param cmd        Command string (without trailing \n)
 * @param out_buf    Buffer where the text output is written
 * @param buf_size   Size of out_buf (including terminator)
 * @param timeout_ms Completion wait timeout in ms
 * @return true if the command was processed within the timeout
 * @note Not reentrant — do not call from two tasks simultaneously.
 */
#if EN_CLI_CAPTURE
bool ca_cli_exec_capture(const char *cmd, char *out_buf,
                         u16 buf_size, u32 timeout_ms);

/**
 * @brief Like ca_cli_exec_capture but STREAMS the output (no fixed buffer).
 * @details Redirects both stdout and the CLI sender to @p out_sender for the
 *          duration of the command, so output flows out as it is produced with
 *          no size limit. @p out_sender is invoked from the stdout drain thread
 *          (and possibly the CLI thread): it must serialise itself if it can be
 *          re-entered, and may block for backpressure.
 * @param cmd        Command string (without trailing \n)
 * @param out_sender sender callback receiving each output chunk
 * @param timeout_ms Completion wait timeout in ms
 * @return true if the command was processed within the timeout
 */
bool ca_cli_exec_capture_stream(const char *cmd, sender out_sender,
                                u32 timeout_ms);
#else
#define ca_cli_exec_capture(cmd, buf, size, ms)  (false)
#define ca_cli_exec_capture_stream(cmd, snd, ms) (false)
#endif

#else  /* ENABLE_CLI == 0 */

#define CLI_PRINTF_N(...)
#define CLI_PUT_N(MSG)
#define ca_cli_init(...)
#define ca_cli_create_thread(...)
#define ca_cli_set_sender(...)
#define ca_cli_is_enabled(...)
#define ca_cli_stats(...)
#define ca_cli_filtered_count(...)  (0U)
#define ca_cli_char_class(...)      (0U)
#define ca_cli_typo_count(...)      (0U)
#define ca_cli_hist_get(...)        ((const char *)0)
#define ca_cli_hist_count(...)      (0U)
#define ca_cli_hist_add(...)
#define ca_cli_hist_forget(...)
#define ca_cli_set_secret_prefix(...)
#define ca_cli_data_from_isr(...)
#define ca_cli_rx_byte(...)
#define ca_cli_ste_hook(...)
#define ca_cli_register(...)
#define ca_cli_exec_capture(cmd, buf, size, ms)  (false)

#endif


#endif /* CLI_H */