#初始化
set (INIT_DIR ${WASMVM_DIR}/init)
include_directories(${INIT_DIR}/include)

#通用
set (COMMON_DIR ${WASMVM_DIR}/common)
include_directories(${COMMON_DIR}/include)

#加载器
set (LOADER_DIR ${WASMVM_DIR}/loader)
include_directories(${LOADER_DIR}/include)

#wasm文件验证
set (VALIDATOR_DIR ${WASMVM_DIR}/validator)
include_directories(${VALIDATOR_DIR}/include)

#实例化
set (INSTANTIATE_DIR ${WASMVM_DIR}/instantiate)
include_directories(${INSTANTIATE_DIR}/include)

#wasm实例运行器
set (EXECUTOR_DIR ${WASMVM_DIR}/executor)
include_directories(${EXECUTOR_DIR}/include)

#WASI
set (WASI_DIR ${WASMVM_DIR}/wasi)
include(${WASI_DIR}/wasi.cmake)

#本地函数调用
set (NATIVE_DIR ${WASMVM_DIR}/native)
include_directories(${NATIVE_DIR})

#wasm虚拟机应用接口
set (APPLICATION_DIR ${WASMVM_DIR}/application)
include_directories(${APPLICATION_DIR}/include)

#异常
set (EXCEPTION_DIR ${WASMVM_DIR}/exception)
include_directories(${EXCEPTION_DIR}/include)

#解释器
set (INTERP_DIR ${WASMVM_DIR}/interpreter)
if (RUNTIME_BUILD_INTERP EQUAL 1)
    include (${INTERP_DIR}/interp.cmake)
endif ()

file (GLOB_RECURSE COMMON_SOURCE
    ${COMMON_DIR}/src/*.c
    ${LOADER_DIR}/src/*.c
    ${INSTANTIATE_DIR}/src/*.c
    ${VALIDATOR_DIR}/src/*.c
    ${APPLICATION_DIR}/src/*.c
    ${INIT_DIR}/src/*.c
    ${NATIVE_DIR}/*.c
    ${EXCEPTION_DIR}/src/*.c
    ${EXECUTOR_DIR}/src/*.c
)

set (WASMVM_SOURCE 
    ${INTERP_SOURCE}
    ${COMMON_SOURCE}
    ${WASI_SOURCE}
)