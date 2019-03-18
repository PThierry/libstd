/*
 *
 * Copyright 2018 The wookey project team <wookey@ssi.gouv.fr>
 *   - Ryad     Benadjila
 *   - Arnauld  Michelizza
 *   - Mathieu  Renard
 *   - Philippe Thierry
 *   - Philippe Trebuchet
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * ur option) any later version.
 *
 * This package is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along
 * with this package; if not, write to the Free Software Foundation, Inc., 51
 * Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */
#include "api/stdio.h"
#include "api/nostd.h"
#include "api/string.h"
#include "api/types.h"
#include "api/syscall.h"
#include "api/semaphore.h"
#include "stream/stream_priv.h"
#include "string/string_priv.h"


/***********************************************
 * local utility functions
 **********************************************/

/*
 * Stdio functions (*printf) use a local ring buffer
 * to hold formated content before sending it to the
 * kernel via the kernel log API (typically sys_log()
 * for EwoK).
 * This ring buffer is holded in the libstd as a global
 * variable, local to this very file.
 * The ring buffer is initialized by the libstd at
 * application boot time, as the libstd is managing the
 * application starting point, including the ring buffer
 * and stack canaries initialization in the do_starttask()
 * function.
 *
 * All the ring-buffer associated function, used only by
 * the stdio functions, are implemented here.
 */


/*********************************************
 * Ring buffer and ring buffer utility functions
 */

/*
 * Here is the effective global holding the ring buffer.
 * The ring buffer is local to this object file only.
 */
static struct s_ring ring_buffer;

/*
 * This is the ring buffer mutex. This mutex is used
 * in order to detect, in concurrent contexts (typically
 * in main thread versus ISR threads) concurrent usage
 * of the ring buffer, which may lead to invalid
 * content.
 *
 * main-thread only API (printf()) is using a blocking
 * mutex on the ring_buffer, which ensure that the
 * ressource is released before executing the function
 * content.
 * ISR compatible functions (s[n]printf() and aprintf())
 * use a trylock mutex mechanism, which can fail, to avoid
 * any potential dead lock with the main thread as ISR are
 * executed with a higher priority.
 */
static volatile uint32_t rb_lock = 0;

/*
 * the ring buffer is a part of bss (not data, making it
 * useless in flash, reducing the task flash consumption).
 *
 * As a consequence, it has to be initialized at boot time.
 * This is done by this function, called by do_starttask().
 */
void init_ring_buffer(void)
{
    /* init flags */
    int i = 0;
    ring_buffer.end = 0;
    ring_buffer.start = ring_buffer.end;
    ring_buffer.full = false;

    /* memsetting buffer
     * NOTE: This may be useless as, in EwoK, the BSS is zeroified
     * at boot time.
     */
    for (i = 0; i < BUF_MAX; i++) {
        ring_buffer.buf[i] = '\0';
    }
}

/*
 * add a char in the ring buffer.
 *
 * INFO: by now, there is no bound check here. As a consequence, if
 * the ring buffer is full,
 *
 * WARNING: this function is the only one holding the ring buffer full
 * flag detection. As a consequence, any write access to the ring buffer
 * must be done through this function *exclusively*.
 */
static inline void ring_buffer_write_char(const char c)
{
    /* if the ring buffer is full when we try to put char in it,
     * the car is discared, waiting for the ring buffer to be flushed.
     */
    if (ring_buffer.full) {
        goto end;
    }
	ring_buffer.buf[ring_buffer.end] = c;
    if (((ring_buffer.end + 1) % BUF_MAX) != ring_buffer.start) {
        ring_buffer.end++;
        ring_buffer.end %= BUF_MAX;
    } else {
        /* full buffer detection */
        ring_buffer.full = true;
    }
end:
    return;
}

/*
 * Write a digit to the ring buffer.
 * This function convert a basic digit into a printable one.
 *
 * This function support usual bases such as binary
 *
 * INFO: this function can write digits in base up to hexadecimal one.
 * Bases bigger than hex are not supported.
 *
 */
static inline void ring_buffer_write_digit(uint8_t digit)
{
    if (digit < 0xa) {
        digit += '0';
        ring_buffer_write_char(digit);
    } else if (digit <= 0xf) {
        digit += 'a' - 0xa;
        ring_buffer_write_char(digit);
    }
}

/*
 * copy a string to the ring buffer. This is an abstraction of the
 * ring_buffer_write_char() function.
 *
 * This function is a helper function above ring_buffer_write_char().
 */
