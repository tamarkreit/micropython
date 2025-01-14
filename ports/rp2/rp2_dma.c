/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2021 Nicko van Someren
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */


#include <string.h>

#include "py/runtime.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/objint.h"
#include "py/objarray.h"
#include "modrp2.h"

#include "hardware/irq.h"
#include "hardware/dma.h"
#include "shared/runtime/mpirq.h"

#define CHANNEL_CLOSED 0xff

// Forward declaration
STATIC const mp_obj_dict_t rp2_dma_locals_dict;

typedef struct _rp2_dma_ctrl_obj_t {
    mp_obj_base_t base;
    uint32_t value;
} rp2_dma_config_obj_t;

typedef struct _rp2_dma_obj_t {
    mp_obj_base_t base;
    uint8_t channel;
    uint8_t irq_flag : 1;
    uint8_t irq_trigger : 1;
} rp2_dma_obj_t;

typedef struct _rp2_dma_ctrl_field_t {
    qstr name;
    uint8_t shift : 5;
    uint8_t length : 3;
    uint8_t read_only : 1;
} rp2_dma_ctrl_field_t;

STATIC rp2_dma_ctrl_field_t rp2_dma_ctrl_fields_table[] = {
    { MP_QSTR_enable,        0, 1, 0 },
    { MP_QSTR_high_priority, 1, 1, 0 },
    { MP_QSTR_size,          2, 2, 0 },
    { MP_QSTR_inc_read,      4, 1, 0 },
    { MP_QSTR_inc_write,     5, 1, 0 },
    { MP_QSTR_ring_size,     6, 4, 0 },
    { MP_QSTR_ring_sel,     10, 1, 0 },
    { MP_QSTR_chain_to,     11, 4, 0 },
    { MP_QSTR_treq_sel,     15, 6, 0 },
    { MP_QSTR_IRQ_quiet,    21, 1, 0 },
    { MP_QSTR_bswap,        22, 1, 0 },
    { MP_QSTR_sniff_en,     23, 1, 0 },
    { MP_QSTR_busy,         24, 1, 1 },
    // bits 25 through 28 are reserved
    { MP_QSTR_write_error,  29, 1, 0 },
    { MP_QSTR_read_error,   30, 1, 0 },
    { MP_QSTR_ahb_error,    31, 1, 1 },
};
STATIC uint32_t rp2_dma_ctrl_field_count = sizeof(rp2_dma_ctrl_fields_table) / sizeof(rp2_dma_ctrl_field_t);


#define REG_TYPE_COUNT       0  // Accept just integers
#define REG_TYPE_CONF        1  // Accept integers or ctrl values
#define REG_TYPE_ADDR_READ   2  // Accept integers, buffers or objects that can be read from
#define REG_TYPE_ADDR_WRITE  3  // Accept integers, buffers or objects that can be written to

STATIC uint32_t rp2_dma_register_value_from_obj(mp_obj_t o, int reg_type) {
    if (reg_type == REG_TYPE_ADDR_READ || reg_type == REG_TYPE_ADDR_WRITE) {
        mp_buffer_info_t buf_info;
        mp_uint_t flags = MP_BUFFER_READ;
        if (mp_get_buffer(o, &buf_info, flags)) {
            return (uint32_t)buf_info.buf;
        }
    }

    // DMAConfigs can cast as integers; we don't want them as counts or addresses
    if (reg_type != REG_TYPE_CONF && mp_obj_is_type(o, &rp2_dma_config_type)) {
        mp_raise_ValueError(MP_ERROR_TEXT("DMAConfig only allowed for ctrl"));
    }

    if (!mp_obj_is_int(o)) {
        o = mp_unary_op(MP_UNARY_OP_INT, o);
        if (o == MP_OBJ_NULL) {
            mp_raise_ValueError(MP_ERROR_TEXT("value can's be converted to integer"));
        }
    }

    if (mp_obj_is_small_int(o)) {
        // For small ints, just get the value
        return (uint32_t)MP_OBJ_SMALL_INT_VALUE(o);
    #if MICROPY_LONGINT_IMPL != MICROPY_LONGINT_IMPL_NONE
    } else if (mp_obj_is_int(o)) {
        // For non-small ints, we need to unpack the value
        uint32_t value;
        mp_obj_int_to_bytes_impl(o, MP_ENDIANNESS_BIG, 4, (byte *)&value);
        return value;
    #endif
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("value can's be converted to integer"));
    }
}


