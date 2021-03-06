/*
    src/var.cpp -- Variable/computation graph-related functions.

    Copyright (c) 2020 Wenzel Jakob <wenzel.jakob@epfl.ch>

    All rights reserved. Use of this source code is governed by a BSD-style
    license that can be found in the LICENSE file.
*/

#include "var.h"
#include "internal.h"
#include "log.h"
#include "eval.h"
#include "util.h"

/// Descriptive names for the various variable types
const char *var_type_name[(int) VarType::Count]{
    "invalid", "global", "mask",  "int8",   "uint8",   "int16",   "uint16",
    "int32", "uint32",  "int64", "uint64", "float16", "float32", "float64",
    "pointer"
};

/// Descriptive names for the various variable types (extra-short version)
const char *var_type_name_short[(int) VarType::Count]{
    "inv", "glo", "msk", "i8",  "u8",  "i16", "u16", "i32",
    "u32", "i64", "u64", "f16", "f32", "f64", "ptr"
};

/// CUDA PTX type names
const char *var_type_name_ptx[(int) VarType::Count]{
    "???", "???", "pred", "s8",  "u8",  "s16", "u16", "s32",
    "u32", "s64",  "u64", "f16", "f32", "f64", "u64"
};

/// CUDA PTX type names (binary view)
const char *var_type_name_ptx_bin[(int) VarType::Count]{
    "???", "???", "pred", "b8",  "b8",  "b16", "b16", "b32",
    "b32", "b64",  "b64", "b16", "b32", "b64", "b64"
};

/// LLVM IR type names (does not distinguish signed vs unsigned)
const char *var_type_name_llvm[(int) VarType::Count]{
    "???", "???", "i1",  "i8",  "i8",   "i16",   "i16",    "i32",
    "i32", "i64", "i64", "half", "float", "double", "i8*"
};

/// Double size integer arrays for mulhi()
const char *var_type_name_llvm_big[(int) VarType::Count]{
    "???", "???", "???",  "i16",  "i16",   "i32",   "i32",    "i64",
    "i64", "i128", "i128", "???", "???", "???", "???"
};

/// Abbreviated LLVM IR type names
const char *var_type_name_llvm_abbrev[(int) VarType::Count]{
    "???", "???", "i1",  "i8",  "i8",  "i16", "i16", "i32",
    "i32", "i64", "i64", "f16", "f32", "f64", "i8*"
};

/// LLVM IR type names (binary view)
const char *var_type_name_llvm_bin[(int) VarType::Count]{
    "???", "???", "i1",  "i8",  "i8",  "i16", "i16", "i32",
    "i32", "i64", "i64", "i16", "i32", "i64", "i64"
};

/// LLVM/CUDA register name prefixes
const char *var_type_prefix[(int) VarType::Count] {
    "%u", "gl", "%p", "%b", "%b", "%w", "%w", "%r",
    "%r", "%rd", "%rd", "%h", "%f", "%d", "%rd"
};

/// Maps types to byte sizes
const uint32_t var_type_size[(int) VarType::Count] {
    0, 0, 1, 1, 1, 2, 2, 4, 4, 8, 8, 2, 4, 8, 8
};

/// String version of the above
const char *var_type_size_str[(int) VarType::Count] {
    "0", "0", "1", "1", "1", "2", "2", "4", "4", "8", "8", "2", "4", "8", "8"
};

/// Label prefix, doesn't depend on variable type
const char *var_type_label[(int) VarType::Count] {
    "L", "L", "L", "L", "L", "L", "L", "L",
    "L", "L", "L", "L", "L", "L", "L"
};

/// Access a variable by ID, terminate with an error if it doesn't exist
Variable *jit_var(uint32_t index) {
    auto it = state.variables.find(index);
    if (unlikely(it == state.variables.end()))
        jit_fail("jit_var(%u): unknown variable!", index);
    return &it.value();
}

/// Remove a variable from the cache used for common subexpression elimination
void jit_cse_drop(uint32_t index, const Variable *v) {
    if (state.cse_cache.empty())
        return;
    VariableKey key(*v);
    auto it = state.cse_cache.find(key);
    if (it != state.cse_cache.end() && it.value() == index)
        state.cse_cache.erase(it);
}

/// Cleanup handler, called when the internal/external reference count reaches zero
void jit_var_free(uint32_t index, Variable *v) {
    bool trace = std::max(state.log_level_stderr, state.log_level_callback) >=
                 LogLevel::Trace;

    if (unlikely(trace))
        jit_trace("jit_var_free(%u)", index);

    if (v->stmt)
        jit_cse_drop(index, v);

    uint32_t dep[4];
    memcpy(dep, v->dep, sizeof(uint32_t) * 4);
    void *data = v->data;
    bool direct_pointer = v->direct_pointer,
         has_extra = v->has_extra;

    // Release GPU memory
    if (likely(v->data && !v->retain_data))
        jit_free(v->data);

    // Free strings
    if (unlikely(v->free_stmt))
        free(v->stmt);

    // Remove from hash table. 'v' should not be accessed from here on.
    state.variables.erase(index);

    if (likely(!direct_pointer)) {
        // Decrease reference count of dependencies
        for (int i = 0; i < 4; ++i) {
            uint32_t index2 = dep[i];
            if (index2 == 0)
                break;

            // Inlined implementation of jit_var_dec_ref_int
            auto it = state.variables.find(index2);
            Variable *v2 = &it.value();
            v2->ref_count_int--;

            if (unlikely(trace))
                jit_trace("jit_var_dec_ref_int(%u): %u", index2, v2->ref_count_int);
            if (v2->ref_count_ext == 0 && v2->ref_count_int == 0)
                jit_var_free(index2, v2);
        }
    } else {
        // Free reverse pointer table entry, if needed
        auto it = state.variable_from_ptr.find(data);
        if (unlikely(it == state.variable_from_ptr.end()))
            jit_fail("jit_var_free(): direct pointer not found!");
        state.variable_from_ptr.erase(it);

        // Decrease reference count of dependencies
        jit_var_dec_ref_ext(dep[0]);
    }

    if (unlikely(has_extra)) {
        auto it = state.extra.find(index);
        if (it == state.extra.end())
            jit_fail("jit_var_free(): entry in 'extra' hash table not found!");
        Extra extra = it.value();
        state.extra.erase(it);

        // Free descriptive label
        free(extra.label);

        if (extra.free_callback) {
            unlock_guard guard(state.mutex);
            extra.free_callback(extra.callback_payload);
        }

        if (extra.vcall_bucket_count) {
            for (uint32_t i = 0; i < extra.vcall_bucket_count; ++i)
                jit_var_dec_ref_ext(extra.vcall_buckets[i].index);
            jit_free(extra.vcall_buckets);
        }
    }
}

