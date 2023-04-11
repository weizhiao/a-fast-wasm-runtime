#include "wasm_opcode.h"
#include "wasm_validator.h"
#include "wasm_memory.h"
#include "runtime_utils.h"

#define REF_ANY VALUE_TYPE_ANY
#define REF_I32 VALUE_TYPE_I32
#define REF_F32 VALUE_TYPE_F32
#define REF_I64_1 VALUE_TYPE_I64
#define REF_I64_2 VALUE_TYPE_I64
#define REF_F64_1 VALUE_TYPE_F64
#define REF_F64_2 VALUE_TYPE_F64
#define REF_V128_1 VALUE_TYPE_V128
#define REF_V128_2 VALUE_TYPE_V128
#define REF_V128_3 VALUE_TYPE_V128
#define REF_V128_4 VALUE_TYPE_V128

static char *
type2str(uint8 type);

static inline bool
is_32bit_type(uint8 type)
{
    if (type == VALUE_TYPE_I32 || type == VALUE_TYPE_F32
    )
        return true;
    return false;
}

static inline bool
is_64bit_type(uint8 type)
{
    if (type == VALUE_TYPE_I64 || type == VALUE_TYPE_F64)
        return true;
    return false;
}

static bool
check_stack_pop(uint8 *frame_ref, int32 stack_cell_num, uint8 type, char *error_buf,
                uint32 error_buf_size)
{

    if (stack_cell_num > 0 && *(frame_ref - 1) == VALUE_TYPE_ANY) {
        /* the stack top is a value of any type, return success */
        return true;
    }

    if ((is_32bit_type(type) && stack_cell_num < 1)
        || (is_64bit_type(type) && stack_cell_num < 2)
    ) {
        set_error_buf(error_buf, error_buf_size,
                      "type mismatch: expect data but stack was empty");
        return false;
    }

    if ((is_32bit_type(type) && *(frame_ref - 1) != type)
        || (is_64bit_type(type)
            && (*(frame_ref - 2) != type || *(frame_ref - 1) != type))
    ) {
        set_error_buf_v(error_buf, error_buf_size, "%s%s%s",
                        "type mismatch: expect ", type2str(type),
                        " but got other");
        return false;
    }

    return true;
}

static bool
wasm_loader_push_frame_ref(WASMLoaderContext *ctx, uint8 type, char *error_buf,
                           uint32 error_buf_size)
{
    if (type == VALUE_TYPE_VOID)
        return true;

    //如果空间不够会增加16个位置
    if (ctx->frame_ref >= ctx->frame_ref_boundary) {
        ctx->frame_ref_bottom = wasm_runtime_realloc(ctx->frame_ref_bottom, ctx->frame_ref_size + 16);
        if (!ctx->frame_ref_bottom) {
            return false;
        }
        ctx->frame_ref_size += 16;
        ctx->frame_ref_boundary = ctx->frame_ref_bottom + ctx->frame_ref_size;
        ctx->frame_ref = ctx->frame_ref_bottom + ctx->stack_cell_num;
    }

    *ctx->frame_ref++ = type;
    ctx->stack_cell_num++;
    if (is_32bit_type(type) || type == VALUE_TYPE_ANY)
        goto check_stack_and_return;

    ctx->stack_cell_num++;

check_stack_and_return:
    if (ctx->stack_cell_num > ctx->max_stack_cell_num) {
        ctx->max_stack_cell_num = ctx->stack_cell_num;
        if (ctx->max_stack_cell_num > UINT16_MAX) {
            set_error_buf(error_buf, error_buf_size,
                          "operand stack depth limit exceeded");
            return false;
        }
    }
    return true;
}

static bool
wasm_loader_pop_frame_ref(WASMLoaderContext *ctx, uint8 type, char *error_buf,
                          uint32 error_buf_size)
{
    BranchBlock *cur_block = ctx->frame_csp - 1;
    int32 available_stack_cell =
        (int32)(ctx->stack_cell_num - cur_block->stack_cell_num);

    if (available_stack_cell <= 0 && cur_block->is_stack_polymorphic)
        return true;

    if (type == VALUE_TYPE_VOID)
        return true;

    if (!check_stack_pop(ctx->frame_ref, available_stack_cell, type, error_buf, error_buf_size))
        return false;

    ctx->frame_ref--;
    ctx->stack_cell_num--;

    if (is_32bit_type(type) || *ctx->frame_ref == VALUE_TYPE_ANY)
        return true;

    ctx->frame_ref--;
    ctx->stack_cell_num--;

    return true;
}

static bool
wasm_loader_push_frame_csp(WASMLoaderContext *ctx, uint8 label_type,
                           BlockType block_type, uint8 *start_addr,
                           char *error_buf, uint32 error_buf_size)
{
    BranchBlock *frame_csp;
    if (ctx->frame_csp >= ctx->frame_csp_boundary) {                 
            ctx->frame_csp_bottom = wasm_runtime_realloc(                                                  
                ctx->frame_csp_bottom,               
                (ctx->frame_csp_size + 8) * sizeof(BranchBlock));
            if(!ctx->frame_csp_bottom){
                return false;
            }
            ctx->frame_csp_size += 8;    
            ctx->frame_csp_boundary = ctx->frame_csp_bottom + ctx->frame_csp_size;              
            ctx->frame_csp = ctx->frame_csp_bottom + ctx->csp_num;        
    }
    frame_csp = ctx->frame_csp;
    frame_csp->label_type = label_type;
    frame_csp->block_type = block_type;
    frame_csp->start_addr = start_addr;
    frame_csp->stack_cell_num = ctx->stack_cell_num;
    frame_csp->table_stack_num = 0;
    frame_csp->table_stack_size = 8;
    if (!(frame_csp->table_stack_bottom = frame_csp->table_stack = wasm_runtime_malloc(sizeof(uint32) * 8))){
        return false;
    }

    if (label_type == LABEL_TYPE_LOOP) {
        frame_csp->branch_table_end_idx = ctx->branch_table_num;
    }

    ctx->frame_csp++;
    ctx->csp_num++;
    return true;
}

static bool
wasm_loader_pop_frame_csp(WASMLoaderContext *ctx, char *error_buf,
                          uint32 error_buf_size)
{
    BranchBlock *frame_csp;
    WASMBranchTable *branch_table;
    uint32 i;
    if (ctx->csp_num < 1) {                                     
            set_error_buf(error_buf, error_buf_size,                
                          "type mismatch: "                         
                          "expect data but block stack was empty"); 
            return false;                                             
    }

    frame_csp = ctx->frame_csp - 1;
    if (frame_csp->label_type != LABEL_TYPE_LOOP) {
        frame_csp->branch_table_end_idx = ctx->branch_table_num;
    }

    for (i = 0; i < frame_csp->table_stack_num; i++) {
        branch_table = ctx->branch_table_bottom + frame_csp->table_stack_bottom[i];
        uint32 opcode = branch_table->stp;
        branch_table->stp = frame_csp->branch_table_end_idx;

        switch (opcode) {
            case WASM_OP_BR:
            case WASM_OP_BR_IF:
            case WASM_OP_BR_TABLE:
                branch_table->ip = frame_csp->end_addr;
                break;
            case WASM_OP_IF:
                branch_table->ip = frame_csp->else_addr;
                break;
            case WASM_OP_ELSE:
                branch_table->ip = frame_csp->end_addr;
                break;
            default:
                set_error_buf(error_buf, error_buf_size, "unsupport branch opcode");
                return false;
        }
    }

    wasm_runtime_free(frame_csp->table_stack_bottom);

    ctx->frame_csp--;
    ctx->csp_num--;

    return true;
}

static bool
wasm_emit_branch_table(WASMLoaderContext *ctx, uint8 opcode, uint32 depth, char *error_buf,
                          uint32 error_buf_size)
{
    WASMBranchTable *branch_table;
    BranchBlock *frame_csp;
    BlockType *block_type;
    uint32 push;
    if (ctx->branch_table >= ctx->branch_table_boundary) {
        ctx->branch_table_bottom = wasm_runtime_realloc(
            ctx->branch_table_bottom, (ctx->branch_table_size + 8) * sizeof(WASMBranchTable));
        if (!ctx->branch_table_bottom) {
            return false;
        }
        ctx->branch_table_size += 8;
        ctx->branch_table_boundary = ctx->branch_table_bottom + ctx->branch_table_size;
        ctx->branch_table = ctx->branch_table_bottom + ctx->branch_table_num;
    }

    branch_table = ctx->branch_table;

    //目前用于存放指令字节
    branch_table->stp = opcode;

    //将该跳转指令对应的表索引存入相应控制块拥有的栈中
    frame_csp = ctx->frame_csp - depth - 1;

    if (frame_csp->table_stack_num >= frame_csp->table_stack_size) {
        frame_csp->table_stack_bottom = wasm_runtime_realloc(
            frame_csp->table_stack_bottom, (frame_csp->table_stack_size + 8) * sizeof(uint32)
        );
        if (!frame_csp->table_stack_bottom) {
            return false;
        } 
        frame_csp->table_stack_size += 8;
        frame_csp->table_stack = frame_csp->table_stack_bottom + frame_csp->table_stack_num;
    }

    *(frame_csp->table_stack) = ctx->branch_table_num;
    frame_csp->table_stack++;
    frame_csp->table_stack_num++;

    switch (opcode) {
        case WASM_OP_BR:
        case WASM_OP_BR_IF:
        case WASM_OP_BR_TABLE:
            block_type = &(frame_csp->block_type);
            if (block_type->is_value_type) {
                push = wasm_value_type_cell_num(block_type->u.value_type);
            }
            else {
                push = block_type->u.type->ret_cell_num;
            }
            branch_table->pop = ctx->stack_cell_num - frame_csp->stack_cell_num;
            branch_table->push = push;
            break;
        case WASM_OP_IF:
            branch_table->pop = 0;
            branch_table->push = 0;
            break;
        case WASM_OP_ELSE:
            branch_table->pop = 0;
            branch_table->push = 0;
            break;
        default:
            return false;
        }

    ctx->branch_table++;
    ctx->branch_table_num++;
    return true;
}

