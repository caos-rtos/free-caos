#ifndef _SEL_MCU_HEADER
#define _SEL_MCU_HEADER

/* =============================================================================
 * TYPEDEF BREVI PER TIPI INTERI
 * ============================================================================= */

/* Tipi signed */
typedef signed char         i8;     /* Signed 8-bit  (-128 to 127) */
typedef signed short        i16;    /* Signed 16-bit (-32768 to 32767) */
typedef signed int          i32;    /* Signed 32-bit (-2147483648 to 2147483647) */
typedef signed long long    i64;    /* Signed 64-bit */

/* Tipi unsigned */
typedef unsigned char       u8;     /* Unsigned 8-bit  (0 to 255) */
typedef unsigned short      u16;    /* Unsigned 16-bit (0 to 65535) */
typedef unsigned int        u32;    /* Unsigned 32-bit (0 to 4294967295) */
typedef unsigned long long  u64;    /* Unsigned 64-bit */


/* NULL pointer */
#ifndef NULL
#define NULL ((void *)(0))
#endif


#define DEFAULT_MSG_SIZE     132



typedef struct tag_string {
    u16 size;
    char data[DEFAULT_MSG_SIZE - sizeof(u16)];  /* 130 bytes, totale struct = 132 */
} string, *p_str;


typedef const struct tag_string * c_str;
typedef const char *              cstr;

/* Boolean type */
typedef u16 bool;
#define true        1
#define false       0


/**
 * @brief Tipo funzione sender per output stringhe generico
 */
typedef u16 (*sender)(p_str str);

#define PUBLICSTRING(name, msg) \
    const string name = { \
        .size = sizeof(msg) - 1, \
        .data = msg \
    } 

#define PRIVATE_STRING(name, msg) \
    static const string priv_##name = { \
        .size = sizeof(msg) - 1, \
        .data = msg \
    }

/* The line an example prints when the part it is running on cannot support it.
 *
 * WHY IT IS A MACRO AND NOT A CONVENTION
 *
 * Most examples already carry an #else branch that says why they did nothing -
 * 75 of them - but each says it differently: "EN_ETH /EN_NET == 0: no ETH tasks
 * created", "EN_SHA512_TEST / EN_X25519_TEST / EN_ED25519_TEST disabled", and
 * fourteen say nothing at all. A test bench cannot recognise that, and it is
 * the bench that has to: U575 has no MAC, so every ETH example is inapplicable
 * there and must be SKIPPED rather than reported as failing.
 *
 * So the FIRST line is fixed and identical everywhere, and the reason follows
 * on its own line - free text, as detailed as the example likes. The bench
 * matches the first, a person reads the second.
 *
 * USE
 *
 *   PRIVATE_STRING(why, "EN_ETH/EN_NET == 0: this part has no MAC");
 *   void ca_os_configure(void)
 *   {
 *       CA_EXAMPLE_UNSUPPORTED();
 *       PUT_N(&priv_why);
 *   }
 *
 * PUT_N and not ca_stdout_*: this runs before ca_os_run(), where there is no
 * task context and the stdout layer has no TLB to write through.
 */
#define CA_EXAMPLE_UNSUPPORTED_MSG "EXAMPLE IS DISABLED FOR UNSUPPORTED HARDWARE"

#define CA_EXAMPLE_UNSUPPORTED() \
    do { \
        PRIVATE_STRING(ca_unsup_, CA_EXAMPLE_UNSUPPORTED_MSG); \
        PUT_N(&priv_ca_unsup_); \
    } while (0)


#endif //_SEL_MCU_HEADER