/// Increase the external reference count of a given variable
void jit_var_inc_ref_ext(uint32_t index, Variable *v) noexcept(true) {
    v->ref_count_ext++;
    jit_trace("jit_var_inc_ref_ext(%u): %u", index, v->ref_count_ext);
}

/// Increase the external reference count of a given variable
void jit_var_inc_ref_ext(uint32_t index) noexcept(true) {
    if (index != 0)
        jit_var_inc_ref_ext(index, jit_var(index));
}

/// Increase the internal reference count of a given variable
void jit_var_inc_ref_int(uint32_t index, Variable *v) noexcept(true) {
    v->ref_count_int++;
    jit_trace("jit_var_inc_ref_int(%u): %u", index, v->ref_count_int);
}

/// Increase the internal reference count of a given variable
void jit_var_inc_ref_int(uint32_t index) noexcept(true) {
    if (index != 0)
        jit_var_inc_ref_int(index, jit_var(index));
}

/// Decrease the external reference count of a given variable
void jit_var_dec_ref_ext(uint32_t index, Variable *v) noexcept(true) {
    if (unlikely(v->ref_count_ext == 0))
        jit_fail("jit_var_dec_ref_ext(): variable %u has no external references!", index);

    jit_trace("jit_var_dec_ref_ext(%u): %u", index, v->ref_count_ext - 1);
    v->ref_count_ext--;

    if (v->ref_count_ext == 0 && v->ref_count_int == 0)
        jit_var_free(index, v);
}

/// Decrease the external reference count of a given variable
void jit_var_dec_ref_ext(uint32_t index) noexcept(true) {
    if (index != 0)
        jit_var_dec_ref_ext(index, jit_var(index));
}

/// Decrease the internal reference count of a given variable
void jit_var_dec_ref_int(uint32_t index, Variable *v) noexcept(true) {
    if (unlikely(v->ref_count_int == 0))
        jit_fail("jit_var_dec_ref_int(): variable %u has no internal references!", index);

    jit_trace("jit_var_dec_ref_int(%u): %u", index, v->ref_count_int - 1);
    v->ref_count_int--;

    if (v->ref_count_ext == 0 && v->ref_count_int == 0)
        jit_var_free(index, v);
}

/// Decrease the internal reference count of a given variable
void jit_var_dec_ref_int(uint32_t index) noexcept(true) {
    if (index != 0)
        jit_var_dec_ref_int(index, jit_var(index));
}

/// Append the given variable to the instruction trace and return its ID
std::pair<uint32_t, Variable *> jit_var_new(Variable &v, bool disable_cse_) {
    Stream *stream = active_stream;
    if (unlikely(!stream)) {
        jit_raise("jit_var_new(): you must invoke jitc_set_device() to "
                  "choose a target device before performing computation using "
                  "the JIT compiler.");
    } else if (unlikely(v.cuda != stream->cuda)) {
        jit_raise("jit_var_new(): attempted to issue %s computation "
                  "to the %s backend! You must invoke jit_set_device() to set "
                  "the right backend!", v.cuda ? "CUDA" : "LLVM",
                  stream->cuda ? "CUDA" : "LLVM");
    }

    CSECache::iterator key_it;
    bool is_special  = (VarType) v.type == VarType::Invalid,
         disable_cse = v.stmt == nullptr || v.direct_pointer ||
                       !stream->enable_cse || disable_cse_ ||
                       is_special,
         cse_key_inserted = false;

    // Check if this exact statement already exists ..
    if (!disable_cse)
        std::tie(key_it, cse_key_inserted) =
            state.cse_cache.try_emplace(VariableKey(v), 0);

    uint32_t index;
    Variable *v_out;

    if (likely(disable_cse || cse_key_inserted)) {
        // .. nope, it is new.
        VariableMap::iterator var_it;
        bool var_inserted;
        do {
            index = state.variable_index++;

            if (unlikely(index == 0)) // overflow
                index = state.variable_index++;

            std::tie(var_it, var_inserted) =
                state.variables.try_emplace(index, v);
        } while (!var_inserted);

        if (cse_key_inserted)
            key_it.value() = index;

        v_out = &var_it.value();
    } else {
        // .. found a match! Deallocate 'v'.
        if (v.free_stmt)
            free(v.stmt);

        for (int i = 0; i < 4; ++i)
            jit_var_dec_ref_int(v.dep[i]);

        index = key_it.value();
        v_out = jit_var(index);
    }

    return std::make_pair(index, v_out);
}

/// Query the pointer variable associated with a given variable
void *jit_var_ptr(uint32_t index) {
    return index == 0u ? nullptr : jit_var(index)->data;
}

/// Query the size of a given variable
uint32_t jit_var_size(uint32_t index) {
    return jit_var(index)->size;
}

/// Query the type of a given variable
VarType jit_var_type(uint32_t index) {
    return (VarType) jit_var(index)->type;
}

// Resize a scalar variable
uint32_t jit_var_set_size(uint32_t index, uint32_t size) {
    Variable *v = jit_var(index);
    jit_log(Debug, "jit_var_set_size(%u): %u", index, size);
    if (v->size == size) {
        jit_var_inc_ref_ext(index, v);
        return index; // Nothing to do
    } else if (v->size != 1) {
        jit_raise("jit_var_set_size(): variable %u must be a scalar variable!",
                  index);
    } else if (v->stmt && v->ref_count_int == 0 && v->ref_count_ext == 1) {
        jit_var_inc_ref_ext(index, v);
        jit_cse_drop(index, v);
        v->size = size;
        return index;
    } else if (v->is_literal_zero) {
        return jit_var_new_literal((VarType) v->type, v->cuda, 0, size, 0);
    } else {
        Stream *stream = active_stream;
        uint32_t index_new;
        if (stream->cuda) {
            index_new = jit_var_new_1((VarType) v->type, "mov.$t0 $r0, $r1", 1,
                                      1, index);
        } else {

            const char *op = jitc_is_floating_point((VarType) v->type)
                                 ? "$r0 = fadd <$w x $t0> $r1, $z"
                                 : "$r0 = add <$w x $t0> $r1, $z";
            index_new = jit_var_new_1((VarType) v->type, op, 1, 0, index);
        }

        Variable *v2 = jit_var(index_new);
        jit_cse_drop(index_new, v2);
        v2->size = size;
        return index_new;
    }
}

