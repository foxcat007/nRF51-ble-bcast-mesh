/***********************************************************************************
Copyright (c) Nordic Semiconductor ASA
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions and the following disclaimer in the documentation and/or
  other materials provided with the distribution.

  3. Neither the name of Nordic Semiconductor ASA nor the names of other
  contributors to this software may be used to endorse or promote products
  derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
************************************************************************************/
#include <stdio.h>
#include <string.h>
#include "bootloader_info.h"
#include "dfu_types_mesh.h"
#include "bootloader_app_bridge.h"
#include "nrf51.h"
#include "nrf_error.h"
#include "toolchain.h"
#include "SEGGER_RTT.h"

#define WORD_ALIGN(data) data = (((uint32_t) data + 4) & 0xFFFFFFFC)

#define HEADER_LEN       (sizeof(bootloader_info_header_t))

#ifdef DEBUG
#undef DEBUG
#define DEBUG 0
#endif

#define PIN_INVALIDATE      (0)
#define PIN_RESET           (1)
#define PIN_ENTRY_GET       (2)
#define PIN_SET_LEN         (3)
#define PIN_ENTRY_PUT       (4)
#define PIN_INIT            (5)

#define INFO_WRITE_BUFLEN   (128)

typedef enum
{
    BL_INFO_STATE_UNINITIALIZED,
    BL_INFO_STATE_IDLE,
    BL_INFO_STATE_AWAITING_RECOVERY,
    BL_INFO_STATE_BACKUP,
    BL_INFO_STATE_RESET,
    BL_INFO_STATE_RECOVER
} bl_info_state_t;

typedef struct
{
    uint16_t len;
    uint16_t type;
} bootloader_info_header_t;

typedef struct
{
    bootloader_info_header_t header;
    bl_info_entry_t entry;
} info_buffer_t;

typedef enum
{
    INFO_COPY_STATE_IDLE,
    INFO_COPY_STATE_ERASE,
    INFO_COPY_STATE_METAWRITE,
    INFO_COPY_STATE_DATAWRITE,
    INFO_COPY_STATE_WAIT_FOR_IDLE,
} info_copy_state_t;

typedef struct
{
    info_copy_state_t state;
    bootloader_info_t* p_dst;
    bootloader_info_t* p_src;
    info_buffer_t* p_src_info;
    info_buffer_t* p_dst_info;
} info_copy_t;

static bootloader_info_t*               mp_bl_info_page;
static bootloader_info_t*               mp_bl_info_bank_page;
static uint8_t                          mp_info_entry_buffer[INFO_WRITE_BUFLEN] __attribute__((aligned(4)));
static info_buffer_t*                   mp_info_entry_head;
static info_buffer_t*                   mp_info_entry_tail;
static const bootloader_info_header_t   m_invalid_header = {0xFFFF, 0x0000}; /**< Flash buffer used for invalidating entries. */
static const bootloader_info_header_t   m_last_header = {0xFFFF, BL_INFO_TYPE_LAST}; /**< Flash buffer used for flashing the "last" header. */
static bl_info_state_t                  m_state;
static bool                             m_page_full;
static info_copy_t                      m_info_copy;
static char*                            mp_copy_state_str[] = {"IDLE", "ERASE", "METAWRITE", "DATAWRITE", "WAIT FOR IDLE"};

/******************************************************************************
* Static functions
******************************************************************************/
static inline info_buffer_t* bootloader_info_iterate(info_buffer_t* p_buf)
{
    return (info_buffer_t*) (((uint32_t) p_buf) + ((uint32_t) p_buf->header.len) * 4);
}