static inline void ring_buffer_write_string(char *str, uint32_t len)
{
    if (!str) {
        goto end;
    }
    for (uint32_t i = 0; i < len && str[i]; ++i) {
        ring_buffer_write_char(str[i]);
    }
end:
    return;
}

/*
 * Write a number in the ring buffer.
 * This function is a helper function above ring_buffer_write_char().
 */
static void ring_buffer_write_number(uint64_t value, uint8_t base)
{
    /* we define a local storage to hold the digits list
     * in any possible base up to base 2 (64 bits) */
    uint8_t number[64] = { 0 };
    int index = 0;

    for (; value / base != 0; value /= base) {
        number[index++] = value % base;
    }
    /* finishing with most significant unit */
    number[index++] = value % base;

    /* Due to the last 'index++', index is targetting the first free cell.
     * We make it points the last *used* cell instead */
    index--;

    /* now we can print out, starting with the most significant unit */
    for (; index >= 0; index--) {
        ring_buffer_write_digit(number[index]);
    }
}



/*
 * Print the ring buffer content (if there is some), and reset its
 * state to empty state.
 * The ring buffer is also memset'ed to 0.
 *
 * The buffer content is sent to the kernel log API.
 */
void print_and_reset_buffer(void)
{

    /* there is two cases here:
     *    * end is after start in the ring buffer. This means that
     *      all the string chars are contigous and can be printed once
     *    * start is after end, the string must be printed in two
     *      sections
     */
    if (ring_buffer.end > ring_buffer.start) {
        sys_log(ring_buffer.end - ring_buffer.start,
                &(ring_buffer.buf[ring_buffer.start]));
    } else if (ring_buffer.end < ring_buffer.start) {
        sys_log(BUF_SIZE - ring_buffer.start,
                &(ring_buffer.buf[ring_buffer.start]));
        sys_log(ring_buffer.end, &(ring_buffer.buf[0]));
    }
    /* reset the ring buffer flags now that the content has been
     * sent to the kernel I/O API
     */
    ring_buffer.end = 0;
    ring_buffer.start = ring_buffer.end;
    ring_buffer.full = false;

    for (uint32_t i = 0; i < BUF_MAX; i++) {
        ring_buffer.buf[i] = '\0';
    }
}

/*
 * Rewind the ring buffer of the given len. This function remove
 * len chars (at most) from the ring buffer and return the effectively
 * removed number of chars, depending on the current ring buffer state
 */
uint32_t ring_buffer_rewind(uint32_t len)
{
    if (len >= BUF_MAX) {
      return 0;
    }
    if (ring_buffer.end >= len) {
        for (uint16_t i = ring_buffer.end - len; i < ring_buffer.end; i++) {
            ring_buffer.buf[i] = '\0';
        }
        ring_buffer.end -= len;
    } else {
        uint32_t first = ring_buffer.end;
        for (uint16_t i = 0; i < ring_buffer.end; i++) {
            ring_buffer.buf[i] = '\0';
        }
        for (uint16_t i = BUF_MAX - len + first; i < BUF_MAX; i++) {
            ring_buffer.buf[i] = '\0';
        }
        ring_buffer.end =  BUF_MAX - len + first;
    }
    return len;
}


/*********************************************
 * other, not ring-buffer associated local utility functions
 */

/*
 * Return the number of digits of the given number, considering
 * the base in which the number is encoded.
 */
static uint8_t get_number_len(uint64_t value, uint8_t base)
{
    /* at least, if value is 0, its lenght is 1 digit */
    uint8_t len = 1;

    /* now we calculate the number of digits in the number */
    for (; value / base != 0; value /= base) {
        len++;
    }
    return len;
}

/**************************************************
 * printf lexer implementation
 *************************************************/

typedef enum {
    FS_NUM_DECIMAL,
    FS_NUM_HEX,
    FS_NUM_UCHAR,
    FS_NUM_SHORT,
    FS_NUM_LONG,
    FS_NUM_LONGLONG,
    FS_NUM_UNSIGNED,
} fs_num_mode_t;

typedef struct {
    bool           attr_0len;
    bool           attr_size;
    uint8_t        size;
    fs_num_mode_t  numeric_mode;
    bool           started;
    uint8_t        consumed;
} fs_properties_t;


/*
 * Handle one format string (starting with '%' char).
 *
 * This function transform a format string into an effective content using given
 * va_list argument.
 *
 * The function updated the consumed argument with the number of char consumed
 * by the format string itself, and return 0 if the format string has been
 * correctly parsed, or 1 if the format string parsing failed.
 */
