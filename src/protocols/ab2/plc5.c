/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever     *
 * you choose.                                                             *
 *                                                                         *
 * MPL 2.0:                                                                *
 *                                                                         *
 *   This Source Code Form is subject to the terms of the Mozilla Public   *
 *   License, v. 2.0. If a copy of the MPL was not distributed with this   *
 *   file, You can obtain one at http://mozilla.org/MPL/2.0/.              *
 *                                                                         *
 *                                                                         *
 * LGPL 2:                                                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include <stddef.h>
#include <ab2/plc5.h>
#include <ab2/common_defs.h>
#include <ab2/df1.h>
#include <ab2/pccc_eip_plc.h>
#include <util/attr.h>
#include <util/debug.h>
#include <util/mem.h>
#include <util/plc.h>
#include <util/string.h>


typedef struct {
    struct plc_tag_t base_tag;

    uint16_t elem_size;
    uint16_t elem_count;

    /* data type info */
    df1_file_t data_file_type;
    int data_file_num;
    int data_file_elem;
    int data_file_sub_elem;

    /* plc and request info */
    plc_p plc;
    struct plc_request_s request;

    uint16_t tsn; /* transfer sequence number of the most recent request. */
    uint16_t trans_offset;

} ab2_plc5_tag_t;
typedef ab2_plc5_tag_t *ab2_plc5_tag_p;


#define PLC5_RANGE_READ_FUNC ((uint8_t)(0x01))
#define PLC5_RANGE_WRITE_FUNC ((uint8_t)(0x00))

#define PLC5_WORD_RANGE_READ_MAX_PAYLOAD (244)
#define PLC5_WORD_RANGE_WRITE_MAX_PAYLOAD (244)

static void plc5_tag_destroy(void *tag_arg);

static int plc5_tag_abort(plc_tag_p tag);
static int plc5_tag_read(plc_tag_p tag);
static int plc5_tag_status(plc_tag_p tag);
static int plc5_tag_tickler(plc_tag_p tag);
static int plc5_tag_write(plc_tag_p tag);
static int plc5_get_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int default_value);
static int plc5_set_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int new_value);


/* vtable for PLC-5 tags */
static struct tag_vtable_t plc5_vtable = {
    plc5_tag_abort,
    plc5_tag_read,
    plc5_tag_status,
    plc5_tag_tickler,
    plc5_tag_write,

    /* attribute accessors */
    plc5_get_int_attrib,
    plc5_set_int_attrib
};


static int build_read_request_callback(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id req_id);
static int handle_read_response_callback(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id req_id);
static int build_write_request_callback(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id req_id);
static int handle_write_response_callback(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id req_id);

static int encode_plc5_logical_address(uint8_t *buffer, int buffer_capacity, int *offset, int data_file_num, int data_file_elem, int data_file_sub_elem);

plc_tag_p ab2_plc5_tag_create(attr attribs)
{
    int rc = PLCTAG_STATUS_OK;
    ab2_plc5_tag_p tag = NULL;
    plc_p plc = NULL;
    const char *tag_name = NULL;

    pdebug(DEBUG_INFO, "Starting.");

    tag = (ab2_plc5_tag_p)base_tag_create(sizeof(*tag), (void (*)(void*))plc5_tag_destroy);
    if(!tag) {
        pdebug(DEBUG_WARN, "Unable to allocate new PLC/5 tag!");
        return NULL;
    }

    /* parse the PLC-5 tag name */
    tag_name = attr_get_str(attribs, "name", NULL);
    if(!tag_name) {
        pdebug(DEBUG_WARN, "Data file name and offset missing!");
        rc_dec(tag);
        return NULL;
    }

    rc = df1_parse_logical_address(tag_name, &(tag->data_file_type),&(tag->data_file_num), &(tag->data_file_elem), &(tag->data_file_sub_elem));
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Malformed data file name!");
        rc_dec(tag);
        return NULL;
    }

    plc = pccc_eip_plc_get(attribs);
    if(!plc) {
        pdebug(DEBUG_WARN, "Unable to get PLC!");
        rc_dec(tag);
        return NULL;
    }

    /* set the vtable for base functions. */
    tag->base_tag.vtable = &plc5_vtable;

    pdebug(DEBUG_INFO, "Done.");

    return (plc_tag_p)tag;
}