static bl_info_entry_t* info_entry_get(bootloader_info_t* p_bl_info_page, bl_info_type_t type)
{
    if (p_bl_info_page->metadata.metadata_len == 0xFF)
    {
        return NULL;
    }
    info_buffer_t* p_buffer =
        (info_buffer_t*)
        ((uint32_t) p_bl_info_page + ((bootloader_info_t*) p_bl_info_page)->metadata.metadata_len);

    uint32_t iterations = 0;
    bl_info_type_t iter_type = (bl_info_type_t) p_buffer->header.type;
    while (iter_type != type)
    {
        p_buffer = bootloader_info_iterate(p_buffer);
        if ((uint32_t) p_buffer > ((uint32_t) p_bl_info_page) + PAGE_SIZE ||
            (
             (iter_type == BL_INFO_TYPE_LAST) &&
             (iter_type != type)
            ) ||
            ++iterations > PAGE_SIZE / 2)
        {
            return NULL; /* out of bounds */
        }
        iter_type = (bl_info_type_t) p_buffer->header.type;
    }
    return &p_buffer->entry;
}

static uint32_t entry_header_invalidate(bootloader_info_header_t* p_header)
{
    /* TODO: optimization: check if the write only adds 0-bits to the current value,
       in which case we can just overwrite the current. */
    if ((uint32_t) p_header & 0x03)
    {
        APP_ERROR_CHECK(NRF_ERROR_INVALID_ADDR);
    }

    return flash_write((uint32_t*) p_header, (uint8_t*) &m_invalid_header, HEADER_LEN);
}

static void free_write_buffer(info_buffer_t* p_buf)
{
    /* This must happen in order. */
    APP_ERROR_CHECK_BOOL(p_buf == mp_info_entry_tail);

    /* The end-of-entries entry may have max length - treat as empty entry */
    if (mp_info_entry_tail->header.len == 0xFFFF)
    {
        mp_info_entry_head->header.len += 1;
    }
    else
    {
        mp_info_entry_head->header.len += p_buf->header.len;
    }
    mp_info_entry_tail = (info_buffer_t*) ((uint32_t) p_buf + p_buf->header.len * 4);
    if (mp_info_entry_tail == (info_buffer_t*) (mp_info_entry_buffer + INFO_WRITE_BUFLEN))
    {
        mp_info_entry_tail = (info_buffer_t*) mp_info_entry_buffer;
    }
    if (mp_info_entry_tail->header.type == BL_INFO_TYPE_LAST &&
        mp_info_entry_tail->header.len == 0xFFFF)
    {
        /* Also free the end-entry */
        mp_info_entry_tail  = ((info_buffer_t*) ((uint8_t*) mp_info_entry_tail + 4));

        if (mp_info_entry_tail == (info_buffer_t*) (mp_info_entry_buffer + INFO_WRITE_BUFLEN))
        {
            mp_info_entry_tail = (info_buffer_t*) mp_info_entry_buffer;
        }
    }
}

static void place_head_after_entry(info_buffer_t* p_entry)
{
    mp_info_entry_head = (info_buffer_t*) ((uint32_t) p_entry + p_entry->header.len * 4);
    if (mp_info_entry_head == (info_buffer_t*) (mp_info_entry_buffer + INFO_WRITE_BUFLEN))
    {
        mp_info_entry_head = (info_buffer_t*) mp_info_entry_buffer;
    }
    if (mp_info_entry_head != mp_info_entry_tail)
    {
        mp_info_entry_head->header.len = ((uint32_t) mp_info_entry_tail - (uint32_t) mp_info_entry_head) / 4;
        if (mp_info_entry_tail < mp_info_entry_head)
        {
            mp_info_entry_head->header.len += INFO_WRITE_BUFLEN / 4;
        }
        mp_info_entry_head->header.type = BL_INFO_TYPE_INVALID;
    }
}