static uint8_t print_handle_format_string(const char *fmt, va_list *args, uint8_t *consumed)
{
    fs_properties_t fs_prop = {
        .attr_0len     = false,
        .attr_size     = false,
        .size          = 0,
        .numeric_mode  = FS_NUM_DECIMAL, /*default */
        .started       = false,
        .consumed      = 0
    };

    /*
     * Sanitation
     */
    if (!fmt || !args || !consumed) {
        return 1;
    }

    /* Let parse the format string ... */
    do {
        /*
         * Handling '%' character
         */
        switch (fmt[fs_prop.consumed]) {
            case  '%':
            {
                if (fs_prop.started == false) {
                    /* starting string format parsing */
                    fs_prop.started = true;
                } else  if (fs_prop.consumed == 1) {
                    /* detecting '%' just after '%' */
                    ring_buffer_write_char('%');
                    /* => end of format string */
                    goto end;
                } else {
                    /* invalid: there is content before two '%' chars
                     * in the same format_string (e.g. %02%) */
                    goto err;
                }
                break;
            }
            case '0':
            {
                /*
                 * Handling '0' character
                 */
                if (fs_prop.started == false) {
                    goto err;
                }
                fs_prop.attr_0len = true;
                /* 0 must be completed with size content. We check it now */
                while (fmt[fs_prop.consumed + 1] >= '0' && fmt[fs_prop.consumed + 1] <= '9') {
                    /* getting back the size. Here only decimal values are handled */
                    fs_prop.size = (fs_prop.size * 10) + (fmt[fs_prop.consumed + 1] - '0');
                    fs_prop.consumed++;
                }
                /* if digits have been found after the 0len format string, attr_size is
                 * set to true
                 */
                if (fs_prop.size != 0) {
                    fs_prop.attr_size = true;
                }
                break;
            }
            case 'd':
            case 'i':
            {
                /*
                 * Handling integers
                 */
                if (fs_prop.started == false) {
                    goto err;
                }
                fs_prop.numeric_mode = FS_NUM_DECIMAL;
                int val = va_arg(*args, int);
                uint8_t len = get_number_len(val, 10);
                if (fs_prop.attr_size && fs_prop.attr_0len) {
                    /* we have to pad with 0 the number to reach
                     * the desired size */
                    for (uint32_t i = len; i < fs_prop.size; ++i) {
                        ring_buffer_write_char('0');
                    }
                }
                /* now we can print the number in argument */
                ring_buffer_write_number(val, 10);
                /* => end of format string */
                goto end;
            }
            case 'l':
            {
                /*
                 * Handling long and long long int
                 */
                long lval;
                long long llval;
                uint8_t len;

                if (fs_prop.started == false) {
                    goto err;
                }
                fs_prop.numeric_mode = FS_NUM_LONG;
                /* detecting long long */
                if (fmt[fs_prop.consumed + 1] == 'l') {
                    fs_prop.numeric_mode = FS_NUM_LONGLONG;
                    fs_prop.consumed++;
                }
                if (fs_prop.numeric_mode == FS_NUM_LONG) {
                    lval = va_arg(*args, long);
                    len = get_number_len(lval, 10);
                } else {
                    llval = va_arg(*args, long long);
                    len = get_number_len(llval, 10);
                }
                if (fs_prop.attr_size && fs_prop.attr_0len) {
                    /* we have to pad with 0 the number to reach
                     * the desired size */
                    for (uint32_t i = len; i < fs_prop.size; ++i) {
                        ring_buffer_write_char('0');
                    }
                }
                /* now we can print the number in argument */
                if (fs_prop.numeric_mode == FS_NUM_LONG) {
                    ring_buffer_write_number(lval, 10);
                } else {
                    ring_buffer_write_number(llval, 10);
                }
                /* => end of format string */
                goto end;
            }
            case 'h':
            {
                /*
                 * Handling long and long long int
                 */
                short s_val;
                unsigned char uc_val;
                uint8_t len;

                if (fs_prop.started == false) {
                    goto err;
                }
                fs_prop.numeric_mode = FS_NUM_SHORT;
                /* detecting long long */
                if (fmt[fs_prop.consumed + 1] == 'h') {
                    fs_prop.numeric_mode = FS_NUM_UCHAR;
                    fs_prop.consumed++;
                }
                if (fs_prop.numeric_mode == FS_NUM_SHORT) {
                    s_val = (short)va_arg(*args, int);
                    len = get_number_len(s_val, 10);
                } else {
                    uc_val = (unsigned char)va_arg(*args, int);
                    len = get_number_len(uc_val, 10);
                }
                if (fs_prop.attr_size && fs_prop.attr_0len) {
                    /* we have to pad with 0 the number to reach
                     * the desired size */
                    for (uint32_t i = len; i < fs_prop.size; ++i) {
                        ring_buffer_write_char('0');
                    }
                }
                /* now we can print the number in argument */
                if (fs_prop.numeric_mode == FS_NUM_SHORT) {
                    ring_buffer_write_number(s_val, 10);
                } else {
                    ring_buffer_write_number(uc_val, 10);
                }
                /* => end of format string */
                goto end;
            }
            case 'u':
            {
                /*
                 * Handling unsigned
                 */
                if (fs_prop.started == false) {
                    goto err;
                }
                fs_prop.numeric_mode = FS_NUM_UNSIGNED;
                uint32_t val = va_arg(*args, uint32_t);
                uint8_t len = get_number_len(val, 10);
                if (fs_prop.attr_size && fs_prop.attr_0len) {
                    /* we have to pad with 0 the number to reach
                     * the desired size */
                    for (uint32_t i = len; i < fs_prop.size; ++i) {
                        ring_buffer_write_char('0');
                    }
                }
                /* now we can print the number in argument */
                ring_buffer_write_number(val, 10);
                /* => end of format string */
                goto end;
            }
            case 'p':
            {
                /*
                 * Handling pointers. Include 0x prefix, as if using
                 * %#x format string in POSIX printf.
                 */
                if (fs_prop.started == false) {
                    goto err;
                }
                uint32_t val = va_arg(*args, physaddr_t);
                uint8_t len = get_number_len(val, 16);
                ring_buffer_write_string("0x", 2);
                for (uint32_t i = len; i < fs_prop.size; ++i) {
                    ring_buffer_write_char('0');
                }
                /* now we can print the number in argument */
                ring_buffer_write_number(val, 16);
                /* => end of format string */
                goto end;
            }

            case 'x':
            {
                /*
                 * Handling hexadecimal
                 */
                if (fs_prop.started == false) {
                    goto err;
                }
                fs_prop.numeric_mode = FS_NUM_UNSIGNED;
                uint32_t val = va_arg(*args, uint32_t);
                uint8_t len = get_number_len(val, 16);
                if (fs_prop.attr_size && fs_prop.attr_0len) {
                    /* we have to pad with 0 the number to reach
                     * the desired size */
                    for (uint32_t i = len; i < fs_prop.size; ++i) {
                        ring_buffer_write_char('0');
                    }
                }
                /* now we can print the number in argument */
                ring_buffer_write_number(val, 16);
                /* => end of format string */
                goto end;
            }
            case 'o':
            {
                /*
                 * Handling octal
                 */
                if (fs_prop.started == false) {
                    goto err;
                }
                fs_prop.numeric_mode = FS_NUM_UNSIGNED;
                uint32_t val = va_arg(*args, uint32_t);
                uint8_t len = get_number_len(val, 8);
                if (fs_prop.attr_size && fs_prop.attr_0len) {
                    /* we have to pad with 0 the number to reach
                     * the desired size */
                    for (uint32_t i = len; i < fs_prop.size; ++i) {
                        ring_buffer_write_char('0');
                    }
                }
                /* now we can print the number in argument */
                ring_buffer_write_number(val, 8);

                /* => end of format string */
                goto end;
            }
            case 's':
            {
                /*
                 * Handling strings
                 */
                if (fs_prop.started == false) {
                    goto err;
                }
                /* no size or 0len attribute for strings */
                if (fs_prop.attr_size && fs_prop.attr_0len) {
                    goto err;
                }
                char *str = va_arg(*args, char*);
                /* now we can print the number in argument */
                ring_buffer_write_string(str, strlen(str));

                /* => end of format string */
                goto end;
            }
            case 'c':
            {
                /*
                 * Handling chars
                 */
                if (fs_prop.started == false) {
                    goto err;
                }
                /* no size or 0len attribute for strings */
                if (fs_prop.attr_size && fs_prop.attr_0len) {
                    goto err;
                }
                unsigned char val = (unsigned char)va_arg(*args, int);
                /* now we can print the number in argument */
                ring_buffer_write_char(val);

                /* => end of format string */
                goto end;
            }

            /* none of the above. Unsupported format */
            default:
            {
                /* should not happend, unable to parse format string */
                goto err;
                break;
            }

        }
        fs_prop.consumed++;
    } while (fmt[fs_prop.consumed]);
end:
    *consumed = fs_prop.consumed + 1; /* consumed is starting with 0 */
    return 0;
err:
    *consumed = 0;
    return 1;
}