/* helper functions. */
void plc5_tag_destroy(void *tag_arg)
{
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Null tag pointer passed to destructor!");
        return;
    }

    /* get rid of any outstanding timers and events. */

    /* unlink the protocol layers. */
    tag->plc = rc_dec(tag->plc);

    /* delete the base tag parts. */
    base_tag_destroy((plc_tag_p)tag);

    pdebug(DEBUG_INFO, "Done.");
}


int plc5_tag_abort(plc_tag_p tag_arg)
{
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    plc_stop_request(tag->plc, &(tag->request));

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_OK;
}


int plc5_tag_read(plc_tag_p tag_arg)
{
    int rc = PLCTAG_STATUS_OK;
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    rc = plc_start_request(tag->plc, &(tag->request), tag, build_read_request_callback, handle_read_response_callback);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to start read request!");
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}


int plc5_tag_status(plc_tag_p tag_arg)
{
    int rc = PLCTAG_STATUS_OK;
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    rc = tag->base_tag.status;

    pdebug(DEBUG_INFO, "Done.");

    return rc;
}


int plc5_tag_tickler(plc_tag_p tag)
{
    (void)tag;

    pdebug(DEBUG_INFO, "Starting.");

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_ERR_UNSUPPORTED;
}


int plc5_tag_write(plc_tag_p tag_arg)
{
    int rc = PLCTAG_STATUS_OK;
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)tag_arg;

    pdebug(DEBUG_INFO, "Starting.");

    if(!tag) {
        pdebug(DEBUG_WARN, "Tag pointer is null!");
        return PLCTAG_ERR_NULL_PTR;
    }

    rc = plc_start_request(tag->plc, &(tag->request), (void *)tag, build_write_request_callback, handle_write_response_callback);
    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to start read request!");
        return rc;
    }

    pdebug(DEBUG_INFO, "Done.");

    return PLCTAG_STATUS_PENDING;
}


int plc5_get_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int default_value)
{
    int res = default_value;
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)raw_tag;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* assume we have a match. */
    tag->base_tag.status = PLCTAG_STATUS_OK;

    /* match the attribute. */
    if(str_cmp_i(attrib_name, "elem_size") == 0) {
        res = tag->elem_size;
    } else if(str_cmp_i(attrib_name, "elem_count") == 0) {
        res = tag->elem_count;
    } else {
        pdebug(DEBUG_WARN, "Unsupported attribute name \"%s\"!", attrib_name);
        tag->base_tag.status = PLCTAG_ERR_UNSUPPORTED;
        return default_value;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return res;
}


int plc5_set_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int new_value)
{
    (void)attrib_name;
    (void)new_value;

    pdebug(DEBUG_WARN, "Unsupported attribute \"%s\"!", attrib_name);

    raw_tag->status  = PLCTAG_ERR_UNSUPPORTED;

    return PLCTAG_ERR_UNSUPPORTED;
}