/// Query the descriptive label associated with a given variable
const char *jit_var_label(uint32_t index) {
    ExtraMap::iterator it = state.extra.find(index);
    return it != state.extra.end() ? it.value().label : nullptr;
}

/// Assign a descriptive label to a given variable
void jit_var_set_label(uint32_t index, const char *label) {
    Variable *v = jit_var(index);

    jit_log(Debug, "jit_var_set_label(%u): \"%s\"", index,
            label ? label : "(null)");

    v->has_extra = true;
    Extra &extra = state.extra[index];
    free(extra.label);
    extra.label = label ? strdup(label) : nullptr;
}

void jit_var_set_free_callback(uint32_t index, void (*callback)(void *), void *payload) {
    Variable *v = jit_var(index);

    jit_log(Debug, "jit_var_set_callback(%u): " ENOKI_PTR " (" ENOKI_PTR ")",
            index, (uintptr_t) callback, (uintptr_t) payload);

    v->has_extra = true;
    Extra &extra = state.extra[index];
    if (unlikely(extra.free_callback))
        jit_fail("jit_var_set_free_callback(): a callback was already set!");
    extra.free_callback = callback;
    extra.callback_payload = payload;
}

uint32_t jit_var_new_literal(VarType type, int cuda,
                             uint64_t value, uint32_t size,
                             int eval) {
    if (unlikely(size == 0))
        return 0;

    if (unlikely(eval)) {
        void *ptr = jit_malloc(cuda ? AllocType::Device : AllocType::HostAsync,
                               size * var_type_size[(int) type]);
        if (size == 1)
            jit_poke(ptr, &value, var_type_size[(int) type]);
        else
            jit_memset_async(ptr, size, var_type_size[(int) type], &value);
        return jit_var_map_mem(type, cuda, ptr, size, true);
    }

    bool is_literal_one, is_literal_zero = value == 0;
    bool is_float = true, is_uint8 = false;
    switch (type) {
        case VarType::Float16:
            is_literal_one = value == 0x3c00ull;
            break;

        case VarType::Float32:
            is_literal_one = value == 0x3f800000ull;

            // LLVM: double hex floats, even in single precision
            if (!cuda) {
                float f = 0.f;
                memcpy(&f, &value, sizeof(float));
                double d = (double) f;
                memcpy(&value, &d, sizeof(double));
            }
            break;

        case VarType::Float64:
            is_literal_one = value == 0x3ff0000000000000ull;
            break;

        default:
            is_literal_one = value == 1;
            is_float = false;
            is_uint8 = type == VarType::Int8 || type == VarType::UInt8;
            break;
    }

    const char *digits = "0123456789abcdef";
    char digits_buf[20];
    int n_digits = 0;

    /* This is function is performance-critical. The following hand-written
       code replaces a previous 'snprintf' implementation that was too slow. */
    if (cuda || is_float) {
        // Hex digits for CUDA, and floats in LLVM mode
        do {
            digits_buf[sizeof(digits_buf) - 1 - n_digits++] = digits[value & 0xF];
            value >>= 4;
        } while (value);
    } else {
        // Base-10 digits for LLVM integers
        do {
            digits_buf[sizeof(digits_buf) - 1 - n_digits++] = digits[value % 10];
            value /= 10;
        } while (value);
    }

    int bytes_required;
    if (cuda)
        bytes_required = is_uint8 ? 37 : 16;
    else
        bytes_required = 124;
    bytes_required += n_digits;

    char *stmt = (char *) malloc_check(bytes_required),
         *ptr = stmt;
    if (cuda) {
        if (unlikely(is_uint8)) {
            memcpy(ptr, "mov.b16 %w1, 0x", 15);
            ptr += 15;

            memcpy(ptr, digits_buf + sizeof(digits_buf) - n_digits, n_digits);
            ptr += n_digits;

            memcpy(ptr, "$ncvt.u8.u16 $r0, %w1", 21);
            ptr += 21;
        } else {
            memcpy(ptr, "mov.$b0 $r0, 0x", 15);
            ptr += 15;

            memcpy(ptr, digits_buf + sizeof(digits_buf) - n_digits, n_digits);
            ptr += n_digits;
        }
    } else {
        memcpy(ptr, "$r0_0 = insertelement <$w x $t0> undef, $t0 ", 44);
        ptr += 44;

        if (type == VarType::Float32 || type == VarType::Float64) {
            memcpy(ptr, "0x", 2);
            ptr += 2;
        }

        memcpy(ptr, digits_buf + sizeof(digits_buf) - n_digits, n_digits);
        ptr += n_digits;

        memcpy(ptr, ", i32 0$n$r0 = shufflevector <$w x $t0> $r0_0, "
               "<$w x $t0> undef, <$w x i32> $z", 78);
        ptr += 78;
    }
    *ptr = '\0';

    Variable v;
    v.type = (uint32_t) type;
    v.size = size;
    v.stmt = stmt;
    v.tsize = 1;
    v.free_stmt = 1;
    v.cuda = cuda;
    v.is_literal_zero = is_literal_zero;
    v.is_literal_one = is_literal_one;

    uint32_t index; Variable *vo;
    std::tie(index, vo) = jit_var_new(v, size != 1);
    jit_log(Debug, "jit_var_new_literal(%u): %s%s", index, vo->stmt,
            vo->ref_count_int + vo->ref_count_ext == 0 ? "" : " (reused)");

    jit_var_inc_ref_ext(index, vo);

    return index;
}