/*
 * Print a given fmt string, considering variable arguments given in args.
 * This function *does not* flush the ring buffer, but only fullfill it.
 */
int print(char *fmt, va_list args)
{
    int i = 0;
    uint8_t consumed = 0;

    while (fmt[i]) {
        if (fmt[i] == '%') {
             if (print_handle_format_string(&(fmt[i]), &args, &consumed) != 0) {
                 /* the string format parsing has failed ! */
                 goto err;
             }
             i += consumed;
        } else {
            ring_buffer_write_char(fmt[i++]);
        }
    }
    return i;
err:
    return -1;
}


/*************************************************************
 * libstream exported API implementation: POSIX compilant API
 ************************************************************/

/*
 * Standard printf API.
 *
 */
int printf(char *fmt, ...)
{
    int res = 0;
    va_list args;

    /* locking the ring buffer, waiting if needed */
    mutex_lock(&rb_lock);
    /*
     * if there is some asyncrhonous printf to pass to the kernel, do it
     * before execute the current printf command
     */
    print_and_reset_buffer();
    va_start(args, fmt);
    res = print(fmt, args);
    va_end(args);
    print_and_reset_buffer();
    /* unlocking the ring buffer */
    mutex_unlock(&rb_lock);
    return res;
}

int snprintf(char *dst, size_t len, char *fmt, ...)
{
    va_list args;
    size_t sizew = 0;
    size_t to_copy;

    /* sanitize */
    if (!dst) {
        return -1;
    }
    if (!mutex_trylock(&rb_lock)) {
        /* unable to lock the ring buffer, another context is currently
         * using it. As we may be executed in ISR mode, we prefer to
         * give up instead of waiting for the buffer to be released */
        return -1;
    }
    va_start(args, fmt);
    sizew = print(fmt, args);
    va_end(args);
    /* copy the string we have just written to the ring buffer
     * into the dst string
     */
    if (sizew >= len) {
       /* POSIX specify that len includes the terminating byte */
       to_copy = len - 1;
    } else {
      to_copy = sizew;
    }
    memcpy(dst, &(ring_buffer.buf[ring_buffer.end - sizew]), to_copy);
    dst[to_copy] = '\0';
    /* rewind ring buffer content we have just written */
    ring_buffer_rewind(sizew);
    /* unlocking the ring buffer */
    mutex_unlock(&rb_lock);
    /* returning the number of written chars, casted to int
     * as defined by POSIX standard, to support negative return
     * on error.
     * We consider here that size_t is smaller enough to
     * be casted into int without being truncated
     */
    return (int)to_copy;
}

