#ifndef _wasm_jit_EMIT_CONTROL_H_
#define _wasm_jit_EMIT_CONTROL_H_

#include "wasm_jit_compiler.h"

bool wasm_compile_op_block(JITCompContext *comp_ctx, JITFuncContext *func_ctx,
                           WASMBlock *wasm_block, uint32 label_type,
                           uint32 param_count, uint8 *param_types,
                           uint32 result_count, uint8 *result_types);

bool wasm_jit_compile_op_else(JITCompContext *comp_ctx, JITFuncContext *func_ctx);

bool wasm_jit_compile_op_end(JITCompContext *comp_ctx, JITFuncContext *func_ctx);

bool wasm_jit_compile_op_br(JITCompContext *comp_ctx, JITFuncContext *func_ctx,
                            uint32 br_depth);

bool wasm_jit_compile_op_br_if(JITCompContext *comp_ctx, JITFuncContext *func_ctx,
                               uint32 br_depth);

bool wasm_jit_compile_op_br_table(JITCompContext *comp_ctx, JITFuncContext *func_ctx,
                                  uint32 *br_depths, uint32 br_count);

bool wasm_jit_compile_op_return(JITCompContext *comp_ctx, JITFuncContext *func_ctx);

bool wasm_jit_compile_op_unreachable(JITCompContext *comp_ctx, JITFuncContext *func_ctx);

#endif /* end of _wasm_jit_EMIT_CONTROL_H_ */