static info_buffer_t* get_write_buffer(uint32_t req_space)
{
    uint32_t was_masked;
    _DISABLE_IRQS(was_masked);

    if (mp_info_entry_head->header.len * 4 < req_space || mp_info_entry_head->header.type != BL_INFO_TYPE_INVALID)
    {
        _ENABLE_IRQS(was_masked);
        return NULL;
    }

    if (mp_info_entry_head == mp_info_entry_tail)
    {
        /* the entire buffer is empty, let's just reset */
        mp_info_entry_head = (info_buffer_t*) mp_info_entry_buffer;
        mp_info_entry_tail = (info_buffer_t*) mp_info_entry_buffer;

        info_buffer_t* p_ret = mp_info_entry_head;
        p_ret->header.len = req_space / 4;
        p_ret->header.type = BL_INFO_TYPE_INVALID;
        place_head_after_entry(p_ret);
        _ENABLE_IRQS(was_masked);
        return p_ret;
    }

    const uint32_t space_until_rollover = ((uint32_t) mp_info_entry_buffer + INFO_WRITE_BUFLEN - (uint32_t) mp_info_entry_head);

    /* check if we have to put it on the other side of the rollover. */
    if (req_space > space_until_rollover)
    {
        /* can we fit it at the other side of the rollover? */
        if (req_space <= (uint32_t) mp_info_entry_head->header.len * 4 - space_until_rollover)
        {
            /* the last entry before the rollover is now padding, let's mark it as unused */
            uint16_t old_head_len = mp_info_entry_head->header.len;
            mp_info_entry_head->header.type = BL_INFO_TYPE_LAST;
            mp_info_entry_head->header.len = space_until_rollover / 4;

            mp_info_entry_head = (info_buffer_t*) mp_info_entry_buffer;
            info_buffer_t* p_ret = mp_info_entry_head;
            p_ret->header.len = req_space / 4;
            p_ret->header.type = BL_INFO_TYPE_INVALID;
            place_head_after_entry(p_ret);
            _ENABLE_IRQS(was_masked);
            return p_ret;
        }
        else
        {
            /* Couldn't fit the entry at either side of the rollover */
            _ENABLE_IRQS(was_masked);
            return NULL;
        }
    }
    else
    {
        /* straight forward put */
        info_buffer_t* p_ret = mp_info_entry_head;
        p_ret->header.len = req_space / 4;
        p_ret->header.type = BL_INFO_TYPE_INVALID;
        place_head_after_entry(p_ret);
        _ENABLE_IRQS(was_masked);
        return p_ret;
    }
}

static bootloader_info_header_t* bootloader_info_header_get(bl_info_entry_t* p_entry)
{
    if (p_entry == NULL || (uint32_t) p_entry == FLASH_SIZE)
    {
        return NULL;
    }
    return (bootloader_info_header_t*) ((uint32_t) p_entry - 4 * mp_bl_info_page->metadata.entry_header_length);
}

static inline info_buffer_t* bootloader_info_first_unused_get(bootloader_info_t* p_bl_info_page)
{
    bl_info_entry_t* p_entry = info_entry_get(
            p_bl_info_page,
            BL_INFO_TYPE_LAST);

    return (info_buffer_t*) (p_entry ? bootloader_info_header_get(p_entry) : NULL);
}

static void info_copy_state_set(info_copy_state_t state)
{
    m_info_copy.state = state;
    SEGGER_RTT_printf(0, "STATE: %s\n", mp_copy_state_str[state]);
}

/* COPY PAGE FUNCTIONS */
static void copy_page_write_meta(void)
{
    /* Write metadata header */
    SEGGER_RTT_WriteString(0, "Writing meta to dest\n");
    flash_write(&m_info_copy.p_dst->metadata,
                &m_info_copy.p_src->metadata,
                sizeof(m_info_copy.p_src->metadata));
}