int sprintf(char *dst, char *fmt, ...)
{
    va_list args;
    size_t sizew = 0;

    /* sanitize */
    if (!dst) {
        return -1;
    }
    if (!mutex_trylock(&rb_lock)) {
        /* unable to lock the ring buffer, another context is currently
         * using it. As we may be executed in ISR mode, we prefer to
         * give up instead of waiting for the buffer to be released */
        return -1;
    }
    va_start(args, fmt);
    sizew = print(fmt, args);
    va_end(args);
    /* copy the string we have just written to the ring buffer
     * into the dst string
     */
    memcpy(dst, &(ring_buffer.buf[ring_buffer.end - sizew]), sizew);
    dst[sizew] = '\0';
    /* rewind ring buffer content we have just written */
    ring_buffer_rewind(sizew);
    /* unlocking the ring buffer */
    mutex_unlock(&rb_lock);
    /* returning the number of written chars, casted to int
     * as defined by POSIX standard, to support negative return
     * on error.
     * We consider here that size_t is smaller enough to
     * be casted into int without being truncated
     */
    return (int)sizew;
}

/***********************************************************
 * libstream exported API implementation: custom API
 **********************************************************/


/* asyncrhonous printf, for handlers */
int aprintf(char *fmt, ...)
{
    int res = -1;
    va_list args;

    if (!mutex_trylock(&rb_lock)) {
        /* unable to lock the ring buffer, another context is currently
         * using it. As we may be executed in ISR mode, we prefer to
         * give up instead of waiting for the buffer to be released */
        return res;
    }
    va_start(args, fmt);
    res = print(fmt, args);
    va_end(args);
    /* unlocking the ring buffer */
    mutex_unlock(&rb_lock);
    return res;
}

int aprintf_flush(void)
{
    if (!mutex_trylock(&rb_lock)) {
        /* unable to lock the ring buffer, another context is currently
         * using it. we prefer to give up instead of waiting for the
         * buffer to be released */
        return -1;
    }
    print_and_reset_buffer();
    /* unlocking the ring buffer */
    mutex_unlock(&rb_lock);
    return 0;
}