/// Append a variable to the instruction trace (no operands)
uint32_t jit_var_new_0(VarType type, const char *stmt, int stmt_static,
                       int cuda, uint32_t size) {
    if (unlikely(size == 0))
        return 0;

    Variable v;
    v.type = (uint32_t) type;
    v.size = size;
    v.stmt = stmt_static ? (char *) stmt : strdup(stmt);
    v.tsize = 1;
    v.free_stmt = stmt_static == 0;
    v.cuda = cuda;

    uint32_t index; Variable *vo;
    std::tie(index, vo) = jit_var_new(v);
    jit_log(Debug, "jit_var_new(%u): %s%s",
            index, vo->stmt,
            vo->ref_count_int + vo->ref_count_ext == 0 ? "" : " (reused)");

    jit_var_inc_ref_ext(index, vo);

    return index;
}

/// Append a variable to the instruction trace (1 operand)
uint32_t jit_var_new_1(VarType type, const char *stmt, int stmt_static,
                       int cuda, uint32_t op1) {
    if (unlikely(op1 == 0))
        return 0;

    Variable *v1 = jit_var(op1);

    Variable v;
    v.type = (uint32_t) type;
    v.size = v1->size;
    v.stmt = stmt_static ? (char *) stmt : strdup(stmt);
    v.dep[0] = op1;
    v.tsize = 1 + v1->tsize;
    v.free_stmt = stmt_static == 0;
    v.cuda = cuda;

    if (unlikely(v1->pending_scatter)) {
        jit_eval();
        v1 = jit_var(op1);
        v.tsize = 2;
    }

    jit_var_inc_ref_int(op1, v1);

    uint32_t index; Variable *vo;
    std::tie(index, vo) = jit_var_new(v);
    jit_log(Debug, "jit_var_new(%u <- %u): %s%s",
            index, op1, vo->stmt,
            vo->ref_count_int + vo->ref_count_ext == 0 ? "" : " (reused)");

    jit_var_inc_ref_ext(index, vo);

    return index;
}

/// Append a variable to the instruction trace (2 operands)
uint32_t jit_var_new_2(VarType type, const char *stmt, int stmt_static,
                       int cuda, uint32_t op1, uint32_t op2) {
    if (unlikely(op1 == 0 && op2 == 0))
        return 0;
    if (unlikely(op1 == 0 || op2 == 0))
        jit_raise("jit_var_new(): arithmetic involving "
                  "uninitialized variable!");

    Variable *v1 = jit_var(op1),
             *v2 = jit_var(op2);

    Variable v;
    v.type = (uint32_t) type;
    v.size = std::max(v1->size, v2->size);
    v.stmt = stmt_static ? (char *) stmt : strdup(stmt);
    v.dep[0] = op1;
    v.dep[1] = op2;
    v.tsize = 1 + v1->tsize + v2->tsize;
    v.free_stmt = stmt_static == 0;
    v.cuda = cuda;

    if (unlikely((v1->size != 1 && v1->size != v.size) ||
                 (v2->size != 1 && v2->size != v.size))) {
        jit_raise(
            "jit_var_new(): arithmetic involving arrays of incompatible "
            "size (%u and %u). The instruction was \"%s\".",
            v1->size, v2->size, stmt);
    } else if (unlikely(v1->pending_scatter || v2->pending_scatter)) {
        jit_eval();
        v1 = jit_var(op1);
        v2 = jit_var(op2);
        v.tsize = 3;
    }

    jit_var_inc_ref_int(op1, v1);
    jit_var_inc_ref_int(op2, v2);

    uint32_t index; Variable *vo;
    std::tie(index, vo) = jit_var_new(v);
    jit_log(Debug, "jit_var_new(%u <- %u, %u): %s%s",
            index, op1, op2, vo->stmt,
            vo->ref_count_int + vo->ref_count_ext == 0 ? "" : " (reused)");

    jit_var_inc_ref_ext(index, vo);

    return index;
}

/// Append a variable to the instruction trace (3 operands)
uint32_t jit_var_new_3(VarType type, const char *stmt, int stmt_static,
                       int cuda, uint32_t op1, uint32_t op2, uint32_t op3) {
    if (unlikely(op1 == 0 && op2 == 0 && op3 == 0))
        return 0;
    else if (unlikely(op1 == 0 || op2 == 0 || op3 == 0))
        jit_raise("jit_var_new(): arithmetic involving "
                  "uninitialized variable!");

    Variable *v1 = jit_var(op1),
             *v2 = jit_var(op2),
             *v3 = jit_var(op3);

    Variable v;
    v.type = (uint32_t) type;
    v.size = std::max({ v1->size, v2->size, v3->size });
    v.stmt = stmt_static ? (char *) stmt : strdup(stmt);
    v.dep[0] = op1;
    v.dep[1] = op2;
    v.dep[2] = op3;
    v.tsize = 1 + v1->tsize + v2->tsize + v3->tsize;
    v.free_stmt = stmt_static == 0;
    v.cuda = cuda;

    if (unlikely((v1->size != 1 && v1->size != v.size) ||
                 (v2->size != 1 && v2->size != v.size) ||
                 (v3->size != 1 && v3->size != v.size))) {
        jit_raise("jit_var_new(): arithmetic involving arrays of incompatible "
                  "size (%u, %u, and %u). The instruction was \"%s\".",
                  v1->size, v2->size, v3->size, stmt);
    } else if (unlikely(v1->pending_scatter || v2->pending_scatter || v3->pending_scatter)) {
        jit_eval();
        v1 = jit_var(op1);
        v2 = jit_var(op2);
        v3 = jit_var(op3);
        v.tsize = 4;
    }

    jit_var_inc_ref_int(op1, v1);
    jit_var_inc_ref_int(op2, v2);
    jit_var_inc_ref_int(op3, v3);

    uint32_t index; Variable *vo;
    std::tie(index, vo) = jit_var_new(v);
    jit_log(Debug, "jit_var_new(%u <- %u, %u, %u): %s%s",
            index, op1, op2, op3, vo->stmt,
            vo->ref_count_int + vo->ref_count_ext == 0 ? "" : " (reused)");

    jit_var_inc_ref_ext(index, vo);

    return index;
}