static void copy_page_write_next_data(void)
{
    while ((uint32_t) m_info_copy.p_src_info < ((uint32_t) m_info_copy.p_src + PAGE_SIZE))
    {
        bl_info_type_t type = (bl_info_type_t) m_info_copy.p_src_info->header.type;

        if (type == BL_INFO_TYPE_UNUSED)
        {
            /* We've reached the end */
            break;
        }

        if (type != BL_INFO_TYPE_INVALID)
        {
            SEGGER_RTT_printf(0, "Write entry 0x%x to dest... ", type);
            /* copy entry */
            uint32_t len = m_info_copy.p_src_info->header.len * 4;

            if (type == BL_INFO_TYPE_LAST)
            {
                len = 4;
            }

            if ((uint8_t*) m_info_copy.p_dst_info + len > (uint8_t*) m_info_copy.p_dst + PAGE_SIZE)
            {
                /* Implementation error */
                APP_ERROR_CHECK(NRF_ERROR_INTERNAL);
            }

            if (flash_write(m_info_copy.p_dst_info, m_info_copy.p_src_info, len) != NRF_SUCCESS)
            {
                /* We've filled the flash queue. Wait for a slot to open to move on. */
                SEGGER_RTT_WriteString(0, "FAIL\n");
                return;
            }

            SEGGER_RTT_WriteString(0, "OK\n");
            /* Only iterate the dst entry if we actually wrote anything. */
            m_info_copy.p_dst_info = (info_buffer_t*) ((uint8_t*) m_info_copy.p_dst_info + len);
        }

        m_info_copy.p_src_info = bootloader_info_iterate(m_info_copy.p_src_info);
    }

    /* we only get here when we're done writing all the data */
    info_copy_state_set(INFO_COPY_STATE_WAIT_FOR_IDLE);
}

static void copy_page_on_flash_idle(void)
{
    switch (m_info_copy.state)
    {
        case INFO_COPY_STATE_ERASE:
            /* We went idle without getting the erase operation success, try again */
            flash_erase(m_info_copy.p_dst, PAGE_SIZE);
            break;
        case INFO_COPY_STATE_METAWRITE:
            /* We went idle without getting the write operation success, try again */
            copy_page_write_meta();
            break;
        case INFO_COPY_STATE_WAIT_FOR_IDLE:
            /* all operations have finished. */
            info_copy_state_set(INFO_COPY_STATE_IDLE);
            break;
        default:
            break;
    }
}

static void copy_page_on_flash_op_end(flash_op_type_t op, void* p_context)
{
    /* execute next step in the process. */
    SEGGER_RTT_WriteString(0, "\tflash op end\n");
    switch (m_info_copy.state)
    {
        case INFO_COPY_STATE_ERASE:
            if (op == FLASH_OP_TYPE_ERASE && p_context == m_info_copy.p_dst)
            {
                info_copy_state_set(INFO_COPY_STATE_METAWRITE);
                copy_page_write_meta();
            }
            break;
        case INFO_COPY_STATE_METAWRITE:
            if (op == FLASH_OP_TYPE_WRITE && p_context == &m_info_copy.p_src->metadata)
            {
                SEGGER_RTT_WriteString(0, "Writing data to dest\n");
                info_copy_state_set(INFO_COPY_STATE_DATAWRITE);
                m_info_copy.p_src_info = (info_buffer_t*) m_info_copy.p_src->data;
                m_info_copy.p_dst_info = (info_buffer_t*) m_info_copy.p_dst->data;
                copy_page_write_next_data();
            }
            else
            {
                SEGGER_RTT_printf(0, "METAWRITE FAIL: op=0x%x, context = 0x%x\n", op, p_context);
            }
            break;
        case INFO_COPY_STATE_DATAWRITE:
            copy_page_write_next_data();
            break;

        default:
            break;
    }
}

static uint32_t copy_page(bootloader_info_t* p_dst, bootloader_info_t* p_src)
{
    if (m_info_copy.state != INFO_COPY_STATE_IDLE)
    {
        return NRF_ERROR_INVALID_STATE;
    }
    SEGGER_RTT_printf(0, "COPYING 0x%x->0x%x\n", p_src, p_dst);

    m_info_copy.p_dst = p_dst;
    m_info_copy.p_src = p_src;
    info_copy_state_set(INFO_COPY_STATE_ERASE);
    flash_erase(p_dst, PAGE_SIZE);
    return NRF_SUCCESS;
}

static uint32_t backup(void)
{
    uint32_t error_code = copy_page(mp_bl_info_bank_page, mp_bl_info_page);
    if (error_code == NRF_SUCCESS)
    {
        m_state = BL_INFO_STATE_BACKUP;
    }
    return error_code;
}

/** Pull in all entries from the bank. */
static uint32_t recover(void)
{
    uint32_t error_code = copy_page(mp_bl_info_page, mp_bl_info_bank_page);
    if (error_code == NRF_SUCCESS)
    {
        m_state = BL_INFO_STATE_RECOVER;
    }
    return error_code;
}