#define TEMPLATE_PUSH(Type)                                             \
    do {                                                                \
        if (!(wasm_loader_push_frame_ref(loader_ctx, VALUE_TYPE_##Type, \
                                         error_buf, error_buf_size)))   \
            goto fail;                                                  \
    } while (0)

#define TEMPLATE_POP(Type)                                             \
    do {                                                               \
        if (!(wasm_loader_pop_frame_ref(loader_ctx, VALUE_TYPE_##Type, \
                                        error_buf, error_buf_size)))   \
            goto fail;                                                 \
    } while (0)

#define POP_AND_PUSH(type_pop, type_push)                              \
    do {                                                               \
        if (!(wasm_loader_push_pop_frame_ref(loader_ctx, 1, type_push, \
                                             type_pop, error_buf,      \
                                             error_buf_size)))         \
            goto fail;                                                 \
    } while (0)

/* type of POPs should be the same */
#define POP2_AND_PUSH(type_pop, type_push)                             \
    do {                                                               \
        if (!(wasm_loader_push_pop_frame_ref(loader_ctx, 2, type_push, \
                                             type_pop, error_buf,      \
                                             error_buf_size)))         \
            goto fail;                                                 \
    } while (0)

#define PUSH_I32() TEMPLATE_PUSH(I32)
#define PUSH_F32() TEMPLATE_PUSH(F32)
#define PUSH_I64() TEMPLATE_PUSH(I64)
#define PUSH_F64() TEMPLATE_PUSH(F64)
#define PUSH_V128() TEMPLATE_PUSH(V128)
#define PUSH_FUNCREF() TEMPLATE_PUSH(FUNCREF)
#define PUSH_EXTERNREF() TEMPLATE_PUSH(EXTERNREF)

#define POP_I32() TEMPLATE_POP(I32)
#define POP_F32() TEMPLATE_POP(F32)
#define POP_I64() TEMPLATE_POP(I64)
#define POP_F64() TEMPLATE_POP(F64)
#define POP_V128() TEMPLATE_POP(V128)
#define POP_FUNCREF() TEMPLATE_POP(FUNCREF)
#define POP_EXTERNREF() TEMPLATE_POP(EXTERNREF)

#define PUSH_TYPE(type)                                               \
    do {                                                              \
        if (!(wasm_loader_push_frame_ref(loader_ctx, type, error_buf, \
                                         error_buf_size)))            \
            goto fail;                                                \
    } while (0)

#define POP_TYPE(type)                                               \
    do {                                                             \
        if (!(wasm_loader_pop_frame_ref(loader_ctx, type, error_buf, \
                                        error_buf_size)))            \
            goto fail;                                               \
    } while (0)

#define PUSH_CSP(label_type, block_type, _start_addr)                       \
    do {                                                                    \
        if (!wasm_loader_push_frame_csp(loader_ctx, label_type, block_type, \
                                        _start_addr, error_buf,             \
                                        error_buf_size))                    \
            goto fail;                                                      \
    } while (0)

#define POP_CSP()                                                              \
    do {                                                                       \
        if (!wasm_loader_pop_frame_csp(loader_ctx, error_buf, error_buf_size)) \
            goto fail;                                                         \
    } while (0)

#define GET_LOCAL_INDEX_TYPE_AND_OFFSET()                              \
    do {                                                               \
        read_leb_uint32(p, p_end, local_idx);                          \
        if (local_idx >= param_count + local_count) {                  \
            set_error_buf(error_buf, error_buf_size, "unknown local"); \
            goto fail;                                                 \
        }                                                              \
        local_type = local_idx < param_count                           \
                         ? param_types[local_idx]                      \
                         : local_types[local_idx - param_count];       \
        local_offset = local_offsets[local_idx];                       \
    } while (0)

static bool
check_memory(WASMModule *module, char *error_buf, uint32 error_buf_size)
{
    if (module->memory_count == 0 && module->import_memory_count == 0) {
        set_error_buf(error_buf, error_buf_size, "unknown memory");
        return false;
    }
    return true;
}

#define CHECK_MEMORY()                                        \
    do {                                                      \
        if (!check_memory(module, error_buf, error_buf_size)) \
            goto fail;                                        \
    } while (0)

static bool
check_memory_access_align(uint8 opcode, uint32 align, char *error_buf,
                          uint32 error_buf_size)
{
    uint8 mem_access_aligns[] = {
        2, 3, 2, 3, 0, 0, 1, 1, 0, 0, 1, 1, 2, 2, /* loads */
        2, 3, 2, 3, 0, 1, 0, 1, 2                 /* stores */
    };
    if (align > mem_access_aligns[opcode - WASM_OP_I32_LOAD]) {
        set_error_buf(error_buf, error_buf_size,
                      "alignment must not be larger than natural");
        return false;
    }
    return true;
}

static inline uint32
block_type_get_param_types(BlockType *block_type, uint8 **p_param_types)
{
    uint32 param_count = 0;
    if (!block_type->is_value_type) {
        WASMType *wasm_type = block_type->u.type;
        *p_param_types = wasm_type->param;
        param_count = wasm_type->param_count;
    }
    else {
        *p_param_types = NULL;
        param_count = 0;
    }

    return param_count;
}

static inline uint32
block_type_get_result_types(BlockType *block_type, uint8 **p_result_types)
{
    uint32 result_count = 0;
    if (block_type->is_value_type) {
        if (block_type->u.value_type != VALUE_TYPE_VOID) {
            *p_result_types = &block_type->u.value_type;
            result_count = 1;
        }
    }
    else {
        WASMType *wasm_type = block_type->u.type;
        *p_result_types = wasm_type->result;
        result_count = wasm_type->result_count;
    }
    return result_count;
}

static bool
is_byte_a_type(uint8 type)
{
    return is_value_type(type) || (type == VALUE_TYPE_VOID);
}

static char *
type2str(uint8 type)
{
    char *type_str[] = { "v128", "f64", "f32", "i64", "i32" };

    if (type >= VALUE_TYPE_V128 && type <= VALUE_TYPE_I32)
        return type_str[type - VALUE_TYPE_V128];
    else if (type == VALUE_TYPE_FUNCREF)
        return "funcref";
    else if (type == VALUE_TYPE_EXTERNREF)
        return "externref";
    else
        return "unknown type";
}


static bool
wasm_loader_check_br(WASMLoaderContext *loader_ctx, uint32 depth,
                     char *error_buf, uint32 error_buf_size)
{
    BranchBlock *target_block, *cur_block;
    BlockType *target_block_type;
    uint8 *types = NULL, *frame_ref;
    uint32 arity = 0;
    int32 i, available_stack_cell;
    uint16 cell_num;

    if (loader_ctx->csp_num < depth + 1) {
        set_error_buf(error_buf, error_buf_size,
                      "unknown label, "
                      "unexpected end of section or function");
        return false;
    }

    cur_block = loader_ctx->frame_csp - 1;
    target_block = loader_ctx->frame_csp - (depth + 1);
    target_block_type = &target_block->block_type;
    frame_ref = loader_ctx->frame_ref;

    //对于loop需要特殊处理
    if (target_block->label_type == LABEL_TYPE_LOOP)
        arity = block_type_get_param_types(target_block_type, &types);
    else
        arity = block_type_get_result_types(target_block_type, &types);

    if (cur_block->is_stack_polymorphic) {
        for (i = (int32)arity - 1; i >= 0; i--) {
            POP_TYPE(types[i]);
        }
        for (i = 0; i < (int32)arity; i++) {
            PUSH_TYPE(types[i]);
        }
        return true;
    }

    available_stack_cell =
        (int32)(loader_ctx->stack_cell_num - cur_block->stack_cell_num);

    /* Check stack top values match target block type */
    for (i = (int32)arity - 1; i >= 0; i--) {
        if (!check_stack_pop(frame_ref, available_stack_cell, types[i],
                                    error_buf, error_buf_size))
            return false;
        cell_num = wasm_value_type_cell_num(types[i]);
        frame_ref -= cell_num;
        available_stack_cell -= cell_num;
    }

    return true;

fail:
    return false;
}

