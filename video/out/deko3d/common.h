#pragma once

#include <deko3d.h>

typedef struct {
    DkDevice device;
    DkMemBlock cmdbuf_memblock;
    DkCmdBuf cmdbuf;
    DkQueue queue;

    // Temporary memblocks used for mapping buffers in the GPU address space
    // Freed when the frame is completed
    DkMemBlock *tmp_memblocks;
    size_t num_tmp_memblocks;
} mp_dk_ctx;
