#include "wasm_jit_llvm.h"
#include "wasm_jit_compiler.h"
#include "wasm_jit_emit_exception.h"
#include "runtime_log.h"

LLVMTypeRef
wasm_type_to_llvm_type(JITLLVMTypes *llvm_types, uint8 wasm_type)
{
    switch (wasm_type)
    {
    case VALUE_TYPE_I32:
        return llvm_types->int32_type;
    case VALUE_TYPE_I64:
        return llvm_types->int64_type;
    case VALUE_TYPE_F32:
        return llvm_types->float32_type;
    case VALUE_TYPE_F64:
        return llvm_types->float64_type;
    case VALUE_TYPE_VOID:
        return llvm_types->void_type;
    default:
        break;
    }
    return NULL;
}

static LLVMValueRef
wasm_jit_add_llvm_func(JITCompContext *comp_ctx, LLVMModuleRef module,
                       uint32 type_index, uint32 func_index,
                       LLVMTypeRef *p_func_type)
{
    LLVMValueRef func = NULL;
    LLVMTypeRef llvm_func_type;
    LLVMValueRef local_value;
    LLVMTypeRef func_type_wrapper;
    LLVMValueRef func_wrapper;
    LLVMBasicBlockRef func_begin;
    char func_name[48];
    uint32 j = 0;
    uint32 backend_thread_num, compile_thread_num;

    llvm_func_type = comp_ctx->jit_func_types[type_index].llvm_func_type;

    snprintf(func_name, sizeof(func_name), "%s%d", WASM_JIT_FUNC_PREFIX, func_index);
    if (!(func = LLVMAddFunction(module, func_name, llvm_func_type)))
    {
        wasm_jit_set_last_error("add LLVM function failed.");
        goto fail;
    }

    j = 0;
    local_value = LLVMGetParam(func, j++);
    LLVMSetValueName(local_value, "exec_env");

    if (p_func_type)
        *p_func_type = llvm_func_type;

    backend_thread_num = WASM_ORC_JIT_BACKEND_THREAD_NUM;
    compile_thread_num = WASM_ORC_JIT_COMPILE_THREAD_NUM;

    if ((func_index % (backend_thread_num * compile_thread_num) < backend_thread_num))
    {
        func_type_wrapper = LLVMFunctionType(VOID_TYPE, NULL, 0, false);
        if (!func_type_wrapper)
        {
            wasm_jit_set_last_error("create LLVM function type failed.");
            goto fail;
        }

        snprintf(func_name, sizeof(func_name), "%s%d%s", WASM_JIT_FUNC_PREFIX,
                 func_index, "_wrapper");
        if (!(func_wrapper =
                  LLVMAddFunction(module, func_name, func_type_wrapper)))
        {
            wasm_jit_set_last_error("add LLVM function failed.");
            goto fail;
        }

        if (!(func_begin = LLVMAppendBasicBlockInContext(
                  comp_ctx->context, func_wrapper, "func_begin")))
        {
            wasm_jit_set_last_error("add LLVM basic block failed.");
            goto fail;
        }

        LLVMPositionBuilderAtEnd(comp_ctx->builder, func_begin);
        if (!LLVMBuildRetVoid(comp_ctx->builder))
        {
            wasm_jit_set_last_error("llvm build ret failed.");
            goto fail;
        }
    }

fail:
    return func;
}

static bool
wasm_jit_create_func_block(JITCompContext *comp_ctx, JITFuncContext *func_ctx, WASMType *func_type)
{
    JITBlock *block = func_ctx->block_stack;
    uint32 param_count = func_type->param_count,
           result_count = func_type->result_count;

    block->label_type = LABEL_TYPE_FUNCTION;
    block->param_count = param_count;
    block->param_types = func_type->param;
    block->result_count = result_count;
    block->result_types = func_type->result;
    block->else_param_phis = NULL;
    block->is_translate_else = false;
    block->param_phis = NULL;
    block->result_phis = NULL;
    block->llvm_end_block = NULL;
    block->llvm_else_block = NULL;
    block->is_polymorphic = false;

    if (!(block->llvm_entry_block = LLVMAppendBasicBlockInContext(
              comp_ctx->context, func_ctx->func, "func_begin")))
    {
        wasm_jit_set_last_error("add LLVM basic block failed.");
        return false;
    }

    func_ctx->block_stack++;
    return true;
}

static bool
create_argv_buf(JITCompContext *comp_ctx, JITFuncContext *func_ctx)
{
    LLVMValueRef argv_buf_offset = I32_THREE, argv_buf_addr;
    LLVMBuilderRef builder = comp_ctx->builder;

    LLVMBuildGEP(argv_buf_addr, OPQ_PTR_TYPE, func_ctx->exec_env,
                 argv_buf_offset, "argv_buf_addr");

    if (!(func_ctx->argv_buf = LLVMBuildBitCast(comp_ctx->builder, argv_buf_addr,
                                                I32_TYPE_PTR, "argv_buf_ptr")))
    {
        wasm_jit_set_last_error("llvm build load failed");
        return false;
    }

    return true;
}