// Defailt is quiet, unpaced, read and write incrementing, word transfers, enabled
#define DEFAULT_DMA_CONFIG (1 << 21) | (0x3f << 15) | (1 << 5) | (1 << 4) | (2 << 2) | (1 << 0)

STATIC bool rp2_dma_config_set_field(uint32_t *old_value, qstr name, mp_obj_t field_value) {
    rp2_dma_ctrl_field_t *table = rp2_dma_ctrl_fields_table;
    for (int i = 0; i < rp2_dma_ctrl_field_count; i++) {
        if (name == table[i].name) {
            if (table[i].read_only) {
                return false;
            }
            mp_int_t fv = mp_obj_get_int(field_value);
            uint32_t mask = ((1 << table[i].length) - 1) << table[i].shift;
            *old_value = (*old_value & (~mask)) | ((fv << table[i].shift) & mask);
            return true;
        }
    }

    return false;
}

// We can do this because fields can't be 32 bits wide
#define FIELD_NOT_FOUND 0xffffffffu

STATIC uint32_t rp2_dma_config_get_field(uint32_t value, qstr name) {
    rp2_dma_ctrl_field_t *table = rp2_dma_ctrl_fields_table;
    for (int i = 0; i < rp2_dma_ctrl_field_count; i++) {
        if (name == table[i].name) {
            return (value >> table[i].shift) & ((1 << table[i].length) - 1);
        }
    }
    return FIELD_NOT_FOUND;
}

STATIC mp_obj_t rp2_dma_config_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 1, true);

    uint32_t value = DEFAULT_DMA_CONFIG;
    if (n_args > 0) {
        value = rp2_dma_register_value_from_obj(args[0], REG_TYPE_CONF);
        args++;
    }

    for (int i = 0; i < n_kw * 2; i += 2) {
        qstr q_name = mp_obj_str_get_qstr(args[i]);
        if (!rp2_dma_config_set_field(&value, q_name, args[i + 1])) {
            mp_raise_msg_varg(&mp_type_AttributeError, MP_ERROR_TEXT("DMAConfig has no '%s' field"), qstr_str(q_name));
        }
    }

    rp2_dma_config_obj_t *self = m_new_obj(rp2_dma_config_obj_t);
    self->base.type = &rp2_dma_config_type;
    self->value = value;
    return MP_OBJ_FROM_PTR(self);
}

STATIC void rp2_dma_config_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    mp_printf(print, "DMAConfig(");
    uint32_t value = ((rp2_dma_config_obj_t *)MP_OBJ_TO_PTR(self_in))->value;
    rp2_dma_ctrl_field_t *table = rp2_dma_ctrl_fields_table;

    for (int i = 0; i < rp2_dma_ctrl_field_count; i++) {
        if (i != 0) {
            mp_printf(print, ", ");
        }
        uint32_t field_value = (value >> table[i].shift) & ((1 << table[i].length) - 1);
        mp_printf(print, "%s=%d", qstr_str(table[i].name), field_value);
    }
    mp_printf(print, ")");
}

STATIC void rp2_dma_config_attr(mp_obj_t self_in, qstr attr_in, mp_obj_t *dest) {
    rp2_dma_config_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (dest[0] == MP_OBJ_NULL) {
        // Load attribute
        uint32_t field_value = rp2_dma_config_get_field(self->value, attr_in);
        if (field_value != FIELD_NOT_FOUND) {
            dest[0] = mp_obj_new_int_from_uint(field_value);
        }
    } else {
        // Set or delete attribute
        if (dest[1] == MP_OBJ_NULL) {
            // We don't support deleting attributes.
            return;
        }
        if (rp2_dma_config_set_field(&self->value, attr_in, dest[1])) {
            dest[0] = MP_OBJ_NULL; // indicate success
        }
    }
}

mp_obj_t rp2_dma_config_unary_op(mp_unary_op_t op, mp_obj_t self_in) {
    rp2_dma_config_obj_t *self = MP_OBJ_TO_PTR(self_in);
    switch (op) {
        case MP_UNARY_OP_INT:
            return mp_obj_new_int_from_uint(self->value);
        default:
            return MP_OBJ_NULL;      // op not supported
    }
}

const mp_obj_type_t rp2_dma_config_type = {
    { &mp_type_type },
    .name = MP_QSTR_DMAConfig,
    .print = rp2_dma_config_print,
    .make_new = rp2_dma_config_make_new,
    .unary_op = rp2_dma_config_unary_op,
    .attr = rp2_dma_config_attr,
};


