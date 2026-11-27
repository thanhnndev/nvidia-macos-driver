/*
 * Wired memory and kernel mappings for the port, on IOKit memory descriptors. Compiled with Apple's headers only.
 *
 * Allocations are IOBufferMemoryDescriptors (wired, kernel-mapped); a physically contiguous one is requested with
 * kIOMemoryPhysicallyContiguous and a physical mask derived from the caller's address limit and alignment.
 * Mappings of physical memory (GPU BARs, vmap of scattered pages) are IOMemoryDescriptor mappings into the kernel
 * with the requested cache mode, which is how IOKit drivers get write-combined or uncached views.
 */
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/OSAtomic.h>
#include "nmx_xnu.h"

extern "C" {
extern void *kernel_pmap;
extern ppnum_t pmap_find_phys(void *pmap, addr64_t va);
}

static IOOptionBits cache_option(int cache)
{
    switch (cache) {
    case NMX_CACHE_UC: return kIOMapInhibitCache;
    case NMX_CACHE_WC: return kIOMapWriteCombineCache;
    case NMX_CACHE_WT: return kIOMapWriteThruCache;
    default: return kIOMapDefaultCache;
    }
}

extern "C" void *nmx_xnu_mem_alloc(size_t size, int contiguous, uint64_t max_phys, size_t align, void **handle)
{
    IOBufferMemoryDescriptor *md;
    *handle = nullptr;
    if (contiguous) {
        /* the mask's low bits force alignment, its high bits bound the physical address */
        mach_vm_address_t mask = (max_phys ? (mach_vm_address_t)max_phys : ~0ULL);
        if (align > PAGE_SIZE)
            mask &= ~((mach_vm_address_t)align - 1);
        else
            mask &= ~((mach_vm_address_t)PAGE_SIZE - 1);
        md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
                 kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryKernelUserShared, size, mask);
    } else {
        md = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared,
                                                         size, PAGE_SIZE);
    }
    if (md == nullptr)
        return nullptr;
    if (md->prepare() != kIOReturnSuccess) {
        md->release();
        return nullptr;
    }
    *handle = md;
    return md->getBytesNoCopy();
}

extern "C" void nmx_xnu_mem_free(void *handle)
{
    IOBufferMemoryDescriptor *md = (IOBufferMemoryDescriptor *)handle;
    if (md == nullptr)
        return;
    md->complete();
    md->release();
}

extern "C" uint64_t nmx_xnu_kva_to_phys(const void *va)
{
    ppnum_t pn = pmap_find_phys(kernel_pmap, (addr64_t)(uintptr_t)va);
    if (pn == 0)
        return 0;
    return ((uint64_t)pn << PAGE_SHIFT) | ((uintptr_t)va & PAGE_MASK);
}

extern "C" void *nmx_xnu_map_phys(uint64_t pa, size_t size, int cache, void **handle)
{
    *handle = nullptr;
    uint64_t base = pa & ~(uint64_t)PAGE_MASK;
    size_t len = (size_t)(((pa + size + PAGE_MASK) & ~(uint64_t)PAGE_MASK) - base);
    IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress((IOPhysicalAddress)base, len, kIODirectionInOut);
    if (md == nullptr)
        return nullptr;
    IOMemoryMap *map = md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | cache_option(cache));
    md->release();      /* the map keeps the descriptor */
    if (map == nullptr)
        return nullptr;
    *handle = map;
    return (void *)(uintptr_t)(map->getVirtualAddress() + (pa - base));
}