static bool
check_block_stack(WASMLoaderContext *loader_ctx, BranchBlock *block,
                  char *error_buf, uint32 error_buf_size)
{
    BlockType *block_type = &block->block_type;
    uint8 *return_types = NULL;
    uint32 return_count = 0;
    int32 available_stack_cell, return_cell_num, i;
    uint8 *frame_ref = NULL;

    available_stack_cell =
        (int32)(loader_ctx->stack_cell_num - block->stack_cell_num);

    return_count = block_type_get_result_types(block_type, &return_types);
    return_cell_num =
        return_count > 0 ? wasm_get_cell_num(return_types, return_count) : 0;

    if (block->is_stack_polymorphic) {
        for (i = (int32)return_count - 1; i >= 0; i--) {
            POP_TYPE(return_types[i]);
        }

        /* Check stack is empty */
        if (loader_ctx->stack_cell_num != block->stack_cell_num) {
            set_error_buf(
                error_buf, error_buf_size,
                "type mismatch: stack size does not match block type");
            goto fail;
        }

        for (i = 0; i < (int32)return_count; i++) {
            PUSH_TYPE(return_types[i]);
        }
        return true;
    }

    /* Check stack cell num equals return cell num */
    if (available_stack_cell != return_cell_num) {
        set_error_buf(error_buf, error_buf_size,
                      "type mismatch: stack size does not match block type");
        goto fail;
    }

    /* Check stack values match return types */
    frame_ref = loader_ctx->frame_ref;
    for (i = (int32)return_count - 1; i >= 0; i--) {
        if (!check_stack_pop(frame_ref, available_stack_cell,
                                    return_types[i], error_buf, error_buf_size))
            return false;
        frame_ref -= wasm_value_type_cell_num(return_types[i]);
        available_stack_cell -= wasm_value_type_cell_num(return_types[i]);
    }

    return true;

fail:
    return false;
}

/* reset the stack to the state of before entering the last block */
#define RESET_STACK()                                                  \
    do {                                                               \
        loader_ctx->stack_cell_num =                                   \
            (loader_ctx->frame_csp - 1)->stack_cell_num;               \
        loader_ctx->frame_ref =                                        \
            loader_ctx->frame_ref_bottom + loader_ctx->stack_cell_num; \
    } while (0)

/* set current block's stack polymorphic state */
#define SET_CUR_BLOCK_STACK_POLYMORPHIC_STATE(flag)          \
    do {                                                     \
        BranchBlock *_cur_block = loader_ctx->frame_csp - 1; \
        _cur_block->is_stack_polymorphic = flag;             \
    } while (0)

#define BLOCK_HAS_PARAM(block_type) \
    (!block_type.is_value_type && block_type.u.type->param_count > 0)

static bool
check_table_index(const WASMModule *module, uint32 table_index, char *error_buf,
                  uint32 error_buf_size)
{
#if WASM_ENABLE_REF_TYPES == 0
    if (table_index != 0) {
        set_error_buf(error_buf, error_buf_size, "zero byte expected");
        return false;
    }
#endif

    if (table_index >= module->import_table_count + module->table_count) {
        set_error_buf_v(error_buf, error_buf_size, "unknown table %d",
                        table_index);
        return false;
    }
    return true;
}

static bool
check_function_index(const WASMModule *module, uint32 function_index,
                     char *error_buf, uint32 error_buf_size)
{
    if (function_index
        >= module->import_function_count + module->function_count) {
        set_error_buf_v(error_buf, error_buf_size, "unknown function %d",
                        function_index);
        return false;
    }
    return true;
}

static void
wasm_loader_ctx_destroy(WASMLoaderContext *ctx)
{
    // if (ctx) {
    //     if (ctx->frame_ref_bottom) {
    //         wasm_runtime_free(ctx->frame_ref_bottom);
    //     }
    //     if (ctx->frame_csp_bottom) {
    //         wasm_runtime_free(ctx->frame_csp_bottom);
    //     }
    //     wasm_runtime_free(ctx);
    // }
}

static WASMLoaderContext *
wasm_loader_ctx_init(WASMFunction *func, char *error_buf, uint32 error_buf_size)
{
    WASMLoaderContext *loader_ctx =
        wasm_runtime_malloc(sizeof(WASMLoaderContext));
    if (!loader_ctx)
        return NULL;
    
    //初始化数值栈
    loader_ctx->stack_cell_num = 0;
    loader_ctx->max_stack_cell_num = 0;
    loader_ctx->frame_ref_size = 32;
    if (!(loader_ctx->frame_ref_bottom = loader_ctx->frame_ref = wasm_runtime_malloc(
              loader_ctx->frame_ref_size)))
        goto fail;
    loader_ctx->frame_ref_boundary = loader_ctx->frame_ref_bottom + 32;

    //初始化控制栈
    loader_ctx->csp_num = 0;
    loader_ctx->max_csp_num = 0;
    loader_ctx->frame_csp_size = 8;
    if (!(loader_ctx->frame_csp_bottom = loader_ctx->frame_csp = wasm_runtime_malloc(
              8 * sizeof(BranchBlock))))
        goto fail;
    loader_ctx->frame_csp_boundary = loader_ctx->frame_csp_bottom + 8;

    //初始化控制表
    loader_ctx->branch_table_num = 0;
    loader_ctx->branch_table_size = 8;
    if(!(loader_ctx->branch_table_bottom = loader_ctx->branch_table = wasm_runtime_malloc(
    loader_ctx->branch_table_size)))
        goto fail;
    loader_ctx->branch_table_boundary = loader_ctx->branch_table_bottom + 8;

    return loader_ctx;

fail:
    wasm_loader_ctx_destroy(loader_ctx);
    return NULL;
}

static bool
wasm_loader_push_pop_frame_ref(WASMLoaderContext *ctx, uint8 pop_cnt,
                               uint8 type_push, uint8 type_pop, char *error_buf,
                               uint32 error_buf_size)
{
    for (int i = 0; i < pop_cnt; i++) {
        if (!wasm_loader_pop_frame_ref(ctx, type_pop, error_buf,
                                       error_buf_size))
            return false;
    }
    if (!wasm_loader_push_frame_ref(ctx, type_push, error_buf, error_buf_size))
        return false;
    return true;
}