int build_read_request_callback(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id req_id)
{
    int rc = PLCTAG_STATUS_OK;
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)context;
    int req_off = *data_start;
    int max_trans_size = 0;
    int num_elems = 0;
    int trans_size = 0;

    (void)req_id;

    pdebug(DEBUG_DETAIL, "Starting.");

    /* encode the request. */

    do {
        /* PCCC command type byte */
        TRY_SET_BYTE(buffer, buffer_capacity, req_off, PCCC_TYPED_CMD);

        /* status, always zero */
        TRY_SET_BYTE(buffer, buffer_capacity, req_off, 0);

        /* TSN - 16-bit value */
        rc = (uint16_t)pccc_eip_plc_get_tsn(tag->plc, &(tag->tsn));
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to get TSN!");
            break;
        }
        TRY_SET_U16_LE(buffer, buffer_capacity, req_off, tag->tsn);

        /* PLC5 read function. */
        TRY_SET_BYTE(buffer, buffer_capacity, req_off, PLC5_RANGE_READ_FUNC);

        /* offset of the transfer in words */
        TRY_SET_U16_LE(buffer, buffer_capacity, req_off, tag->trans_offset);

        /* total transfer size in words. */
        TRY_SET_U16_LE(buffer, buffer_capacity, req_off, tag->base_tag.size/2);

        /* set the logical PLC-5 address. */
        rc = encode_plc5_logical_address(buffer, buffer_capacity, &req_off, tag->data_file_num, tag->data_file_elem, tag->data_file_sub_elem);
        if(rc != PLCTAG_STATUS_OK) break;

        /* max transfer size in bytes. */
        /* TODO - is this correct logic?  What about the TSN in the response? */
        max_trans_size = PLC5_WORD_RANGE_READ_MAX_PAYLOAD;

        pdebug(DEBUG_DETAIL, "Maximum transfer size %d.", max_trans_size);

        /* max or remaining size of the transfer in bytes. */
        if((tag->base_tag.size - (int32_t)(uint32_t)tag->trans_offset) < max_trans_size) {
            max_trans_size = (int)(tag->base_tag.size - (int32_t)(uint32_t)tag->trans_offset);
        }

        pdebug(DEBUG_DETAIL, "Available transfer size %d.", max_trans_size);

        /* The transfer must be a multiple of the element size. */
        num_elems = max_trans_size / tag->elem_size;

        pdebug(DEBUG_DETAIL, "Number of elements possible to transfer %d.", num_elems);

        trans_size = num_elems * tag->elem_size;

        pdebug(DEBUG_DETAIL, "Actual bytes to transfer %d.", trans_size);

        TRY_SET_BYTE(buffer, buffer_capacity, req_off, trans_size);

        *data_end = req_off;
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to build read request, got error %s!", plc_tag_decode_error(rc));
        tag->base_tag.status = (int8_t)rc;
        return rc;
    }

    pdebug(DEBUG_DETAIL, "Read request packet:");
    pdebug_dump_bytes(DEBUG_DETAIL, buffer + *data_start, *data_end - *data_start);

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