static bool
create_local_variables(JITCompContext *comp_ctx,
                       JITFuncContext *func_ctx, WASMFunction *func)
{
    WASMType *func_type = func->func_type;
    char local_name[32];
    uint32 i, j = 1;

    for (i = 0; i < func_type->param_count; i++, j++)
    {
        snprintf(local_name, sizeof(local_name), "l%d", i);
        func_ctx->locals[i] =
            LLVMBuildAlloca(comp_ctx->builder,
                            TO_LLVM_TYPE(func_type->param[i]), local_name);
        if (!func_ctx->locals[i])
        {
            wasm_jit_set_last_error("llvm build alloca failed.");
            return false;
        }
        if (!LLVMBuildStore(comp_ctx->builder, LLVMGetParam(func_ctx->func, j),
                            func_ctx->locals[i]))
        {
            wasm_jit_set_last_error("llvm build store failed.");
            return false;
        }
    }

    for (i = 0; i < func->local_count; i++)
    {
        LLVMTypeRef local_type;
        LLVMValueRef local_value = NULL;
        snprintf(local_name, sizeof(local_name), "l%d",
                 func_type->param_count + i);
        local_type = TO_LLVM_TYPE(func->local_types[i]);
        func_ctx->locals[func_type->param_count + i] =
            LLVMBuildAlloca(comp_ctx->builder, local_type, local_name);
        if (!func_ctx->locals[func_type->param_count + i])
        {
            wasm_jit_set_last_error("llvm build alloca failed.");
            return false;
        }
        switch (func->local_types[i])
        {
        case VALUE_TYPE_I32:
            local_value = I32_ZERO;
            break;
        case VALUE_TYPE_I64:
            local_value = I64_ZERO;
            break;
        case VALUE_TYPE_F32:
            local_value = F32_ZERO;
            break;
        case VALUE_TYPE_F64:
            local_value = F64_ZERO;
            break;
        default:
            break;
        }
        if (!LLVMBuildStore(comp_ctx->builder, local_value,
                            func_ctx->locals[func_type->param_count + i]))
        {
            wasm_jit_set_last_error("llvm build store failed.");
            return false;
        }
    }

    return true;
}

static void
create_table_info(JITCompContext *comp_ctx, JITFuncContext *func_ctx)
{
    LLVMValueRef llvm_offset, tables_base_addr;
    LLVMBuilderRef builder = comp_ctx->builder;
    LLVMValueRef llvm_wasm_module = func_ctx->wasm_module;
    uint32 offset;

    offset = offsetof(WASMModule, tables);
    llvm_offset = I32_CONST(offset);

    LLVMBuildGEP(tables_base_addr, INT8_TYPE, llvm_wasm_module,
                 llvm_offset, "table_base_addr_offset");

    tables_base_addr = LLVMBuildBitCast(builder, tables_base_addr, INT8_PPTR_TYPE,
                                        "table_base_addr_ptr");

    tables_base_addr = LLVMBuildLoad2(builder, INT8_TYPE_PTR,
                                      tables_base_addr, "table_base_addr");

    func_ctx->tables_base_addr = tables_base_addr;
}

static void
create_global_info(JITCompContext *comp_ctx, JITFuncContext *func_ctx)
{
    LLVMValueRef llvm_offset, global_base_addr;
    LLVMBuilderRef builder = comp_ctx->builder;
    uint32 offset;
    // 获取全局变量信息
    offset = offsetof(WASMModule, global_data);

    llvm_offset = I32_CONST(offset);
    LLVMBuildGEP(global_base_addr, INT8_TYPE,
                 func_ctx->wasm_module, llvm_offset, "global_base_addr_offset");

    global_base_addr = LLVMBuildBitCast(comp_ctx->builder, global_base_addr, INT8_PPTR_TYPE,
                                        "global_base_addr_ptr");

    global_base_addr = LLVMBuildLoad2(comp_ctx->builder, INT8_TYPE_PTR,
                                      global_base_addr, "global_base_addr");

    func_ctx->global_base_addr = global_base_addr;
}