STATIC void rp2_dma_irq_handler(void) {
    // Main IRQ handler
    uint32_t irq_bits = dma_hw->ints0;
    dma_hw->ints0 = 0xffff;

    for (int i = 0; i < NUM_DMA_CHANNELS; i++) {
        if (irq_bits & (1u << i)) {
            mp_irq_obj_t *handler = MP_STATE_PORT(rp2_dma_irq_obj[i]);
            if (handler) {
                rp2_dma_obj_t *self = (rp2_dma_obj_t *)handler->parent;
                self->irq_flag = 1;
                mp_irq_handler(handler);
            } else {
                // We got an interrupt with no handler. Disable the channel
                dma_channel_set_irq0_enabled(i, false);
            }
        }
    }
}

STATIC mp_uint_t rp2_dma_irq_trigger(mp_obj_t self_in, mp_uint_t new_trigger) {
    rp2_dma_obj_t *self = MP_OBJ_TO_PTR(self_in);
    irq_set_enabled(DMA_IRQ_0, false);
    self->irq_flag = 0;
    dma_channel_set_irq0_enabled(self->channel, (new_trigger != 0));
    irq_set_enabled(DMA_IRQ_0, true);
    return 0;
}

STATIC mp_uint_t rp2_dma_irq_info(mp_obj_t self_in, mp_uint_t info_type) {
    rp2_dma_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (info_type == MP_IRQ_INFO_FLAGS) {
        return self->irq_flag;
    } else if (info_type == MP_IRQ_INFO_TRIGGERS) {
        return (dma_hw->ints0 & (1u << self->channel)) != 0;
    }
    return 0;
}

STATIC const mp_irq_methods_t rp2_dma_irq_methods = {
    .trigger = rp2_dma_irq_trigger,
    .info = rp2_dma_irq_info,
};

STATIC mp_obj_t rp2_dma_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 0, false);

    int dma_channel = dma_claim_unused_channel(false);
    if (dma_channel < 0) {
        mp_raise_OSError(MP_EBUSY);
    }

    rp2_dma_obj_t *self = m_new_obj(rp2_dma_obj_t);
    self->base.type = &rp2_dma_type;
    self->channel = dma_channel;

    // Return the DMA object.
    return MP_OBJ_FROM_PTR(self);
}

STATIC void rp2_dma_error_if_closed(rp2_dma_obj_t *self) {
    if (self->channel == CHANNEL_CLOSED) {
        mp_raise_ValueError(MP_ERROR_TEXT("Channel closed"));
    }
}