uint32_t entry_invalidate(bootloader_info_t* p_info_page, bl_info_type_t type)
{
    APP_ERROR_CHECK_BOOL(m_state == BL_INFO_STATE_IDLE);

    bootloader_info_header_t* p_header = bootloader_info_header_get(info_entry_get(p_info_page, type));
    if (p_header)
    {
        return entry_header_invalidate(p_header);
    }
    return NRF_ERROR_NOT_FOUND;
}

uint32_t entry_write(info_buffer_t* p_dst, bl_info_type_t type, bl_info_entry_t* p_entry, uint32_t entry_len, bool is_last_entry)
{
    entry_len = (entry_len + 3) & ~0x03; /* word-pad */
    info_buffer_t* p_write_buf = get_write_buffer(HEADER_LEN + entry_len + (is_last_entry * HEADER_LEN));
    if (p_write_buf == NULL)
    {
        return NRF_ERROR_NO_MEM;
    }

    if (is_last_entry)
    {
        /* set the padding + last-header to FF */
        memset((uint8_t*) p_write_buf + entry_len, 0xFF, 8);
        ((bootloader_info_header_t*) ((uint8_t*) p_write_buf + HEADER_LEN + entry_len))->type = BL_INFO_TYPE_LAST;
    }

    p_write_buf->header.len = (entry_len + HEADER_LEN + 3) / 4;
    p_write_buf->header.type = type;
    memcpy(&p_write_buf->entry, p_entry, entry_len);

    APP_ERROR_CHECK(flash_write((uint32_t*) p_dst, (uint8_t*) p_write_buf, entry_length + (is_last_entry * HEADER_LEN)));

    return NRF_SUCCESS;
}

/******************************************************************************
* Interface functions
******************************************************************************/
uint32_t bootloader_info_init(uint32_t* p_bl_info_page, uint32_t* p_bl_info_bank_page)
{
    m_state = BL_INFO_STATE_UNINITIALIZED;

    if ((uint32_t) p_bl_info_page & (PAGE_SIZE - 1) ||
        (uint32_t) p_bl_info_bank_page & (PAGE_SIZE - 1))
    {
        return NRF_ERROR_INVALID_ADDR; /* must be page aligned */
    }

    mp_bl_info_page      = (bootloader_info_t*) p_bl_info_page;
    mp_bl_info_bank_page = (bootloader_info_t*) p_bl_info_bank_page;
    mp_info_entry_head = (info_buffer_t*) mp_info_entry_buffer;
    mp_info_entry_tail = (info_buffer_t*) mp_info_entry_buffer;
    mp_info_entry_head->header.len = INFO_WRITE_BUFLEN / 4;

    /* make sure we have an end-of-entries entry */
    if (bootloader_info_first_unused_get(mp_bl_info_page) == NULL)
    {
        /* need to restore the bank */
        info_buffer_t* p_first_unused = bootloader_info_first_unused_get(mp_bl_info_bank_page);
        if (p_first_unused == NULL)
        {
            /* bank is invalid too, no way to recover */
            return NRF_ERROR_INVALID_DATA;
        }

        APP_ERROR_CHECK(flash_erase((uint32_t*) mp_bl_info_page, PAGE_SIZE));
        APP_ERROR_CHECK(flash_write((uint32_t*) mp_bl_info_page,
                            (uint8_t*) mp_bl_info_bank_page,
                            (uint32_t) p_first_unused + 4 - (uint32_t) mp_bl_info_bank_page));
    }

    m_state = BL_INFO_STATE_IDLE;
    return NRF_SUCCESS;
}

bootloader_info_t* bootloader_info_get(void)
{
    return (bootloader_info_t*) (mp_bl_info_page);
}

bl_info_entry_t* bootloader_info_entry_get(bl_info_type_t type)
{
    return info_entry_get((bootloader_info_t*) BOOTLOADER_INFO_ADDRESS, type);
}

