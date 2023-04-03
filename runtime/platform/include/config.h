#ifndef _CONFIG_H
#define _CONFIG_H

#define EXCEPTION_BUF_LEN 128

#ifndef BLOCK_ADDR_CACHE_SIZE
#define BLOCK_ADDR_CACHE_SIZE 64
#endif

#define BLOCK_ADDR_CONFLICT_SIZE 2
#define DEFAULT_WASM_STACK_SIZE (16 * 1024)

#ifndef WASM_ENABLE_LIBC_WASI
#define WASM_ENABLE_LIBC_WASI 1
#endif

#ifndef WASM_ENABLE_INTERP
#define WASM_ENABLE_INTERP 1
#endif

#ifndef WASM_ENABLE_BULK_MEMORY
#define WASM_ENABLE_BULK_MEMORY 1
#endif

#endif