STATIC void rp2_dma_attr(mp_obj_t self_in, qstr attr_in, mp_obj_t *dest) {
    rp2_dma_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (dest[0] == MP_OBJ_NULL) {
        // Load attribute
        dma_channel_hw_t *reg_block = dma_channel_hw_addr(self->channel);
        if (attr_in == MP_QSTR_active) {
            rp2_dma_error_if_closed(self);
            uint32_t busy = dma_channel_is_busy(self->channel);
            dest[0] = mp_obj_new_bool((mp_int_t)busy);
        } else if (attr_in == MP_QSTR_default_ctrl) {
            // Get the default ctrl for _this_ channel, i.e. with chaining disabled.
            rp2_dma_config_obj_t *conf = m_new_obj(rp2_dma_config_obj_t);
            conf->base.type = &rp2_dma_config_type;
            conf->value = DEFAULT_DMA_CONFIG | ((self->channel & 0xf) << 11);
            dest[0] = conf;
        } else if (attr_in == MP_QSTR_read) {
            rp2_dma_error_if_closed(self);
            dest[0] = mp_obj_new_int_from_uint((mp_uint_t)reg_block->read_addr);
        } else if (attr_in == MP_QSTR_write) {
            rp2_dma_error_if_closed(self);
            dest[0] = mp_obj_new_int_from_uint((mp_uint_t)reg_block->write_addr);
        } else if (attr_in == MP_QSTR_count) {
            rp2_dma_error_if_closed(self);
            dest[0] = mp_obj_new_int_from_uint((mp_uint_t)reg_block->transfer_count);
        } else if (attr_in == MP_QSTR_ctrl) {
            rp2_dma_error_if_closed(self);
            dest[0] = mp_obj_new_int_from_uint((mp_uint_t)reg_block->al1_ctrl);
        } else if (attr_in == MP_QSTR_channel_id) {
            dest[0] = mp_obj_new_int_from_uint(self->channel);
        } else if (attr_in == MP_QSTR_registers) {
            mp_obj_array_t *reg_view = m_new_obj(mp_obj_array_t);
            mp_obj_memoryview_init(reg_view, 'I', 0, 16, dma_channel_hw_addr(self->channel));
            dest[0] = reg_view;
        } else {
            // If a Micropython class supports attributes then the locals dict is not searched.
            // We do it manually here.
            mp_map_t *locals_map = (mp_map_t *)&rp2_dma_locals_dict.map;
            mp_map_elem_t *elem = mp_map_lookup(locals_map, MP_OBJ_NEW_QSTR(attr_in), MP_MAP_LOOKUP);
            if (elem != NULL) {
                mp_convert_member_lookup(self, &rp2_dma_type, elem->value, dest);
            }
        }
    } else {
        // Set or delete attribute
        if (dest[1] == MP_OBJ_NULL) {
            // We don't support deleting attributes.
            return;
        }

        rp2_dma_error_if_closed(self);

        if (attr_in == MP_QSTR_active) {
            if (mp_obj_is_true(dest[1])) {
                dma_channel_start(self->channel);
            } else {
                dma_channel_abort(self->channel);
            }
            dest[0] = MP_OBJ_NULL; // indicate success
        } else if (attr_in == MP_QSTR_read) {
            uint32_t value = rp2_dma_register_value_from_obj(dest[1], REG_TYPE_ADDR_READ);
            dma_channel_set_read_addr(self->channel, (volatile void *)value, false);
            dest[0] = MP_OBJ_NULL; // indicate success
        } else if (attr_in == MP_QSTR_write) {
            uint32_t value = rp2_dma_register_value_from_obj(dest[1], REG_TYPE_ADDR_WRITE);
            dma_channel_set_write_addr(self->channel, (volatile void *)value, false);
            dest[0] = MP_OBJ_NULL; // indicate success
        } else if (attr_in == MP_QSTR_count) {
            uint32_t value = rp2_dma_register_value_from_obj(dest[1], REG_TYPE_COUNT);
            dma_channel_set_trans_count(self->channel, value, false);
            dest[0] = MP_OBJ_NULL; // indicate success
        } else if (attr_in == MP_QSTR_ctrl) {
            uint32_t value = rp2_dma_register_value_from_obj(dest[1], REG_TYPE_CONF);
            dma_channel_set_config(self->channel, (dma_channel_config *)&value, false);
            dest[0] = MP_OBJ_NULL; // indicate success
        }
    }
}

STATIC void rp2_dma_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    rp2_dma_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "DMA(%u)", self->channel);
}


