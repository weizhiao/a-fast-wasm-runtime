#ifndef _WASM_JIT_LLVM_H
#define _WASM_JIT_LLVM_H

#include "wasm_jit.h"
#include "wasm_memory.h"
#include "wasm_type.h"
#include "llvm/Config/llvm-config.h"
#include "llvm-c/Types.h"
#include "llvm-c/Target.h"
#include "llvm-c/Core.h"
#include "llvm-c/Object.h"
#include "llvm-c/ExecutionEngine.h"
#include "llvm-c/Analysis.h"
#include "llvm-c/BitWriter.h"
#include "llvm-c/Transforms/Utils.h"
#include "llvm-c/Transforms/Scalar.h"
#include "llvm-c/Transforms/Vectorize.h"
#include "llvm-c/Transforms/PassManagerBuilder.h"

#include "llvm-c/Orc.h"
#include "llvm-c/Error.h"
#include "llvm-c/Support.h"
#include "llvm-c/Initialization.h"
#include "llvm-c/TargetMachine.h"
#include "llvm-c/LLJIT.h"

#include "wasm_jit_orc_extra.h"
#ifdef __cplusplus
extern "C"
{
#endif

#define OPQ_PTR_TYPE INT8_TYPE_PTR
    typedef struct JITValue
    {
        LLVMValueRef value;
    } JITValue;

    typedef struct JITValueStack
    {
        JITValue *value_list_head;
        JITValue *value_list_end;
    } JITValueStack;

    typedef struct JITBlock
    {

        uint32 label_type;

        // 是否在翻译else分支
        bool is_translate_else;
        bool is_polymorphic;

        uint8 *else_addr;
        uint8 *end_addr;

        LLVMBasicBlockRef llvm_entry_block;
        LLVMBasicBlockRef llvm_else_block;
        LLVMBasicBlockRef llvm_end_block;

        uint32 param_count;
        uint32 stack_num;
        uint8 *param_types;
        LLVMValueRef *param_phis;
        LLVMValueRef *else_param_phis;

        uint32 result_count;
        uint8 *result_types;
        LLVMValueRef *result_phis;
    } JITBlock;

    typedef struct JITMemInfo
    {
        LLVMValueRef mem_base_addr;
        LLVMValueRef mem_data_size_addr;
        LLVMValueRef mem_cur_page_count_addr;
    } JITMemInfo;

    typedef struct JITFuncContext
    {
        WASMFunction *wasm_func;
        LLVMValueRef func;
        LLVMTypeRef llvm_func_type;
        LLVMModuleRef module;

        JITBlock *block_stack;
        JITBlock *block_stack_bottom;
        JITValue *value_stack;
        JITValue *value_stack_bottom;

        LLVMValueRef exec_env;
        LLVMValueRef wasm_module;
        LLVMValueRef argv_buf;
        LLVMValueRef func_ptrs;

        JITMemInfo mem_info;
        LLVMValueRef global_base_addr;
        LLVMValueRef tables_base_addr;

        LLVMValueRef cur_exception;

        bool mem_space_unchanged;

        LLVMBasicBlockRef got_exception_block;
        LLVMBasicBlockRef func_return_block;
        LLVMValueRef exception_id_phi;
        LLVMValueRef func_type_indexes;
        LLVMValueRef locals[1];
    } JITFuncContext;

    typedef struct JITLLVMTypes
    {
        LLVMTypeRef int1_type;
        LLVMTypeRef int8_type;
        LLVMTypeRef int16_type;
        LLVMTypeRef int32_type;
        LLVMTypeRef int64_type;
        LLVMTypeRef float32_type;
        LLVMTypeRef float64_type;
        LLVMTypeRef void_type;

        LLVMTypeRef int8_ptr_type;
        LLVMTypeRef int8_pptr_type;
        LLVMTypeRef int16_ptr_type;
        LLVMTypeRef int32_ptr_type;
        LLVMTypeRef int64_ptr_type;
        LLVMTypeRef float32_ptr_type;
        LLVMTypeRef float64_ptr_type;

        LLVMTypeRef meta_data_type;
    } JITLLVMTypes;

    typedef struct JITLLVMConsts
    {
        LLVMValueRef i1_zero;
        LLVMValueRef i1_one;
        LLVMValueRef i8_zero;
        LLVMValueRef i32_zero;
        LLVMValueRef i64_zero;
        LLVMValueRef f32_zero;
        LLVMValueRef f64_zero;
        LLVMValueRef i32_one;
        LLVMValueRef i32_two;
        LLVMValueRef i32_three;
        LLVMValueRef i32_neg_one;
        LLVMValueRef i64_neg_one;
        LLVMValueRef i32_min;
        LLVMValueRef i64_min;
        LLVMValueRef i32_31;
        LLVMValueRef i32_32;
        LLVMValueRef i64_63;
        LLVMValueRef i64_64;
    } JITLLVMConsts;

    typedef struct JITFuncType
    {
        LLVMTypeRef *llvm_param_types;
        LLVMTypeRef *llvm_result_types;
        LLVMTypeRef llvm_func_type;
    } JITFuncType;

    typedef struct JITCompContext
    {

        LLVMContextRef context;
        LLVMBuilderRef builder;
        LLVMTargetMachineRef target_machine;
        char *target_cpu;
        char target_arch[16];

        LLVMOrcLLLazyJITRef orc_jit;
        LLVMOrcThreadSafeContextRef orc_thread_safe_context;

        LLVMModuleRef module;

        uint32 opt_level;
        uint32 size_level;

        LLVMValueRef fp_rounding_mode;

        LLVMValueRef fp_exception_behavior;

        JITLLVMTypes basic_types;
        LLVMTypeRef exec_env_type;
        LLVMTypeRef wasm_module_type;

        JITLLVMConsts llvm_consts;
        JITFuncContext **jit_func_ctxes;
        JITFuncType *jit_func_types;
        uint32 func_ctx_count;
    } JITCompContext;

    JITCompContext *
    wasm_jit_create_comp_context(WASMModule *wasm_module);

    void wasm_jit_destroy_comp_context(JITCompContext *comp_ctx);

    bool wasm_jit_compile_wasm(WASMModule *module);

    uint8 *
    wasm_jit_emit_elf_file(JITCompContext *comp_ctx, uint32 *p_elf_file_size);

    void wasm_jit_destroy_elf_file(uint8 *elf_file);

    void wasm_jit_block_destroy(JITBlock *block);

    LLVMTypeRef
    wasm_type_to_llvm_type(JITLLVMTypes *llvm_types, uint8 wasm_type);

    bool wasm_jit_build_zero_function_ret(JITCompContext *comp_ctx, JITFuncContext *func_ctx,
                                          WASMType *func_type);

    LLVMValueRef
    wasm_jit_call_llvm_intrinsic(const JITCompContext *comp_ctx,
                                 const JITFuncContext *func_ctx, const char *intrinsic,
                                 LLVMTypeRef ret_type, LLVMTypeRef *param_types,
                                 int param_count, ...);

    LLVMValueRef
    wasm_jit_call_llvm_intrinsic_v(const JITCompContext *comp_ctx,
                                   const JITFuncContext *func_ctx, const char *intrinsic,
                                   LLVMTypeRef ret_type, LLVMTypeRef *param_types,
                                   int param_count, va_list param_value_list);

    void wasm_jit_add_expand_memory_op_pass(LLVMPassManagerRef pass);

    void wasm_jit_add_simple_loop_unswitch_pass(LLVMPassManagerRef pass);

    void wasm_jit_apply_llvm_new_pass_manager(JITCompContext *comp_ctx, LLVMModuleRef module);

    void wasm_jit_handle_llvm_errmsg(const char *string, LLVMErrorRef err);

#ifdef __cplusplus
}
#endif

#endif