static void
create_memory_info(WASMModule *module, JITCompContext *comp_ctx, JITFuncContext *func_ctx)
{
    LLVMValueRef llvm_offset, memory_inst;
    LLVMBuilderRef builder = comp_ctx->builder;
    LLVMTypeRef int8_pptr_type = comp_ctx->basic_types.int8_pptr_type;
    bool mem_space_unchanged = !module->has_op_memory_grow;

    func_ctx->mem_space_unchanged = mem_space_unchanged;
    memory_inst = func_ctx->wasm_module;

    llvm_offset = I32_CONST(offsetof(WASMMemory, memory_data));
    LLVMBuildGEP(
        func_ctx->mem_info.mem_base_addr, INT8_TYPE, memory_inst, llvm_offset,
        "mem_base_addr_offset");

    llvm_offset = I32_CONST(offsetof(WASMMemory, cur_page_count));
    LLVMBuildGEP(func_ctx->mem_info.mem_cur_page_count_addr, INT8_TYPE,
                 memory_inst, llvm_offset, "mem_cur_page_offset");

    llvm_offset = I32_CONST(offsetof(WASMMemory, memory_data_size));
    LLVMBuildGEP(func_ctx->mem_info.mem_data_size_addr, INT8_TYPE,
                 memory_inst, llvm_offset, "mem_data_size_offset");

    func_ctx->mem_info.mem_base_addr = LLVMBuildBitCast(
        comp_ctx->builder, func_ctx->mem_info.mem_base_addr,
        int8_pptr_type, "mem_base_addr_ptr");
    func_ctx->mem_info.mem_cur_page_count_addr = LLVMBuildBitCast(
        comp_ctx->builder, func_ctx->mem_info.mem_cur_page_count_addr,
        I32_TYPE_PTR, "mem_cur_page_ptr");
    func_ctx->mem_info.mem_data_size_addr = LLVMBuildBitCast(
        comp_ctx->builder, func_ctx->mem_info.mem_data_size_addr,
        I32_TYPE_PTR, "mem_data_size_ptr");
    if (mem_space_unchanged)
    {
        func_ctx->mem_info.mem_base_addr = LLVMBuildLoad2(
            comp_ctx->builder, OPQ_PTR_TYPE,
            func_ctx->mem_info.mem_base_addr, "mem_base_addr");
        func_ctx->mem_info.mem_cur_page_count_addr =
            LLVMBuildLoad2(comp_ctx->builder, I32_TYPE,
                           func_ctx->mem_info.mem_cur_page_count_addr,
                           "mem_cur_page_count");
        func_ctx->mem_info.mem_data_size_addr = LLVMBuildLoad2(
            comp_ctx->builder, I32_TYPE,
            func_ctx->mem_info.mem_data_size_addr, "mem_data_size");
    }
}

static void
create_cur_exception(JITCompContext *comp_ctx, JITFuncContext *func_ctx)
{
    LLVMValueRef llvm_offset;
    LLVMBuilderRef builder = comp_ctx->builder;
    llvm_offset = I32_CONST(offsetof(WASMModule, cur_exception));

    LLVMBuildGEP(func_ctx->cur_exception, INT8_TYPE,
                 func_ctx->wasm_module, llvm_offset, "cur_exception");
}

static void
create_func_type_indexes(JITCompContext *comp_ctx, JITFuncContext *func_ctx)
{
    LLVMValueRef llvm_offset, func_type_indexes_ptr;
    LLVMBuilderRef builder = comp_ctx->builder;
    LLVMTypeRef int32_ptr_type;

    llvm_offset = I32_CONST(offsetof(WASMModule, func_type_indexes));

    LLVMBuildGEP(func_type_indexes_ptr, INT8_TYPE,
                 func_ctx->wasm_module, llvm_offset, "func_type_indexes_ptr");

    int32_ptr_type = LLVMPointerType(I32_TYPE_PTR, 0);

    func_ctx->func_type_indexes =
        LLVMBuildBitCast(comp_ctx->builder, func_type_indexes_ptr,
                         int32_ptr_type, "func_type_indexes_tmp");

    func_ctx->func_type_indexes =
        LLVMBuildLoad2(comp_ctx->builder, I32_TYPE_PTR,
                       func_ctx->func_type_indexes, "func_type_indexes");
}

static void
create_func_ptrs(JITCompContext *comp_ctx, JITFuncContext *func_ctx)
{
    LLVMValueRef llvm_offset;
    LLVMBuilderRef builder = comp_ctx->builder;

    llvm_offset = I32_CONST(offsetof(WASMModule, func_ptrs));
    LLVMBuildGEP(func_ctx->func_ptrs, INT8_TYPE,
                 func_ctx->wasm_module, llvm_offset, "func_ptrs_offset");
    func_ctx->func_ptrs =
        LLVMBuildBitCast(comp_ctx->builder, func_ctx->func_ptrs,
                         INT8_PPTR_TYPE, "func_ptrs_tmp");

    func_ctx->func_ptrs = LLVMBuildLoad2(comp_ctx->builder, OPQ_PTR_TYPE,
                                         func_ctx->func_ptrs, "func_ptrs_ptr");

    func_ctx->func_ptrs =
        LLVMBuildBitCast(comp_ctx->builder, func_ctx->func_ptrs,
                         INT8_PPTR_TYPE, "func_ptrs");
}