/*
 * ga403wm Step90: map write-combined / uncached memory one physically contiguous range at a time.
 *
 * A multi-range kIOMemoryTypePhysical64 descriptor is backed by a non-contiguous device pager, and xnu enters each of
 * its pages as a private vm_page: vm_page_insert_internal calls pmap_set_cache_attributes() on the PHYSICAL page with
 * the object's mode (rewriting the cache bits of every existing PTE of that page) and vm_page_remove() resets the page
 * to write-back when any such object dies. So the last mapping made or torn down decided the cache mode of every
 * mapping of the page: a vunmap or another process's munmap turned a live WC mapping write-back, and with no self-snoop
 * the GPU read stale memory (RADV descriptors read as 0). TTM order 0 (Step89) makes every multi-page BO multi-range.
 *
 * A single-range descriptor gets a contiguous device pager: no vm_page, no physical attribute change, and the fault
 * path block-maps it with the WC/UC mode in the PTE. So a WC/UC page list with more than one range is mapped as a
 * reservation of the whole size, then one single-range descriptor per range mapped over it with kIOMapOverwrite.
 * Cached page lists and single ranges keep one mapping. The reservation never inserts a vm_page of the buffer:
 *  - user task: a read-only map of one NMXGPU text page repeated (faulted lazily, never touched in practice);
 *  - kernel: IOKit prefaults kernel maps, and a repeated page would grow that page's pv list by one per page, so it
 *    is ONE contiguous read-only uncached range of the size starting at the first range (no vm_page either), and it
 *    is replaced by the overwrite maps before anything can use it.
 * A fixed user mapping has no reservation: the ranges go straight over the caller's range.
 * Releasing the reservation first deallocates the whole range (overwrite maps never deallocate), then the subs go.
 * The handle of a split mapping is (struct NmxSplitMap *) | 1.
 */
struct NmxSplitMap {
    size_t count;
    IOMemoryMap *reservation;
    IOMemoryMap *subs[];
};

static IOMemoryDescriptor *phys_ranges(IOAddressRange *ranges, size_t n)
{
    return IOMemoryDescriptor::withAddressRanges(ranges, (UInt32)n, kIODirectionInOut | kIOMemoryTypePhysical64, nullptr);
}

extern "C" void nmx_xnu_map_release(void *handle)
{
    if (handle == nullptr)
        return;
    if (((uintptr_t)handle & 1) == 0) {
        ((IOMemoryMap *)handle)->release();
        return;
    }
    NmxSplitMap *sm = (NmxSplitMap *)((uintptr_t)handle & ~(uintptr_t)1);
    if (sm->reservation)
        sm->reservation->release();
    for (size_t i = 0; i < sm->count; i++)
        if (sm->subs[i])
            sm->subs[i]->release();
    IOFree(sm, sizeof(NmxSplitMap) + sm->count * sizeof(IOMemoryMap *));
}

extern "C" {
vm_map_t get_task_map(task_t task);
kern_return_t vm_deallocate(vm_map_t target, vm_offset_t address, vm_size_t size);
}