/// Append a variable to the instruction trace (4 operands)
uint32_t jit_var_new_4(VarType type, const char *stmt, int stmt_static,
                       int cuda, uint32_t op1, uint32_t op2, uint32_t op3,
                       uint32_t op4) {
    if (unlikely(op1 == 0 && op2 == 0 && op3 == 0 && op4 == 0))
        return 0;
    else if (unlikely(op1 == 0 || op2 == 0 || op3 == 0 || op4 == 0))
        jit_raise("jit_var_new(): arithmetic involving "
                  "uninitialized variable!");

    Variable *v1 = jit_var(op1),
             *v2 = jit_var(op2),
             *v3 = jit_var(op3),
             *v4 = jit_var(op4);

    Variable v;
    v.type = (uint32_t) type;
    v.size = std::max({ v1->size, v2->size, v3->size, v4->size });
    v.stmt = stmt_static ? (char *) stmt : strdup(stmt);
    v.dep[0] = op1;
    v.dep[1] = op2;
    v.dep[2] = op3;
    v.dep[3] = op4;
    v.tsize = 1 + v1->tsize + v2->tsize + v3->tsize + v4->tsize;
    v.free_stmt = stmt_static == 0;
    v.cuda = cuda;

    if (unlikely((v1->size != 1 && v1->size != v.size) ||
                 (v2->size != 1 && v2->size != v.size) ||
                 (v3->size != 1 && v3->size != v.size) ||
                 (v4->size != 1 && v4->size != v.size))) {
        jit_raise(
            "jit_var_new(): arithmetic involving arrays of incompatible "
            "size (%u, %u, %u, and %u). The instruction was \"%s\".",
            v1->size, v2->size, v3->size, v4->size, stmt);
    } else if (unlikely(v1->pending_scatter || v2->pending_scatter ||
                        v3->pending_scatter || v4->pending_scatter)) {
        jit_eval();
        v1 = jit_var(op1);
        v2 = jit_var(op2);
        v3 = jit_var(op3);
        v4 = jit_var(op4);
        v.tsize = 5;
    }

    jit_var_inc_ref_int(op1, v1);
    jit_var_inc_ref_int(op2, v2);
    jit_var_inc_ref_int(op3, v3);
    jit_var_inc_ref_int(op4, v4);

    uint32_t index; Variable *vo;
    std::tie(index, vo) = jit_var_new(v);
    jit_log(Debug, "jit_var_new(%u <- %u, %u, %u, %u): %s%s",
            index, op1, op2, op3, op4, vo->stmt,
            vo->ref_count_int + vo->ref_count_ext == 0 ? "" : " (reused)");

    jit_var_inc_ref_ext(index, vo);

    return index;
}

/// Register an existing variable with the JIT compiler
uint32_t jit_var_map_mem(VarType type, int cuda, void *ptr, uint32_t size, int free) {
    if (unlikely(size == 0))
        return 0;

    Variable v;
    v.type = (uint32_t) type;
    v.data = ptr;
    v.size = size;
    v.retain_data = free == 0;
    v.tsize = 1;
    v.cuda = cuda;
    if (cuda == 0) {
        uintptr_t align =
            std::min(64u, jit_llvm_vector_width * var_type_size[(int) type]);
        v.unaligned = uintptr_t(ptr) % align != 0;
    }

    uint32_t index; Variable *vo;
    std::tie(index, vo) = jit_var_new(v);
    jit_log(Debug, "jit_var_map_mem(%u): " ENOKI_PTR ", size=%u, free=%i",
            index, (uintptr_t) ptr, size, (int) free);

    jit_var_inc_ref_ext(index, vo);

    return index;
}

/// Copy a memory region onto the device and return its variable index
uint32_t jit_var_copy_mem(AllocType atype, VarType vtype, int cuda, const void *ptr,
                          uint32_t size) {
    Stream *stream = active_stream;

    if (unlikely(!stream)) {
        jit_raise("jit_var_copy_mem(): you must invoke jitc_set_device() to "
                  "choose a target device before using this function.");
    } else if (unlikely((bool) cuda != stream->cuda)) {
        jit_raise(
            "jit_var_copy_mem(): attempted to copy to a %s array while the %s "
            "backend was active! You must invoke jit_set_device() to set "
            "the right backend!",
            cuda ? "CUDA" : "LLVM", stream->cuda ? "CUDA" : "LLVM");
    }

    size_t total_size = (size_t) size * (size_t) var_type_size[(int) vtype];
    void *target_ptr;

    if (stream->cuda) {
        target_ptr = jit_malloc(AllocType::Device, total_size);

        if (atype == AllocType::Auto) {
            unsigned int result = 0;
            CUresult rv = cuPointerGetAttribute(
                &result, CU_POINTER_ATTRIBUTE_MEMORY_TYPE, (CUdeviceptr) ptr);
            if (rv == CUDA_ERROR_INVALID_VALUE || result == CU_MEMORYTYPE_HOST)
                atype = AllocType::Host;
            else
                atype = AllocType::Device;
        }

        scoped_set_context guard(stream->context);
        if (atype == AllocType::HostAsync) {
            jit_fail("jit_var_copy_mem(): copy from HostAsync to GPU memory not supported!");
        } else if (atype == AllocType::Host) {
            void *host_ptr = jit_malloc(AllocType::HostPinned, total_size);
            memcpy(host_ptr, ptr, total_size);
            cuda_check(cuMemcpyAsync((CUdeviceptr) target_ptr,
                                     (CUdeviceptr) host_ptr, total_size,
                                     stream->handle));
            jit_free(host_ptr);
        } else {
            cuda_check(cuMemcpyAsync((CUdeviceptr) target_ptr,
                                     (CUdeviceptr) ptr, total_size,
                                     stream->handle));
        }
    } else {
        if (atype == AllocType::HostAsync) {
            target_ptr = jit_malloc(AllocType::HostAsync, total_size);
            jit_memcpy_async(target_ptr, ptr, total_size);
        } else if (atype == AllocType::Host) {
            target_ptr = jit_malloc(AllocType::Host, total_size);
            memcpy(target_ptr, ptr, total_size);
            target_ptr = jit_malloc_migrate(target_ptr, AllocType::HostAsync, 1);
        } else {
            jit_fail("jit_var_copy_mem(): copy from GPU to HostAsync memory not supported!");
        }
    }

    uint32_t index = jit_var_map_mem(vtype, cuda, target_ptr, size, true);
    jit_log(Debug, "jit_var_copy_mem(%u, size=%u)", index, size);
    return index;
}

