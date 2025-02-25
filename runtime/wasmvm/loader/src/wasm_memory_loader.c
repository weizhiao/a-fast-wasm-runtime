#include "wasm_loader.h"

bool load_memory_section(const uint8 *buf, const uint8 *buf_end, WASMModule *module)
{
    const uint8 *p = buf, *p_end = buf_end;
    uint8 flag;
    uint32 max_page_count;
    uint32 memory_count, i, cur_page_count;
    WASMMemory *memory;

    read_leb_uint32(p, p_end, memory_count);

    if (memory_count)
    {
        /* load each memory */
        memory = module->memories + module->import_memory_count;
        for (i = 0; i < memory_count; i++, memory++)
        {
            read_leb_uint1(p, p_end, flag);
            read_leb_uint32(p, p_end, cur_page_count);
            if (flag)
            {
                read_leb_uint32(p, p_end, max_page_count);
            }
            else
            {
                max_page_count = DEFAULT_MAX_PAGES;
            }
            // 赋值
            memory->cur_page_count = cur_page_count;
            memory->max_page_count = max_page_count;
            memory->num_bytes_per_page = DEFAULT_NUM_BYTES_PER_PAGE;
        }
    }

    if (p != p_end)
    {
        wasm_set_exception(module, "section size mismatch");
        return false;
    }

    LOG_VERBOSE("Load memory section success.\n");
    return true;
fail:
    LOG_VERBOSE("Load memory section fail.\n");
    return false;
}