bool
wasm_validator_code(WASMModule *module, WASMFunction *func,
                             uint32 cur_func_idx, char *error_buf,
                             uint32 error_buf_size)
{
    uint8 *p = (uint8 *)func->func_ptr, *p_end = func->code_end, *p_org;
    uint32 param_count, local_count, global_count;
    uint8 *param_types, *local_types, local_type, global_type;
    BlockType func_block_type;
    uint16 *local_offsets, local_offset;
    uint32 type_idx, func_idx, local_idx, global_idx, table_idx;
    uint32 table_seg_idx, data_seg_idx, count, align, mem_offset, i;
    int32 i32_const = 0;
    int64 i64_const;
    uint8 opcode;
    WASMLoaderContext *loader_ctx;
    BranchBlock *frame_csp_tmp;

    global_count = module->import_global_count + module->global_count;


    param_count = func->param_count;
    param_types = func->func_type->param;

    func_block_type.is_value_type = false;
    func_block_type.u.type = func->func_type;

    local_count = func->local_count;
    local_types = func->local_types;
    local_offsets = func->local_offsets;

    if (!(loader_ctx = wasm_loader_ctx_init(func, error_buf, error_buf_size))) {
        goto fail;
    }

    PUSH_CSP(LABEL_TYPE_FUNCTION, func_block_type, p);

    while (p < p_end) {
        opcode = *p++;

        switch (opcode) {
            case WASM_OP_UNREACHABLE:
                RESET_STACK();
                SET_CUR_BLOCK_STACK_POLYMORPHIC_STATE(true);
                break;

            case WASM_OP_NOP:
                break;

            case WASM_OP_IF:
                POP_I32();
                
                goto handle_op_block_and_loop;
            case WASM_OP_BLOCK:
            case WASM_OP_LOOP:
            handle_op_block_and_loop:
            {
                uint8 value_type;
                BlockType block_type;

                value_type = read_uint8(p);
                if (is_byte_a_type(value_type)) {
                    /* If the first byte is one of these special values:
                     * 0x40/0x7F/0x7E/0x7D/0x7C, take it as the type of
                     * the single return value. */
                    block_type.is_value_type = true;
                    block_type.u.value_type = value_type;
                }
                else {
                    uint32 type_index;
                    /* Resolve the leb128 encoded type index as block type */
                    p--;
                    read_leb_uint32(p, p_end, type_index);
                    if (type_index >= module->type_count) {
                        set_error_buf(error_buf, error_buf_size,
                                      "unknown type");
                        goto fail;
                    }
                    block_type.is_value_type = false;
                    block_type.u.type = module->types[type_index];
                }

                /* Pop block parameters from stack */
                if (BLOCK_HAS_PARAM(block_type)) {
                    WASMType *wasm_type = block_type.u.type;
                    for (i = 0; i < wasm_type->param_count; i++)
                        POP_TYPE(
                            wasm_type->param[wasm_type->param_count - i - 1]);
                }

                PUSH_CSP(LABEL_TYPE_BLOCK + (opcode - WASM_OP_BLOCK),
                         block_type, p);

                /* Pass parameters to block */
                if (BLOCK_HAS_PARAM(block_type)) {
                    WASMType *wasm_type = block_type.u.type;
                    for (i = 0; i < wasm_type->param_count; i++)
                        PUSH_TYPE(wasm_type->param[i]);
                }

                if (opcode == WASM_OP_IF && 
                !wasm_emit_branch_table(loader_ctx, WASM_OP_IF, 0, error_buf, error_buf_size))
                    goto fail;                

                break;
            }

            case WASM_OP_ELSE:
            {
                BlockType block_type = (loader_ctx->frame_csp - 1)->block_type;

                if (loader_ctx->csp_num < 2
                    || (loader_ctx->frame_csp - 1)->label_type
                           != LABEL_TYPE_IF) {
                    set_error_buf(
                        error_buf, error_buf_size,
                        "opcode else found without matched opcode if");
                    goto fail;
                }

                /* check whether if branch's stack matches its result type */
                if (!check_block_stack(loader_ctx, loader_ctx->frame_csp - 1,
                                       error_buf, error_buf_size))
                    goto fail;

                (loader_ctx->frame_csp - 1)->else_addr = p - 1;
                RESET_STACK();
                SET_CUR_BLOCK_STACK_POLYMORPHIC_STATE(false);

                /* Pass parameters to if-false branch */
                if (BLOCK_HAS_PARAM(block_type)) {
                    for (i = 0; i < block_type.u.type->param_count; i++)
                        PUSH_TYPE(block_type.u.type->param[i]);
                }

                if (!wasm_emit_branch_table(loader_ctx, WASM_OP_ELSE, 0, error_buf, error_buf_size))
                    goto fail;

                break;
            }

            case WASM_OP_END:
            {
                BranchBlock *cur_block = loader_ctx->frame_csp - 1;

                /* check whether block stack matches its result type */
                if (!check_block_stack(loader_ctx, cur_block, error_buf,
                                       error_buf_size))
                    goto fail;

                /* if no else branch, and return types do not match param types,
                 * fail */
                if (cur_block->label_type == LABEL_TYPE_IF
                    && !cur_block->else_addr) {
                    uint32 block_param_count = 0, block_ret_count = 0;
                    uint8 *block_param_types = NULL, *block_ret_types = NULL;
                    BlockType *cur_block_type = &cur_block->block_type;
                    if (cur_block_type->is_value_type) {
                        if (cur_block_type->u.value_type != VALUE_TYPE_VOID) {
                            block_ret_count = 1;
                            block_ret_types = &cur_block_type->u.value_type;
                        }
                    }
                    else {
                        block_param_count = cur_block_type->u.type->param_count;
                        block_ret_count = cur_block_type->u.type->result_count;
                        block_param_types = cur_block_type->u.type->param;
                        block_ret_types = cur_block_type->u.type->result;
                    }
                    if (block_param_count != block_ret_count
                        || (block_param_count
                            && memcmp(block_param_types, block_ret_types,
                                      block_param_count))) {
                        set_error_buf(error_buf, error_buf_size,
                                      "type mismatch: else branch missing");
                        goto fail;
                    }
                }

                cur_block->end_addr = p - 1;

                POP_CSP();

                if (loader_ctx->csp_num == 0 && p < p_end) {

                    set_error_buf(error_buf, error_buf_size,
                                      "section size mismatch");
                    goto fail;
                }

                break;
            }

            case WASM_OP_BR:
            {
                uint32 depth;
                read_leb_uint32(p, p_end, depth);
                if (!wasm_loader_check_br(loader_ctx, depth, error_buf, 
                                  error_buf_size))              
                    goto fail;  
                if (!wasm_emit_branch_table(loader_ctx, WASM_OP_BR, depth, error_buf, error_buf_size))
                    goto fail;
                RESET_STACK();
                SET_CUR_BLOCK_STACK_POLYMORPHIC_STATE(true);
                break;
            }

            case WASM_OP_BR_IF:
            {
                POP_I32();
                uint32 depth;
                read_leb_uint32(p, p_end, depth);
                if (!wasm_loader_check_br(loader_ctx, depth, error_buf, 
                                  error_buf_size))              
                    goto fail;  
                if (!wasm_emit_branch_table(loader_ctx, WASM_OP_BR_IF, depth, error_buf, error_buf_size))
                    goto fail;
                
                break;
            }

            case WASM_OP_BR_TABLE:
            {
                uint8 *ret_types = NULL;
                uint32 ret_count = 0;

                uint32 depth;

                read_leb_uint32(p, p_end, count);
                POP_I32();


                for (i = 0; i <= count; i++) {
                    read_leb_uint32(p, p_end, depth);
                    if (!wasm_loader_check_br(loader_ctx, depth, error_buf, 
                                    error_buf_size))              
                        goto fail;  
                    if (!wasm_emit_branch_table(loader_ctx, WASM_OP_BR_TABLE, depth, error_buf, error_buf_size))
                        goto fail;
                    
                    frame_csp_tmp = loader_ctx->frame_csp - depth - 1;

                    if (i == 0) {
                        if (frame_csp_tmp->label_type != LABEL_TYPE_LOOP)
                            ret_count = block_type_get_result_types(
                                &frame_csp_tmp->block_type, &ret_types);
                    }
                    else {
                        uint8 *tmp_ret_types = NULL;
                        uint32 tmp_ret_count = 0;

                        /* Check whether all table items have the same return
                         * type */
                        if (frame_csp_tmp->label_type != LABEL_TYPE_LOOP)
                            tmp_ret_count = block_type_get_result_types(
                                &frame_csp_tmp->block_type, &tmp_ret_types);

                        if (ret_count != tmp_ret_count
                            || (ret_count
                                && 0
                                       != memcmp(ret_types, tmp_ret_types,
                                                 ret_count))) {
                            set_error_buf(
                                error_buf, error_buf_size,
                                "type mismatch: br_table targets must "
                                "all use same result type");
                            goto fail;
                        }
                    }
                }


                RESET_STACK();
                SET_CUR_BLOCK_STACK_POLYMORPHIC_STATE(true);
                break;
            }

            case WASM_OP_RETURN:
            {
                int32 idx;
                uint8 ret_type;
                for (idx = (int32)func->result_count - 1; idx >= 0;
                     idx--) {
                    ret_type = func->result_types[idx];
                    POP_TYPE(ret_type);
                }

                RESET_STACK();
                SET_CUR_BLOCK_STACK_POLYMORPHIC_STATE(true);

                break;
            }

            case WASM_OP_CALL:
#if WASM_ENABLE_TAIL_CALL != 0
            case WASM_OP_RETURN_CALL:
#endif
            {
                WASMType *func_type;
                int32 idx;

                read_leb_uint32(p, p_end, func_idx);
                if (!check_function_index(module, func_idx, error_buf,
                                          error_buf_size)) {
                    goto fail;
                }

                func_type = module->functions[func_idx].func_type;

                if (func_type->param_count > 0) {
                    for (idx = (int32)(func_type->param_count - 1); idx >= 0;
                         idx--) {
                        POP_TYPE(func_type->param[idx]);
                    }
                }

#if WASM_ENABLE_TAIL_CALL != 0
                if (opcode == WASM_OP_CALL) {
#endif
                    for (i = 0; i < func_type->result_count; i++) {
                        PUSH_TYPE(func_type->result[i]);
                    }
#if WASM_ENABLE_TAIL_CALL != 0
                }
                else {
                    uint8 type;
                    if (func_type->result_count
                        != func->func_type->result_count) {
                        set_error_buf_v(error_buf, error_buf_size, "%s%u%s",
                                        "type mismatch: expect ",
                                        func->func_type->result_count,
                                        " return values but got other");
                        goto fail;
                    }
                    for (i = 0; i < func_type->result_count; i++) {
                        type = func->func_type
                                   ->types[func->func_type->param_count + i];
                        if (func_type->types[func_type->param_count + i]
                            != type) {
                            set_error_buf_v(error_buf, error_buf_size, "%s%s%s",
                                            "type mismatch: expect ",
                                            type2str(type), " but got other");
                            goto fail;
                        }
                    }
                    RESET_STACK();
                    SET_CUR_BLOCK_STACK_POLYMORPHIC_STATE(true);
                }
#endif
                break;
            }

            /*
             * if disable reference type: call_indirect typeidx, 0x00
             * if enable reference type:  call_indirect typeidx, tableidx
             */
            case WASM_OP_CALL_INDIRECT:
#if WASM_ENABLE_TAIL_CALL != 0
            case WASM_OP_RETURN_CALL_INDIRECT:
#endif
            {
                int32 idx;
                WASMType *func_type;

                read_leb_uint32(p, p_end, type_idx);
#if WASM_ENABLE_REF_TYPES != 0
                read_leb_uint32(p, p_end, table_idx);
#else
                table_idx = read_uint8(p);
#endif
                if (!check_table_index(module, table_idx, error_buf,
                                       error_buf_size)) {
                    goto fail;
                }

                /* skip elem idx */
                POP_I32();

                if (type_idx >= module->type_count) {
                    set_error_buf(error_buf, error_buf_size, "unknown type");
                    goto fail;
                }

                func_type = module->types[type_idx];

                if (func_type->param_count > 0) {
                    for (idx = (int32)(func_type->param_count - 1); idx >= 0;
                         idx--) {
                        POP_TYPE(func_type->param[idx]);
                    }
                }

#if WASM_ENABLE_TAIL_CALL != 0
                if (opcode == WASM_OP_CALL_INDIRECT) {
#endif
                    for (i = 0; i < func_type->result_count; i++) {
                        PUSH_TYPE(func_type->result[i]);
                    }
#if WASM_ENABLE_TAIL_CALL != 0
                }
                else {
                    uint8 type;
                    if (func_type->result_count
                        != func->func_type->result_count) {
                        set_error_buf_v(error_buf, error_buf_size, "%s%u%s",
                                        "type mismatch: expect ",
                                        func->func_type->result_count,
                                        " return values but got other");
                        goto fail;
                    }
                    for (i = 0; i < func_type->result_count; i++) {
                        type = func->func_type
                                   ->types[func->func_type->param_count + i];
                        if (func_type->types[func_type->param_count + i]
                            != type) {
                            set_error_buf_v(error_buf, error_buf_size, "%s%s%s",
                                            "type mismatch: expect ",
                                            type2str(type), " but got other");
                            goto fail;
                        }
                    }
                    RESET_STACK();
                    SET_CUR_BLOCK_STACK_POLYMORPHIC_STATE(true);
                }
#endif
                break;
            }

            case WASM_OP_DROP:
            {
                BranchBlock *cur_block = loader_ctx->frame_csp - 1;
                int32 available_stack_cell =
                    (int32)(loader_ctx->stack_cell_num
                            - cur_block->stack_cell_num);

                if (available_stack_cell <= 0
                    && !cur_block->is_stack_polymorphic) {
                    set_error_buf(error_buf, error_buf_size,
                                  "type mismatch, opcode drop was found "
                                  "but stack was empty");
                    goto fail;
                }

                if (available_stack_cell > 0) {
                    if (is_32bit_type(*(loader_ctx->frame_ref - 1))) {
                        loader_ctx->frame_ref--;
                        loader_ctx->stack_cell_num--;
                    }
                    else if (is_64bit_type(*(loader_ctx->frame_ref - 1))) {
                        loader_ctx->frame_ref -= 2;
                        loader_ctx->stack_cell_num -= 2;
                        *(p - 1) = WASM_OP_DROP_64;
                    }
#if WASM_ENABLE_SIMD != 0
#if (WASM_ENABLE_WAMR_COMPILER != 0) || (WASM_ENABLE_JIT != 0)
                    else if (*(loader_ctx->frame_ref - 1) == REF_V128_1) {
                        loader_ctx->frame_ref -= 4;
                        loader_ctx->stack_cell_num -= 4;
                    }
#endif
#endif
                    else {
                        set_error_buf(error_buf, error_buf_size,
                                      "type mismatch");
                        goto fail;
                    }
                }
                break;
            }

            case WASM_OP_SELECT:
            {
                uint8 ref_type;
                BranchBlock *cur_block = loader_ctx->frame_csp - 1;
                int32 available_stack_cell;

                POP_I32();

                available_stack_cell = (int32)(loader_ctx->stack_cell_num
                                               - cur_block->stack_cell_num);

                if (available_stack_cell <= 0
                    && !cur_block->is_stack_polymorphic) {
                    set_error_buf(error_buf, error_buf_size,
                                  "type mismatch or invalid result arity, "
                                  "opcode select was found "
                                  "but stack was empty");
                    goto fail;
                }

                if (available_stack_cell > 0) {
                    switch (*(loader_ctx->frame_ref - 1)) {
                        case REF_I32:
                        case REF_F32:
                            break;
                        case REF_I64_2:
                        case REF_F64_2:
                            *(p - 1) = WASM_OP_SELECT_64;
                            break;
#if WASM_ENABLE_SIMD != 0
#if (WASM_ENABLE_WAMR_COMPILER != 0) || (WASM_ENABLE_JIT != 0)
                        case REF_V128_4:
                            break;
#endif /* (WASM_ENABLE_WAMR_COMPILER != 0) || (WASM_ENABLE_JIT != 0) */
#endif /* WASM_ENABLE_SIMD != 0 */
                        default:
                        {
                            set_error_buf(error_buf, error_buf_size,
                                          "type mismatch");
                            goto fail;
                        }
                    }

                    ref_type = *(loader_ctx->frame_ref - 1);
                    POP2_AND_PUSH(ref_type, ref_type);
                }
                else {
                    PUSH_TYPE(VALUE_TYPE_ANY);
                }
                break;
            }

#if WASM_ENABLE_REF_TYPES != 0
            case WASM_OP_SELECT_T:
            {
                uint8 vec_len, ref_type;

                read_leb_uint32(p, p_end, vec_len);
                if (!vec_len) {
                    set_error_buf(error_buf, error_buf_size,
                                  "invalid result arity");
                    goto fail;
                }

                CHECK_BUF(p, p_end, 1);
                ref_type = read_uint8(p);
                if (!is_value_type(ref_type)) {
                    set_error_buf(error_buf, error_buf_size,
                                  "unknown value type");
                    goto fail;
                }

                POP_I32();

#if WASM_ENABLE_FAST_INTERP != 0
                if (loader_ctx->p_code_compiled) {
                    uint8 opcode_tmp = WASM_OP_SELECT;
                    uint8 *p_code_compiled_tmp =
                        loader_ctx->p_code_compiled - 2;

                    if (ref_type == VALUE_TYPE_V128) {
#if (WASM_ENABLE_SIMD == 0) \
    || ((WASM_ENABLE_WAMR_COMPILER == 0) && (WASM_ENABLE_JIT == 0))
                        set_error_buf(error_buf, error_buf_size,
                                      "SIMD v128 type isn't supported");
                        goto fail;
#endif
                    }
                    else {
                        if (ref_type == VALUE_TYPE_F64
                            || ref_type == VALUE_TYPE_I64)
                            opcode_tmp = WASM_OP_SELECT_64;
#if WASM_ENABLE_LABELS_AS_VALUES != 0
#if WASM_CPU_SUPPORTS_UNALIGNED_ADDR_ACCESS != 0
                        *(void **)(p_code_compiled_tmp - sizeof(void *)) =
                            handle_table[opcode_tmp];
#else
                        int32 offset = (int32)((uint8 *)handle_table[opcode_tmp]
                                               - (uint8 *)handle_table[0]);
                        if (!(offset >= INT16_MIN && offset < INT16_MAX)) {
                            set_error_buf(
                                error_buf, error_buf_size,
                                "pre-compiled label offset out of range");
                            goto fail;
                        }
                        *(int16 *)(p_code_compiled_tmp - sizeof(int16)) =
                            (int16)offset;
#endif /* end of WASM_CPU_SUPPORTS_UNALIGNED_ADDR_ACCESS */
#else  /* else of WASM_ENABLE_LABELS_AS_VALUES */
#if WASM_CPU_SUPPORTS_UNALIGNED_ADDR_ACCESS != 0
                        *(p_code_compiled_tmp - 1) = opcode_tmp;
#else
                        *(p_code_compiled_tmp - 2) = opcode_tmp;
#endif /* end of WASM_CPU_SUPPORTS_UNALIGNED_ADDR_ACCESS */
#endif /* end of WASM_ENABLE_LABELS_AS_VALUES */
                    }
                }
#endif /* WASM_ENABLE_FAST_INTERP != 0 */

#if WASM_ENABLE_FAST_INTERP != 0
                POP_OFFSET_TYPE(ref_type);
                POP_TYPE(ref_type);
                POP_OFFSET_TYPE(ref_type);
                POP_TYPE(ref_type);
                PUSH_OFFSET_TYPE(ref_type);
                PUSH_TYPE(ref_type);
#else
                POP2_AND_PUSH(ref_type, ref_type);
#endif /* WASM_ENABLE_FAST_INTERP != 0 */

                (void)vec_len;
                break;
            }

            /* table.get x. tables[x]. [i32] -> [t] */
            /* table.set x. tables[x]. [i32 t] -> [] */
            case WASM_OP_TABLE_GET:
            case WASM_OP_TABLE_SET:
            {
                uint8 decl_ref_type;

                read_leb_uint32(p, p_end, table_idx);
                if (!get_table_elem_type(module, table_idx, &decl_ref_type,
                                         error_buf, error_buf_size))
                    goto fail;

#if WASM_ENABLE_FAST_INTERP != 0
                emit_uint32(loader_ctx, table_idx);
#endif

                if (opcode == WASM_OP_TABLE_GET) {
                    POP_I32();
#if WASM_ENABLE_FAST_INTERP != 0
                    PUSH_OFFSET_TYPE(decl_ref_type);
#endif
                    PUSH_TYPE(decl_ref_type);
                }
                else {
#if WASM_ENABLE_FAST_INTERP != 0
                    POP_OFFSET_TYPE(decl_ref_type);
#endif
                    POP_TYPE(decl_ref_type);
                    POP_I32();
                }
                break;
            }
            case WASM_OP_REF_NULL:
            {
                uint8 ref_type;

                CHECK_BUF(p, p_end, 1);
                ref_type = read_uint8(p);
                if (ref_type != VALUE_TYPE_FUNCREF
                    && ref_type != VALUE_TYPE_EXTERNREF) {
                    set_error_buf(error_buf, error_buf_size,
                                  "unknown value type");
                    goto fail;
                }
#if WASM_ENABLE_FAST_INTERP != 0
                PUSH_OFFSET_TYPE(ref_type);
#endif
                PUSH_TYPE(ref_type);
                break;
            }
            case WASM_OP_REF_IS_NULL:
            {

                if (!wasm_loader_pop_frame_ref(loader_ctx, VALUE_TYPE_FUNCREF,
                                               error_buf, error_buf_size)
                    && !wasm_loader_pop_frame_ref(loader_ctx,
                                                  VALUE_TYPE_EXTERNREF,
                                                  error_buf, error_buf_size)) {
                    goto fail;
                }
                PUSH_I32();
                break;
            }
            case WASM_OP_REF_FUNC:
            {
                read_leb_uint32(p, p_end, func_idx);

                if (!check_function_index(module, func_idx, error_buf,
                                          error_buf_size)) {
                    goto fail;
                }

                if (func_idx == cur_func_idx + module->import_function_count) {
                    WASMTableSeg *table_seg = module->table_segments;
                    bool func_declared = false;
                    uint32 j;

                    /* Check whether current function is declared */
                    for (i = 0; i < module->table_seg_count; i++, table_seg++) {
                        if (table_seg->elem_type == VALUE_TYPE_FUNCREF
                            && wasm_elem_is_declarative(table_seg->mode)) {
                            for (j = 0; j < table_seg->function_count; j++) {
                                if (table_seg->func_indexes[j] == func_idx) {
                                    func_declared = true;
                                    break;
                                }
                            }
                        }
                    }
                    if (!func_declared) {
                        set_error_buf(error_buf, error_buf_size,
                                      "undeclared function reference");
                        goto fail;
                    }
                }

#if WASM_ENABLE_FAST_INTERP != 0
                emit_uint32(loader_ctx, func_idx);
#endif
                PUSH_FUNCREF();
                break;
            }
#endif /* WASM_ENABLE_REF_TYPES */

            case WASM_OP_GET_LOCAL:
            {
                p_org = p - 1;
                GET_LOCAL_INDEX_TYPE_AND_OFFSET();
                PUSH_TYPE(local_type);

                if (local_offset < 0x80) {
                    *p_org++ = EXT_OP_GET_LOCAL_FAST;
                    if (is_32bit_type(local_type)) {
                        *p_org++ = (uint8)local_offset;
                    }
                    else {
                        *p_org++ = (uint8)(local_offset | 0x80);
                    }
                    while (p_org < p) {
                        *p_org++ = WASM_OP_NOP;
                    }
                }
                break;
            }

            case WASM_OP_SET_LOCAL:
            {
                p_org = p - 1;
                GET_LOCAL_INDEX_TYPE_AND_OFFSET();
                POP_TYPE(local_type);

                if (local_offset < 0x80) {
                    *p_org++ = EXT_OP_SET_LOCAL_FAST;
                    if (is_32bit_type(local_type)) {
                        *p_org++ = (uint8)local_offset;
                    }
                    else {
                        *p_org++ = (uint8)(local_offset | 0x80);
                    }
                    while (p_org < p) {
                        *p_org++ = WASM_OP_NOP;
                    }
                }
                break;
            }

            case WASM_OP_TEE_LOCAL:
            {
                p_org = p - 1;
                GET_LOCAL_INDEX_TYPE_AND_OFFSET();
                POP_TYPE(local_type);
                PUSH_TYPE(local_type);

                if (local_offset < 0x80) {
                    *p_org++ = EXT_OP_TEE_LOCAL_FAST;
                    if (is_32bit_type(local_type)) {
                        *p_org++ = (uint8)local_offset;
                    }
                    else {
                        *p_org++ = (uint8)(local_offset | 0x80);
                    }
                    while (p_org < p) {
                        *p_org++ = WASM_OP_NOP;
                    }
                }
                break;
            }

            case WASM_OP_GET_GLOBAL:
            {
                p_org = p - 1;
                read_leb_uint32(p, p_end, global_idx);
                if (global_idx >= global_count) {
                    set_error_buf(error_buf, error_buf_size, "unknown global");
                    goto fail;
                }

                global_type = module->globals[global_idx].type;

                PUSH_TYPE(global_type);

                if (global_type == VALUE_TYPE_I64
                    || global_type == VALUE_TYPE_F64) {
                    *p_org = WASM_OP_GET_GLOBAL_64;
                }
                break;
            }

            case WASM_OP_SET_GLOBAL:
            {
                bool is_mutable = false;

                p_org = p - 1;
                read_leb_uint32(p, p_end, global_idx);
                if (global_idx >= global_count) {
                    set_error_buf(error_buf, error_buf_size, "unknown global");
                    goto fail;
                }

                is_mutable = module->globals[global_idx].is_mutable;
                if (!is_mutable) {
                    set_error_buf(error_buf, error_buf_size,
                                  "global is immutable");
                    goto fail;
                }

                global_type = module->globals[global_idx].type;

                POP_TYPE(global_type);


                if (global_type == VALUE_TYPE_I64
                    || global_type == VALUE_TYPE_F64) {
                    *p_org = WASM_OP_SET_GLOBAL_64;
                }
                break;
            }

            /* load */
            case WASM_OP_I32_LOAD:
            case WASM_OP_I32_LOAD8_S:
            case WASM_OP_I32_LOAD8_U:
            case WASM_OP_I32_LOAD16_S:
            case WASM_OP_I32_LOAD16_U:
            case WASM_OP_I64_LOAD:
            case WASM_OP_I64_LOAD8_S:
            case WASM_OP_I64_LOAD8_U:
            case WASM_OP_I64_LOAD16_S:
            case WASM_OP_I64_LOAD16_U:
            case WASM_OP_I64_LOAD32_S:
            case WASM_OP_I64_LOAD32_U:
            case WASM_OP_F32_LOAD:
            case WASM_OP_F64_LOAD:
            /* store */
            case WASM_OP_I32_STORE:
            case WASM_OP_I32_STORE8:
            case WASM_OP_I32_STORE16:
            case WASM_OP_I64_STORE:
            case WASM_OP_I64_STORE8:
            case WASM_OP_I64_STORE16:
            case WASM_OP_I64_STORE32:
            case WASM_OP_F32_STORE:
            case WASM_OP_F64_STORE:
            {
                CHECK_MEMORY();
                read_leb_uint32(p, p_end, align);      /* align */
                read_leb_uint32(p, p_end, mem_offset); /* offset */
                if (!check_memory_access_align(opcode, align, error_buf,
                                               error_buf_size)) {
                    goto fail;
                }
                switch (opcode) {
                    /* load */
                    case WASM_OP_I32_LOAD:
                    case WASM_OP_I32_LOAD8_S:
                    case WASM_OP_I32_LOAD8_U:
                    case WASM_OP_I32_LOAD16_S:
                    case WASM_OP_I32_LOAD16_U:
                        POP_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_I32);
                        break;
                    case WASM_OP_I64_LOAD:
                    case WASM_OP_I64_LOAD8_S:
                    case WASM_OP_I64_LOAD8_U:
                    case WASM_OP_I64_LOAD16_S:
                    case WASM_OP_I64_LOAD16_U:
                    case WASM_OP_I64_LOAD32_S:
                    case WASM_OP_I64_LOAD32_U:
                        POP_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_I64);
                        break;
                    case WASM_OP_F32_LOAD:
                        POP_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_F32);
                        break;
                    case WASM_OP_F64_LOAD:
                        POP_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_F64);
                        break;
                    /* store */
                    case WASM_OP_I32_STORE:
                    case WASM_OP_I32_STORE8:
                    case WASM_OP_I32_STORE16:
                        POP_I32();
                        POP_I32();
                        break;
                    case WASM_OP_I64_STORE:
                    case WASM_OP_I64_STORE8:
                    case WASM_OP_I64_STORE16:
                    case WASM_OP_I64_STORE32:
                        POP_I64();
                        POP_I32();
                        break;
                    case WASM_OP_F32_STORE:
                        POP_F32();
                        POP_I32();
                        break;
                    case WASM_OP_F64_STORE:
                        POP_F64();
                        POP_I32();
                        break;
                    default:
                        break;
                }
                break;
            }

            case WASM_OP_MEMORY_SIZE:
                CHECK_MEMORY();
                /* reserved byte 0x00 */
                if (*p++ != 0x00) {
                    set_error_buf(error_buf, error_buf_size,
                                  "zero byte expected");
                    goto fail;
                }
                PUSH_I32();

                module->possible_memory_grow = true;
                break;

            case WASM_OP_MEMORY_GROW:
                CHECK_MEMORY();
                /* reserved byte 0x00 */
                if (*p++ != 0x00) {
                    set_error_buf(error_buf, error_buf_size,
                                  "zero byte expected");
                    goto fail;
                }
                POP_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_I32);

                module->possible_memory_grow = true;
                break;

            case WASM_OP_I32_CONST:
                read_leb_int32(p, p_end, i32_const);
                (void)i32_const;

                PUSH_I32();
                break;

            case WASM_OP_I64_CONST:
                read_leb_int64(p, p_end, i64_const);

                PUSH_I64();
                break;

            case WASM_OP_F32_CONST:
                p += sizeof(float32);
                PUSH_F32();
                break;

            case WASM_OP_F64_CONST:
                p += sizeof(float64);
                PUSH_F64();
                break;

            case WASM_OP_I32_EQZ:
                POP_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_I32);
                break;

            case WASM_OP_I32_EQ:
            case WASM_OP_I32_NE:
            case WASM_OP_I32_LT_S:
            case WASM_OP_I32_LT_U:
            case WASM_OP_I32_GT_S:
            case WASM_OP_I32_GT_U:
            case WASM_OP_I32_LE_S:
            case WASM_OP_I32_LE_U:
            case WASM_OP_I32_GE_S:
            case WASM_OP_I32_GE_U:
                POP2_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_I32);
                break;

            case WASM_OP_I64_EQZ:
                POP_AND_PUSH(VALUE_TYPE_I64, VALUE_TYPE_I32);
                break;

            case WASM_OP_I64_EQ:
            case WASM_OP_I64_NE:
            case WASM_OP_I64_LT_S:
            case WASM_OP_I64_LT_U:
            case WASM_OP_I64_GT_S:
            case WASM_OP_I64_GT_U:
            case WASM_OP_I64_LE_S:
            case WASM_OP_I64_LE_U:
            case WASM_OP_I64_GE_S:
            case WASM_OP_I64_GE_U:
                POP2_AND_PUSH(VALUE_TYPE_I64, VALUE_TYPE_I32);
                break;

            case WASM_OP_F32_EQ:
            case WASM_OP_F32_NE:
            case WASM_OP_F32_LT:
            case WASM_OP_F32_GT:
            case WASM_OP_F32_LE:
            case WASM_OP_F32_GE:
                POP2_AND_PUSH(VALUE_TYPE_F32, VALUE_TYPE_I32);
                break;

            case WASM_OP_F64_EQ:
            case WASM_OP_F64_NE:
            case WASM_OP_F64_LT:
            case WASM_OP_F64_GT:
            case WASM_OP_F64_LE:
            case WASM_OP_F64_GE:
                POP2_AND_PUSH(VALUE_TYPE_F64, VALUE_TYPE_I32);
                break;

            case WASM_OP_I32_CLZ:
            case WASM_OP_I32_CTZ:
            case WASM_OP_I32_POPCNT:
                POP_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_I32);
                break;

            case WASM_OP_I32_ADD:
            case WASM_OP_I32_SUB:
            case WASM_OP_I32_MUL:
            case WASM_OP_I32_DIV_S:
            case WASM_OP_I32_DIV_U:
            case WASM_OP_I32_REM_S:
            case WASM_OP_I32_REM_U:
            case WASM_OP_I32_AND:
            case WASM_OP_I32_OR:
            case WASM_OP_I32_XOR:
            case WASM_OP_I32_SHL:
            case WASM_OP_I32_SHR_S:
            case WASM_OP_I32_SHR_U:
            case WASM_OP_I32_ROTL:
            case WASM_OP_I32_ROTR:
                POP2_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_I32);
                break;

            case WASM_OP_I64_CLZ:
            case WASM_OP_I64_CTZ:
            case WASM_OP_I64_POPCNT:
                POP_AND_PUSH(VALUE_TYPE_I64, VALUE_TYPE_I64);
                break;

            case WASM_OP_I64_ADD:
            case WASM_OP_I64_SUB:
            case WASM_OP_I64_MUL:
            case WASM_OP_I64_DIV_S:
            case WASM_OP_I64_DIV_U:
            case WASM_OP_I64_REM_S:
            case WASM_OP_I64_REM_U:
            case WASM_OP_I64_AND:
            case WASM_OP_I64_OR:
            case WASM_OP_I64_XOR:
            case WASM_OP_I64_SHL:
            case WASM_OP_I64_SHR_S:
            case WASM_OP_I64_SHR_U:
            case WASM_OP_I64_ROTL:
            case WASM_OP_I64_ROTR:
                POP2_AND_PUSH(VALUE_TYPE_I64, VALUE_TYPE_I64);
                break;

            case WASM_OP_F32_ABS:
            case WASM_OP_F32_NEG:
            case WASM_OP_F32_CEIL:
            case WASM_OP_F32_FLOOR:
            case WASM_OP_F32_TRUNC:
            case WASM_OP_F32_NEAREST:
            case WASM_OP_F32_SQRT:
                POP_AND_PUSH(VALUE_TYPE_F32, VALUE_TYPE_F32);
                break;

            case WASM_OP_F32_ADD:
            case WASM_OP_F32_SUB:
            case WASM_OP_F32_MUL:
            case WASM_OP_F32_DIV:
            case WASM_OP_F32_MIN:
            case WASM_OP_F32_MAX:
            case WASM_OP_F32_COPYSIGN:
                POP2_AND_PUSH(VALUE_TYPE_F32, VALUE_TYPE_F32);
                break;

            case WASM_OP_F64_ABS:
            case WASM_OP_F64_NEG:
            case WASM_OP_F64_CEIL:
            case WASM_OP_F64_FLOOR:
            case WASM_OP_F64_TRUNC:
            case WASM_OP_F64_NEAREST:
            case WASM_OP_F64_SQRT:
                POP_AND_PUSH(VALUE_TYPE_F64, VALUE_TYPE_F64);
                break;

            case WASM_OP_F64_ADD:
            case WASM_OP_F64_SUB:
            case WASM_OP_F64_MUL:
            case WASM_OP_F64_DIV:
            case WASM_OP_F64_MIN:
            case WASM_OP_F64_MAX:
            case WASM_OP_F64_COPYSIGN:
                POP2_AND_PUSH(VALUE_TYPE_F64, VALUE_TYPE_F64);
                break;

            case WASM_OP_I32_WRAP_I64:
                POP_AND_PUSH(VALUE_TYPE_I64, VALUE_TYPE_I32);
                break;

            case WASM_OP_I32_TRUNC_S_F32:
            case WASM_OP_I32_TRUNC_U_F32:
                POP_AND_PUSH(VALUE_TYPE_F32, VALUE_TYPE_I32);
                break;

            case WASM_OP_I32_TRUNC_S_F64:
            case WASM_OP_I32_TRUNC_U_F64:
                POP_AND_PUSH(VALUE_TYPE_F64, VALUE_TYPE_I32);
                break;

            case WASM_OP_I64_EXTEND_S_I32:
            case WASM_OP_I64_EXTEND_U_I32:
                POP_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_I64);
                break;

            case WASM_OP_I64_TRUNC_S_F32:
            case WASM_OP_I64_TRUNC_U_F32:
                POP_AND_PUSH(VALUE_TYPE_F32, VALUE_TYPE_I64);
                break;

            case WASM_OP_I64_TRUNC_S_F64:
            case WASM_OP_I64_TRUNC_U_F64:
                POP_AND_PUSH(VALUE_TYPE_F64, VALUE_TYPE_I64);
                break;

            case WASM_OP_F32_CONVERT_S_I32:
            case WASM_OP_F32_CONVERT_U_I32:
                POP_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_F32);
                break;

            case WASM_OP_F32_CONVERT_S_I64:
            case WASM_OP_F32_CONVERT_U_I64:
                POP_AND_PUSH(VALUE_TYPE_I64, VALUE_TYPE_F32);
                break;

            case WASM_OP_F32_DEMOTE_F64:
                POP_AND_PUSH(VALUE_TYPE_F64, VALUE_TYPE_F32);
                break;

            case WASM_OP_F64_CONVERT_S_I32:
            case WASM_OP_F64_CONVERT_U_I32:
                POP_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_F64);
                break;

            case WASM_OP_F64_CONVERT_S_I64:
            case WASM_OP_F64_CONVERT_U_I64:
                POP_AND_PUSH(VALUE_TYPE_I64, VALUE_TYPE_F64);
                break;

            case WASM_OP_F64_PROMOTE_F32:
                POP_AND_PUSH(VALUE_TYPE_F32, VALUE_TYPE_F64);
                break;

            case WASM_OP_I32_REINTERPRET_F32:
                POP_AND_PUSH(VALUE_TYPE_F32, VALUE_TYPE_I32);
                break;

            case WASM_OP_I64_REINTERPRET_F64:
                POP_AND_PUSH(VALUE_TYPE_F64, VALUE_TYPE_I64);
                break;

            case WASM_OP_F32_REINTERPRET_I32:
                POP_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_F32);
                break;

            case WASM_OP_F64_REINTERPRET_I64:
                POP_AND_PUSH(VALUE_TYPE_I64, VALUE_TYPE_F64);
                break;

            case WASM_OP_I32_EXTEND8_S:
            case WASM_OP_I32_EXTEND16_S:
                POP_AND_PUSH(VALUE_TYPE_I32, VALUE_TYPE_I32);
                break;

            case WASM_OP_I64_EXTEND8_S:
            case WASM_OP_I64_EXTEND16_S:
            case WASM_OP_I64_EXTEND32_S:
                POP_AND_PUSH(VALUE_TYPE_I64, VALUE_TYPE_I64);
                break;

            case WASM_OP_MISC_PREFIX:
            {
                uint32 opcode1;

                read_leb_uint32(p, p_end, opcode1);
#if WASM_ENABLE_FAST_INTERP != 0
                emit_byte(loader_ctx, ((uint8)opcode1));
#endif
                switch (opcode1) {
                    case WASM_OP_I32_TRUNC_SAT_S_F32:
                    case WASM_OP_I32_TRUNC_SAT_U_F32:
                        POP_AND_PUSH(VALUE_TYPE_F32, VALUE_TYPE_I32);
                        break;
                    case WASM_OP_I32_TRUNC_SAT_S_F64:
                    case WASM_OP_I32_TRUNC_SAT_U_F64:
                        POP_AND_PUSH(VALUE_TYPE_F64, VALUE_TYPE_I32);
                        break;
                    case WASM_OP_I64_TRUNC_SAT_S_F32:
                    case WASM_OP_I64_TRUNC_SAT_U_F32:
                        POP_AND_PUSH(VALUE_TYPE_F32, VALUE_TYPE_I64);
                        break;
                    case WASM_OP_I64_TRUNC_SAT_S_F64:
                    case WASM_OP_I64_TRUNC_SAT_U_F64:
                        POP_AND_PUSH(VALUE_TYPE_F64, VALUE_TYPE_I64);
                        break;
#if WASM_ENABLE_BULK_MEMORY != 0
                    case WASM_OP_MEMORY_INIT:
                    {
                        read_leb_uint32(p, p_end, data_seg_idx);
#if WASM_ENABLE_FAST_INTERP != 0
                        emit_uint32(loader_ctx, data_seg_idx);
#endif
                        if (module->import_memory_count == 0
                            && module->memory_count == 0)
                            goto fail_unknown_memory;

                        if (*p++ != 0x00)
                            goto fail_zero_byte_expected;

                        if (data_seg_idx >= module->data_seg_count) {
                            set_error_buf_v(error_buf, error_buf_size,
                                            "unknown data segment %d",
                                            data_seg_idx);
                            goto fail;
                        }

                        if (module->data_seg_count1 == 0)
                            goto fail_data_cnt_sec_require;

                        POP_I32();
                        POP_I32();
                        POP_I32();
#if WASM_ENABLE_JIT != 0 || WASM_ENABLE_WAMR_COMPILER != 0
                        func->has_memory_operations = true;
#endif
                        break;
                    }
                    case WASM_OP_DATA_DROP:
                    {
                        read_leb_uint32(p, p_end, data_seg_idx);
#if WASM_ENABLE_FAST_INTERP != 0
                        emit_uint32(loader_ctx, data_seg_idx);
#endif
                        if (data_seg_idx >= module->data_seg_count) {
                            set_error_buf(error_buf, error_buf_size,
                                          "unknown data segment");
                            goto fail;
                        }

                        if (module->data_seg_count1 == 0)
                            goto fail_data_cnt_sec_require;

#if WASM_ENABLE_JIT != 0 || WASM_ENABLE_WAMR_COMPILER != 0
                        func->has_memory_operations = true;
#endif
                        break;
                    }
                    case WASM_OP_MEMORY_COPY:
                    {
                        /* both src and dst memory index should be 0 */
                        if (*(int16 *)p != 0x0000)
                            goto fail_zero_byte_expected;
                        p += 2;

                        if (module->import_memory_count == 0
                            && module->memory_count == 0)
                            goto fail_unknown_memory;

                        POP_I32();
                        POP_I32();
                        POP_I32();
                        break;
                    }
                    case WASM_OP_MEMORY_FILL:
                    {
                        if (*p++ != 0x00) {
                            goto fail_zero_byte_expected;
                        }
                        if (module->import_memory_count == 0
                            && module->memory_count == 0) {
                            goto fail_unknown_memory;
                        }

                        POP_I32();
                        POP_I32();
                        POP_I32();
                        break;
                    }
                    fail_zero_byte_expected:
                        set_error_buf(error_buf, error_buf_size,
                                      "zero byte expected");
                        goto fail;

                    fail_unknown_memory:
                        set_error_buf(error_buf, error_buf_size,
                                      "unknown memory 0");
                        goto fail;
                    fail_data_cnt_sec_require:
                        set_error_buf(error_buf, error_buf_size,
                                      "data count section required");
                        goto fail;
#endif /* WASM_ENABLE_BULK_MEMORY */

                    default:
                        set_error_buf_v(error_buf, error_buf_size,
                                        "%s %02x %02x", "unsupported opcode",
                                        0xfc, opcode1);
                        goto fail;
                }
                break;
            }

            default:
                set_error_buf_v(error_buf, error_buf_size, "%s %02x",
                                "unsupported opcode", opcode);
                goto fail;
        }

    }

    if (loader_ctx->csp_num > 0) {
        if (cur_func_idx < module->function_count - 1)
            /* Function with missing end marker (between two functions) */
            set_error_buf(error_buf, error_buf_size, "END opcode expected");
        else
            /* Function with missing end marker
               (at EOF or end of code sections) */
            set_error_buf(error_buf, error_buf_size,
                          "unexpected end of section or function, "
                          "or section size mismatch");
        goto fail;
    }


    func->branch_table = loader_ctx->branch_table_bottom;
    func->max_stack_cell_num = loader_ctx->max_stack_cell_num;
    func->max_block_num = loader_ctx->max_csp_num;
    return true;