/// Register pointer literal as a special variable within the JIT compiler
uint32_t jit_var_copy_ptr(const void *ptr, uint32_t index) {
    auto it = state.variable_from_ptr.find(ptr);
    if (it != state.variable_from_ptr.end()) {
        uint32_t index = it.value();
        jit_var_inc_ref_ext(index);
        return index;
    }

    Variable v;
    v.type = (uint32_t) VarType::Pointer;
    v.data = (void *) ptr;
    v.size = 1;
    v.tsize = 0;
    v.retain_data = true;
    v.dep[0] = index;
    v.direct_pointer = true;
    v.cuda = active_stream->cuda;

    jit_var_inc_ref_ext(index);

    uint32_t index_o; Variable *vo;
    std::tie(index_o, vo) = jit_var_new(v);
    jit_log(Debug, "jit_var_copy_ptr(%u <- %u): " ENOKI_PTR, index_o, index,
            (uintptr_t) ptr);

    jit_var_inc_ref_ext(index_o, vo);
    state.variable_from_ptr[ptr] = index_o;
    return index_o;
}

uint32_t jit_var_copy_var(uint32_t index) {
    if (index == 0)
        return 0;

    Variable *v = jit_var(index);
    if (v->pending_scatter) {
        jit_var_eval(index);
        v = jit_var(index);
    }

    uint32_t index_old = index;
    if (v->data) {
        index = jit_var_copy_mem(v->cuda ? AllocType::Device : AllocType::HostAsync,
                                 (VarType) v->type, v->cuda, v->data, v->size);
    } else {
        Variable v2 = *v;
        v2.ref_count_int = 0;
        v2.ref_count_ext = 0;
        v2.has_extra = 0;

        if (v2.free_stmt)
            v2.stmt = strdup(v2.stmt);

        std::tie(index, v) = jit_var_new(v2, true);
        jit_var_inc_ref_ext(index, v);
    }
    jit_log(Debug, "jit_var_copy_var(%u <- %u)", index, index_old);
    return index;
}

/// Migrate a variable to a different flavor of memory
uint32_t jit_var_migrate(uint32_t src_index, AllocType dst_type) {
    if (src_index == 0)
        return 0;

    jit_var_eval(src_index);

    Variable *v = jit_var(src_index);
    auto it = state.alloc_used.find(v->data);
    if (unlikely(it == state.alloc_used.end()))
        jit_raise("jit_var_migrate(): Cannot resolve pointer to actual allocation!");
    AllocInfo ai = it.value();

    uint32_t dst_index = src_index;

    void *src_ptr = v->data,
         *dst_ptr = jit_malloc_migrate(src_ptr, dst_type, 0);

    if (src_ptr != dst_ptr) {
        Variable v2 = *v;
        v2.data = dst_ptr;
        v2.retain_data = false;
        v2.ref_count_int = 0;
        v2.ref_count_ext = 0;
        std::tie(dst_index, v) = jit_var_new(v2);
    }

    jit_var_inc_ref_ext(dst_index, v);

    jit_log(Debug, "jit_var_migrate(%u -> %u, " ENOKI_PTR " -> " ENOKI_PTR ", %s -> %s)",
            src_index, dst_index, (uintptr_t) src_ptr, (uintptr_t) dst_ptr,
            alloc_type_name[ai.type], alloc_type_name[(int) dst_type]);

    return dst_index;
}

/// Query the current (or future, if not yet evaluated) allocation flavor of a variable
AllocType jit_var_alloc_type(uint32_t index) {
    const Variable *v = jit_var(index);

    if (v->data)
        return jit_malloc_type(v->data);

    return v->cuda ? AllocType::Device : AllocType::HostAsync;
}

/// Query the device (or future, if not yet evaluated) associated with a variable
int jit_var_device(uint32_t index) {
    const Variable *v = jit_var(index);

    if (v->data)
        return jit_malloc_device(v->data);

    Stream *stream = active_stream;
    if (unlikely(!stream))
        jit_raise("jit_var_device(): you must invoke jitc_set_device() to "
                  "choose a target device before using this function.");

    return stream->device;
}

/// Mark a variable as a scatter operation that writes to 'target'
void jit_var_mark_scatter(uint32_t index, uint32_t target) {
    Stream *stream = active_stream;
    if (unlikely(!stream))
        jit_raise("jit_var_mark_scatter(): you must invoke jitc_set_device() to "
                  "choose a target device before using this function.");

    jit_log(Debug, "jit_var_mark_scatter(%u, %u)", index, target);

    // Update scatter operation
    Variable *v = jit_var(index);
    v->scatter = true;
    stream->todo.push_back(index);
    stream->side_effect_counter++;

    // Update target variable
    if (target) {
        v = jit_var(target);
        v->pending_scatter = true;
    }
}

/// Is the given variable a literal that equals zero?
int jit_var_is_literal_zero(uint32_t index) {
    if (index == 0)
        return 0;
    return jit_var(index)->is_literal_zero;
}

/// Is the given variable a literal that equals one?
int jit_var_is_literal_one(uint32_t index) {
    if (index == 0)
        return 0;
    return jit_var(index)->is_literal_one;
}