/* opts = cache bits | kIOMapReadOnly. Returns the unmap handle (nullptr on failure) and the address in *addr. */
extern "C" void *nmx_xnu_map_ranges(task_t task, const uint64_t *pages, size_t npages, IOOptionBits opts, uint64_t fixed,
                                    uint64_t *addr)
{
    *addr = 0;
    IOAddressRange *ranges = (IOAddressRange *)IOMalloc(sizeof(IOAddressRange) * npages);
    if (ranges == nullptr)
        return nullptr;
    /* coalesce physically adjacent pages into one range each */
    size_t n = 0;
    for (size_t i = 0; i < npages; i++) {
        if (n > 0 && ranges[n - 1].address + ranges[n - 1].length == pages[i]) {
            ranges[n - 1].length += PAGE_SIZE;
        } else {
            ranges[n].address = pages[i];
            ranges[n].length = PAGE_SIZE;
            n++;
        }
    }
    void *handle = nullptr;
    if (n == 1 || (opts & kIOMapCacheMask) == kIOMapDefaultCache) {
        IOMemoryDescriptor *md = phys_ranges(ranges, n);
        IOFree(ranges, sizeof(IOAddressRange) * npages);
        if (md == nullptr)
            return nullptr;
        IOMemoryMap *map = md->createMappingInTask(task, fixed, opts | (fixed ? kIOMapOverwrite : kIOMapAnywhere));
        md->release();
        if (map == nullptr)
            return nullptr;
        *addr = map->getAddress();
        return map;
    }
    size_t smsize = sizeof(NmxSplitMap) + n * sizeof(IOMemoryMap *);
    NmxSplitMap *sm = (NmxSplitMap *)IOMalloc(smsize);
    if (sm == nullptr) {
        IOFree(ranges, sizeof(IOAddressRange) * npages);
        return nullptr;
    }
    bzero(sm, smsize);
    sm->count = n;
    handle = (void *)((uintptr_t)sm | 1);
    uint64_t base = fixed;
    if (!fixed) {
        IOMemoryDescriptor *rmd;
        IOOptionBits ropts = kIOMapAnywhere | kIOMapReadOnly;
        if (task == kernel_task) {
            IOAddressRange whole = { ranges[0].address, (mach_vm_size_t)npages * PAGE_SIZE };
            rmd = phys_ranges(&whole, 1);
            ropts |= kIOMapInhibitCache;
        } else {
            uint64_t anchor = nmx_xnu_kva_to_phys((const void *)&nmx_xnu_map_ranges) & ~(uint64_t)PAGE_MASK;
            IOAddressRange *rr = anchor ? (IOAddressRange *)IOMalloc(sizeof(IOAddressRange) * npages) : nullptr;
            rmd = nullptr;
            if (rr != nullptr) {
                for (size_t i = 0; i < npages; i++) {
                    rr[i].address = anchor;
                    rr[i].length = PAGE_SIZE;
                }
                rmd = phys_ranges(rr, npages);
                IOFree(rr, sizeof(IOAddressRange) * npages);
            }
        }
        if (rmd != nullptr) {
            sm->reservation = rmd->createMappingInTask(task, 0, ropts);
            rmd->release();
        }
        if (sm->reservation == nullptr)
            goto fail;
        base = sm->reservation->getAddress();
    }
    {
        uint64_t off = 0;
        for (size_t i = 0; i < n; i++) {
            IOMemoryDescriptor *md = phys_ranges(&ranges[i], 1);
            if (md == nullptr)
                goto fail;
            sm->subs[i] = md->createMappingInTask(task, base + off, opts | kIOMapOverwrite);
            md->release();
            if (sm->subs[i] == nullptr)
                goto fail;
            off += ranges[i].length;
        }
    }
    IOFree(ranges, sizeof(IOAddressRange) * npages);
    *addr = base;
    return handle;
fail:
    if (fixed)  /* drop what was already entered over the caller's range */
        vm_deallocate(get_task_map(task), (vm_offset_t)fixed, (vm_size_t)npages * PAGE_SIZE);
    nmx_xnu_map_release(handle);
    IOFree(ranges, sizeof(IOAddressRange) * npages);
    return nullptr;
}

/* ga403wm Step99 flip imports: physical address of a prepared descriptor's byte offset (and the segment length) */
extern "C" uint64_t nmx_xnu_md_phys(void *md, uint64_t offset, uint64_t *len)
{
    IOByteCount l = 0;
    addr64_t pa = ((IOMemoryDescriptor *)md)->getPhysicalSegment((IOByteCount)offset, &l, kIOMemoryMapperNone);
    if (len)
        *len = l;
    return pa;
}

extern "C" void *nmx_xnu_map_pages(const uint64_t *pages, size_t npages, int cache, void **handle)
{
    *handle = nullptr;
    if (npages == 0)
        return nullptr;
    uint64_t addr;
    void *h = nmx_xnu_map_ranges(kernel_task, pages, npages, cache_option(cache), 0, &addr);
    if (h == nullptr)
        return nullptr;
    *handle = h;
    return (void *)(uintptr_t)addr;
}

extern "C" void nmx_xnu_unmap(void *handle)
{
    nmx_xnu_map_release(handle);
}