fail:
    wasm_loader_ctx_destroy(loader_ctx);

    (void)table_idx;
    (void)table_seg_idx;
    (void)data_seg_idx;
    (void)i64_const;
    (void)local_offset;
    (void)p_org;
    (void)mem_offset;
    (void)align;
    return false;
}

bool wasm_validator(WASMModule *module, char *error_buf,
                             uint32 error_buf_size)
{
    uint32 i;
    WASMTable *table;
    WASMFunction *func;
    WASMGlobal *global, *globals;

        //     if (!check_memory_max_size(init_page_count,
        //                            max_page_count, error_buf,
        //                            error_buf_size)) {
        //     return false;
        // }

    //检查table的正确性
    // if (!check_table_max_size(table->cur_size, table->max_size, error_buf,
    //                               error_buf_size))
    // return false;

            //     for (j = 0; j < i; j++) {
            //     name = module->exports[j].name;
            //     if (strlen(name) == str_len && memcmp(name, p, str_len) == 0) {
            //         set_error_buf(error_buf, error_buf_size,
            //                       "duplicate export name");
            //         return false;
            //     }
            // }

// switch (export->kind) {
//                 /* function index */
//                 case EXPORT_KIND_FUNC:
//                     if (index >= module->function_count
//                                      + module->import_function_count) {
//                         set_error_buf(error_buf, error_buf_size,
//                                       "unknown function");
//                         return false;
//                     }
//                     break;
//                 case EXPORT_KIND_TABLE:
//                     if (index
//                         >= module->table_count + module->import_table_count) {
//                         set_error_buf(error_buf, error_buf_size,
//                                       "unknown table");
//                         return false;
//                     }
//                     break;
//                 /* memory index */
//                 case EXPORT_KIND_MEMORY:
//                     if (index
//                         >= module->memory_count + module->import_memory_count) {
//                         set_error_buf(error_buf, error_buf_size,
//                                       "unknown memory");
//                         return false;
//                     }
//                     break;
//                 /* global index */
//                 case EXPORT_KIND_GLOBAL:
//                     if (index
//                         >= module->global_count + module->import_global_count) {
//                         set_error_buf(error_buf, error_buf_size,
//                                       "unknown global");
//                         return false;
//                     }
//                     break;
//                 default:
//                     set_error_buf(error_buf, error_buf_size,
//                                   "invalid export kind");
//                     return false;
//             }

//函数type验证
            // if (type_index >= module->type_count) {
            //     set_error_buf(error_buf, error_buf_size, "unknown type");
            //     return false;
            // }

    globals = module->globals;
    global = module->globals + module->import_global_count;
    for( i = 0; i < module->global_count; i++, global++) {
        if (global->type == VALUE_TYPE_GLOBAL) {
            global->type = globals[global->initial_value.global_index].type;
            memcpy(
                &(global->initial_value),
                &(globals[global->initial_value.global_index].initial_value),
                sizeof(WASMValue));
        }
    }

    func = module->functions + module->import_function_count;
    for ( i = 0; i < module->function_count; i++, func++) {
        if (!wasm_validator_code(module, func, i, error_buf,
                                          error_buf_size)) {
            goto fail;
        }
    }

    if (!module->possible_memory_grow) {
        WASMMemory *memory = module->memories;

        for(i = 0; i < module->memory_count; i++, memory++)
            if (memory->cur_page_count < DEFAULT_MAX_PAGES)
                memory->num_bytes_per_page *= memory->cur_page_count;
            else
                memory->num_bytes_per_page = UINT32_MAX;

            if (memory->cur_page_count > 0)
                memory->cur_page_count = memory->max_page_count = 1;
            else
                memory->cur_page_count = memory->max_page_count = 0;
    }

    LOG_VERBOSE("Validate success.\n");
    return true;
fail:
    LOG_VERBOSE("Validate fail.\n");
    wasm_module_destory(module,Validate);
    return false;
}