/// Return a human-readable summary of registered variables
const char *jit_var_whos() {
    buffer.clear();
    buffer.put("\n  ID        Type       Status       E/I Refs  Entries     Storage    Label");
    buffer.put("\n  ========================================================================\n");

    std::vector<uint32_t> indices;
    indices.reserve(state.variables.size());
    for (const auto& it : state.variables)
        indices.push_back(it.first);
    std::sort(indices.begin(), indices.end());

    size_t mem_size_evaluated = 0,
           mem_size_saved = 0,
           mem_size_unevaluated = 0;

    for (uint32_t index: indices) {
        const Variable *v = jit_var(index);
        size_t mem_size = (size_t) v->size * (size_t) var_type_size[v->type];

        buffer.fmt("  %-9u %s %3s   ", index, v->cuda ? "cuda" : "llvm", var_type_name_short[v->type]);

        if (v->direct_pointer) {
            buffer.put("direct ptr.");
        } else if (v->data) {
            auto it = state.alloc_used.find(v->data);
            if (unlikely(it == state.alloc_used.end())) {
                if (!v->retain_data)
                    jit_raise("jit_var_whos(): Cannot resolve pointer to actual allocation!");
                else
                    buffer.put("mapped mem.");
            } else {
                AllocInfo ai = it.value();

                if ((AllocType) ai.type == AllocType::Device)
                    buffer.fmt("device %-4i", (int) ai.device);
                else
                    buffer.put(alloc_type_name_short[ai.type]);
            }
        } else {
            buffer.put("[not ready]");
        }

        size_t sz = buffer.fmt("  %u / %u", v->ref_count_ext, v->ref_count_int);
        const char *label = jit_var_label(index);

        buffer.fmt("%*s%-12u%-8s   %s\n", 12 - (int) sz, "", v->size,
                   jit_mem_string(mem_size), label ? label : "");

        if (v->direct_pointer)
            continue;
        else if (v->data)
            mem_size_evaluated += mem_size;
        else if (v->ref_count_ext == 0)
            mem_size_saved += mem_size;
        else
            mem_size_unevaluated += mem_size;
    }
    if (indices.empty())
        buffer.put("                       -- No variables registered --\n");

    buffer.put("  ========================================================================\n\n");
    buffer.put("  JIT compiler\n");
    buffer.put("  ============\n");
    buffer.fmt("   - Memory usage (evaluated)   : %s.\n",
               jit_mem_string(mem_size_evaluated));
    buffer.fmt("   - Memory usage (unevaluated) : %s.\n",
               jit_mem_string(mem_size_unevaluated));
    buffer.fmt("   - Memory usage (saved)       : %s.\n",
               jit_mem_string(mem_size_saved));
    buffer.fmt("   - Kernel launches            : %zu (%zu cache hits, "
               "%zu soft, %zu hard misses).\n\n",
               state.kernel_launches, state.kernel_hits,
               state.kernel_soft_misses, state.kernel_hard_misses);

    buffer.put("  Memory allocator\n");
    buffer.put("  ================\n");
    for (int i = 0; i < (int) AllocType::Count; ++i)
        buffer.fmt("   - %-20s: %s/%s used (peak: %s).\n",
                   alloc_type_name[i],
                   std::string(jit_mem_string(state.alloc_usage[i])).c_str(),
                   std::string(jit_mem_string(state.alloc_allocated[i])).c_str(),
                   std::string(jit_mem_string(state.alloc_watermark[i])).c_str());

    return buffer.get();
}

/// Return a GraphViz representation of registered variables
const char *jit_var_graphviz() {
    std::vector<uint32_t> indices;
    indices.reserve(state.variables.size());
    for (const auto& it : state.variables)
        indices.push_back(it.first);

    std::sort(indices.begin(), indices.end());
    buffer.clear();
    buffer.put("digraph {\n");
    buffer.put("  graph [dpi=50];\n");
    buffer.put("  node [shape=record fontname=Consolas];\n");
    buffer.put("  edge [fontname=Consolas];\n");
    for (uint32_t index: indices) {
        const Variable *v = jit_var(index);

        const char *color = "";
        const char *stmt = v->stmt;
        if (v->direct_pointer) {
            color = " fillcolor=wheat style=filled";
            stmt = "[direct pointer]";
        } else if (v->data) {
            color = " fillcolor=salmon style=filled";
            stmt = "[evaluated array]";
        } else if (v->scatter) {
            color = " fillcolor=cornflowerblue style=filled";
        }

        char *out = (char *) malloc(strlen(stmt) * 2 + 1),
             *ptr = out;
        for (int j = 0; ; ++j) {
            if (stmt[j] == '$' && stmt[j + 1] == 'n') {
                *ptr++='\\';
                continue;
            } else if (stmt[j] == '<' || stmt[j] == '>') {
                *ptr++='\\';
            }
            *ptr++ = stmt[j];
            if (stmt[j] == '\0')
                break;
        }

        const char *label = jit_var_label(index);
        buffer.fmt("  %u [label=\"{%s%s%s%s%s|{Type: %s %s|Size: %u}|{ID "
                   "#%u|E:%u|I:%u}}\"%s];\n",
                   index, out, label ? "|Label: \\\"" : "",
                   label ? label : "", label ? "\\\"" : "",
                   v->pending_scatter ? "| ** DIRTY **" : "",
                   v->cuda ? "cuda" : "llvm",
                   v->type == (int) VarType::Invalid ? "none"
                                 : var_type_name_short[v->type],
                   v->size, index, v->ref_count_ext, v->ref_count_int, color);

        free(out);

        for (uint32_t i = 0; i< 4; ++i) {
            if (v->dep[i])
                buffer.fmt("  %u -> %u [label=\" %u\"];\n", v->dep[i], index, i + 1);
        }
    }
    buffer.put("}\n");
    return buffer.get();
}