STATIC mp_obj_t rp2_dma_config(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    rp2_dma_obj_t *self = MP_OBJ_TO_PTR(*pos_args);

    rp2_dma_error_if_closed(self);

    enum {
        ARG_read,
        ARG_write,
        ARG_count,
        ARG_ctrl,
        ARG_trigger,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_read,         MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_write,        MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_count,        MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_ctrl,         MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_trigger,      MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    // Don't include self in arg parsing
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    // We only do anything if there was at least one argument
    if (kw_args->used) {
        bool trigger = args[ARG_trigger].u_bool;
        mp_int_t value_count = trigger ? kw_args->used - 1 : kw_args->used;
        if (trigger && (value_count == 0)) {
            // Only a "true" trigger was passed; just start a transfer
            dma_channel_start(self->channel);
        } else {
            if (args[ARG_read].u_obj != MP_OBJ_NULL) {
                uint32_t value = rp2_dma_register_value_from_obj(args[ARG_read].u_obj, REG_TYPE_ADDR_READ);
                value_count--;
                dma_channel_set_read_addr(self->channel, (volatile void *)value, trigger && (value_count == 0));
            }
            if (args[ARG_write].u_obj != MP_OBJ_NULL) {
                uint32_t value = rp2_dma_register_value_from_obj(args[ARG_write].u_obj, REG_TYPE_ADDR_WRITE);
                value_count--;
                dma_channel_set_write_addr(self->channel, (volatile void *)value, trigger && (value_count == 0));
            }
            if (args[ARG_count].u_obj != MP_OBJ_NULL) {
                uint32_t value = rp2_dma_register_value_from_obj(args[ARG_count].u_obj, REG_TYPE_COUNT);
                value_count--;
                dma_channel_set_trans_count(self->channel, value, trigger && (value_count == 0));
            }
            if (args[ARG_ctrl].u_obj != MP_OBJ_NULL) {
                uint32_t value = rp2_dma_register_value_from_obj(args[ARG_ctrl].u_obj, REG_TYPE_CONF);
                value_count--;
                dma_channel_set_config(self->channel, (dma_channel_config *)&value, trigger && (value_count == 0));
            }
        }
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(rp2_dma_config_obj, 1, rp2_dma_config);

STATIC mp_obj_t rp2_dma_irq(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_handler, ARG_hard };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_handler, MP_ARG_OBJ, {.u_rom_obj = mp_const_none} },
        { MP_QSTR_hard, MP_ARG_BOOL, {.u_bool = false} },
    };

    // Parse the arguments.
    rp2_dma_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    // Get the IRQ object.
    mp_irq_obj_t *irq = MP_STATE_PORT(rp2_dma_irq_obj[self->channel]);

    // Allocate the IRQ object if it doesn't already exist.
    if (irq == NULL) {
        irq = mp_irq_new(&rp2_dma_irq_methods, MP_OBJ_FROM_PTR(self));
        MP_STATE_PORT(rp2_dma_irq_obj[self->channel]) = irq;
    }

    if (n_args > 1 || kw_args->used != 0) {
        // Disable all IRQs while data is updated.
        irq_set_enabled(DMA_IRQ_0, false);

        // Update IRQ data.
        irq->handler = args[ARG_handler].u_obj;
        irq->ishard = args[ARG_hard].u_bool;
        self->irq_flag = 0;

        // Enable IRQ if a handler is given.
        bool enable = (args[ARG_handler].u_obj != mp_const_none);
        dma_channel_set_irq0_enabled(self->channel, enable);

        irq_set_enabled(DMA_IRQ_0, true);
    }

    return MP_OBJ_FROM_PTR(irq);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(rp2_dma_irq_obj, 1, rp2_dma_irq);


STATIC mp_obj_t rp2_dma_close(mp_obj_t self_in) {
    rp2_dma_obj_t *self = MP_OBJ_TO_PTR(self_in);
    uint8_t channel = self->channel;

    if (channel != CHANNEL_CLOSED) {
        // Clean up interrupt handler to ensure garbage collection
        mp_irq_obj_t *irq = MP_STATE_PORT(rp2_dma_irq_obj[channel]);
        MP_STATE_PORT(rp2_dma_irq_obj[channel]) = MP_OBJ_NULL;
        if (irq) {
            irq->parent = MP_OBJ_NULL;
            irq->handler = MP_OBJ_NULL;
            dma_channel_set_irq0_enabled(channel, false);
        }
        dma_channel_unclaim(channel);
        self->channel = CHANNEL_CLOSED;
    }

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(rp2_dma_close_obj, rp2_dma_close);

STATIC const mp_rom_map_elem_t rp2_dma_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_config), MP_ROM_PTR(&rp2_dma_config_obj) },
    { MP_ROM_QSTR(MP_QSTR_irq), MP_ROM_PTR(&rp2_dma_irq_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&rp2_dma_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&rp2_dma_close_obj) },
};
STATIC MP_DEFINE_CONST_DICT(rp2_dma_locals_dict, rp2_dma_locals_dict_table);


const mp_obj_type_t rp2_dma_type = {
    { &mp_type_type },
    .name = MP_QSTR_DMA,
    .print = rp2_dma_print,
    .make_new = rp2_dma_make_new,
    .attr = rp2_dma_attr,
    // NOTE: Since we set .attr we have to search the locals dict manually there.
    .locals_dict = (mp_obj_dict_t *)&rp2_dma_locals_dict,
};

void rp2_dma_init(void) {
    // Set up interrupts.
    memset(MP_STATE_PORT(rp2_dma_irq_obj), 0, sizeof(MP_STATE_PORT(rp2_dma_irq_obj)));
    irq_set_exclusive_handler(DMA_IRQ_0, rp2_dma_irq_handler);
}

void rp2_dma_deinit(void) {
    // Disable and clear interrupts.
    irq_set_mask_enabled(1u << DMA_IRQ_0, false);
    irq_remove_handler(DMA_IRQ_0, rp2_dma_irq_handler);
}