// 创建函数编译环境
static JITFuncContext *
wasm_jit_create_func_context(WASMModule *wasm_module, JITCompContext *comp_ctx,
                             WASMFunction *wasm_func, uint32 func_index)
{
    JITFuncContext *func_ctx;
    WASMType *func_type = wasm_func->func_type;
    uint32 type_index = wasm_func->type_index;
    LLVMBuilderRef builder = comp_ctx->builder;
    JITBlock *wasm_jit_block;
    LLVMValueRef wasm_module_offset = I32_TWO, wasm_module_addr;
    uint64 size;

    size = offsetof(JITFuncContext, locals) + sizeof(LLVMValueRef) * ((uint64)func_type->param_count + wasm_func->local_count);
    if (size >= UINT32_MAX || !(func_ctx = wasm_runtime_malloc(size)))
    {
        wasm_jit_set_last_error("allocate memory failed.");
        return NULL;
    }

    memset(func_ctx, 0, (uint32)size);
    func_ctx->wasm_func = wasm_func;
    func_ctx->module = comp_ctx->module;

    size = wasm_func->max_block_num * sizeof(JITBlock);

    if (!(func_ctx->block_stack_bottom = func_ctx->block_stack = wasm_runtime_malloc(size)))
    {
        wasm_jit_set_last_error("allocate memory failed.");
        return NULL;
    }

    size = wasm_func->max_stack_num * sizeof(JITValue);

    if (!(func_ctx->value_stack_bottom = func_ctx->value_stack = wasm_runtime_malloc(size)))
    {
        wasm_jit_set_last_error("allocate memory failed.");
        return NULL;
    }

    if (!(func_ctx->func =
              wasm_jit_add_llvm_func(comp_ctx, func_ctx->module, type_index,
                                     func_index, &func_ctx->llvm_func_type)))
    {
        goto fail;
    }

    if (!wasm_jit_create_func_block(comp_ctx, func_ctx, func_type))
    {
        goto fail;
    }

    wasm_jit_block = func_ctx->block_stack - 1;

    LLVMPositionBuilderAtEnd(comp_ctx->builder, wasm_jit_block->llvm_entry_block);

    func_ctx->exec_env = LLVMGetParam(func_ctx->func, 0);

    LLVMBuildGEP(wasm_module_addr, OPQ_PTR_TYPE, func_ctx->exec_env,
                 wasm_module_offset, "wasm_module_addr");

    if (!(func_ctx->wasm_module = LLVMBuildLoad2(comp_ctx->builder, OPQ_PTR_TYPE,
                                                 wasm_module_addr, "wasm_module")))
    {
        wasm_jit_set_last_error("llvm build load failed");
        goto fail;
    }

    if (!create_argv_buf(comp_ctx, func_ctx))
    {
        goto fail;
    }

    if (!create_local_variables(comp_ctx, func_ctx, wasm_func))
    {
        goto fail;
    }

    if (wasm_func->has_op_memory)
    {
        create_memory_info(wasm_module, comp_ctx, func_ctx);
    }

    create_global_info(comp_ctx, func_ctx);
    create_table_info(comp_ctx, func_ctx);
    create_cur_exception(comp_ctx, func_ctx);

    if (wasm_func->has_op_call_indirect)
    {
        create_func_type_indexes(comp_ctx, func_ctx);
    }

    create_func_ptrs(comp_ctx, func_ctx);

    return func_ctx;

fail:
    wasm_runtime_free(func_ctx);
    return NULL;
}

static void
wasm_jit_destroy_func_contexts(JITFuncContext **func_ctxes, uint32 count)
{
    uint32 i;

    for (i = 0; i < count; i++)
        if (func_ctxes[i])
        {
            wasm_runtime_free(func_ctxes[i]);
        }
    wasm_runtime_free(func_ctxes);
}

static JITFuncContext **
wasm_jit_create_func_contexts(WASMModule *wasm_module, JITCompContext *comp_ctx)
{
    JITFuncContext **func_ctxes;
    WASMFunction *func;
    uint32 define_function_count;
    uint64 size;
    uint32 i;

    define_function_count = comp_ctx->func_ctx_count;

    size = sizeof(JITFuncContext *) * (uint64)define_function_count;
    if (!(func_ctxes = wasm_runtime_malloc(size)))
    {
        wasm_jit_set_last_error("allocate memory failed.");
        return NULL;
    }

    memset(func_ctxes, 0, size);

    func = wasm_module->functions + wasm_module->import_function_count;

    for (i = 0; i < define_function_count; i++, func++)
    {
        if (!(func_ctxes[i] =
                  wasm_jit_create_func_context(wasm_module, comp_ctx, func, i)))
        {
            wasm_jit_destroy_func_contexts(func_ctxes, define_function_count);
            return NULL;
        }
    }

    return func_ctxes;
}