/// Return a human-readable summary of the contents of a variable
const char *jit_var_str(uint32_t index) {
    jit_var_eval(index);

    const Variable *v = jit_var(index);

    Stream *stream = active_stream;
    if (unlikely(v->cuda != stream->cuda))
        jit_raise("jit_var_str(): attempted to stringify a %s variable "
                  "while the %s backend was activated! You must invoke "
                  "jit_set_device() to set the right backend!",
                  v->cuda ? "CUDA" : "LLVM", stream->cuda ? "CUDA" : "LLVM");
    else if (unlikely(v->pending_scatter))
        jit_raise("jit_var_str(): element remains dirty after evaluation!");
    else if (unlikely(!v->data))
        jit_raise("jit_var_str(): invalid/uninitialized variable!");

    size_t size            = v->size,
           isize           = var_type_size[v->type],
           limit_remainder = std::min(5u, (state.print_limit + 3) / 4) * 2;

    uint8_t dst[8];
    const uint8_t *src = (const uint8_t *) v->data;

    buffer.clear();
    buffer.putc('[');
    for (uint32_t i = 0; i < size; ++i) {
        if (size > state.print_limit && i == limit_remainder / 2) {
            buffer.fmt(".. %zu skipped .., ", size - limit_remainder);
            i = (uint32_t) (size - limit_remainder / 2 - 1);
            continue;
        }

        const uint8_t *src_offset = src + i * isize;

        jit_memcpy(dst, src_offset, isize);

        const char *comma = i + 1 < (uint32_t) size ? ", " : "";
        switch ((VarType) v->type) {
            case VarType::Bool:    buffer.fmt("%"   PRIu8  "%s", *(( uint8_t *) dst), comma); break;
            case VarType::Int8:    buffer.fmt("%"   PRId8  "%s", *((  int8_t *) dst), comma); break;
            case VarType::UInt8:   buffer.fmt("%"   PRIu8  "%s", *(( uint8_t *) dst), comma); break;
            case VarType::Int16:   buffer.fmt("%"   PRId16 "%s", *(( int16_t *) dst), comma); break;
            case VarType::UInt16:  buffer.fmt("%"   PRIu16 "%s", *((uint16_t *) dst), comma); break;
            case VarType::Int32:   buffer.fmt("%"   PRId32 "%s", *(( int32_t *) dst), comma); break;
            case VarType::UInt32:  buffer.fmt("%"   PRIu32 "%s", *((uint32_t *) dst), comma); break;
            case VarType::Int64:   buffer.fmt("%"   PRId64 "%s", *(( int64_t *) dst), comma); break;
            case VarType::UInt64:  buffer.fmt("%"   PRIu64 "%s", *((uint64_t *) dst), comma); break;
            case VarType::Pointer: buffer.fmt("0x%" PRIx64 "%s", *((uint64_t *) dst), comma); break;
            case VarType::Float32: buffer.fmt("%g%s", *((float *) dst), comma); break;
            case VarType::Float64: buffer.fmt("%g%s", *((double *) dst), comma); break;
            default: jit_fail("jit_var_str(): invalid type!");
        }
    }
    buffer.putc(']');
    return buffer.get();
}

/// Schedule a variable \c index for future evaluation via \ref jitc_eval()
int jit_var_schedule(uint32_t index) {
    Stream *stream = active_stream;
    if (unlikely(!stream))
        jit_raise("jit_var_schedule(): you must invoke jitc_set_device() to "
                  "choose a target device before using this function.");

    auto it = state.variables.find(index);
    if (unlikely(it == state.variables.end()))
        jit_raise("jit_var_schedule(%u): unknown variable!", index);
    Variable *v = &it.value();

    if (v->data == nullptr && !v->direct_pointer) {
        if (unlikely(v->cuda != stream->cuda))
            jit_raise("jit_var_schedule(): attempted to schedule a %s variable "
                      "while the %s backend was activated! You must invoke "
                      "jit_set_device() to set the right backend!",
                      v->cuda ? "CUDA" : "LLVM", stream->cuda ? "CUDA" : "LLVM");

        stream->todo.push_back(index);
        jit_log(Debug, "jit_var_schedule(%u)", index);

        return 1;
    } else if (v->pending_scatter) {
        return 1;
    }

    return 0;
}

/// Evaluate the variable \c index right away, if it is unevaluated/dirty.
int jit_var_eval(uint32_t index) {
    Stream *stream = active_stream;
    auto it = state.variables.find(index);
    if (unlikely(it == state.variables.end()))
        jit_raise("jit_var_eval(%u): unknown variable!", index);
    Variable *v = &it.value();

    bool unevaluated = v->data == nullptr && !v->direct_pointer;

    if (unevaluated || v->pending_scatter) {
        if (unlikely(!stream))
            jit_raise("jit_var_eval(): you must invoke jitc_set_device() to "
                      "choose a target device before using this function.");
        else if (unlikely(v->cuda != stream->cuda))
            jit_raise("jit_var_eval(): attempted to evaluate a %s variable "
                      "while the %s backend was activated! You must invoke "
                      "jit_set_device() to set the right backend!",
                      v->cuda ? "CUDA" : "LLVM", stream->cuda ? "CUDA" : "LLVM");

        if (unevaluated) {
            if (v->is_literal_zero) {
                /* Optimization: don't bother building a kernel just to
                   zero-initialize a single variable and use an
                   jit_memset_async() call instead. This fits the common use
                   case of creating an arrays of zero and then scattering into
                   it (which will call jit_var_eval() on the target array)*/

                jit_cse_drop(index, v);
                if (v->free_stmt) {
                    free(v->stmt);
                    v->free_stmt = 0;
                }
                v->stmt = nullptr;
                v->is_literal_zero = false;

                uint32_t isize = var_type_size[v->type];
                v->data = jit_malloc(stream->cuda ? AllocType::Device
                                                  : AllocType::HostAsync,
                                     (size_t) v->size * (size_t) isize);

                uint64_t zero = 0;
                jit_memset_async(v->data, v->size, isize, &zero);

                return 1;
            } else {
                stream->todo.push_back(index);
            }
        }
        jit_eval();
        v = jit_var(index);

        if (unlikely(v->pending_scatter))
            jit_raise("jit_var_eval(): element remains dirty after evaluation!");
        else if (unlikely(!v->data))
            jit_raise("jit_var_eval(): invalid/uninitialized variable!");

        return 1;
    }

    return 0;
}

/// Read a single element of a variable and write it to 'dst'
void jit_var_read(uint32_t index, uint32_t offset, void *dst) {
    jit_var_eval(index);

    Variable *v = jit_var(index);
    if (v->size == 1)
        offset = 0;
    else if (unlikely(offset >= v->size))
        jit_raise("jit_var_read(): attempted to access entry %u in an array of "
                  "size %u!", offset, v->size);

    size_t isize = var_type_size[v->type];
    const uint8_t *src = (const uint8_t *) v->data + (size_t) offset * isize;

    jit_memcpy(dst, src, isize);
}

/// Reverse of jit_var_read(). Copy 'dst' to a single element of a variable
void jit_var_write(uint32_t index, uint32_t offset, const void *src) {
    jit_var_eval(index);

    Variable *v = jit_var(index);
    if (unlikely(offset >= v->size))
        jit_raise("jit_var_write(): attempted to access entry %u in an array of "
                  "size %u!", offset, v->size);

    uint32_t isize = var_type_size[v->type];
    uint8_t *dst = (uint8_t *) v->data + (size_t) offset * isize;
    jit_poke(dst, src, isize);
}