bl_info_entry_t* bootloader_info_entry_put(bl_info_type_t type,
                                           bl_info_entry_t* p_entry,
                                           uint32_t length)
{
    APP_ERROR_CHECK_BOOL(m_state == BL_INFO_STATE_IDLE);

    /* previous entry, to be invalidated after we've stored the current entry. */
    bootloader_info_header_t* p_old_header =
        bootloader_info_header_get(bootloader_info_entry_get(type));

    /* find first unused space */
    info_buffer_t* p_new_buf =
        bootloader_info_first_unused_get(mp_bl_info_page);

    /* we have to be able to fit the end-of-entries entry at the last word, therefore check entry
       length + HEADER_LEN */
    if (p_new_buf == NULL ||
        ((uint32_t) p_new_buf + HEADER_LEN + length + HEADER_LEN) >= (uint32_t) ((uint32_t) mp_bl_info_page + PAGE_SIZE))
    {
        APP_ERROR_CHECK(NRF_ERROR_INVALID_STATE);
        return NULL;
    }

    /* store new entry in the available space, and pad */
    if (entry_write(p_new_buf, type, p_entry, length, true) != NRF_SUCCESS)
    {
        APP_ERROR_CHECK(NRF_ERROR_INVALID_STATE);
        return NULL;
    }

    /* invalidate old entry of this type */
    if (p_old_header != NULL)
    {
        if (entry_header_invalidate(p_old_header) != NRF_SUCCESS)
        {
            return NULL;
        }
    }

    /* We've run out of space, and need to recover. */
    if (((uint32_t) p_new_buf + HEADER_LEN + length + HEADER_LEN) >= (uint32_t) ((uint32_t) mp_bl_info_page + PAGE_SIZE))
    {
        bootloader_info_reset();
    }
    SEGGER_RTT_printf(0, "PUT 0x%x @0x%x\n", type, &p_new_buf->entry);
    return &p_new_buf->entry;
}

uint32_t bootloader_info_entry_overwrite(bl_info_type_t type, bl_info_entry_t* p_entry)
{
    info_buffer_t* p_dst = (info_buffer_t*) bootloader_info_header_get(bootloader_info_entry_get(type));
    if (p_dst == NULL)
    {
        return NRF_ERROR_NOT_FOUND;
    }
    /* store new entry in the available space, and pad */
    uint32_t entry_length = p_dst->header.len * 4 - HEADER_LEN;

    return entry_write(p_dst, type, p_entry, entry_length, false);
}

uint32_t bootloader_info_entry_invalidate(bl_info_type_t type)
{
    return entry_invalidate((bootloader_info_t*) BOOTLOADER_INFO_ADDRESS, type);
}

uint32_t bootloader_info_reset(void)
{
    APP_ERROR_CHECK_BOOL(m_state == BL_INFO_STATE_IDLE);

    SEGGER_RTT_WriteString(0, "RESET\n");
    m_state = BL_INFO_STATE_RESET;
    m_page_full = true;
    return backup();
}

bool bootloader_info_available(void)
{
    return (m_state == BL_INFO_STATE_IDLE);
}

void bootloader_info_on_flash_op_end(flash_op_type_t type, void* p_context)
{
    switch (m_state)
    {
        case BL_INFO_STATE_IDLE:
            if (type == FLASH_OP_TYPE_WRITE)
            {
                if (p_context >= (void*) mp_info_entry_buffer &&
                    p_context < (void*) ((uint32_t) mp_info_entry_buffer + INFO_WRITE_BUFLEN))
                {
                    free_write_buffer(p_context);
                }
            }
            break;
        default:
            break;
    }
    copy_page_on_flash_op_end(type, p_context);
}

void bootloader_info_on_flash_idle(void)
{
    SEGGER_RTT_WriteString(0, "IDLE\n");
    if (m_info_copy.state != INFO_COPY_STATE_IDLE)
    {
        copy_page_on_flash_idle();
        if (m_info_copy.state == INFO_COPY_STATE_IDLE && m_page_full)
        {
            m_page_full = false;
            recover();
        }
    }
}


