#!/usr/bin/env python3
# Pulls the descriptor snapshot path (bind table, address table, nvmtl_snapshot_set_md/_c, desc-cache) out of
# plugin/nvmtl_vk.c, unchanged, so tests/desccache_test.c can run it against a fake Vulkan descriptor store.
import sys, re
src = open(sys.argv[1]).read().split('\n')
def grab(start_pat, end_pat, inclusive=True):
    for i, l in enumerate(src):
        if re.match(start_pat, l):
            for j in range(i, len(src)):
                if re.match(end_pat, src[j]) and (j > i or end_pat == start_pat):
                    return '\n'.join(src[i:j + (1 if inclusive else 0)])
    sys.exit('not found: ' + start_pat)
out = []
out.append(grab(r'#define NVMTL_NBUF ', r'#define NVMTL_MAX_SETS'))
out.append(grab(r'enum \{ NVMTL_SLOT_NONE', r'.*\} nvmtl_bindtable;'))
out.append(grab(r'static const uint32_t g_dyn_zero', r'^\}'))
out.append(grab(r'static int nvmtl_bind_slot\(', r'^#define NVMTL_TABLE_BYTES'))
out.append('typedef struct nvmtl_address_page { nvk_buffer buffer; struct nvmtl_address_page *next; } nvmtl_address_page;')
out.append(grab(r'static int nvmtl_address_snapshot\(', r'^\}'))
out.append(grab(r'#define NVMTL_DESC_BATCH_MAX', r'^int nvmtl_vk_bind_buffer\(', inclusive=False))
out.append(grab(r'int nvmtl_vk_bind_buffer\(', r'^\}'))
out.append(grab(r'int nvmtl_vk_bind_texture\(', r'^\}'))
out.append(grab(r'int nvmtl_vk_bind_sampler\(', r'^\}'))
out.append(grab(r'void nvmtl_vk_cmd_bind_set\(', r'^\}'))
print('\n\n'.join(out))