static bool
set_llvm_basic_types(JITLLVMTypes *basic_types, LLVMContextRef context)
{
    basic_types->int1_type = LLVMInt1TypeInContext(context);
    basic_types->int8_type = LLVMInt8TypeInContext(context);
    basic_types->int16_type = LLVMInt16TypeInContext(context);
    basic_types->int32_type = LLVMInt32TypeInContext(context);
    basic_types->int64_type = LLVMInt64TypeInContext(context);
    basic_types->float32_type = LLVMFloatTypeInContext(context);
    basic_types->float64_type = LLVMDoubleTypeInContext(context);
    basic_types->void_type = LLVMVoidTypeInContext(context);

    basic_types->meta_data_type = LLVMMetadataTypeInContext(context);

    basic_types->int8_ptr_type = LLVMPointerType(basic_types->int8_type, 0);

    basic_types->int8_pptr_type = LLVMPointerType(basic_types->int8_ptr_type, 0);

    basic_types->int16_ptr_type = LLVMPointerType(basic_types->int16_type, 0);
    basic_types->int32_ptr_type = LLVMPointerType(basic_types->int32_type, 0);
    basic_types->int64_ptr_type = LLVMPointerType(basic_types->int64_type, 0);
    basic_types->float32_ptr_type = LLVMPointerType(basic_types->float32_type, 0);
    basic_types->float64_ptr_type = LLVMPointerType(basic_types->float64_type, 0);

    return (basic_types->int8_ptr_type && basic_types->int8_pptr_type && basic_types->int16_ptr_type && basic_types->int32_ptr_type && basic_types->int64_ptr_type && basic_types->float32_ptr_type && basic_types->float64_ptr_type && basic_types->meta_data_type)
               ? true
               : false;
}

static bool
create_llvm_func_types(WASMModule *wasm_module, JITCompContext *comp_ctx)
{
    LLVMTypeRef *llvm_param_types, *llvm_result_types, llvm_ret_type, llvm_func_type;
    uint32 type_count = wasm_module->type_count;
    uint32 total_size = type_count * sizeof(JITFuncType);
    WASMType *wasm_type;
    WASMType **wasm_types = wasm_module->types;
    uint8 *wasm_param_types, *wasm_result_types;
    uint32 total_param_count;
    uint32 param_count, result_count;
    JITFuncType *func_type;
    uint32 i, j, k;

    if (!(comp_ctx->jit_func_types = wasm_runtime_malloc(total_size)))
    {
        return false;
    }

    func_type = comp_ctx->jit_func_types;

    for (i = 0; i < type_count; i++, func_type++)
    {
        wasm_type = wasm_types[i];
        param_count = wasm_type->param_count;
        result_count = wasm_type->result_count;
        wasm_param_types = wasm_type->param;
        wasm_result_types = wasm_type->result;

        total_param_count = 1 + param_count;

        if (result_count > 1)
            total_param_count += result_count - 1;

        total_size = sizeof(LLVMTypeRef) * (uint64)total_param_count;
        if (!(llvm_param_types = wasm_runtime_malloc((uint32)total_size)))
        {
            return false;
        }

        if (result_count)
        {
            total_size = sizeof(LLVMTypeRef) * result_count;
        }
        else
        {
            total_size = sizeof(LLVMTypeRef);
        }
        if (!(llvm_result_types = wasm_runtime_malloc((uint32)total_size)))
        {
            return false;
        }
        j = 0;
        llvm_param_types[j++] = INT8_PPTR_TYPE;
        for (k = 0; k < param_count; k++)
            llvm_param_types[j++] = TO_LLVM_TYPE(wasm_param_types[k]);

        if (result_count)
        {
            llvm_ret_type = TO_LLVM_TYPE(wasm_result_types[0]);
        }
        else
        {
            llvm_ret_type = VOID_TYPE;
        }

        llvm_result_types[0] = llvm_ret_type;
        for (k = 1; k < result_count; k++, j++)
        {
            llvm_param_types[j] = TO_LLVM_TYPE(wasm_result_types[k]);
            llvm_result_types[k] = llvm_param_types[j];
            if (!(llvm_param_types[j] = LLVMPointerType(llvm_param_types[j], 0)))
            {
                return false;
            }
        }

        llvm_func_type =
            LLVMFunctionType(llvm_ret_type, llvm_param_types, total_param_count, false);

        func_type->llvm_param_types = llvm_param_types;
        func_type->llvm_func_type = llvm_func_type;
        func_type->llvm_result_types = llvm_result_types;
    }
    return true;
}