int handle_read_response_callback(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id req_id)
{
    int rc = PLCTAG_STATUS_OK;
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)context;
    int resp_data_size = 0;
    uint8_t *data = buffer + *data_start;
    int data_size = *data_end - *data_start;

    (void)buffer_capacity;

    pdebug(DEBUG_DETAIL, "Starting for %" REQ_ID_FMT ".", req_id);

    do {
        /* check the response */
        if(data_size < 4) {
            pdebug(DEBUG_WARN, "Unexpectedly short PCCC response!");
            rc = PLCTAG_ERR_TOO_SMALL;
            break;
        }

        if(data[0] != (PCCC_TYPED_CMD | PCCC_CMD_OK)) {
            pdebug(DEBUG_WARN, "Unexpected PCCC packet response type %d!", (int)(unsigned int)data[0]);
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        if(data[1] != 0) {
            pdebug(DEBUG_WARN, "Received error response %s (%d)!", df1_decode_error(&(data[1]), data_size - 1));
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        pdebug(DEBUG_DETAIL, "Read response packet:");
        pdebug_dump_bytes(DEBUG_DETAIL, data, data_size);

        /*
        * copy the data.
        *
        * Note that we start at byte 4.  Bytes 0 and 1 are the CMD and
        * STS bytes, respectively, then we have the TSN.
        */
        resp_data_size = data_size - 4;

        for(int i=0; i < resp_data_size; i++) {
            tag->base_tag.data[tag->trans_offset + i] = data[i + 4];
        }

        tag->trans_offset += (uint16_t)(unsigned int)resp_data_size;

        /* do we have more work to do? */
        if(tag->trans_offset < tag->base_tag.size) {
            pdebug(DEBUG_DETAIL, "Starting new read request for remaining data.");
            rc = plc_start_request(tag->plc, &(tag->request), tag, build_read_request_callback, handle_read_response_callback);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error queuing up next request!");
                break;
            }
        } else {
            /* done! */
            tag->trans_offset = 0;
            rc = PLCTAG_STATUS_OK;
            tag->base_tag.status = (int8_t)rc;
        }

        *data_start = *data_end;
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, handling read response!", plc_tag_decode_error(rc));
        tag->base_tag.status = (int8_t)rc;
        return rc;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


int build_write_request_callback(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id req_id)
{
    int rc = PLCTAG_STATUS_OK;
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)context;
    int req_off = 0;
    int encoded_file_start = 0;
    int max_trans_size = 0;
    int num_elems = 0;
    int trans_size = 0;

    pdebug(DEBUG_DETAIL, "Starting for request %" REQ_ID_FMT ".", req_id);

    /* encode the request. */

    do {
        /* PCCC command type byte */
        TRY_SET_BYTE(buffer, buffer_capacity, req_off, PCCC_TYPED_CMD);

        /* status, always zero */
        TRY_SET_BYTE(buffer, buffer_capacity, req_off, 0);

        /* TSN - 16-bit value */
        rc = pccc_eip_plc_get_tsn(tag->plc, &(tag->tsn));
        if(rc != PLCTAG_STATUS_OK) {
            pdebug(DEBUG_WARN, "Unable to get TSN for request, error %s!", plc_tag_decode_error(rc));
            break;
        }

        TRY_SET_U16_LE(buffer, buffer_capacity, req_off, tag->tsn);

        /* PLC5 read function. */
        TRY_SET_BYTE(buffer, buffer_capacity, req_off, PLC5_RANGE_WRITE_FUNC);

        /* offset of the transfer in words */
        TRY_SET_U16_LE(buffer, buffer_capacity, req_off, tag->trans_offset/2);

        /* total transfer size in words. */
        TRY_SET_U16_LE(buffer, buffer_capacity, req_off, tag->base_tag.size/2);

        /* set the logical PLC-5 address. */
        encoded_file_start = req_off;
        rc = encode_plc5_logical_address(buffer, buffer_capacity, &req_off, tag->data_file_num, tag->data_file_elem, tag->data_file_sub_elem);
        if(rc != PLCTAG_STATUS_OK) break;

        /* max transfer size. */
        /* TODO - is this correct logic?   What about the TSN? */
        max_trans_size = PLC5_WORD_RANGE_WRITE_MAX_PAYLOAD - (req_off - encoded_file_start);

        pdebug(DEBUG_DETAIL, "Maximum transfer size %d.", max_trans_size);

        /* size of the transfer in bytes. */
        if((tag->base_tag.size - (int32_t)(uint32_t)tag->trans_offset) < max_trans_size) {
            max_trans_size = (int)(tag->base_tag.size - (int32_t)(uint32_t)tag->trans_offset);
        }

        pdebug(DEBUG_DETAIL, "Available transfer size %d.", max_trans_size);

        /* The transfer must be a multiple of the element size. */
        num_elems = max_trans_size / tag->elem_size;

        pdebug(DEBUG_DETAIL, "Number of elements possible to transfer %d.", num_elems);

        trans_size = num_elems * tag->elem_size;

        pdebug(DEBUG_DETAIL, "Actual bytes to transfer %d.", trans_size);

        /* copy the data. */
        for(int i=0; i < trans_size; i++) {
            buffer[req_off] = tag->base_tag.data[tag->trans_offset + i];
            req_off++;
        }

        if(rc != PLCTAG_STATUS_OK) break;

        /* update the amount transfered. */
        tag->trans_offset += (uint16_t)(unsigned int)trans_size;

        pdebug(DEBUG_DETAIL, "Write request packet:");
        pdebug_dump_bytes(DEBUG_DETAIL, buffer + *data_start, req_off - *data_start);

        /* we are done, mark the packet space as used. */
        *data_start = *data_end = req_off;
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Unable to build read request, got error %s!", plc_tag_decode_error(rc));
        tag->base_tag.status = (int8_t)rc;
        return rc;
    }

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


int handle_write_response_callback(void *context, uint8_t *buffer, int buffer_capacity, int *data_start, int *data_end, plc_request_id req_id)
{
    int rc = PLCTAG_STATUS_OK;
    ab2_plc5_tag_p tag = (ab2_plc5_tag_p)context;
    int data_size = *data_end - *data_start;
    uint8_t *data = buffer + *data_start;

    (void)buffer_capacity;

    pdebug(DEBUG_DETAIL, "Starting for request %" REQ_ID_FMT ".", req_id);

    do {
        /* check the response */
        if(data_size < 4) {
            pdebug(DEBUG_WARN, "Unexpectedly short PCCC response!");
            rc = PLCTAG_ERR_TOO_SMALL;
            break;
        }

        if(data[0] != (PCCC_TYPED_CMD | PCCC_CMD_OK)) {
            pdebug(DEBUG_WARN, "Unexpected PCCC packet response type %d!", (int)(unsigned int)data[0]);
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        if(data[1] != 0) {
            pdebug(DEBUG_WARN, "Received error response %s (%d)!", df1_decode_error(&data[1], data_size - 1), (int)(unsigned int)data[1]);
            rc = PLCTAG_ERR_BAD_REPLY;
            break;
        }

        pdebug(DEBUG_DETAIL, "Write response packet:");
        pdebug_dump_bytes(DEBUG_DETAIL, data, data_size);

        /* do we have more work to do? */
        if(tag->trans_offset < tag->base_tag.size) {
            pdebug(DEBUG_DETAIL, "Starting new write request for remaining data.");
            rc = plc_start_request(tag->plc, &(tag->request), tag, build_write_request_callback, handle_write_response_callback);
            if(rc != PLCTAG_STATUS_OK) {
                pdebug(DEBUG_WARN, "Error, %s, queuing up next request!", plc_tag_decode_error(rc));
                break;
            }
        } else {
            /* done! */
            tag->trans_offset = 0;
            rc = PLCTAG_STATUS_OK;
            tag->base_tag.status = (int8_t)rc;
        }
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        tag->base_tag.status = (int8_t)rc;
        return rc;
    }

    /* Clear out the buffer.   This marks that we processed it all. */
    *data_start = *data_end;

    pdebug(DEBUG_DETAIL, "Done.");

    return rc;
}


int encode_plc5_logical_address(uint8_t *buffer, int buffer_capacity, int *offset, int data_file_num, int data_file_elem, int data_file_sub_elem)
{
    int rc = PLCTAG_STATUS_OK;

    pdebug(DEBUG_DETAIL, "Starting.");

    do {
        /*
         * do the required levels.  Remember we start at the low bit!
         *
         * 0x0E = 0b1110 = levels 1, 2, and 3.  3 = subelement.
         * 0x06 = 0b0110 = levels 1, and 2.
         */
        if(data_file_sub_elem > 0) {
            TRY_SET_BYTE(buffer, buffer_capacity, *offset, 0x0E);
            //if(*offset < buffer_capacity) { buffer[*offset] = (uint8_t)(unsigned int)(0x0E); } else { rc = PLCTAG_ERR_OUT_OF_BOUNDS; break; } (offset++);
        } else {
            TRY_SET_BYTE(buffer, buffer_capacity, *offset, 0x06);
        }

        /* add in the data file number. */
        if(data_file_num <= 0xFE) {
            TRY_SET_BYTE(buffer, buffer_capacity, *offset, data_file_num);
        } else {
            TRY_SET_BYTE(buffer, buffer_capacity, *offset, 0xFF);
            TRY_SET_U16_LE(buffer, buffer_capacity, *offset, data_file_num);
        }

        /* add in the element number */
        if(data_file_elem <= 0xFE) {
            TRY_SET_BYTE(buffer, buffer_capacity, *offset, data_file_elem);
        } else {
            TRY_SET_BYTE(buffer, buffer_capacity, *offset, 0xFF);
            TRY_SET_U16_LE(buffer, buffer_capacity, *offset, data_file_elem);
        }

        /* check to see if we need to put in a subelement. */
        if(data_file_sub_elem >= 0) {
            if(data_file_sub_elem <= 0xFE) {
                TRY_SET_BYTE(buffer, buffer_capacity, *offset, data_file_sub_elem);
            } else {
                TRY_SET_BYTE(buffer, buffer_capacity, *offset, 0xFF);
                TRY_SET_U16_LE(buffer, buffer_capacity, *offset, data_file_sub_elem);
            }
        }
    } while(0);

    if(rc != PLCTAG_STATUS_OK) {
        pdebug(DEBUG_WARN, "Error, %s, while building encoded data file tag!", plc_tag_decode_error(rc));
        return rc;
    }

    pdebug(DEBUG_DETAIL,"Done.");

    return rc;
}