static bool
create_llvm_consts(JITLLVMConsts *consts, JITCompContext *comp_ctx)
{
#define CREATE_I1_CONST(name, value)                                       \
    if (!(consts->i1_##name =                                              \
              LLVMConstInt(comp_ctx->basic_types.int1_type, value, true))) \
        return false;

    CREATE_I1_CONST(zero, 0)
    CREATE_I1_CONST(one, 1)
#undef CREATE_I1_CONST

    if (!(consts->i8_zero = I8_CONST(0)))
        return false;

    if (!(consts->f32_zero = F32_CONST(0)))
        return false;

    if (!(consts->f64_zero = F64_CONST(0)))
        return false;

#define CREATE_I32_CONST(name, value)                                \
    if (!(consts->i32_##name = LLVMConstInt(I32_TYPE, value, true))) \
        return false;

    CREATE_I32_CONST(min, (uint32)INT32_MIN)
    CREATE_I32_CONST(neg_one, (uint32)-1)
    CREATE_I32_CONST(zero, 0)
    CREATE_I32_CONST(one, 1)
    CREATE_I32_CONST(two, 2)
    CREATE_I32_CONST(three, 3)
    CREATE_I32_CONST(31, 31)
    CREATE_I32_CONST(32, 32)
#undef CREATE_I32_CONST

#define CREATE_I64_CONST(name, value)                                \
    if (!(consts->i64_##name = LLVMConstInt(I64_TYPE, value, true))) \
        return false;

    CREATE_I64_CONST(min, (uint64)INT64_MIN)
    CREATE_I64_CONST(neg_one, (uint64)-1)
    CREATE_I64_CONST(zero, 0)
    CREATE_I64_CONST(63, 63)
    CREATE_I64_CONST(64, 64)
#undef CREATE_I64_CONST

    return true;
}

static void
get_target_arch_from_triple(const char *triple, char *arch_buf, uint32 buf_size)
{
    uint32 i = 0;
    while (*triple != '-' && *triple != '\0' && i < buf_size - 1)
        arch_buf[i++] = *triple++;
}

void wasm_jit_handle_llvm_errmsg(const char *string, LLVMErrorRef err)
{
    char *err_msg = LLVMGetErrorMessage(err);
    wasm_jit_set_last_error_v("%s: %s", string, err_msg);
    LLVMDisposeErrorMessage(err_msg);
}

static bool
create_target_machine_detect_host(JITCompContext *comp_ctx)
{
    char *triple = NULL;
    LLVMTargetRef target = NULL;
    char *err_msg = NULL;
    char *cpu = NULL;
    char *features = NULL;
    LLVMTargetMachineRef target_machine = NULL;
    bool ret = false;

    triple = LLVMGetDefaultTargetTriple();
    if (triple == NULL)
    {
        wasm_jit_set_last_error("failed to get default target triple.");
        goto fail;
    }

    if (LLVMGetTargetFromTriple(triple, &target, &err_msg) != 0)
    {
        wasm_jit_set_last_error_v("failed to get llvm target from triple %s.",
                                  err_msg);
        LLVMDisposeMessage(err_msg);
        goto fail;
    }

    if (!LLVMTargetHasJIT(target))
    {
        wasm_jit_set_last_error("unspported JIT on this platform.");
        goto fail;
    }

    cpu = LLVMGetHostCPUName();
    if (cpu == NULL)
    {
        wasm_jit_set_last_error("failed to get host cpu information.");
        goto fail;
    }

    features = LLVMGetHostCPUFeatures();
    if (features == NULL)
    {
        wasm_jit_set_last_error("failed to get host cpu features.");
        goto fail;
    }

    LOG_VERBOSE("LLVM ORCJIT detected CPU \"%s\", with features \"%s\"\n", cpu,
                features);

    target_machine = LLVMCreateTargetMachine(
        target, triple, cpu, features, LLVMCodeGenLevelDefault,
        LLVMRelocDefault, LLVMCodeModelJITDefault);
    if (!target_machine)
    {
        wasm_jit_set_last_error("failed to create target machine.");
        goto fail;
    }
    comp_ctx->target_machine = target_machine;

    get_target_arch_from_triple(triple, comp_ctx->target_arch,
                                sizeof(comp_ctx->target_arch));
    ret = true;

fail:
    if (triple)
        LLVMDisposeMessage(triple);
    if (features)
        LLVMDisposeMessage(features);
    if (cpu)
        LLVMDisposeMessage(cpu);

    return ret;
}

static bool
orc_jit_create(JITCompContext *comp_ctx)
{
    LLVMErrorRef err;
    LLVMOrcLLLazyJITRef orc_jit = NULL;
    LLVMOrcLLLazyJITBuilderRef builder = NULL;
    LLVMOrcJITTargetMachineBuilderRef jtmb = NULL;
    bool ret = false;

    builder = LLVMOrcCreateLLLazyJITBuilder();
    if (builder == NULL)
    {
        wasm_jit_set_last_error("failed to create jit builder.");
        goto fail;
    }

    err = LLVMOrcJITTargetMachineBuilderDetectHost(&jtmb);
    if (err != LLVMErrorSuccess)
    {
        wasm_jit_handle_llvm_errmsg(
            "quited to create LLVMOrcJITTargetMachineBuilderRef", err);
        goto fail;
    }

    LLVMOrcLLLazyJITBuilderSetNumCompileThreads(
        builder, WASM_ORC_JIT_COMPILE_THREAD_NUM);

    LLVMOrcLLLazyJITBuilderSetJITTargetMachineBuilder(builder, jtmb);
    err = LLVMOrcCreateLLLazyJIT(&orc_jit, builder);
    if (err != LLVMErrorSuccess)
    {
        wasm_jit_handle_llvm_errmsg("quited to create llvm lazy orcjit instance",
                                    err);
        goto fail;
    }
    builder = NULL;

    comp_ctx->orc_jit = orc_jit;
    orc_jit = NULL;
    ret = true;

fail:
    if (builder)
        LLVMOrcDisposeLLLazyJITBuilder(builder);

    if (orc_jit)
        LLVMOrcDisposeLLLazyJIT(orc_jit);
    return ret;
}

JITCompContext *
wasm_jit_create_comp_context(WASMModule *wasm_module)
{
    JITCompContext *comp_ctx, *ret = NULL;
    char *fp_round = "round.tonearest",
         *fp_exce = "fpexcept.strict";
    uint32 i;
    LLVMTargetDataRef target_data_ref;

    if (!(comp_ctx = wasm_runtime_malloc(sizeof(JITCompContext))))
    {
        wasm_jit_set_last_error("allocate memory failed.");
        return NULL;
    }

    memset(comp_ctx, 0, sizeof(JITCompContext));

    LLVMInitializeNativeTarget();
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();

    comp_ctx->orc_thread_safe_context = LLVMOrcCreateNewThreadSafeContext();
    if (!comp_ctx->orc_thread_safe_context)
    {
        wasm_jit_set_last_error("create LLVM ThreadSafeContext failed.");
        goto fail;
    }

    if (!(comp_ctx->context = LLVMOrcThreadSafeContextGetContext(
              comp_ctx->orc_thread_safe_context)))
    {
        wasm_jit_set_last_error("get context from LLVM ThreadSafeContext failed.");
        goto fail;
    }

    if (!(comp_ctx->builder = LLVMCreateBuilderInContext(comp_ctx->context)))
    {
        wasm_jit_set_last_error("create LLVM builder failed.");
        goto fail;
    }

    if (!(comp_ctx->module = LLVMModuleCreateWithNameInContext(
              "WASM Module", comp_ctx->context)))
    {
        wasm_jit_set_last_error("create LLVM module failed.");
        goto fail;
    }

    comp_ctx->opt_level = 3;
    comp_ctx->size_level = 3;

    if (!create_target_machine_detect_host(comp_ctx))
        goto fail;

    if (!orc_jit_create(comp_ctx))
        goto fail;

    if (!(target_data_ref =
              LLVMCreateTargetDataLayout(comp_ctx->target_machine)))
    {
        wasm_jit_set_last_error("create LLVM target data layout failed.");
        goto fail;
    }
    LLVMDisposeTargetData(target_data_ref);

    if (!(comp_ctx->fp_rounding_mode = LLVMMDStringInContext(
              comp_ctx->context, fp_round, (uint32)strlen(fp_round))) ||
        !(comp_ctx->fp_exception_behavior = LLVMMDStringInContext(
              comp_ctx->context, fp_exce, (uint32)strlen(fp_exce))))
    {
        wasm_jit_set_last_error("create float llvm metadata failed.");
        goto fail;
    }

    if (!set_llvm_basic_types(&comp_ctx->basic_types, comp_ctx->context))
    {
        wasm_jit_set_last_error("create LLVM basic types failed.");
        goto fail;
    }

    if (!create_llvm_consts(&comp_ctx->llvm_consts, comp_ctx))
    {
        wasm_jit_set_last_error("create LLVM const values failed.");
        goto fail;
    }

    if (!create_llvm_func_types(wasm_module, comp_ctx))
    {
        goto fail;
    }

    comp_ctx->exec_env_type = comp_ctx->basic_types.int8_pptr_type;

    comp_ctx->wasm_module_type = INT8_TYPE_PTR;

    comp_ctx->func_ctx_count = wasm_module->function_count - wasm_module->import_function_count;
    if (comp_ctx->func_ctx_count > 0 && !(comp_ctx->jit_func_ctxes =
                                              wasm_jit_create_func_contexts(wasm_module, comp_ctx)))
        goto fail;

    ret = comp_ctx;

fail:

    if (!ret)
        wasm_jit_destroy_comp_context(comp_ctx);

    (void)i;
    return ret;
}

void wasm_jit_destroy_comp_context(JITCompContext *comp_ctx)
{
    if (!comp_ctx)
        return;

    if (comp_ctx->target_machine)
        LLVMDisposeTargetMachine(comp_ctx->target_machine);

    if (comp_ctx->builder)
        LLVMDisposeBuilder(comp_ctx->builder);

    if (comp_ctx->orc_thread_safe_context)
        LLVMOrcDisposeThreadSafeContext(comp_ctx->orc_thread_safe_context);

    if (comp_ctx->orc_jit)
        LLVMOrcDisposeLLLazyJIT(comp_ctx->orc_jit);

    if (comp_ctx->jit_func_ctxes)
        wasm_jit_destroy_func_contexts(comp_ctx->jit_func_ctxes,
                                       comp_ctx->func_ctx_count);

    if (comp_ctx->target_cpu)
    {
        wasm_runtime_free(comp_ctx->target_cpu);
    }

    wasm_runtime_free(comp_ctx);
}

void wasm_jit_block_destroy(JITBlock *block)
{
    if (block->param_phis)
        wasm_runtime_free(block->param_phis);
    if (block->else_param_phis)
        wasm_runtime_free(block->else_param_phis);
    if (block->result_phis)
        wasm_runtime_free(block->result_phis);
}

bool wasm_jit_build_zero_function_ret(JITCompContext *comp_ctx, JITFuncContext *func_ctx,
                                      WASMType *func_type)
{
    LLVMValueRef ret = NULL;

    if (func_type->result_count)
    {
        switch (func_type->result[0])
        {
        case VALUE_TYPE_I32:
            ret = LLVMBuildRet(comp_ctx->builder, I32_ZERO);
            break;
        case VALUE_TYPE_I64:
            ret = LLVMBuildRet(comp_ctx->builder, I64_ZERO);
            break;
        case VALUE_TYPE_F32:
            ret = LLVMBuildRet(comp_ctx->builder, F32_ZERO);
            break;
        case VALUE_TYPE_F64:
            ret = LLVMBuildRet(comp_ctx->builder, F64_ZERO);
            break;
        default:;
        }
    }
    else
    {
        ret = LLVMBuildRetVoid(comp_ctx->builder);
    }

    if (!ret)
    {
        wasm_jit_set_last_error("llvm build ret failed.");
        return false;
    }
    return true;
}

static LLVMValueRef
__call_llvm_intrinsic(const JITCompContext *comp_ctx,
                      const JITFuncContext *func_ctx, const char *name,
                      LLVMTypeRef ret_type, LLVMTypeRef *param_types,
                      int param_count, LLVMValueRef *param_values)
{
    LLVMValueRef func, ret;
    LLVMTypeRef func_type;

    if (!(func = LLVMGetNamedFunction(func_ctx->module, name)))
    {
        if (!(func_type = LLVMFunctionType(ret_type, param_types,
                                           (uint32)param_count, false)))
        {
            wasm_jit_set_last_error(
                "create LLVM intrinsic function type failed.");
            return NULL;
        }

        if (!(func = LLVMAddFunction(func_ctx->module, name, func_type)))
        {
            wasm_jit_set_last_error("add LLVM intrinsic function failed.");
            return NULL;
        }
    }

    func_type =
        LLVMFunctionType(ret_type, param_types, (uint32)param_count, false);

    if (!(ret = LLVMBuildCall2(comp_ctx->builder, func_type, func, param_values,
                               (uint32)param_count, "call")))
    {
        wasm_jit_set_last_error("llvm build intrinsic call failed.");
        return NULL;
    }

    return ret;
}

LLVMValueRef
wasm_jit_call_llvm_intrinsic(const JITCompContext *comp_ctx,
                             const JITFuncContext *func_ctx, const char *intrinsic,
                             LLVMTypeRef ret_type, LLVMTypeRef *param_types,
                             int param_count, ...)
{
    LLVMValueRef *param_values, ret;
    va_list argptr;
    uint64 total_size;
    int i = 0;

    total_size = sizeof(LLVMValueRef) * (uint64)param_count;
    if (!(param_values = wasm_runtime_malloc((uint32)total_size)))
    {
        wasm_jit_set_last_error("allocate memory for param values failed.");
        return false;
    }

    va_start(argptr, param_count);
    while (i < param_count)
        param_values[i++] = va_arg(argptr, LLVMValueRef);
    va_end(argptr);

    ret = __call_llvm_intrinsic(comp_ctx, func_ctx, intrinsic, ret_type,
                                param_types, param_count, param_values);

    wasm_runtime_free(param_values);

    return ret;
}

LLVMValueRef
wasm_jit_call_llvm_intrinsic_v(const JITCompContext *comp_ctx,
                               const JITFuncContext *func_ctx, const char *intrinsic,
                               LLVMTypeRef ret_type, LLVMTypeRef *param_types,
                               int param_count, va_list param_value_list)
{
    LLVMValueRef *param_values, ret;
    uint64 total_size;
    int i = 0;

    total_size = sizeof(LLVMValueRef) * (uint64)param_count;
    if (!(param_values = wasm_runtime_malloc((uint32)total_size)))
    {
        wasm_jit_set_last_error("allocate memory for param values failed.");
        return false;
    }

    while (i < param_count)
        param_values[i++] = va_arg(param_value_list, LLVMValueRef);

    ret = __call_llvm_intrinsic(comp_ctx, func_ctx, intrinsic, ret_type,
                                param_types, param_count, param_values);

    wasm_runtime_free(param_values);

    return ret;
}