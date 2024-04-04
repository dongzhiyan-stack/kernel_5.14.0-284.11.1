// SPDX-License-Identifier: GPL-2.0-only
/*
 *	linux/mm/filemap.c
 *
 * Copyright (C) 1994-1999  Linus Torvalds
 */

/*
 * This file handles the generic file mmap semantics used by
 * most "normal" filesystems (but you don't /have/ to use this:
 * the NFS filesystem used to do this differently, for example)
 */
#include <linux/export.h>
#include <linux/compiler.h>
#include <linux/dax.h>
#include <linux/fs.h>
#include <linux/sched/signal.h>
#include <linux/uaccess.h>
#include <linux/capability.h>
#include <linux/kernel_stat.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/mman.h>
#include <linux/pagemap.h>
#include <linux/file.h>
#include <linux/uio.h>
#include <linux/error-injection.h>
#include <linux/hash.h>
#include <linux/writeback.h>
#include <linux/backing-dev.h>
#include <linux/pagevec.h>
#include <linux/security.h>
#include <linux/cpuset.h>
#include <linux/hugetlb.h>
#include <linux/memcontrol.h>
#include <linux/shmem_fs.h>
#include <linux/rmap.h>
#include <linux/delayacct.h>
#include <linux/psi.h>
#include <linux/ramfs.h>
#include <linux/page_idle.h>
#include <linux/migrate.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>
#include "internal.h"

#define CREATE_TRACE_POINTS
#include <trace/events/filemap.h>

/*
 * FIXME: remove all knowledge of the buffer layer from the core VM
 */
#include <linux/buffer_head.h> /* for try_to_free_buffers */

#include <asm/mman.h>

#define ASYNC_MEMORY_RECLAIM_IN_KERNEL
#include "async_memory_reclaim_for_cold_file_area.h"
#define FILE_AREA_PAGE_COUNT_SHIFT (XA_CHUNK_SHIFT + PAGE_COUNT_IN_AREA_SHIFT)//6+2
#define FILE_AREA_PAGE_COUNT_MASK ((1 << FILE_AREA_PAGE_COUNT_SHIFT) - 1)//0x7F 

//file_area->file_area_state 的bit31~bit28 这个4个bit位标志file_area
//#define PAGE_BIT_OFFSET_IN_FILE_AREA_BASE (sizeof(&p_file_area->file_area_state)*8 - PAGE_COUNT_IN_AREA_SHIFT)//28  这个编译不通过!!!!!!!!!!!!!
#define PAGE_BIT_OFFSET_IN_FILE_AREA_BASE (sizeof(unsigned int)*8 - PAGE_COUNT_IN_AREA_SHIFT)//28


unsigned int xarray_tree_node_cache_hit;
inline struct file_area *entry_to_file_area(void * file_area_entry)
{
    return (struct file_area *)((unsigned long)file_area_entry | 0x8000000000000000);
}
inline void *file_area_to_entry(struct file_area *p_file_area)
{
    return (void *)((unsigned long)p_file_area & 0x7fffffffffffffff);
}
inline int is_file_area_entry(void *file_area_entry)
{
	//最高的4个bit位依次是 0、1、1、1 则说明是file_area_entry
    return ((unsigned long)file_area_entry & 0xF000000000000000) == 0x7000000000000000;
}

inline void clear_file_area_page_bit(struct file_area *p_file_area,unsigned char page_offset_in_file_area)
{
    unsigned int file_area_page_bit_clear = ~(1 << (PAGE_BIT_OFFSET_IN_FILE_AREA_BASE + page_offset_in_file_area));
	//如果这个page在 p_file_area->file_area_state对应的bit位已经置1了，触发panic
	if((p_file_area->file_area_state | file_area_page_bit_clear) != (sizeof(&p_file_area->file_area_state)*8 - 1))
		panic("%s file_area:0x%llx file_area_state:0x%x page_offset_in_file_area:%d already clear\n",__func__,(u64)p_file_area,p_file_area->file_area_state,page_offset_in_file_area);
    
	//page在 p_file_area->file_area_state对应的bit位置1
	p_file_area->file_area_state = p_file_area->file_area_state & file_area_page_bit_clear;

}
inline void set_file_area_page_bit(struct file_area *p_file_area,unsigned char page_offset_in_file_area)
{
    unsigned int file_area_page_bit_set = 1 << (PAGE_BIT_OFFSET_IN_FILE_AREA_BASE + page_offset_in_file_area);
	//如果这个page在 p_file_area->file_area_state对应的bit位已经置1了，触发panic
	if(p_file_area->file_area_state & file_area_page_bit_set)
		panic("%s file_area:0x%llx file_area_state:0x%x page_offset_in_file_area:%d already set\n",__func__,(u64)p_file_area,p_file_area->file_area_state,page_offset_in_file_area);
    
	//page在 p_file_area->file_area_state对应的bit位置1
	p_file_area->file_area_state = p_file_area->file_area_state | file_area_page_bit_set;
}
inline int file_area_have_page(struct file_area *p_file_area)
{
    return  (p_file_area->file_area_state & ~((1 << PAGE_BIT_OFFSET_IN_FILE_AREA_BASE) - 1));//0XF000 0000
}
int is_test_file(struct address_space *mapping){
    #define TEST_FILE_NAME "kern"
	struct dentry *dentry = NULL;
	struct inode *inode = NULL;
  
    if(mapping && mapping->host && !hlist_empty(&mapping->host->i_dentry)){
		inode = mapping->host;
        spin_lock(&inode->i_lock);
		if(atomic_read(&inode->i_count) > 0){
			dentry = hlist_entry(mapping->host->i_dentry.first, struct dentry, d_u.d_alias);
			if(dentry){
				if(strncmp(dentry->d_iname,TEST_FILE_NAME,4) == 0){
					spin_unlock(&inode->i_lock);
					return 1;
                }
			}
        }
		spin_unlock(&inode->i_lock);
	}

	return 0;
}

inline struct  file_area * find_file_area_from_xarray_cache_node(struct xa_state *xas,struct file_stat *p_file_stat, pgoff_t index)
{
    //这段代码必须放到rcu lock里，保证node结构不会被释放
	//判断要查找的page是否在xarray tree的cache node里
	if(p_file_stat->xa_node_cache){
	    //这个内存屏障为了确保delete page函数里，释放掉node后，对p_file_stat->xa_node_cache_base_index 赋值0，执行这个函数的进程在新的rcu周期立即感知到
	    smp_rmb();
	    //要插在的page索引在缓存的node里
	    if((index >= p_file_stat->xa_node_cache_base_index) && (index <= (p_file_stat->xa_node_cache_base_index + FILE_AREA_PAGE_COUNT_MASK))){
			unsigned int xa_offset = index & FILE_AREA_PAGE_COUNT_MASK;
			struct file_area *p_file_area;
			struct xa_node *xa_node_parent = (struct xa_node *)p_file_stat->xa_node_cache;

			p_file_area = xa_node_parent->slots[xa_offset];
			//保存到xarray tree的file_area指针由我的代码完全控制，故先不考虑xa_is_zero、xa_is_retry、xa_is_value 这些异常判断	
			//if(p_file_area && !xa_is_zero(p_file_area) && !xa_is_retry(p_file_area) && !xa_is_value(p_file_area) && p_file_area->start_index == index & PAGE_COUNT_IN_AREA){
			if(p_file_area && !xa_is_retry(p_file_area)){
				p_file_area = entry_to_file_area(p_file_area);
				if(p_file_area->start_index == (index & PAGE_COUNT_IN_AREA)){
				    xas->xa_offset = xa_offset;
				    xas->xa_node = xa_node_parent;
				    return p_file_area;
				}
			}
	    }
	}
    return NULL;
}
/*
 * Shared mappings implemented 30.11.1994. It's not fully working yet,
 * though.
 *
 * Shared mappings now work. 15.8.1995  Bruno.
 *
 * finished 'unifying' the page and buffer cache and SMP-threaded the
 * page-cache, 21.05.1999, Ingo Molnar <mingo@redhat.com>
 *
 * SMP-threaded pagemap-LRU 1999, Andrea Arcangeli <andrea@suse.de>
 */

/*
 * Lock ordering:
 *
 *  ->i_mmap_rwsem		(truncate_pagecache)
 *    ->private_lock		(__free_pte->block_dirty_folio)
 *      ->swap_lock		(exclusive_swap_page, others)
 *        ->i_pages lock
 *
 *  ->i_rwsem
 *    ->invalidate_lock		(acquired by fs in truncate path)
 *      ->i_mmap_rwsem		(truncate->unmap_mapping_range)
 *
 *  ->mmap_lock
 *    ->i_mmap_rwsem
 *      ->page_table_lock or pte_lock	(various, mainly in memory.c)
 *        ->i_pages lock	(arch-dependent flush_dcache_mmap_lock)
 *
 *  ->mmap_lock
 *    ->invalidate_lock		(filemap_fault)
 *      ->lock_page		(filemap_fault, access_process_vm)
 *
 *  ->i_rwsem			(generic_perform_write)
 *    ->mmap_lock		(fault_in_readable->do_page_fault)
 *
 *  bdi->wb.list_lock
 *    sb_lock			(fs/fs-writeback.c)
 *    ->i_pages lock		(__sync_single_inode)
 *
 *  ->i_mmap_rwsem
 *    ->anon_vma.lock		(vma_adjust)
 *
 *  ->anon_vma.lock
 *    ->page_table_lock or pte_lock	(anon_vma_prepare and various)
 *
 *  ->page_table_lock or pte_lock
 *    ->swap_lock		(try_to_unmap_one)
 *    ->private_lock		(try_to_unmap_one)
 *    ->i_pages lock		(try_to_unmap_one)
 *    ->lruvec->lru_lock	(follow_page->mark_page_accessed)
 *    ->lruvec->lru_lock	(check_pte_range->isolate_lru_page)
 *    ->private_lock		(page_remove_rmap->set_page_dirty)
 *    ->i_pages lock		(page_remove_rmap->set_page_dirty)
 *    bdi.wb->list_lock		(page_remove_rmap->set_page_dirty)
 *    ->inode->i_lock		(page_remove_rmap->set_page_dirty)
 *    ->memcg->move_lock	(page_remove_rmap->lock_page_memcg)
 *    bdi.wb->list_lock		(zap_pte_range->set_page_dirty)
 *    ->inode->i_lock		(zap_pte_range->set_page_dirty)
 *    ->private_lock		(zap_pte_range->block_dirty_folio)
 *
 * ->i_mmap_rwsem
 *   ->tasklist_lock            (memory_failure, collect_procs_ao)
 */
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
static void page_cache_delete_for_file_area(struct address_space *mapping,
				   struct folio *folio, void *shadow)
{
	XA_STATE(xas, &mapping->i_pages, folio->index >>PAGE_COUNT_IN_AREA_SHIFT);
	long nr = 1;
	struct file_area *p_file_area; 
	//令page索引与上0x3得到它在file_area的pages[]数组的下标
	unsigned int page_offset_in_file_area = folio->index & PAGE_COUNT_IN_AREA_MASK;
#if 0
	mapping_set_update(&xas, mapping);//xarray shadow 的处理，先不管
#endif
	/* hugetlb pages are represented by a single entry in the xarray */
	if (!folio_test_hugetlb(folio)) {
		if(folio_nr_pages(folio) != 1){
            panic("%s folio_nr_pages:%ld\n",__func__,folio_nr_pages(folio));
		}
#if 0		
		xas_set_order(&xas, folio->index, folio_order(folio));
		nr = folio_nr_pages(folio);
#endif		
	}

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);

    p_file_area = xas_load(&xas);
	if(!p_file_area || !is_file_area_entry(p_file_area))
        panic("%s mapping:0x%llx folio:0x%llx file_area NULL\n",__func__,(u64)mapping,(u64)folio);

	p_file_area = entry_to_file_area(p_file_area);
    if(folio != p_file_area->pages[page_offset_in_file_area]){
        panic("%s mapping:0x%llx folio:0x%llx != p_file_area->pages:0x%llx\n",__func__,(u64)mapping,(u64)folio,(u64)p_file_area->pages[page_offset_in_file_area]);
	}
	p_file_area->pages[page_offset_in_file_area] = NULL;
	//清理这个page在file_area->file_area_statue的对应的bit位，表示这个page被释放了
    clear_file_area_page_bit(p_file_area,page_offset_in_file_area);
	//如果这个file_area还有page，直接返回。否则才会xas_store(&xas, NULL)清空这个file_area
	if(file_area_have_page(p_file_area))
		return;

	//xas_store(&xas, shadow);不再使用shadow机制
	xas_store(&xas, NULL);
	xas_init_marks(&xas);

	folio->mapping = NULL;
	/* Leave page->index set: truncation lookup relies upon it */
	mapping->nrpages -= nr;
}
#endif
static void page_cache_delete(struct address_space *mapping,
				   struct folio *folio, void *shadow)
{
	XA_STATE(xas, &mapping->i_pages, folio->index);
	long nr = 1;
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
	if(mapping->rh_reserved1)
		return page_cache_delete_for_file_area(mapping,folio,shadow);
#endif	

	mapping_set_update(&xas, mapping);

	/* hugetlb pages are represented by a single entry in the xarray */
	if (!folio_test_hugetlb(folio)) {
		xas_set_order(&xas, folio->index, folio_order(folio));
		nr = folio_nr_pages(folio);
	}

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);

	xas_store(&xas, shadow);
	xas_init_marks(&xas);

	folio->mapping = NULL;
	/* Leave page->index set: truncation lookup relies upon it */
	mapping->nrpages -= nr;
}
static void filemap_unaccount_folio(struct address_space *mapping,
		struct folio *folio)
{
	long nr;

	VM_BUG_ON_FOLIO(folio_mapped(folio), folio);
	if (!IS_ENABLED(CONFIG_DEBUG_VM) && unlikely(folio_mapped(folio))) {
		pr_alert("BUG: Bad page cache in process %s  pfn:%05lx\n",
			 current->comm, folio_pfn(folio));
		dump_page(&folio->page, "still mapped when deleted");
		dump_stack();
		add_taint(TAINT_BAD_PAGE, LOCKDEP_NOW_UNRELIABLE);

		if (mapping_exiting(mapping) && !folio_test_large(folio)) {
			int mapcount = page_mapcount(&folio->page);

			if (folio_ref_count(folio) >= mapcount + 2) {
				/*
				 * All vmas have already been torn down, so it's
				 * a good bet that actually the page is unmapped
				 * and we'd rather not leak it: if we're wrong,
				 * another bad page check should catch it later.
				 */
				page_mapcount_reset(&folio->page);
				folio_ref_sub(folio, mapcount);
			}
		}
	}

	/* hugetlb folios do not participate in page cache accounting. */
	if (folio_test_hugetlb(folio))
		return;

	nr = folio_nr_pages(folio);

	__lruvec_stat_mod_folio(folio, NR_FILE_PAGES, -nr);
	if (folio_test_swapbacked(folio)) {
		__lruvec_stat_mod_folio(folio, NR_SHMEM, -nr);
		if (folio_test_pmd_mappable(folio))
			__lruvec_stat_mod_folio(folio, NR_SHMEM_THPS, -nr);
	} else if (folio_test_pmd_mappable(folio)) {
		__lruvec_stat_mod_folio(folio, NR_FILE_THPS, -nr);
		filemap_nr_thps_dec(mapping);
	}

	/*
	 * At this point folio must be either written or cleaned by
	 * truncate.  Dirty folio here signals a bug and loss of
	 * unwritten data - on ordinary filesystems.
	 *
	 * But it's harmless on in-memory filesystems like tmpfs; and can
	 * occur when a driver which did get_user_pages() sets page dirty
	 * before putting it, while the inode is being finally evicted.
	 *
	 * Below fixes dirty accounting after removing the folio entirely
	 * but leaves the dirty flag set: it has no effect for truncated
	 * folio and anyway will be cleared before returning folio to
	 * buddy allocator.
	 */
	if (WARN_ON_ONCE(folio_test_dirty(folio) &&
			 mapping_can_writeback(mapping)))
		folio_account_cleaned(folio, inode_to_wb(mapping->host));
}

/*
 * Delete a page from the page cache and free it. Caller has to make
 * sure the page is locked and that nobody else uses it - or that usage
 * is safe.  The caller must hold the i_pages lock.
 */
void __filemap_remove_folio(struct folio *folio, void *shadow)
{
	struct address_space *mapping = folio->mapping;

	trace_mm_filemap_delete_from_page_cache(folio);
	filemap_unaccount_folio(mapping, folio);
	page_cache_delete(mapping, folio, shadow);
}

void filemap_free_folio(struct address_space *mapping, struct folio *folio)
{
	void (*freepage)(struct page *);
	int refs = 1;

	freepage = mapping->a_ops->freepage;
	if (freepage)
		freepage(&folio->page);

	if (folio_test_large(folio) && !folio_test_hugetlb(folio))
		refs = folio_nr_pages(folio);
	folio_put_refs(folio, refs);
}

/**
 * filemap_remove_folio - Remove folio from page cache.
 * @folio: The folio.
 *
 * This must be called only on folios that are locked and have been
 * verified to be in the page cache.  It will never put the folio into
 * the free list because the caller has a reference on the page.
 */
void filemap_remove_folio(struct folio *folio)
{
	struct address_space *mapping = folio->mapping;

	BUG_ON(!folio_test_locked(folio));
	spin_lock(&mapping->host->i_lock);
	xa_lock_irq(&mapping->i_pages);
	__filemap_remove_folio(folio, NULL);
	xa_unlock_irq(&mapping->i_pages);
	if (mapping_shrinkable(mapping))
		inode_add_lru(mapping->host);
	spin_unlock(&mapping->host->i_lock);

	filemap_free_folio(mapping, folio);
}

/*
 * page_cache_delete_batch - delete several folios from page cache
 * @mapping: the mapping to which folios belong
 * @fbatch: batch of folios to delete
 *
 * The function walks over mapping->i_pages and removes folios passed in
 * @fbatch from the mapping. The function expects @fbatch to be sorted
 * by page index and is optimised for it to be dense.
 * It tolerates holes in @fbatch (mapping entries at those indices are not
 * modified).
 *
 * The function expects the i_pages lock to be held.
 */
static void page_cache_delete_batch(struct address_space *mapping,
			     struct folio_batch *fbatch)
{
	XA_STATE(xas, &mapping->i_pages, fbatch->folios[0]->index);
	long total_pages = 0;
	int i = 0;
	struct folio *folio;

	mapping_set_update(&xas, mapping);
	xas_for_each(&xas, folio, ULONG_MAX) {
		if (i >= folio_batch_count(fbatch))
			break;

		/* A swap/dax/shadow entry got inserted? Skip it. */
		if (xa_is_value(folio))
			continue;
		/*
		 * A page got inserted in our range? Skip it. We have our
		 * pages locked so they are protected from being removed.
		 * If we see a page whose index is higher than ours, it
		 * means our page has been removed, which shouldn't be
		 * possible because we're holding the PageLock.
		 */
		if (folio != fbatch->folios[i]) {
			VM_BUG_ON_FOLIO(folio->index >
					fbatch->folios[i]->index, folio);
			continue;
		}

		WARN_ON_ONCE(!folio_test_locked(folio));

		folio->mapping = NULL;
		/* Leave folio->index set: truncation lookup relies on it */

		i++;
		xas_store(&xas, NULL);
		total_pages += folio_nr_pages(folio);
	}
	mapping->nrpages -= total_pages;
}

void delete_from_page_cache_batch(struct address_space *mapping,
				  struct folio_batch *fbatch)
{
	int i;

	if (!folio_batch_count(fbatch))
		return;

	spin_lock(&mapping->host->i_lock);
	xa_lock_irq(&mapping->i_pages);
	for (i = 0; i < folio_batch_count(fbatch); i++) {
		struct folio *folio = fbatch->folios[i];

		trace_mm_filemap_delete_from_page_cache(folio);
		filemap_unaccount_folio(mapping, folio);
	}
	page_cache_delete_batch(mapping, fbatch);
	xa_unlock_irq(&mapping->i_pages);
	if (mapping_shrinkable(mapping))
		inode_add_lru(mapping->host);
	spin_unlock(&mapping->host->i_lock);

	for (i = 0; i < folio_batch_count(fbatch); i++)
		filemap_free_folio(mapping, fbatch->folios[i]);
}

int filemap_check_errors(struct address_space *mapping)
{
	int ret = 0;
	/* Check for outstanding write errors */
	if (test_bit(AS_ENOSPC, &mapping->flags) &&
	    test_and_clear_bit(AS_ENOSPC, &mapping->flags))
		ret = -ENOSPC;
	if (test_bit(AS_EIO, &mapping->flags) &&
	    test_and_clear_bit(AS_EIO, &mapping->flags))
		ret = -EIO;
	return ret;
}
EXPORT_SYMBOL(filemap_check_errors);

static int filemap_check_and_keep_errors(struct address_space *mapping)
{
	/* Check for outstanding write errors */
	if (test_bit(AS_EIO, &mapping->flags))
		return -EIO;
	if (test_bit(AS_ENOSPC, &mapping->flags))
		return -ENOSPC;
	return 0;
}

/**
 * filemap_fdatawrite_wbc - start writeback on mapping dirty pages in range
 * @mapping:	address space structure to write
 * @wbc:	the writeback_control controlling the writeout
 *
 * Call writepages on the mapping using the provided wbc to control the
 * writeout.
 *
 * Return: %0 on success, negative error code otherwise.
 */
int filemap_fdatawrite_wbc(struct address_space *mapping,
			   struct writeback_control *wbc)
{
	int ret;

	if (!mapping_can_writeback(mapping) ||
	    !mapping_tagged(mapping, PAGECACHE_TAG_DIRTY))
		return 0;

	wbc_attach_fdatawrite_inode(wbc, mapping->host);
	ret = do_writepages(mapping, wbc);
	wbc_detach_inode(wbc);
	return ret;
}
EXPORT_SYMBOL(filemap_fdatawrite_wbc);

/**
 * __filemap_fdatawrite_range - start writeback on mapping dirty pages in range
 * @mapping:	address space structure to write
 * @start:	offset in bytes where the range starts
 * @end:	offset in bytes where the range ends (inclusive)
 * @sync_mode:	enable synchronous operation
 *
 * Start writeback against all of a mapping's dirty pages that lie
 * within the byte offsets <start, end> inclusive.
 *
 * If sync_mode is WB_SYNC_ALL then this is a "data integrity" operation, as
 * opposed to a regular memory cleansing writeback.  The difference between
 * these two operations is that if a dirty page/buffer is encountered, it must
 * be waited upon, and not just skipped over.
 *
 * Return: %0 on success, negative error code otherwise.
 */
int __filemap_fdatawrite_range(struct address_space *mapping, loff_t start,
				loff_t end, int sync_mode)
{
	struct writeback_control wbc = {
		.sync_mode = sync_mode,
		.nr_to_write = LONG_MAX,
		.range_start = start,
		.range_end = end,
	};

	return filemap_fdatawrite_wbc(mapping, &wbc);
}

static inline int __filemap_fdatawrite(struct address_space *mapping,
	int sync_mode)
{
	return __filemap_fdatawrite_range(mapping, 0, LLONG_MAX, sync_mode);
}

int filemap_fdatawrite(struct address_space *mapping)
{
	return __filemap_fdatawrite(mapping, WB_SYNC_ALL);
}
EXPORT_SYMBOL(filemap_fdatawrite);

int filemap_fdatawrite_range(struct address_space *mapping, loff_t start,
				loff_t end)
{
	return __filemap_fdatawrite_range(mapping, start, end, WB_SYNC_ALL);
}
EXPORT_SYMBOL(filemap_fdatawrite_range);

/**
 * filemap_flush - mostly a non-blocking flush
 * @mapping:	target address_space
 *
 * This is a mostly non-blocking flush.  Not suitable for data-integrity
 * purposes - I/O may not be started against all dirty pages.
 *
 * Return: %0 on success, negative error code otherwise.
 */
int filemap_flush(struct address_space *mapping)
{
	return __filemap_fdatawrite(mapping, WB_SYNC_NONE);
}
EXPORT_SYMBOL(filemap_flush);

/**
 * filemap_range_has_page - check if a page exists in range.
 * @mapping:           address space within which to check
 * @start_byte:        offset in bytes where the range starts
 * @end_byte:          offset in bytes where the range ends (inclusive)
 *
 * Find at least one page in the range supplied, usually used to check if
 * direct writing in this range will trigger a writeback.
 *
 * Return: %true if at least one page exists in the specified range,
 * %false otherwise.
 */
bool filemap_range_has_page(struct address_space *mapping,
			   loff_t start_byte, loff_t end_byte)
{
	struct page *page;
	XA_STATE(xas, &mapping->i_pages, start_byte >> PAGE_SHIFT);
	pgoff_t max = end_byte >> PAGE_SHIFT;

	if (end_byte < start_byte)
		return false;

	rcu_read_lock();
	for (;;) {
		page = xas_find(&xas, max);
		if (xas_retry(&xas, page))
			continue;
		/* Shadow entries don't count */
		if (xa_is_value(page))
			continue;
		/*
		 * We don't need to try to pin this page; we're about to
		 * release the RCU lock anyway.  It is enough to know that
		 * there was a page here recently.
		 */
		break;
	}
	rcu_read_unlock();

	return page != NULL;
}
EXPORT_SYMBOL(filemap_range_has_page);

static void __filemap_fdatawait_range(struct address_space *mapping,
				     loff_t start_byte, loff_t end_byte)
{
	pgoff_t index = start_byte >> PAGE_SHIFT;
	pgoff_t end = end_byte >> PAGE_SHIFT;
	struct pagevec pvec;
	int nr_pages;

	if (end_byte < start_byte)
		return;

	pagevec_init(&pvec);
	while (index <= end) {
		unsigned i;

		nr_pages = pagevec_lookup_range_tag(&pvec, mapping, &index,
				end, PAGECACHE_TAG_WRITEBACK);
		if (!nr_pages)
			break;

		for (i = 0; i < nr_pages; i++) {
			struct page *page = pvec.pages[i];

			wait_on_page_writeback(page);
			ClearPageError(page);
		}
		pagevec_release(&pvec);
		cond_resched();
	}
}

/**
 * filemap_fdatawait_range - wait for writeback to complete
 * @mapping:		address space structure to wait for
 * @start_byte:		offset in bytes where the range starts
 * @end_byte:		offset in bytes where the range ends (inclusive)
 *
 * Walk the list of under-writeback pages of the given address space
 * in the given range and wait for all of them.  Check error status of
 * the address space and return it.
 *
 * Since the error status of the address space is cleared by this function,
 * callers are responsible for checking the return value and handling and/or
 * reporting the error.
 *
 * Return: error status of the address space.
 */
int filemap_fdatawait_range(struct address_space *mapping, loff_t start_byte,
			    loff_t end_byte)
{
	__filemap_fdatawait_range(mapping, start_byte, end_byte);
	return filemap_check_errors(mapping);
}
EXPORT_SYMBOL(filemap_fdatawait_range);

/**
 * filemap_fdatawait_range_keep_errors - wait for writeback to complete
 * @mapping:		address space structure to wait for
 * @start_byte:		offset in bytes where the range starts
 * @end_byte:		offset in bytes where the range ends (inclusive)
 *
 * Walk the list of under-writeback pages of the given address space in the
 * given range and wait for all of them.  Unlike filemap_fdatawait_range(),
 * this function does not clear error status of the address space.
 *
 * Use this function if callers don't handle errors themselves.  Expected
 * call sites are system-wide / filesystem-wide data flushers: e.g. sync(2),
 * fsfreeze(8)
 */
int filemap_fdatawait_range_keep_errors(struct address_space *mapping,
		loff_t start_byte, loff_t end_byte)
{
	__filemap_fdatawait_range(mapping, start_byte, end_byte);
	return filemap_check_and_keep_errors(mapping);
}
EXPORT_SYMBOL(filemap_fdatawait_range_keep_errors);

/**
 * file_fdatawait_range - wait for writeback to complete
 * @file:		file pointing to address space structure to wait for
 * @start_byte:		offset in bytes where the range starts
 * @end_byte:		offset in bytes where the range ends (inclusive)
 *
 * Walk the list of under-writeback pages of the address space that file
 * refers to, in the given range and wait for all of them.  Check error
 * status of the address space vs. the file->f_wb_err cursor and return it.
 *
 * Since the error status of the file is advanced by this function,
 * callers are responsible for checking the return value and handling and/or
 * reporting the error.
 *
 * Return: error status of the address space vs. the file->f_wb_err cursor.
 */
int file_fdatawait_range(struct file *file, loff_t start_byte, loff_t end_byte)
{
	struct address_space *mapping = file->f_mapping;

	__filemap_fdatawait_range(mapping, start_byte, end_byte);
	return file_check_and_advance_wb_err(file);
}
EXPORT_SYMBOL(file_fdatawait_range);

/**
 * filemap_fdatawait_keep_errors - wait for writeback without clearing errors
 * @mapping: address space structure to wait for
 *
 * Walk the list of under-writeback pages of the given address space
 * and wait for all of them.  Unlike filemap_fdatawait(), this function
 * does not clear error status of the address space.
 *
 * Use this function if callers don't handle errors themselves.  Expected
 * call sites are system-wide / filesystem-wide data flushers: e.g. sync(2),
 * fsfreeze(8)
 *
 * Return: error status of the address space.
 */
int filemap_fdatawait_keep_errors(struct address_space *mapping)
{
	__filemap_fdatawait_range(mapping, 0, LLONG_MAX);
	return filemap_check_and_keep_errors(mapping);
}
EXPORT_SYMBOL(filemap_fdatawait_keep_errors);

/* Returns true if writeback might be needed or already in progress. */
static bool mapping_needs_writeback(struct address_space *mapping)
{
	return mapping->nrpages;
}

bool filemap_range_has_writeback(struct address_space *mapping,
				 loff_t start_byte, loff_t end_byte)
{
	XA_STATE(xas, &mapping->i_pages, start_byte >> PAGE_SHIFT);
	pgoff_t max = end_byte >> PAGE_SHIFT;
	struct page *page;

	if (end_byte < start_byte)
		return false;

	rcu_read_lock();
	xas_for_each(&xas, page, max) {
		if (xas_retry(&xas, page))
			continue;
		if (xa_is_value(page))
			continue;
		if (PageDirty(page) || PageLocked(page) || PageWriteback(page))
			break;
	}
	rcu_read_unlock();
	return page != NULL;
}
EXPORT_SYMBOL_GPL(filemap_range_has_writeback);

/**
 * filemap_write_and_wait_range - write out & wait on a file range
 * @mapping:	the address_space for the pages
 * @lstart:	offset in bytes where the range starts
 * @lend:	offset in bytes where the range ends (inclusive)
 *
 * Write out and wait upon file offsets lstart->lend, inclusive.
 *
 * Note that @lend is inclusive (describes the last byte to be written) so
 * that this function can be used to write to the very end-of-file (end = -1).
 *
 * Return: error status of the address space.
 */
int filemap_write_and_wait_range(struct address_space *mapping,
				 loff_t lstart, loff_t lend)
{
	int err = 0;

	if (mapping_needs_writeback(mapping)) {
		err = __filemap_fdatawrite_range(mapping, lstart, lend,
						 WB_SYNC_ALL);
		/*
		 * Even if the above returned error, the pages may be
		 * written partially (e.g. -ENOSPC), so we wait for it.
		 * But the -EIO is special case, it may indicate the worst
		 * thing (e.g. bug) happened, so we avoid waiting for it.
		 */
		if (err != -EIO) {
			int err2 = filemap_fdatawait_range(mapping,
						lstart, lend);
			if (!err)
				err = err2;
		} else {
			/* Clear any previously stored errors */
			filemap_check_errors(mapping);
		}
	} else {
		err = filemap_check_errors(mapping);
	}
	return err;
}
EXPORT_SYMBOL(filemap_write_and_wait_range);

void __filemap_set_wb_err(struct address_space *mapping, int err)
{
	errseq_t eseq = errseq_set(&mapping->wb_err, err);

	trace_filemap_set_wb_err(mapping, eseq);
}
EXPORT_SYMBOL(__filemap_set_wb_err);

/**
 * file_check_and_advance_wb_err - report wb error (if any) that was previously
 * 				   and advance wb_err to current one
 * @file: struct file on which the error is being reported
 *
 * When userland calls fsync (or something like nfsd does the equivalent), we
 * want to report any writeback errors that occurred since the last fsync (or
 * since the file was opened if there haven't been any).
 *
 * Grab the wb_err from the mapping. If it matches what we have in the file,
 * then just quickly return 0. The file is all caught up.
 *
 * If it doesn't match, then take the mapping value, set the "seen" flag in
 * it and try to swap it into place. If it works, or another task beat us
 * to it with the new value, then update the f_wb_err and return the error
 * portion. The error at this point must be reported via proper channels
 * (a'la fsync, or NFS COMMIT operation, etc.).
 *
 * While we handle mapping->wb_err with atomic operations, the f_wb_err
 * value is protected by the f_lock since we must ensure that it reflects
 * the latest value swapped in for this file descriptor.
 *
 * Return: %0 on success, negative error code otherwise.
 */
int file_check_and_advance_wb_err(struct file *file)
{
	int err = 0;
	errseq_t old = READ_ONCE(file->f_wb_err);
	struct address_space *mapping = file->f_mapping;

	/* Locklessly handle the common case where nothing has changed */
	if (errseq_check(&mapping->wb_err, old)) {
		/* Something changed, must use slow path */
		spin_lock(&file->f_lock);
		old = file->f_wb_err;
		err = errseq_check_and_advance(&mapping->wb_err,
						&file->f_wb_err);
		trace_file_check_and_advance_wb_err(file, old);
		spin_unlock(&file->f_lock);
	}

	/*
	 * We're mostly using this function as a drop in replacement for
	 * filemap_check_errors. Clear AS_EIO/AS_ENOSPC to emulate the effect
	 * that the legacy code would have had on these flags.
	 */
	clear_bit(AS_EIO, &mapping->flags);
	clear_bit(AS_ENOSPC, &mapping->flags);
	return err;
}
EXPORT_SYMBOL(file_check_and_advance_wb_err);

/**
 * file_write_and_wait_range - write out & wait on a file range
 * @file:	file pointing to address_space with pages
 * @lstart:	offset in bytes where the range starts
 * @lend:	offset in bytes where the range ends (inclusive)
 *
 * Write out and wait upon file offsets lstart->lend, inclusive.
 *
 * Note that @lend is inclusive (describes the last byte to be written) so
 * that this function can be used to write to the very end-of-file (end = -1).
 *
 * After writing out and waiting on the data, we check and advance the
 * f_wb_err cursor to the latest value, and return any errors detected there.
 *
 * Return: %0 on success, negative error code otherwise.
 */
int file_write_and_wait_range(struct file *file, loff_t lstart, loff_t lend)
{
	int err = 0, err2;
	struct address_space *mapping = file->f_mapping;

	if (mapping_needs_writeback(mapping)) {
		err = __filemap_fdatawrite_range(mapping, lstart, lend,
						 WB_SYNC_ALL);
		/* See comment of filemap_write_and_wait() */
		if (err != -EIO)
			__filemap_fdatawait_range(mapping, lstart, lend);
	}
	err2 = file_check_and_advance_wb_err(file);
	if (!err)
		err = err2;
	return err;
}
EXPORT_SYMBOL(file_write_and_wait_range);

/**
 * replace_page_cache_page - replace a pagecache page with a new one
 * @old:	page to be replaced
 * @new:	page to replace with
 *
 * This function replaces a page in the pagecache with a new one.  On
 * success it acquires the pagecache reference for the new page and
 * drops it for the old page.  Both the old and new pages must be
 * locked.  This function does not add the new page to the LRU, the
 * caller must do that.
 *
 * The remove + add is atomic.  This function cannot fail.
 */
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
void replace_page_cache_page_for_file_area(struct page *old, struct page *new)
{
	struct folio *fold = page_folio(old);
	struct folio *fnew = page_folio(new);
	struct address_space *mapping = old->mapping;
	void (*freepage)(struct page *) = mapping->a_ops->freepage;
	pgoff_t offset = old->index;
	//XA_STATE(xas, &mapping->i_pages, offset);
	XA_STATE(xas, &mapping->i_pages, offset >> PAGE_COUNT_IN_AREA_SHIFT);
	struct file_area *p_file_area;
    //令page索引与上0x3得到它在file_area的pages[]数组的下标
	unsigned int page_offset_in_file_area = offset & PAGE_COUNT_IN_AREA_MASK;

	VM_BUG_ON_PAGE(!PageLocked(old), old);
	VM_BUG_ON_PAGE(!PageLocked(new), new);
	VM_BUG_ON_PAGE(new->mapping, new);

	get_page(new);
	new->mapping = mapping;
	new->index = offset;

	mem_cgroup_migrate(fold, fnew);

	xas_lock_irq(&xas);
	//xas_store(&xas, new);
    p_file_area = (struct file_area *)xas_load(&xas);
	if(!p_file_area || !is_file_area_entry(p_file_area))
        panic("%s mapping:0x%llx p_file_area:0x%llx error\n",__func__,(u64)mapping,(u64)p_file_area);

	p_file_area = entry_to_file_area(p_file_area);
    if(old != (struct page *)p_file_area->pages[page_offset_in_file_area]){
        panic("%s mapping:0x%llx old:0x%llx != p_file_area->pages:0x%llx\n",__func__,(u64)mapping,(u64)old,(u64)p_file_area->pages[page_offset_in_file_area]);
	}
	p_file_area->pages[page_offset_in_file_area] = fnew;

	old->mapping = NULL;
	/* hugetlb pages do not participate in page cache accounting. */
	if (!PageHuge(old))
		__dec_lruvec_page_state(old, NR_FILE_PAGES);
	if (!PageHuge(new))
		__inc_lruvec_page_state(new, NR_FILE_PAGES);
	if (PageSwapBacked(old))
		__dec_lruvec_page_state(old, NR_SHMEM);
	if (PageSwapBacked(new))
		__inc_lruvec_page_state(new, NR_SHMEM);
	xas_unlock_irq(&xas);
	if (freepage)
		freepage(old);
	put_page(old);
}
#endif
void replace_page_cache_page(struct page *old, struct page *new)
{
	struct folio *fold = page_folio(old);
	struct folio *fnew = page_folio(new);
	struct address_space *mapping = old->mapping;
	void (*freepage)(struct page *) = mapping->a_ops->freepage;
	pgoff_t offset = old->index;
	XA_STATE(xas, &mapping->i_pages, offset);

#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
	if(mapping->rh_reserved1)
		return replace_page_cache_page_for_file_area(old,new);
#endif

	VM_BUG_ON_PAGE(!PageLocked(old), old);
	VM_BUG_ON_PAGE(!PageLocked(new), new);
	VM_BUG_ON_PAGE(new->mapping, new);

	get_page(new);
	new->mapping = mapping;
	new->index = offset;

	mem_cgroup_migrate(fold, fnew);

	xas_lock_irq(&xas);
	xas_store(&xas, new);

	old->mapping = NULL;
	/* hugetlb pages do not participate in page cache accounting. */
	if (!PageHuge(old))
		__dec_lruvec_page_state(old, NR_FILE_PAGES);
	if (!PageHuge(new))
		__inc_lruvec_page_state(new, NR_FILE_PAGES);
	if (PageSwapBacked(old))
		__dec_lruvec_page_state(old, NR_SHMEM);
	if (PageSwapBacked(new))
		__inc_lruvec_page_state(new, NR_SHMEM);
	xas_unlock_irq(&xas);
	if (freepage)
		freepage(old);
	put_page(old);
}
EXPORT_SYMBOL_GPL(replace_page_cache_page);

#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
noinline int __filemap_add_folio_for_file_area(struct address_space *mapping,
		struct folio *folio, pgoff_t index, gfp_t gfp, void **shadowp)
{
	unsigned int area_index_for_page = index >> PAGE_COUNT_IN_AREA_SHIFT;
	//XA_STATE(xas, &mapping->i_pages, index);
	XA_STATE(xas, &mapping->i_pages, area_index_for_page);
	int huge = folio_test_hugetlb(folio);
	bool charged = false;
	long nr = 1;
	struct file_stat *p_file_stat;
	struct file_area *p_file_area;
	//令page索引与上0x3得到它在file_area的pages[]数组的下标
	unsigned int page_offset_in_file_area = index & PAGE_COUNT_IN_AREA_MASK;

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(folio_test_swapbacked(folio), folio);
	mapping_set_update(&xas, mapping);

	p_file_stat = (struct file_stat *)mapping->rh_reserved1;
	if(!p_file_stat){
		//分配file_stat
	    p_file_stat  = file_stat_alloc_and_init(mapping);
		if(!p_file_stat){
			xas_set_err(&xas, -ENOMEM);
            goto error; 
		}
	}

	if (!huge) {
		int error = mem_cgroup_charge(folio, NULL, gfp);
		VM_BUG_ON_FOLIO(index & (folio_nr_pages(folio) - 1), folio);
		if (error)
			return error;
		charged = true;
		xas_set_order(&xas, index, folio_order(folio));
		nr = folio_nr_pages(folio);
	}

	gfp &= GFP_RECLAIM_MASK;
	folio_ref_add(folio, nr);
	folio->mapping = mapping;
	folio->index = xas.xa_index;
    
	if(index != folio->index || nr != 1 || folio_order(folio) != 0){
        printk("%s index:%ld folio->index:%ld nr:%ld !!!!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,index,folio->index,nr);
	}

	do {
		//这里边有执行xas_load()，感觉浪费性能吧!!!!!!!!!!!!!!!!!!!!!!!!!!!!
		unsigned int order = xa_get_order(xas.xa, xas.xa_index);
		void *entry, *old = NULL;

		if (order > folio_order(folio)){
            panic("%s order:%d folio_order:%d error !!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,order,folio_order(folio));
			xas_split_alloc(&xas, xa_load(xas.xa, xas.xa_index),
					order, gfp);
		}
		xas_lock_irq(&xas);
		xas_for_each_conflict(&xas, entry) {
			old = entry;
			//xas_lock_irq加锁后，检测到待添加的file_area已经被其他进程并发添加到xarray tree了
			if (!xa_is_value(entry)) {

				//if(!p_file_area)从进来说明file_area已经非NULL，不用再判断
				//    goto ;
				p_file_area = entry_to_file_area(entry);

                folio = p_file_area->pages[page_offset_in_file_area];
				//page已经添加到file_area了
				if(NULL != folio){
				    xas_set_err(&xas, -EEXIST);
				    goto unlock;
				}
				//file_area已经添加到xarray tree，但是page还没有赋值到file_area->pages[]数组
				goto find_file_area;
			}
		}
#if 0
		if (old) {
			if (shadowp)
				*shadowp = old;
			/* entry may have been split before we acquired lock */
			order = xa_get_order(xas.xa, xas.xa_index);
			if (order > folio_order(folio)) {
				/* How to handle large swap entries? */
				BUG_ON(shmem_mapping(mapping));
				xas_split(&xas, old, order);
				xas_reset(&xas);
			}
		}
#else
		*shadowp = NULL;
#endif
		//分配file_area
        p_file_area  = file_area_alloc_and_init(area_index_for_page,p_file_stat);
		if(!p_file_area){
		    xas_unlock_irq(&xas);
			xas_set_err(&xas, -ENOMEM);
            goto error; 
		}

		//xas_store(&xas, folio);
		xas_store(&xas, file_area_to_entry(p_file_area));
		if (xas_error(&xas))
			goto unlock;

find_file_area:
		
	    set_file_area_page_bit(p_file_area,page_offset_in_file_area);
		//folio指针保存到file_area
		p_file_area->pages[page_offset_in_file_area] = folio;
		mapping->nrpages += nr;

		/* hugetlb pages do not participate in page cache accounting */
		if (!huge) {
			__lruvec_stat_mod_folio(folio, NR_FILE_PAGES, nr);
			if (folio_test_pmd_mappable(folio))
				__lruvec_stat_mod_folio(folio,
						NR_FILE_THPS, nr);
		}
unlock:
		xas_unlock_irq(&xas);
	} while (xas_nomem(&xas, gfp));

	if (xas_error(&xas))
		goto error;

	trace_mm_filemap_add_to_page_cache(folio);
	return 0;
error:
	if (charged)
		mem_cgroup_uncharge(folio);
	folio->mapping = NULL;
	/* Leave page->index set: truncation relies upon it */
	folio_put_refs(folio, nr);
	return xas_error(&xas);
}
#endif
noinline int __filemap_add_folio(struct address_space *mapping,
		struct folio *folio, pgoff_t index, gfp_t gfp, void **shadowp)
{
	XA_STATE(xas, &mapping->i_pages, index);
	int huge = folio_test_hugetlb(folio);
	bool charged = false;
	long nr = 1;

#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
	/*有多种异常文件entry会保存到文件xarray tree，一共是shadow entry、tmpfs/shmem swap entry、dax entry、匿名页swap entry。
	 *shadow entry已经在被我在page_cache_delete()和page_cache_delete_batch()规避，tmpfs/shmem swap entry、匿名页swap entry
	 *把对xarray tree槽位的赋值不在filemap.c的__filemap_add_folio函数，在其他文件。只剩下dax entry在这里显式指明。
	 * */
	if(!dax_mapping(mapping) && is_test_file(mapping)){
        return __filemap_add_folio_for_file_area(mapping,folio,index,gfp,shadowp);
	}
#endif	
	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(folio_test_swapbacked(folio), folio);
	mapping_set_update(&xas, mapping);

	if (!huge) {
		int error = mem_cgroup_charge(folio, NULL, gfp);
		VM_BUG_ON_FOLIO(index & (folio_nr_pages(folio) - 1), folio);
		if (error)
			return error;
		charged = true;
		xas_set_order(&xas, index, folio_order(folio));
		nr = folio_nr_pages(folio);
	}

	gfp &= GFP_RECLAIM_MASK;
	folio_ref_add(folio, nr);
	folio->mapping = mapping;
	folio->index = xas.xa_index;

	do {
		unsigned int order = xa_get_order(xas.xa, xas.xa_index);
		void *entry, *old = NULL;

		if (order > folio_order(folio))
			xas_split_alloc(&xas, xa_load(xas.xa, xas.xa_index),
					order, gfp);
		xas_lock_irq(&xas);
		xas_for_each_conflict(&xas, entry) {
			old = entry;
			if (!xa_is_value(entry)) {
				xas_set_err(&xas, -EEXIST);
				goto unlock;
			}
		}

		if (old) {
			if (shadowp)
				*shadowp = old;
			/* entry may have been split before we acquired lock */
			order = xa_get_order(xas.xa, xas.xa_index);
			if (order > folio_order(folio)) {
				/* How to handle large swap entries? */
				BUG_ON(shmem_mapping(mapping));
				xas_split(&xas, old, order);
				xas_reset(&xas);
			}
		}

		xas_store(&xas, folio);
		if (xas_error(&xas))
			goto unlock;

		mapping->nrpages += nr;

		/* hugetlb pages do not participate in page cache accounting */
		if (!huge) {
			__lruvec_stat_mod_folio(folio, NR_FILE_PAGES, nr);
			if (folio_test_pmd_mappable(folio))
				__lruvec_stat_mod_folio(folio,
						NR_FILE_THPS, nr);
		}
unlock:
		xas_unlock_irq(&xas);
	} while (xas_nomem(&xas, gfp));

	if (xas_error(&xas))
		goto error;

	trace_mm_filemap_add_to_page_cache(folio);
	return 0;
error:
	if (charged)
		mem_cgroup_uncharge(folio);
	folio->mapping = NULL;
	/* Leave page->index set: truncation relies upon it */
	folio_put_refs(folio, nr);
	return xas_error(&xas);
}
ALLOW_ERROR_INJECTION(__filemap_add_folio, ERRNO);

/**
 * add_to_page_cache_locked - add a locked page to the pagecache
 * @page:	page to add
 * @mapping:	the page's address_space
 * @offset:	page index
 * @gfp_mask:	page allocation mode
 *
 * This function is used to add a page to the pagecache. It must be locked.
 * This function does not add the page to the LRU.  The caller must do that.
 *
 * Return: %0 on success, negative error code otherwise.
 */
int add_to_page_cache_locked(struct page *page, struct address_space *mapping,
		pgoff_t offset, gfp_t gfp_mask)
{
	return __filemap_add_folio(mapping, page_folio(page), offset,
					  gfp_mask, NULL);
}
EXPORT_SYMBOL(add_to_page_cache_locked);

int filemap_add_folio(struct address_space *mapping, struct folio *folio,
				pgoff_t index, gfp_t gfp)
{
	void *shadow = NULL;
	int ret;

	__folio_set_locked(folio);
	ret = __filemap_add_folio(mapping, folio, index, gfp, &shadow);
	if (unlikely(ret))
		__folio_clear_locked(folio);
	else {
		/*
		 * The folio might have been evicted from cache only
		 * recently, in which case it should be activated like
		 * any other repeatedly accessed folio.
		 * The exception is folios getting rewritten; evicting other
		 * data from the working set, only to cache data that will
		 * get overwritten with something else, is a waste of memory.
		 */
		WARN_ON_ONCE(folio_test_active(folio));
		if (!(gfp & __GFP_WRITE) && shadow)
			workingset_refault(folio, shadow);
		folio_add_lru(folio);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(filemap_add_folio);

#ifdef CONFIG_NUMA
struct folio *filemap_alloc_folio(gfp_t gfp, unsigned int order)
{
	int n;
	struct folio *folio;

	if (cpuset_do_page_mem_spread()) {
		unsigned int cpuset_mems_cookie;
		do {
			cpuset_mems_cookie = read_mems_allowed_begin();
			n = cpuset_mem_spread_node();
			folio = __folio_alloc_node(gfp, order, n);
		} while (!folio && read_mems_allowed_retry(cpuset_mems_cookie));

		return folio;
	}
	return folio_alloc(gfp, order);
}
EXPORT_SYMBOL(filemap_alloc_folio);
#endif

/*
 * filemap_invalidate_lock_two - lock invalidate_lock for two mappings
 *
 * Lock exclusively invalidate_lock of any passed mapping that is not NULL.
 *
 * @mapping1: the first mapping to lock
 * @mapping2: the second mapping to lock
 */
void filemap_invalidate_lock_two(struct address_space *mapping1,
				 struct address_space *mapping2)
{
	if (mapping1 > mapping2)
		swap(mapping1, mapping2);
	if (mapping1)
		down_write(&mapping1->invalidate_lock);
	if (mapping2 && mapping1 != mapping2)
		down_write_nested(&mapping2->invalidate_lock, 1);
}
EXPORT_SYMBOL(filemap_invalidate_lock_two);

/*
 * filemap_invalidate_unlock_two - unlock invalidate_lock for two mappings
 *
 * Unlock exclusive invalidate_lock of any passed mapping that is not NULL.
 *
 * @mapping1: the first mapping to unlock
 * @mapping2: the second mapping to unlock
 */
void filemap_invalidate_unlock_two(struct address_space *mapping1,
				   struct address_space *mapping2)
{
	if (mapping1)
		up_write(&mapping1->invalidate_lock);
	if (mapping2 && mapping1 != mapping2)
		up_write(&mapping2->invalidate_lock);
}
EXPORT_SYMBOL(filemap_invalidate_unlock_two);

/*
 * In order to wait for pages to become available there must be
 * waitqueues associated with pages. By using a hash table of
 * waitqueues where the bucket discipline is to maintain all
 * waiters on the same queue and wake all when any of the pages
 * become available, and for the woken contexts to check to be
 * sure the appropriate page became available, this saves space
 * at a cost of "thundering herd" phenomena during rare hash
 * collisions.
 */
#define PAGE_WAIT_TABLE_BITS 8
#define PAGE_WAIT_TABLE_SIZE (1 << PAGE_WAIT_TABLE_BITS)
static wait_queue_head_t folio_wait_table[PAGE_WAIT_TABLE_SIZE] __cacheline_aligned;

static wait_queue_head_t *folio_waitqueue(struct folio *folio)
{
	return &folio_wait_table[hash_ptr(folio, PAGE_WAIT_TABLE_BITS)];
}

void __init pagecache_init(void)
{
	int i;

	for (i = 0; i < PAGE_WAIT_TABLE_SIZE; i++)
		init_waitqueue_head(&folio_wait_table[i]);

	page_writeback_init();
}

/*
 * The page wait code treats the "wait->flags" somewhat unusually, because
 * we have multiple different kinds of waits, not just the usual "exclusive"
 * one.
 *
 * We have:
 *
 *  (a) no special bits set:
 *
 *	We're just waiting for the bit to be released, and when a waker
 *	calls the wakeup function, we set WQ_FLAG_WOKEN and wake it up,
 *	and remove it from the wait queue.
 *
 *	Simple and straightforward.
 *
 *  (b) WQ_FLAG_EXCLUSIVE:
 *
 *	The waiter is waiting to get the lock, and only one waiter should
 *	be woken up to avoid any thundering herd behavior. We'll set the
 *	WQ_FLAG_WOKEN bit, wake it up, and remove it from the wait queue.
 *
 *	This is the traditional exclusive wait.
 *
 *  (c) WQ_FLAG_EXCLUSIVE | WQ_FLAG_CUSTOM:
 *
 *	The waiter is waiting to get the bit, and additionally wants the
 *	lock to be transferred to it for fair lock behavior. If the lock
 *	cannot be taken, we stop walking the wait queue without waking
 *	the waiter.
 *
 *	This is the "fair lock handoff" case, and in addition to setting
 *	WQ_FLAG_WOKEN, we set WQ_FLAG_DONE to let the waiter easily see
 *	that it now has the lock.
 */
static int wake_page_function(wait_queue_entry_t *wait, unsigned mode, int sync, void *arg)
{
	unsigned int flags;
	struct wait_page_key *key = arg;
	struct wait_page_queue *wait_page
		= container_of(wait, struct wait_page_queue, wait);

	if (!wake_page_match(wait_page, key))
		return 0;

	/*
	 * If it's a lock handoff wait, we get the bit for it, and
	 * stop walking (and do not wake it up) if we can't.
	 */
	flags = wait->flags;
	if (flags & WQ_FLAG_EXCLUSIVE) {
		if (test_bit(key->bit_nr, &key->folio->flags))
			return -1;
		if (flags & WQ_FLAG_CUSTOM) {
			if (test_and_set_bit(key->bit_nr, &key->folio->flags))
				return -1;
			flags |= WQ_FLAG_DONE;
		}
	}

	/*
	 * We are holding the wait-queue lock, but the waiter that
	 * is waiting for this will be checking the flags without
	 * any locking.
	 *
	 * So update the flags atomically, and wake up the waiter
	 * afterwards to avoid any races. This store-release pairs
	 * with the load-acquire in folio_wait_bit_common().
	 */
	smp_store_release(&wait->flags, flags | WQ_FLAG_WOKEN);
	wake_up_state(wait->private, mode);

	/*
	 * Ok, we have successfully done what we're waiting for,
	 * and we can unconditionally remove the wait entry.
	 *
	 * Note that this pairs with the "finish_wait()" in the
	 * waiter, and has to be the absolute last thing we do.
	 * After this list_del_init(&wait->entry) the wait entry
	 * might be de-allocated and the process might even have
	 * exited.
	 */
	list_del_init_careful(&wait->entry);
	return (flags & WQ_FLAG_EXCLUSIVE) != 0;
}

static void folio_wake_bit(struct folio *folio, int bit_nr)
{
	wait_queue_head_t *q = folio_waitqueue(folio);
	struct wait_page_key key;
	unsigned long flags;
	wait_queue_entry_t bookmark;

	key.folio = folio;
	key.bit_nr = bit_nr;
	key.page_match = 0;

	bookmark.flags = 0;
	bookmark.private = NULL;
	bookmark.func = NULL;
	INIT_LIST_HEAD(&bookmark.entry);

	spin_lock_irqsave(&q->lock, flags);
	__wake_up_locked_key_bookmark(q, TASK_NORMAL, &key, &bookmark);

	while (bookmark.flags & WQ_FLAG_BOOKMARK) {
		/*
		 * Take a breather from holding the lock,
		 * allow pages that finish wake up asynchronously
		 * to acquire the lock and remove themselves
		 * from wait queue
		 */
		spin_unlock_irqrestore(&q->lock, flags);
		cpu_relax();
		spin_lock_irqsave(&q->lock, flags);
		__wake_up_locked_key_bookmark(q, TASK_NORMAL, &key, &bookmark);
	}

	/*
	 * It's possible to miss clearing waiters here, when we woke our page
	 * waiters, but the hashed waitqueue has waiters for other pages on it.
	 * That's okay, it's a rare case. The next waker will clear it.
	 *
	 * Note that, depending on the page pool (buddy, hugetlb, ZONE_DEVICE,
	 * other), the flag may be cleared in the course of freeing the page;
	 * but that is not required for correctness.
	 */
	if (!waitqueue_active(q) || !key.page_match)
		folio_clear_waiters(folio);

	spin_unlock_irqrestore(&q->lock, flags);
}

static void folio_wake(struct folio *folio, int bit)
{
	if (!folio_test_waiters(folio))
		return;
	folio_wake_bit(folio, bit);
}

/*
 * A choice of three behaviors for folio_wait_bit_common():
 */
enum behavior {
	EXCLUSIVE,	/* Hold ref to page and take the bit when woken, like
			 * __folio_lock() waiting on then setting PG_locked.
			 */
	SHARED,		/* Hold ref to page and check the bit when woken, like
			 * folio_wait_writeback() waiting on PG_writeback.
			 */
	DROP,		/* Drop ref to page before wait, no check when woken,
			 * like folio_put_wait_locked() on PG_locked.
			 */
};

/*
 * Attempt to check (or get) the folio flag, and mark us done
 * if successful.
 */
static inline bool folio_trylock_flag(struct folio *folio, int bit_nr,
					struct wait_queue_entry *wait)
{
	if (wait->flags & WQ_FLAG_EXCLUSIVE) {
		if (test_and_set_bit(bit_nr, &folio->flags))
			return false;
	} else if (test_bit(bit_nr, &folio->flags))
		return false;

	wait->flags |= WQ_FLAG_WOKEN | WQ_FLAG_DONE;
	return true;
}

/* How many times do we accept lock stealing from under a waiter? */
int sysctl_page_lock_unfairness = 5;

static inline int folio_wait_bit_common(struct folio *folio, int bit_nr,
		int state, enum behavior behavior)
{
	wait_queue_head_t *q = folio_waitqueue(folio);
	int unfairness = sysctl_page_lock_unfairness;
	struct wait_page_queue wait_page;
	wait_queue_entry_t *wait = &wait_page.wait;
	bool thrashing = false;
	bool delayacct = false;
	unsigned long pflags;

	if (bit_nr == PG_locked &&
	    !folio_test_uptodate(folio) && folio_test_workingset(folio)) {
		if (!folio_test_swapbacked(folio)) {
			delayacct_thrashing_start();
			delayacct = true;
		}
		psi_memstall_enter(&pflags);
		thrashing = true;
	}

	init_wait(wait);
	wait->func = wake_page_function;
	wait_page.folio = folio;
	wait_page.bit_nr = bit_nr;

repeat:
	wait->flags = 0;
	if (behavior == EXCLUSIVE) {
		wait->flags = WQ_FLAG_EXCLUSIVE;
		if (--unfairness < 0)
			wait->flags |= WQ_FLAG_CUSTOM;
	}

	/*
	 * Do one last check whether we can get the
	 * page bit synchronously.
	 *
	 * Do the folio_set_waiters() marking before that
	 * to let any waker we _just_ missed know they
	 * need to wake us up (otherwise they'll never
	 * even go to the slow case that looks at the
	 * page queue), and add ourselves to the wait
	 * queue if we need to sleep.
	 *
	 * This part needs to be done under the queue
	 * lock to avoid races.
	 */
	spin_lock_irq(&q->lock);
	folio_set_waiters(folio);
	if (!folio_trylock_flag(folio, bit_nr, wait))
		__add_wait_queue_entry_tail(q, wait);
	spin_unlock_irq(&q->lock);

	/*
	 * From now on, all the logic will be based on
	 * the WQ_FLAG_WOKEN and WQ_FLAG_DONE flag, to
	 * see whether the page bit testing has already
	 * been done by the wake function.
	 *
	 * We can drop our reference to the folio.
	 */
	if (behavior == DROP)
		folio_put(folio);

	/*
	 * Note that until the "finish_wait()", or until
	 * we see the WQ_FLAG_WOKEN flag, we need to
	 * be very careful with the 'wait->flags', because
	 * we may race with a waker that sets them.
	 */
	for (;;) {
		unsigned int flags;

		set_current_state(state);

		/* Loop until we've been woken or interrupted */
		flags = smp_load_acquire(&wait->flags);
		if (!(flags & WQ_FLAG_WOKEN)) {
			if (signal_pending_state(state, current))
				break;

			io_schedule();
			continue;
		}

		/* If we were non-exclusive, we're done */
		if (behavior != EXCLUSIVE)
			break;

		/* If the waker got the lock for us, we're done */
		if (flags & WQ_FLAG_DONE)
			break;

		/*
		 * Otherwise, if we're getting the lock, we need to
		 * try to get it ourselves.
		 *
		 * And if that fails, we'll have to retry this all.
		 */
		if (unlikely(test_and_set_bit(bit_nr, folio_flags(folio, 0))))
			goto repeat;

		wait->flags |= WQ_FLAG_DONE;
		break;
	}

	/*
	 * If a signal happened, this 'finish_wait()' may remove the last
	 * waiter from the wait-queues, but the folio waiters bit will remain
	 * set. That's ok. The next wakeup will take care of it, and trying
	 * to do it here would be difficult and prone to races.
	 */
	finish_wait(q, wait);

	if (thrashing) {
		if (delayacct)
			delayacct_thrashing_end();
		psi_memstall_leave(&pflags);
	}

	/*
	 * NOTE! The wait->flags weren't stable until we've done the
	 * 'finish_wait()', and we could have exited the loop above due
	 * to a signal, and had a wakeup event happen after the signal
	 * test but before the 'finish_wait()'.
	 *
	 * So only after the finish_wait() can we reliably determine
	 * if we got woken up or not, so we can now figure out the final
	 * return value based on that state without races.
	 *
	 * Also note that WQ_FLAG_WOKEN is sufficient for a non-exclusive
	 * waiter, but an exclusive one requires WQ_FLAG_DONE.
	 */
	if (behavior == EXCLUSIVE)
		return wait->flags & WQ_FLAG_DONE ? 0 : -EINTR;

	return wait->flags & WQ_FLAG_WOKEN ? 0 : -EINTR;
}

#ifdef CONFIG_MIGRATION
/**
 * migration_entry_wait_on_locked - Wait for a migration entry to be removed
 * @entry: migration swap entry.
 * @ptep: mapped pte pointer. Will return with the ptep unmapped. Only required
 *        for pte entries, pass NULL for pmd entries.
 * @ptl: already locked ptl. This function will drop the lock.
 *
 * Wait for a migration entry referencing the given page to be removed. This is
 * equivalent to put_and_wait_on_page_locked(page, TASK_UNINTERRUPTIBLE) except
 * this can be called without taking a reference on the page. Instead this
 * should be called while holding the ptl for the migration entry referencing
 * the page.
 *
 * Returns after unmapping and unlocking the pte/ptl with pte_unmap_unlock().
 *
 * This follows the same logic as folio_wait_bit_common() so see the comments
 * there.
 */
void migration_entry_wait_on_locked(swp_entry_t entry, pte_t *ptep,
				spinlock_t *ptl)
{
	struct wait_page_queue wait_page;
	wait_queue_entry_t *wait = &wait_page.wait;
	bool thrashing = false;
	bool delayacct = false;
	unsigned long pflags;
	wait_queue_head_t *q;
	struct folio *folio = page_folio(pfn_swap_entry_to_page(entry));

	q = folio_waitqueue(folio);
	if (!folio_test_uptodate(folio) && folio_test_workingset(folio)) {
		if (!folio_test_swapbacked(folio)) {
			delayacct_thrashing_start();
			delayacct = true;
		}
		psi_memstall_enter(&pflags);
		thrashing = true;
	}

	init_wait(wait);
	wait->func = wake_page_function;
	wait_page.folio = folio;
	wait_page.bit_nr = PG_locked;
	wait->flags = 0;

	spin_lock_irq(&q->lock);
	folio_set_waiters(folio);
	if (!folio_trylock_flag(folio, PG_locked, wait))
		__add_wait_queue_entry_tail(q, wait);
	spin_unlock_irq(&q->lock);

	/*
	 * If a migration entry exists for the page the migration path must hold
	 * a valid reference to the page, and it must take the ptl to remove the
	 * migration entry. So the page is valid until the ptl is dropped.
	 */
	if (ptep)
		pte_unmap_unlock(ptep, ptl);
	else
		spin_unlock(ptl);

	for (;;) {
		unsigned int flags;

		set_current_state(TASK_UNINTERRUPTIBLE);

		/* Loop until we've been woken or interrupted */
		flags = smp_load_acquire(&wait->flags);
		if (!(flags & WQ_FLAG_WOKEN)) {
			if (signal_pending_state(TASK_UNINTERRUPTIBLE, current))
				break;

			io_schedule();
			continue;
		}
		break;
	}

	finish_wait(q, wait);

	if (thrashing) {
		if (delayacct)
			delayacct_thrashing_end();
		psi_memstall_leave(&pflags);
	}
}
#endif

void folio_wait_bit(struct folio *folio, int bit_nr)
{
	folio_wait_bit_common(folio, bit_nr, TASK_UNINTERRUPTIBLE, SHARED);
}
EXPORT_SYMBOL(folio_wait_bit);

int folio_wait_bit_killable(struct folio *folio, int bit_nr)
{
	return folio_wait_bit_common(folio, bit_nr, TASK_KILLABLE, SHARED);
}
EXPORT_SYMBOL(folio_wait_bit_killable);

/**
 * folio_put_wait_locked - Drop a reference and wait for it to be unlocked
 * @folio: The folio to wait for.
 * @state: The sleep state (TASK_KILLABLE, TASK_UNINTERRUPTIBLE, etc).
 *
 * The caller should hold a reference on @folio.  They expect the page to
 * become unlocked relatively soon, but do not wish to hold up migration
 * (for example) by holding the reference while waiting for the folio to
 * come unlocked.  After this function returns, the caller should not
 * dereference @folio.
 *
 * Return: 0 if the folio was unlocked or -EINTR if interrupted by a signal.
 */
int folio_put_wait_locked(struct folio *folio, int state)
{
	return folio_wait_bit_common(folio, PG_locked, state, DROP);
}

/**
 * folio_add_wait_queue - Add an arbitrary waiter to a folio's wait queue
 * @folio: Folio defining the wait queue of interest
 * @waiter: Waiter to add to the queue
 *
 * Add an arbitrary @waiter to the wait queue for the nominated @folio.
 */
void folio_add_wait_queue(struct folio *folio, wait_queue_entry_t *waiter)
{
	wait_queue_head_t *q = folio_waitqueue(folio);
	unsigned long flags;

	spin_lock_irqsave(&q->lock, flags);
	__add_wait_queue_entry_tail(q, waiter);
	folio_set_waiters(folio);
	spin_unlock_irqrestore(&q->lock, flags);
}
EXPORT_SYMBOL_GPL(folio_add_wait_queue);

#ifndef clear_bit_unlock_is_negative_byte

/*
 * PG_waiters is the high bit in the same byte as PG_lock.
 *
 * On x86 (and on many other architectures), we can clear PG_lock and
 * test the sign bit at the same time. But if the architecture does
 * not support that special operation, we just do this all by hand
 * instead.
 *
 * The read of PG_waiters has to be after (or concurrently with) PG_locked
 * being cleared, but a memory barrier should be unnecessary since it is
 * in the same byte as PG_locked.
 */
static inline bool clear_bit_unlock_is_negative_byte(long nr, volatile void *mem)
{
	clear_bit_unlock(nr, mem);
	/* smp_mb__after_atomic(); */
	return test_bit(PG_waiters, mem);
}

#endif

/**
 * folio_unlock - Unlock a locked folio.
 * @folio: The folio.
 *
 * Unlocks the folio and wakes up any thread sleeping on the page lock.
 *
 * Context: May be called from interrupt or process context.  May not be
 * called from NMI context.
 */
void folio_unlock(struct folio *folio)
{
	/* Bit 7 allows x86 to check the byte's sign bit */
	BUILD_BUG_ON(PG_waiters != 7);
	BUILD_BUG_ON(PG_locked > 7);
	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	if (clear_bit_unlock_is_negative_byte(PG_locked, folio_flags(folio, 0)))
		folio_wake_bit(folio, PG_locked);
}
EXPORT_SYMBOL(folio_unlock);

/**
 * folio_end_private_2 - Clear PG_private_2 and wake any waiters.
 * @folio: The folio.
 *
 * Clear the PG_private_2 bit on a folio and wake up any sleepers waiting for
 * it.  The folio reference held for PG_private_2 being set is released.
 *
 * This is, for example, used when a netfs folio is being written to a local
 * disk cache, thereby allowing writes to the cache for the same folio to be
 * serialised.
 */
void folio_end_private_2(struct folio *folio)
{
	VM_BUG_ON_FOLIO(!folio_test_private_2(folio), folio);
	clear_bit_unlock(PG_private_2, folio_flags(folio, 0));
	folio_wake_bit(folio, PG_private_2);
	folio_put(folio);
}
EXPORT_SYMBOL(folio_end_private_2);

/**
 * folio_wait_private_2 - Wait for PG_private_2 to be cleared on a folio.
 * @folio: The folio to wait on.
 *
 * Wait for PG_private_2 (aka PG_fscache) to be cleared on a folio.
 */
void folio_wait_private_2(struct folio *folio)
{
	while (folio_test_private_2(folio))
		folio_wait_bit(folio, PG_private_2);
}
EXPORT_SYMBOL(folio_wait_private_2);

/**
 * folio_wait_private_2_killable - Wait for PG_private_2 to be cleared on a folio.
 * @folio: The folio to wait on.
 *
 * Wait for PG_private_2 (aka PG_fscache) to be cleared on a folio or until a
 * fatal signal is received by the calling task.
 *
 * Return:
 * - 0 if successful.
 * - -EINTR if a fatal signal was encountered.
 */
int folio_wait_private_2_killable(struct folio *folio)
{
	int ret = 0;

	while (folio_test_private_2(folio)) {
		ret = folio_wait_bit_killable(folio, PG_private_2);
		if (ret < 0)
			break;
	}

	return ret;
}
EXPORT_SYMBOL(folio_wait_private_2_killable);

/**
 * folio_end_writeback - End writeback against a folio.
 * @folio: The folio.
 */
void folio_end_writeback(struct folio *folio)
{
	/*
	 * folio_test_clear_reclaim() could be used here but it is an
	 * atomic operation and overkill in this particular case. Failing
	 * to shuffle a folio marked for immediate reclaim is too mild
	 * a gain to justify taking an atomic operation penalty at the
	 * end of every folio writeback.
	 */
	if (folio_test_reclaim(folio)) {
		folio_clear_reclaim(folio);
		folio_rotate_reclaimable(folio);
	}

	/*
	 * Writeback does not hold a folio reference of its own, relying
	 * on truncation to wait for the clearing of PG_writeback.
	 * But here we must make sure that the folio is not freed and
	 * reused before the folio_wake().
	 */
	folio_get(folio);
	if (!__folio_end_writeback(folio))
		BUG();

	smp_mb__after_atomic();
	folio_wake(folio, PG_writeback);
	acct_reclaim_writeback(folio);
	folio_put(folio);
}
EXPORT_SYMBOL(folio_end_writeback);

/*
 * After completing I/O on a page, call this routine to update the page
 * flags appropriately
 */
void page_endio(struct page *page, bool is_write, int err)
{
	if (!is_write) {
		if (!err) {
			SetPageUptodate(page);
		} else {
			ClearPageUptodate(page);
			SetPageError(page);
		}
		unlock_page(page);
	} else {
		if (err) {
			struct address_space *mapping;

			SetPageError(page);
			mapping = page_mapping(page);
			if (mapping)
				mapping_set_error(mapping, err);
		}
		end_page_writeback(page);
	}
}
EXPORT_SYMBOL_GPL(page_endio);

/**
 * __folio_lock - Get a lock on the folio, assuming we need to sleep to get it.
 * @folio: The folio to lock
 */
void __folio_lock(struct folio *folio)
{
	folio_wait_bit_common(folio, PG_locked, TASK_UNINTERRUPTIBLE,
				EXCLUSIVE);
}
EXPORT_SYMBOL(__folio_lock);

int __folio_lock_killable(struct folio *folio)
{
	return folio_wait_bit_common(folio, PG_locked, TASK_KILLABLE,
					EXCLUSIVE);
}
EXPORT_SYMBOL_GPL(__folio_lock_killable);

static int __folio_lock_async(struct folio *folio, struct wait_page_queue *wait)
{
	struct wait_queue_head *q = folio_waitqueue(folio);
	int ret = 0;

	wait->folio = folio;
	wait->bit_nr = PG_locked;

	spin_lock_irq(&q->lock);
	__add_wait_queue_entry_tail(q, &wait->wait);
	folio_set_waiters(folio);
	ret = !folio_trylock(folio);
	/*
	 * If we were successful now, we know we're still on the
	 * waitqueue as we're still under the lock. This means it's
	 * safe to remove and return success, we know the callback
	 * isn't going to trigger.
	 */
	if (!ret)
		__remove_wait_queue(q, &wait->wait);
	else
		ret = -EIOCBQUEUED;
	spin_unlock_irq(&q->lock);
	return ret;
}

/*
 * Return values:
 * true - folio is locked; mmap_lock is still held.
 * false - folio is not locked.
 *     mmap_lock has been released (mmap_read_unlock(), unless flags had both
 *     FAULT_FLAG_ALLOW_RETRY and FAULT_FLAG_RETRY_NOWAIT set, in
 *     which case mmap_lock is still held.
 *
 * If neither ALLOW_RETRY nor KILLABLE are set, will always return true
 * with the folio locked and the mmap_lock unperturbed.
 */
bool __folio_lock_or_retry(struct folio *folio, struct mm_struct *mm,
			 unsigned int flags)
{
	if (fault_flag_allow_retry_first(flags)) {
		/*
		 * CAUTION! In this case, mmap_lock is not released
		 * even though return 0.
		 */
		if (flags & FAULT_FLAG_RETRY_NOWAIT)
			return false;

		mmap_read_unlock(mm);
		if (flags & FAULT_FLAG_KILLABLE)
			folio_wait_locked_killable(folio);
		else
			folio_wait_locked(folio);
		return false;
	}
	if (flags & FAULT_FLAG_KILLABLE) {
		bool ret;

		ret = __folio_lock_killable(folio);
		if (ret) {
			mmap_read_unlock(mm);
			return false;
		}
	} else {
		__folio_lock(folio);
	}

	return true;
}

/**
 * page_cache_next_miss() - Find the next gap in the page cache.
 * @mapping: Mapping.
 * @index: Index.
 * @max_scan: Maximum range to search.
 *
 * Search the range [index, min(index + max_scan - 1, ULONG_MAX)] for the
 * gap with the lowest index.
 *
 * This function may be called under the rcu_read_lock.  However, this will
 * not atomically search a snapshot of the cache at a single point in time.
 * For example, if a gap is created at index 5, then subsequently a gap is
 * created at index 10, page_cache_next_miss covering both indices may
 * return 10 if called under the rcu_read_lock.
 *
 * Return: The index of the gap if found, otherwise an index outside the
 * range specified (in which case 'return - index >= max_scan' will be true).
 * In the rare case of index wrap-around, 0 will be returned.
 */
pgoff_t page_cache_next_miss(struct address_space *mapping,
			     pgoff_t index, unsigned long max_scan)
{
	XA_STATE(xas, &mapping->i_pages, index);

	while (max_scan--) {
		void *entry = xas_next(&xas);
		if (!entry || xa_is_value(entry))
			break;
		if (xas.xa_index == 0)
			break;
	}

	return xas.xa_index;
}
EXPORT_SYMBOL(page_cache_next_miss);

/**
 * page_cache_prev_miss() - Find the previous gap in the page cache.
 * @mapping: Mapping.
 * @index: Index.
 * @max_scan: Maximum range to search.
 *
 * Search the range [max(index - max_scan + 1, 0), index] for the
 * gap with the highest index.
 *
 * This function may be called under the rcu_read_lock.  However, this will
 * not atomically search a snapshot of the cache at a single point in time.
 * For example, if a gap is created at index 10, then subsequently a gap is
 * created at index 5, page_cache_prev_miss() covering both indices may
 * return 5 if called under the rcu_read_lock.
 *
 * Return: The index of the gap if found, otherwise an index outside the
 * range specified (in which case 'index - return >= max_scan' will be true).
 * In the rare case of wrap-around, ULONG_MAX will be returned.
 */
pgoff_t page_cache_prev_miss(struct address_space *mapping,
			     pgoff_t index, unsigned long max_scan)
{
	XA_STATE(xas, &mapping->i_pages, index);

	while (max_scan--) {
		void *entry = xas_prev(&xas);
		if (!entry || xa_is_value(entry))
			break;
		if (xas.xa_index == ULONG_MAX)
			break;
	}

	return xas.xa_index;
}
EXPORT_SYMBOL(page_cache_prev_miss);

/*
 * Lockless page cache protocol:
 * On the lookup side:
 * 1. Load the folio from i_pages
 * 2. Increment the refcount if it's not zero
 * 3. If the folio is not found by xas_reload(), put the refcount and retry
 *
 * On the removal side:
 * A. Freeze the page (by zeroing the refcount if nobody else has a reference)
 * B. Remove the page from i_pages
 * C. Return the page to the page allocator
 *
 * This means that any page may have its reference count temporarily
 * increased by a speculative page cache (or fast GUP) lookup as it can
 * be allocated by another user before the RCU grace period expires.
 * Because the refcount temporarily acquired here may end up being the
 * last refcount on the page, any page allocation must be freeable by
 * folio_put().
 */

/*
 * mapping_get_entry - Get a page cache entry.
 * @mapping: the address_space to search
 * @index: The page cache index.
 *
 * Looks up the page cache entry at @mapping & @index.  If it is a folio,
 * it is returned with an increased refcount.  If it is a shadow entry
 * of a previously evicted folio, or a swap entry from shmem/tmpfs,
 * it is returned without further action.
 *
 * Return: The folio, swap or shadow entry, %NULL if nothing is found.
 */
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
static void *mapping_get_entry_for_file_area(struct address_space *mapping, pgoff_t index)
{
	//XA_STATE(xas, &mapping->i_pages, index);
	//page索引除以2，转成file_area索引
	XA_STATE(xas, &mapping->i_pages, index>>PAGE_COUNT_IN_AREA_SHIFT);
	struct folio *folio;
	
	struct file_stat *p_file_stat;
	struct file_area *p_file_area;
	//令page索引与上0x3得到它在file_area的pages[]数组的下标
	unsigned int page_offset_in_file_area = index & PAGE_COUNT_IN_AREA_MASK;
	rcu_read_lock();

	p_file_stat = (struct file_stat *)mapping->rh_reserved1;
	if(p_file_stat && !file_stat_in_delete(p_file_stat)){
		    //如果此时这个file_area正在被释放，这里还能正常被使用吗？用了rcu机制做防护，后续会写详细分析!!!!!!!!!!!!!!!!!!!!!
            p_file_area = find_file_area_from_xarray_cache_node(&xas,p_file_stat,index);
            if(p_file_area){
				//令page索引与上0x3得到它在file_area的pages[]数组的下标
				folio = p_file_area->pages[page_offset_in_file_area];
				if(folio && folio->index == index){
				    xarray_tree_node_cache_hit ++;
				    goto find_folio;
			    }
				/*走到这里，说明找到了file_area但没有找到匹配索引的page。那就重置xas，重新重xarray tree查找。能否这里直接返回NULL，
				 *即判断为查找page失败呢?不能，因为此时其他进程可能也在并发执行__filemap_add_folio、mapping_get_entry、page_cache_delete
				 *并发修改p_file_stat->xa_node_cache和p_file_stat->xa_node_cache_base_index，导致二者不匹配，即不代表同一个node节点。只能重置重新查找了*/
				xas.xa_offset = 0;
				xas.xa_node = XAS_RESTART;
	       }
    }

repeat:
	xas_reset(&xas);

	//folio = xas_load(&xas);
	p_file_area = xas_load(&xas);
	if(!p_file_area)
		goto out;

	//if (xas_retry(&xas, folio))
	if (xas_retry(&xas, p_file_area))
		goto repeat;

	p_file_area = entry_to_file_area(p_file_area);
    folio = p_file_area->pages[page_offset_in_file_area];

	//if (!folio || xa_is_value(folio))
	if (!folio /*|| xa_is_value(p_file_area)*/)//xa_is_value()只是看bit0是否是1，其他bit位不用管
		goto out;

find_folio:
	if (!folio_try_get_rcu(folio))
		goto repeat;

	//if (unlikely(folio != xas_reload(&xas))) {
	if (unlikely(folio != rcu_dereference(p_file_area->pages[page_offset_in_file_area]))) {
		folio_put(folio);
		goto repeat;
	}
    //统计page引用计数
	hot_file_update_file_status(mapping,p_file_stat,p_file_area,1);

    //如果本次查找的page所在xarray tree的父节点变化了，则把最新的保存到mapping->rh_reserved2
	if(p_file_stat->xa_node_cache != xas.xa_node){
	    /*保存父节点node和这个node节点slots里最小的page索引。这两个赋值可能被多进程并发赋值，导致
	     *mapping->rh_reserved2和mapping->rh_reserved3 可能不是同一个node节点的，错乱了。这就有大问题了！
	     *没事，这种情况上边的if(page && page->index == offset)就会不成立了*/
	    p_file_stat->xa_node_cache = xas.xa_node;
	    p_file_stat->xa_node_cache_base_index = index & (~FILE_AREA_PAGE_COUNT_MASK);
	}

out:
	rcu_read_unlock();

	return folio;
}
#endif
static void *mapping_get_entry(struct address_space *mapping, pgoff_t index)
{
	XA_STATE(xas, &mapping->i_pages, index);
	struct folio *folio;

#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
find_file_area:
	/*如果此时有进程在__filemap_add_folio()分配file_stat并赋值给mapping->rh_reserved1,则当前进程在当前函数
	 * 不能立即看到mapping->rh_reserved1被赋值了，还是老的值NULL。于是继续执行rcu_read_lock()后边的代码在xarray tree直接查找page
	 * 这样就出现错乱了。__filemap_add_folio()中向xarray tree中保存的是file_area，这里却是从xarray tree查找page。怎么避免？
	 * 此时下边的代码 folio = xas_load(&xas)或 folio = xas_next(&xas) 查找到的folio是file_area_entry，那就goto find_file_area
	 * 重新跳到filemap_get_read_batch_for_file_area()去xarray tree查找file_area
	 * */
	if(mapping->rh_reserved1){
		return mapping_get_entry_for_file_area(mapping,index);
	}
#endif	

	rcu_read_lock();
repeat:
	xas_reset(&xas);
	folio = xas_load(&xas);
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
		if(is_file_area_entry(folio)){
            if(0 == mapping->rh_reserved1)
				panic("%s mapping:0x%llx NULL\n",__func__,(u64)mapping);

			printk("%s find folio:0x%llx\n",__func__,(u64)folio);
			//goto 前rcu必须解锁
			rcu_read_unlock();
			goto find_file_area;
		}
#endif

	if (xas_retry(&xas, folio))
		goto repeat;
	/*
	 * A shadow entry of a recently evicted page, or a swap entry from
	 * shmem/tmpfs.  Return it without attempting to raise page count.
	 */
	if (!folio || xa_is_value(folio))
		goto out;

	if (!folio_try_get_rcu(folio))
		goto repeat;

	if (unlikely(folio != xas_reload(&xas))) {
		folio_put(folio);
		goto repeat;
	}
out:
	rcu_read_unlock();

	return folio;
}
/**
 * __filemap_get_folio - Find and get a reference to a folio.
 * @mapping: The address_space to search.
 * @index: The page index.
 * @fgp_flags: %FGP flags modify how the folio is returned.
 * @gfp: Memory allocation flags to use if %FGP_CREAT is specified.
 *
 * Looks up the page cache entry at @mapping & @index.
 *
 * @fgp_flags can be zero or more of these flags:
 *
 * * %FGP_ACCESSED - The folio will be marked accessed.
 * * %FGP_LOCK - The folio is returned locked.
 * * %FGP_ENTRY - If there is a shadow / swap / DAX entry, return it
 *   instead of allocating a new folio to replace it.
 * * %FGP_CREAT - If no page is present then a new page is allocated using
 *   @gfp and added to the page cache and the VM's LRU list.
 *   The page is returned locked and with an increased refcount.
 * * %FGP_FOR_MMAP - The caller wants to do its own locking dance if the
 *   page is already in cache.  If the page was allocated, unlock it before
 *   returning so the caller can do the same dance.
 * * %FGP_WRITE - The page will be written to by the caller.
 * * %FGP_NOFS - __GFP_FS will get cleared in gfp.
 * * %FGP_NOWAIT - Don't get blocked by page lock.
 * * %FGP_STABLE - Wait for the folio to be stable (finished writeback)
 *
 * If %FGP_LOCK or %FGP_CREAT are specified then the function may sleep even
 * if the %GFP flags specified for %FGP_CREAT are atomic.
 *
 * If there is a page cache page, it is returned with an increased refcount.
 *
 * Return: The found folio or %NULL otherwise.
 */
struct folio *__filemap_get_folio(struct address_space *mapping, pgoff_t index,
		int fgp_flags, gfp_t gfp)
{
	struct folio *folio;

repeat:
	folio = mapping_get_entry(mapping, index);
	if (xa_is_value(folio)) {
		if (fgp_flags & FGP_ENTRY)
			return folio;
		folio = NULL;
	}
	if (!folio)
		goto no_page;

	if (fgp_flags & FGP_LOCK) {
		if (fgp_flags & FGP_NOWAIT) {
			if (!folio_trylock(folio)) {
				folio_put(folio);
				return NULL;
			}
		} else {
			folio_lock(folio);
		}

		/* Has the page been truncated? */
		if (unlikely(folio->mapping != mapping)) {
			folio_unlock(folio);
			folio_put(folio);
			goto repeat;
		}
		VM_BUG_ON_FOLIO(!folio_contains(folio, index), folio);
	}

	if (fgp_flags & FGP_ACCESSED)
		folio_mark_accessed(folio);
	else if (fgp_flags & FGP_WRITE) {
		/* Clear idle flag for buffer write */
		if (folio_test_idle(folio))
			folio_clear_idle(folio);
	}

	if (fgp_flags & FGP_STABLE)
		folio_wait_stable(folio);
no_page:
	if (!folio && (fgp_flags & FGP_CREAT)) {
		int err;
		if ((fgp_flags & FGP_WRITE) && mapping_can_writeback(mapping))
			gfp |= __GFP_WRITE;
		if (fgp_flags & FGP_NOFS)
			gfp &= ~__GFP_FS;

		folio = filemap_alloc_folio(gfp, 0);
		if (!folio)
			return NULL;

		if (WARN_ON_ONCE(!(fgp_flags & (FGP_LOCK | FGP_FOR_MMAP))))
			fgp_flags |= FGP_LOCK;

		/* Init accessed so avoid atomic mark_page_accessed later */
		if (fgp_flags & FGP_ACCESSED)
			__folio_set_referenced(folio);

		err = filemap_add_folio(mapping, folio, index, gfp);
		if (unlikely(err)) {
			folio_put(folio);
			folio = NULL;
			if (err == -EEXIST)
				goto repeat;
		}

		/*
		 * filemap_add_folio locks the page, and for mmap
		 * we expect an unlocked page.
		 */
		if (folio && (fgp_flags & FGP_FOR_MMAP))
			folio_unlock(folio);
	}

	return folio;
}
EXPORT_SYMBOL(__filemap_get_folio);
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
static inline struct folio *find_get_entry_for_file_area(struct xa_state *xas, pgoff_t max,
		xa_mark_t mark)
{
	struct folio *folio;
	struct file_area *p_file_area;
	pgoff_t index = xas->xa_index;
	unsigned int page_offset_in_file_area = index & PAGE_COUNT_IN_AREA_MASK;
	//page索引转换位file_area索引
	xas->xa_index = xas->xa_index >> PAGE_COUNT_IN_AREA_SHIFT;
	//令page索引与上0x3得到它在file_area的pages[]数组的下标

retry:
	if (mark == XA_PRESENT)
		//folio = xas_find(xas, max);
		p_file_area = xas_find(xas, max);
	else
		//folio = xas_find_marked(xas, max, mark);
		p_file_area = xas_find_marked(xas, max, mark);

	if (!p_file_area)
		return NULL;

	//if (xas_retry(xas, folio))
	if (xas_retry(xas, p_file_area))
		goto retry;
	/*
	 * A shadow entry of a recently evicted page, a swap
	 * entry from shmem/tmpfs or a DAX entry.  Return it
	 * without attempting to raise page count.
	 */
	//if (!folio || xa_is_value(folio))
	//	return folio;
    p_file_area = entry_to_file_area(p_file_area);	
	folio = p_file_area->pages[page_offset_in_file_area];
	/*注意，原本是xas_find()函数里找到max索引的page时，返回NULL。如果page索引不是4对齐，那就可能是file_area->pages[]
	 *数组里的page正好就大于max，因此这个判断就放在这里了*/
	if(!folio || folio->index > max)
		return NULL;

	if (!folio_try_get_rcu(folio))
		goto reset;

	//if (unlikely(folio != xas_reload(xas))) {
	if (unlikely(folio != rcu_dereference(p_file_area->pages[page_offset_in_file_area]))) {
		folio_put(folio);
		goto reset;
	}

	return folio;
reset:
	xas_reset(xas);
	goto retry;
}
#endif
static inline struct folio *find_get_entry(struct xa_state *xas, pgoff_t max,
		xa_mark_t mark)
{
	struct folio *folio;

retry:
	if (mark == XA_PRESENT)
		folio = xas_find(xas, max);
	else
		folio = xas_find_marked(xas, max, mark);

	if (xas_retry(xas, folio))
		goto retry;
	/*
	 * A shadow entry of a recently evicted page, a swap
	 * entry from shmem/tmpfs or a DAX entry.  Return it
	 * without attempting to raise page count.
	 */
	if (!folio || xa_is_value(folio))
		return folio;

	if (!folio_try_get_rcu(folio))
		goto reset;

	if (unlikely(folio != xas_reload(xas))) {
		folio_put(folio);
		goto reset;
	}

	return folio;
reset:
	xas_reset(xas);
	goto retry;
}

/**
 * find_get_entries - gang pagecache lookup
 * @mapping:	The address_space to search
 * @start:	The starting page cache index
 * @end:	The final page index (inclusive).
 * @fbatch:	Where the resulting entries are placed.
 * @indices:	The cache indices corresponding to the entries in @entries
 *
 * find_get_entries() will search for and return a batch of entries in
 * the mapping.  The entries are placed in @fbatch.  find_get_entries()
 * takes a reference on any actual folios it returns.
 *
 * The entries have ascending indexes.  The indices may not be consecutive
 * due to not-present entries or large folios.
 *
 * Any shadow entries of evicted folios, or swap entries from
 * shmem/tmpfs, are included in the returned array.
 *
 * Return: The number of entries which were found.
 */
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
unsigned find_get_entries_for_file_area(struct address_space *mapping, pgoff_t start,
		pgoff_t end, struct folio_batch *fbatch, pgoff_t *indices)
{
	XA_STATE(xas, &mapping->i_pages, start);
	struct folio *folio;

	rcu_read_lock();
	while ((folio = find_get_entry_for_file_area(&xas, end, XA_PRESENT)) != NULL) {
		//indices[fbatch->nr] = xas.xa_index; xax.xa_index现在代表的是file_area索引，不是page索引
		indices[fbatch->nr] = folio->index;
		if (!folio_batch_add(fbatch, folio))
			break;
	}
	rcu_read_unlock();

	return folio_batch_count(fbatch);
}
#endif
unsigned find_get_entries(struct address_space *mapping, pgoff_t start,
		pgoff_t end, struct folio_batch *fbatch, pgoff_t *indices)
{
	XA_STATE(xas, &mapping->i_pages, start);
	struct folio *folio;

#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
find_file_area:
	if(mapping->rh_reserved1)
		find_get_entries_for_file_area(mapping,start,end,fbatch,indices);
#endif

	rcu_read_lock();
	while ((folio = find_get_entry(&xas, end, XA_PRESENT)) != NULL) {
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
		if(is_file_area_entry(folio)){
            if(0 == mapping->rh_reserved1)
				panic("%s mapping:0x%llx NULL\n",__func__,(u64)mapping);

			printk("%s find folio:0x%llx\n",__func__,(u64)folio);
			//goto 前rcu必须解锁
			rcu_read_unlock();
			goto find_file_area;
		}
#endif
		indices[fbatch->nr] = xas.xa_index;
		if (!folio_batch_add(fbatch, folio))
			break;
	}
	rcu_read_unlock();

	return folio_batch_count(fbatch);
}

/**
 * find_lock_entries - Find a batch of pagecache entries.
 * @mapping:	The address_space to search.
 * @start:	The starting page cache index.
 * @end:	The final page index (inclusive).
 * @fbatch:	Where the resulting entries are placed.
 * @indices:	The cache indices of the entries in @fbatch.
 *
 * find_lock_entries() will return a batch of entries from @mapping.
 * Swap, shadow and DAX entries are included.  Folios are returned
 * locked and with an incremented refcount.  Folios which are locked
 * by somebody else or under writeback are skipped.  Folios which are
 * partially outside the range are not returned.
 *
 * The entries have ascending indexes.  The indices may not be consecutive
 * due to not-present entries, large folios, folios which could not be
 * locked or folios under writeback.
 *
 * Return: The number of entries which were found.
 */
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
unsigned find_lock_entries_for_file_area(struct address_space *mapping, pgoff_t start,
		pgoff_t end, struct folio_batch *fbatch, pgoff_t *indices)
{
	XA_STATE(xas, &mapping->i_pages, start);
	struct folio *folio;

	rcu_read_lock();
	while ((folio = find_get_entry_for_file_area(&xas, end, XA_PRESENT))) {
		if (!xa_is_value(folio)) {
			if (folio->index < start)
				goto put;
			if (folio->index + folio_nr_pages(folio) - 1 > end)
				goto put;
			if (!folio_trylock(folio))
				goto put;
			if (folio->mapping != mapping ||
			    folio_test_writeback(folio))
				goto unlock;
			//VM_BUG_ON_FOLIO(!folio_contains(folio, xas.xa_index),
			VM_BUG_ON_FOLIO(!folio_contains(folio, folio->index),
					folio);
		}
		//indices[fbatch->nr] = xas.xa_index;
		indices[fbatch->nr] = folio->index;
		if (!folio_batch_add(fbatch, folio))
			break;
		continue;
unlock:
		folio_unlock(folio);
put:
		folio_put(folio);
	}
	rcu_read_unlock();

	return folio_batch_count(fbatch);
}
#endif
unsigned find_lock_entries(struct address_space *mapping, pgoff_t start,
		pgoff_t end, struct folio_batch *fbatch, pgoff_t *indices)
{
	XA_STATE(xas, &mapping->i_pages, start);
	struct folio *folio;

#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
find_file_area:
	if(mapping->rh_reserved1)
		return find_lock_entries_for_file_area(mapping,start,end,fbatch,indices);
#endif

	rcu_read_lock();
	while ((folio = find_get_entry(&xas, end, XA_PRESENT))) {
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
		if(is_file_area_entry(folio)){
            if(0 == mapping->rh_reserved1)
				panic("%s mapping:0x%llx NULL\n",__func__,(u64)mapping);

			printk("%s find folio:0x%llx\n",__func__,(u64)folio);
			//goto 前rcu必须解锁
			rcu_read_unlock();
			goto find_file_area;
		}
#endif
		if (!xa_is_value(folio)) {
			if (folio->index < start)
				goto put;
			if (folio->index + folio_nr_pages(folio) - 1 > end)
				goto put;
			if (!folio_trylock(folio))
				goto put;
			if (folio->mapping != mapping ||
			    folio_test_writeback(folio))
				goto unlock;
			VM_BUG_ON_FOLIO(!folio_contains(folio, xas.xa_index),
					folio);
		}
		indices[fbatch->nr] = xas.xa_index;
		if (!folio_batch_add(fbatch, folio))
			break;
		continue;
unlock:
		folio_unlock(folio);
put:
		folio_put(folio);
	}
	rcu_read_unlock();

	return folio_batch_count(fbatch);
}

static inline
bool folio_more_pages(struct folio *folio, pgoff_t index, pgoff_t max)
{
	if (!folio_test_large(folio) || folio_test_hugetlb(folio))
		return false;
	if (index >= max)
		return false;
	return index < folio->index + folio_nr_pages(folio) - 1;
}

/**
 * find_get_pages_range - gang pagecache lookup
 * @mapping:	The address_space to search
 * @start:	The starting page index
 * @end:	The final page index (inclusive)
 * @nr_pages:	The maximum number of pages
 * @pages:	Where the resulting pages are placed
 *
 * find_get_pages_range() will search for and return a group of up to @nr_pages
 * pages in the mapping starting at index @start and up to index @end
 * (inclusive).  The pages are placed at @pages.  find_get_pages_range() takes
 * a reference against the returned pages.
 *
 * The search returns a group of mapping-contiguous pages with ascending
 * indexes.  There may be holes in the indices due to not-present pages.
 * We also update @start to index the next page for the traversal.
 *
 * Return: the number of pages which were found. If this number is
 * smaller than @nr_pages, the end of specified range has been
 * reached.
 */
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
unsigned find_get_pages_range_for_file_area(struct address_space *mapping, pgoff_t *start,
			      pgoff_t end, unsigned int nr_pages,
			      struct page **pages)
{
	XA_STATE(xas, &mapping->i_pages, *start);
	struct folio *folio;
	unsigned ret = 0;

	if (unlikely(!nr_pages))
		return 0;

	rcu_read_lock();
	while ((folio = find_get_entry(&xas, end, XA_PRESENT))) {
		/* Skip over shadow, swap and DAX entries */
		if (xa_is_value(folio))
			continue;

//again:
		if(folio_nr_pages(folio) > 1)
			panic("%s folio:0x%llx folio_nr_pages > 1 %ld\n",__func__,(u64)folio,folio_nr_pages(folio));

		//pages[ret] = folio_file_page(folio, xas.xa_index);
		pages[ret] = folio_file_page(folio, folio->index);
		if (++ret == nr_pages) {
			//*start = xas.xa_index + 1;
			*start = folio->index + 1;
			goto out;
		}
		/*
		if (folio_more_pages(folio, xas.xa_index, end)) {
			xas.xa_index++;
			folio_ref_inc(folio);
			goto again;
		}*/
	}

	/*
	 * We come here when there is no page beyond @end. We take care to not
	 * overflow the index @start as it confuses some of the callers. This
	 * breaks the iteration when there is a page at index -1 but that is
	 * already broken anyway.
	 */
	if (end == (pgoff_t)-1)
		*start = (pgoff_t)-1;
	else
		*start = end + 1;
out:
	rcu_read_unlock();

	return ret;
}
#endif
unsigned find_get_pages_range(struct address_space *mapping, pgoff_t *start,
			      pgoff_t end, unsigned int nr_pages,
			      struct page **pages)
{
	XA_STATE(xas, &mapping->i_pages, *start);
	struct folio *folio;
	unsigned ret = 0;

#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
find_file_area:
	if(mapping->rh_reserved1)
		return find_get_pages_range_for_file_area(mapping,start,end,nr_pages,pages);
#endif

	if (unlikely(!nr_pages))
		return 0;

	rcu_read_lock();
	while ((folio = find_get_entry(&xas, end, XA_PRESENT))) {
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
		if(is_file_area_entry(folio)){
            if(0 == mapping->rh_reserved1)
				panic("%s mapping:0x%llx NULL\n",__func__,(u64)mapping);

			printk("%s find folio:0x%llx\n",__func__,(u64)folio);
			//goto 前rcu必须解锁
			rcu_read_unlock();
			goto find_file_area;
		}
#endif
		/* Skip over shadow, swap and DAX entries */
		if (xa_is_value(folio))
			continue;

again:
		pages[ret] = folio_file_page(folio, xas.xa_index);
		if (++ret == nr_pages) {
			*start = xas.xa_index + 1;
			goto out;
		}
		if (folio_more_pages(folio, xas.xa_index, end)) {
			xas.xa_index++;
			folio_ref_inc(folio);
			goto again;
		}
	}

	/*
	 * We come here when there is no page beyond @end. We take care to not
	 * overflow the index @start as it confuses some of the callers. This
	 * breaks the iteration when there is a page at index -1 but that is
	 * already broken anyway.
	 */
	if (end == (pgoff_t)-1)
		*start = (pgoff_t)-1;
	else
		*start = end + 1;
out:
	rcu_read_unlock();

	return ret;
}

/**
 * find_get_pages_contig - gang contiguous pagecache lookup
 * @mapping:	The address_space to search
 * @index:	The starting page index
 * @nr_pages:	The maximum number of pages
 * @pages:	Where the resulting pages are placed
 *
 * find_get_pages_contig() works exactly like find_get_pages_range(),
 * except that the returned number of pages are guaranteed to be
 * contiguous.
 *
 * Return: the number of pages which were found.
 */
unsigned find_get_pages_contig(struct address_space *mapping, pgoff_t index,
			       unsigned int nr_pages, struct page **pages)
{
	XA_STATE(xas, &mapping->i_pages, index);
	struct folio *folio;
	unsigned int ret = 0;

	if (unlikely(!nr_pages))
		return 0;

	rcu_read_lock();
	for (folio = xas_load(&xas); folio; folio = xas_next(&xas)) {
		if (xas_retry(&xas, folio))
			continue;
		/*
		 * If the entry has been swapped out, we can stop looking.
		 * No current caller is looking for DAX entries.
		 */
		if (xa_is_value(folio))
			break;

		if (!folio_try_get_rcu(folio))
			goto retry;

		if (unlikely(folio != xas_reload(&xas)))
			goto put_page;

again:
		pages[ret] = folio_file_page(folio, xas.xa_index);
		if (++ret == nr_pages)
			break;
		if (folio_more_pages(folio, xas.xa_index, ULONG_MAX)) {
			xas.xa_index++;
			folio_ref_inc(folio);
			goto again;
		}
		continue;
put_page:
		folio_put(folio);
retry:
		xas_reset(&xas);
	}
	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL(find_get_pages_contig);

/**
 * find_get_pages_range_tag - Find and return head pages matching @tag.
 * @mapping:	the address_space to search
 * @index:	the starting page index
 * @end:	The final page index (inclusive)
 * @tag:	the tag index
 * @nr_pages:	the maximum number of pages
 * @pages:	where the resulting pages are placed
 *
 * Like find_get_pages_range(), except we only return head pages which are
 * tagged with @tag.  @index is updated to the index immediately after the
 * last page we return, ready for the next iteration.
 *
 * Return: the number of pages which were found.
 */
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
unsigned find_get_pages_range_tag_for_file_area(struct address_space *mapping, pgoff_t *index,
			pgoff_t end, xa_mark_t tag, unsigned int nr_pages,
			struct page **pages)
{
	XA_STATE(xas, &mapping->i_pages, *index);
	struct folio *folio;
	unsigned ret = 0;

	if (unlikely(!nr_pages))
		return 0;

	rcu_read_lock();
	while ((folio = find_get_entry_for_file_area(&xas, end, tag))) {
		/*
		 * Shadow entries should never be tagged, but this iteration
		 * is lockless so there is a window for page reclaim to evict
		 * a page we saw tagged.  Skip over it.
		 */
		if (xa_is_value(folio))
			continue;

		if(folio_nr_pages(folio) > 1)
			panic("%s folio:0x%llx folio_nr_pages > 1 %ld\n",__func__,(u64)folio,folio_nr_pages(folio));

		pages[ret] = &folio->page;
		if (++ret == nr_pages) {
			*index = folio->index + folio_nr_pages(folio);
			goto out;
		}
	}

	/*
	 * We come here when we got to @end. We take care to not overflow the
	 * index @index as it confuses some of the callers. This breaks the
	 * iteration when there is a page at index -1 but that is already
	 * broken anyway.
	 */
	if (end == (pgoff_t)-1)
		*index = (pgoff_t)-1;
	else
		*index = end + 1;
out:
	rcu_read_unlock();

	return ret;
}
#endif
unsigned find_get_pages_range_tag(struct address_space *mapping, pgoff_t *index,
			pgoff_t end, xa_mark_t tag, unsigned int nr_pages,
			struct page **pages)
{
	XA_STATE(xas, &mapping->i_pages, *index);
	struct folio *folio;
	unsigned ret = 0;

#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
find_file_area:
	if(mapping->rh_reserved1)
		return find_get_pages_range_tag_for_file_area(mapping,index,end,tag,nr_pages,pages);
#endif

	if (unlikely(!nr_pages))
		return 0;

	rcu_read_lock();
	while ((folio = find_get_entry(&xas, end, tag))) {
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
		if(is_file_area_entry(folio)){
            if(0 == mapping->rh_reserved1)
				panic("%s mapping:0x%llx NULL\n",__func__,(u64)mapping);

			printk("%s find folio:0x%llx\n",__func__,(u64)folio);
			//goto 前rcu必须解锁
			rcu_read_unlock();
			goto find_file_area;
		}
#endif
		/*
		 * Shadow entries should never be tagged, but this iteration
		 * is lockless so there is a window for page reclaim to evict
		 * a page we saw tagged.  Skip over it.
		 */
		if (xa_is_value(folio))
			continue;

		pages[ret] = &folio->page;
		if (++ret == nr_pages) {
			*index = folio->index + folio_nr_pages(folio);
			goto out;
		}
	}

	/*
	 * We come here when we got to @end. We take care to not overflow the
	 * index @index as it confuses some of the callers. This breaks the
	 * iteration when there is a page at index -1 but that is already
	 * broken anyway.
	 */
	if (end == (pgoff_t)-1)
		*index = (pgoff_t)-1;
	else
		*index = end + 1;
out:
	rcu_read_unlock();

	return ret;
}
EXPORT_SYMBOL(find_get_pages_range_tag);

/*
 * CD/DVDs are error prone. When a medium error occurs, the driver may fail
 * a _large_ part of the i/o request. Imagine the worst scenario:
 *
 *      ---R__________________________________________B__________
 *         ^ reading here                             ^ bad block(assume 4k)
 *
 * read(R) => miss => readahead(R...B) => media error => frustrating retries
 * => failing the whole request => read(R) => read(R+1) =>
 * readahead(R+1...B+1) => bang => read(R+2) => read(R+3) =>
 * readahead(R+3...B+2) => bang => read(R+3) => read(R+4) =>
 * readahead(R+4...B+3) => bang => read(R+4) => read(R+5) => ......
 *
 * It is going insane. Fix it by quickly scaling down the readahead size.
 */
static void shrink_readahead_size_eio(struct file_ra_state *ra)
{
	ra->ra_pages /= 4;
}

/*
 * filemap_get_read_batch - Get a batch of folios for read
 *
 * Get a batch of folios which represent a contiguous range of bytes in
 * the file.  No exceptional entries will be returned.  If @index is in
 * the middle of a folio, the entire folio will be returned.  The last
 * folio in the batch may have the readahead flag set or the uptodate flag
 * clear so that the caller can take the appropriate action.
 */
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
static void filemap_get_read_batch_for_file_area(struct address_space *mapping,
		pgoff_t index, pgoff_t max, struct folio_batch *fbatch)
{
	//XA_STATE(xas, &mapping->i_pages, index);
	XA_STATE(xas, &mapping->i_pages, index>>PAGE_COUNT_IN_AREA_SHIFT);
	struct folio *folio;

	struct file_stat *p_file_stat;
	struct file_area *p_file_area;
	int page_offset_in_file_area;
	int page_offset_in_file_area_origin;
	int reset = 0;
	int first_file_area = 0;
	void * file_area_entry;

	rcu_read_lock();

    p_file_stat = (struct file_stat *)mapping->rh_reserved1;
	if(!file_stat_in_delete(p_file_stat)){
		    //如果此时这个file_area正在被释放，这里还能正常被使用吗？用了rcu机制做防护，后续会写详细分析!!!!!!!!!!!!!!!!!!!!!
            p_file_area = find_file_area_from_xarray_cache_node(&xas,p_file_stat,index);
            if(p_file_area)
				goto find_file_area;
		    //xas->xa_offset = 0;
		    //xas->xa_node = XAS_RESTART;
    }

	file_area_entry = xas_load(&xas);

find_file_area:	
    //得到要查找的第一个page在file_area->pages[]数组里的下标
	page_offset_in_file_area = index & PAGE_COUNT_IN_AREA_MASK;
	page_offset_in_file_area_origin  = page_offset_in_file_area;

	//for (folio = xas_load(&xas); folio; folio = xas_next(&xas)) {
	while(1){
        
		if(page_offset_in_file_area == PAGE_COUNT_IN_AREA || reset){
            file_area_entry = xas_next(&xas);

			/*如果reset置1，从xarray tree重新查找上一个file_area，此时不能对page_offset_in_file_area清0，
			 *这样继续查找上一个page*/
			if(0 == reset){
                /*统计page引用计数。如果是第一次统计，page_offset_in_file_area_origin >=0，此时访问file_area的page的访问计数是
				 * page_offset_in_file_area - page_offset_in_file_area_origin。之后，file_area的page的访问计数是page_offset_in_file_area*/
				if(page_offset_in_file_area_origin == -1)
	                hot_file_update_file_status(mapping,p_file_stat,p_file_area,page_offset_in_file_area);
				else{//访问的第一个file_area的page，访问计数是page_offset_in_file_area - page_offset_in_file_area_origin
	                hot_file_update_file_status(mapping,p_file_stat,p_file_area,page_offset_in_file_area - page_offset_in_file_area_origin);
					page_offset_in_file_area_origin = -1;
				}

				page_offset_in_file_area = 0;
            }
		}

		//1:在查找第一个file_area时，要判断file_area合法，此时page_offset_in_file_area不一定是0
		//2:后续的file_area，只在查找第一个page时才判断file_area合法，查找剩下的3个page时没必要
		//3:触发了reset强制查找xarray tree，此时也要判断一次file_area合法
		if(first_file_area == 0 || page_offset_in_file_area == 0 || reset){
			if(first_file_area == 0)
				first_file_area = 1;

			if(reset)
				reset = 0;

		    if(!file_area_entry)
			    break;

			//if (xas_retry(&xas, folio))
			if (xas_retry(&xas, file_area_entry)){
				//置1，这样下个循环folio = xas_next(&xas)才会从xarray tree查找file_area
				reset = 1;
				continue;
			}	

			/*没必要再判断file_area是xa_is_value()，xas.xa_index > max的判断放到了下边。因为xas.xa_index是file_area的索引,不是page的索引*/
			//if (xas.xa_index > max || xa_is_value(folio))
			//if (xas.xa_index  > max || xa_is_value(p_file_area))
				//break;

			//if (xa_is_sibling(folio))
			if (xa_is_sibling(file_area_entry)){
                printk("%s xa_is_sibling:0x%llx!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,(u64)file_area_entry);
				break;
			}

			p_file_area = entry_to_file_area(file_area_entry);
        }
        folio = p_file_area->pages[page_offset_in_file_area];
		//其实这个folio->index > max的判断，浪费性能，前边废了很大劲找到这个page，结果却没用。完全可以在上一个page->index == max，然后直接退出循环就行了!!!!!!!!!!!!!!!!
		if(!folio || folio->index > max)
			break;

		if (!folio_try_get_rcu(folio))
			goto retry;

		//if (unlikely(folio != xas_reload(&xas)))
	    if (unlikely(folio != rcu_dereference(p_file_area->pages[page_offset_in_file_area]))) 
			goto put_folio;

		if (!folio_batch_add(fbatch, folio))
			break;
		if (!folio_test_uptodate(folio))
			break;
		if (folio_test_readahead(folio))
			break;

		if(folio_nr_pages(folio) > 1){
            printk("%s index:%ld folio_nr_pages:%ld!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",__func__,index,folio_nr_pages(folio));
		}
        /*folio代表单个page时，看着本质是xas->xa_index = folio->index，xas->xa_offset= folio->index & XA_CHUNK_MASK。
		 *这里的核心操作是，当folio->index大于64时，folio->index & XA_CHUNK_MASK后只取出不足64的部分，即在xarray tree槽位的偏移.
		 *但是folio = xas_next(&xas)里会判断出xas->xa_offset == 63后，会自动取下一个父节点查找page*/		
		//xas_advance(&xas, folio->index + folio_nr_pages(folio) - 1);

        //file_area的page索引加1，下轮循环从file_area->pages[]得到下一个page。如果大于3则要从xarray tree查找下一个file_area
		page_offset_in_file_area ++;
		continue;
put_folio:
		folio_put(folio);
retry:
		//这里xas->xa_node = XAS_RESTART，然后folio = xas_next(&xas)里只能从xarray tree重新查找一次file_area
		xas_reset(&xas);
		//置1，这样下个循环folio = xas_next(&xas)才会从xarray tree查找file_area
		reset = 1;
	}
	/*统计page引用计数。如果是第一次统计，page_offset_in_file_area_origin >=0，此时访问file_area的page的访问计数是
	 * page_offset_in_file_area - page_offset_in_file_area_origin。之后，file_area的page的访问计数是page_offset_in_file_area*/
	if(page_offset_in_file_area_origin == -1){
		if(page_offset_in_file_area)
		    hot_file_update_file_status(mapping,p_file_stat,p_file_area,page_offset_in_file_area);
	}
	else{//访问的第一个file_area的page，访问计数是page_offset_in_file_area - page_offset_in_file_area_origin
		hot_file_update_file_status(mapping,p_file_stat,p_file_area,page_offset_in_file_area - page_offset_in_file_area_origin);
	}

	//如果本次查找的page所在xarray tree的父节点变化了，则把最新的保存到mapping->rh_reserved2
	if(p_file_stat->xa_node_cache != xas.xa_node){
	    /*保存父节点node和这个node节点slots里最小的page索引。这两个赋值可能被多进程并发赋值，导致
	     *mapping->rh_reserved2和mapping->rh_reserved3 可能不是同一个node节点的，错乱了。这就有大问题了！
	     *没事，这种情况上边的if(page && page->index == offset)就会不成立了*/
	    p_file_stat->xa_node_cache = xas.xa_node;
	    p_file_stat->xa_node_cache_base_index = index & (~FILE_AREA_PAGE_COUNT_MASK);
	}

	rcu_read_unlock();
}
#endif
static void filemap_get_read_batch(struct address_space *mapping,
		pgoff_t index, pgoff_t max, struct folio_batch *fbatch)
{
	XA_STATE(xas, &mapping->i_pages, index);
	struct folio *folio;

#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
find_file_area:
	/*如果此时有进程在__filemap_add_folio()分配file_stat并赋值给mapping->rh_reserved1,则当前进程在filemap_get_read_batch函数
	 * 不能立即看到mapping->rh_reserved1被赋值了，还是老的值NULL。于是继续执行rcu_read_lock()后边的代码在xarray tree直接查找page
	 * 这样就出现错乱了。__filemap_add_folio()中向xarray tree中保存的是file_area，这里却是从xarray tree查找page。怎么避免？
	 * 此时下边的代码 folio = xas_load(&xas)或 folio = xas_next(&xas) 查找到的folio是file_area_entry，那就goto find_file_area
	 * 重新跳到filemap_get_read_batch_for_file_area()去xarray tree查找file_area
	 * */
	if(mapping->rh_reserved1){
		/*如果fbatch->nr非0，说明下边for循环已经找到了一些page，那就清0失效，现在执行filemap_get_read_batch_for_file_area重新查找*/
		if(fbatch->nr)
			fbatch->nr = 0;
		return filemap_get_read_batch_for_file_area(mapping,index,max,fbatch);
	}
#endif	
	rcu_read_lock();
	for (folio = xas_load(&xas); folio; folio = xas_next(&xas)) {
		
#ifdef ASYNC_MEMORY_RECLAIM_IN_KERNEL
		if(is_file_area_entry(folio)){
            if(0 == mapping->rh_reserved1)
				panic("%s mapping:0x%llx NULL\n",__func__,(u64)mapping);

			printk("%s find folio:0x%llx\n",__func__,(u64)folio);
			rcu_read_unlock();
			goto find_file_area;
		}
#endif
		if (xas_retry(&xas, folio))
			continue;
		if (xas.xa_index > max || xa_is_value(folio))
			break;
		if (xa_is_sibling(folio))
			break;
		if (!folio_try_get_rcu(folio))
			goto retry;

		if (unlikely(folio != xas_reload(&xas)))
			goto put_folio;

		if (!folio_batch_add(fbatch, folio))
			break;
		if (!folio_test_uptodate(folio))
			break;
		if (folio_test_readahead(folio))
			break;
		xas_advance(&xas, folio->index + folio_nr_pages(folio) - 1);
		continue;
put_folio:
		folio_put(folio);
retry:
		xas_reset(&xas);
	}
	rcu_read_unlock();
}
static int filemap_read_folio(struct file *file, struct address_space *mapping,
		struct folio *folio)
{
	bool workingset = folio_test_workingset(folio);
	unsigned long pflags;
	int error;

	/*
	 * A previous I/O error may have been due to temporary failures,
	 * eg. multipath errors.  PG_error will be set again if readpage
	 * fails.
	 */
	folio_clear_error(folio);

	/* Start the actual read. The read will unlock the page. */
	if (unlikely(workingset))
		psi_memstall_enter(&pflags);
	error = mapping->a_ops->readpage(file, &folio->page);
	if (unlikely(workingset))
		psi_memstall_leave(&pflags);
	if (error)
		return error;

	error = folio_wait_locked_killable(folio);
	if (error)
		return error;
	if (folio_test_uptodate(folio))
		return 0;
	shrink_readahead_size_eio(&file->f_ra);
	return -EIO;
}

static bool filemap_range_uptodate(struct address_space *mapping,
		loff_t pos, struct iov_iter *iter, struct folio *folio)
{
	int count;

	if (folio_test_uptodate(folio))
		return true;
	/* pipes can't handle partially uptodate pages */
	if (iov_iter_is_pipe(iter))
		return false;
	if (!mapping->a_ops->is_partially_uptodate)
		return false;
	if (mapping->host->i_blkbits >= folio_shift(folio))
		return false;

	count = iter->count;
	if (folio_pos(folio) > pos) {
		count -= folio_pos(folio) - pos;
		pos = 0;
	} else {
		pos -= folio_pos(folio);
	}

	return mapping->a_ops->is_partially_uptodate(folio, pos, count);
}

static int filemap_update_page(struct kiocb *iocb,
		struct address_space *mapping, struct iov_iter *iter,
		struct folio *folio)
{
	int error;

	if (iocb->ki_flags & IOCB_NOWAIT) {
		if (!filemap_invalidate_trylock_shared(mapping))
			return -EAGAIN;
	} else {
		filemap_invalidate_lock_shared(mapping);
	}

	if (!folio_trylock(folio)) {
		error = -EAGAIN;
		if (iocb->ki_flags & (IOCB_NOWAIT | IOCB_NOIO))
			goto unlock_mapping;
		if (!(iocb->ki_flags & IOCB_WAITQ)) {
			filemap_invalidate_unlock_shared(mapping);
			/*
			 * This is where we usually end up waiting for a
			 * previously submitted readahead to finish.
			 */
			folio_put_wait_locked(folio, TASK_KILLABLE);
			return AOP_TRUNCATED_PAGE;
		}
		error = __folio_lock_async(folio, iocb->ki_waitq);
		if (error)
			goto unlock_mapping;
	}

	error = AOP_TRUNCATED_PAGE;
	if (!folio->mapping)
		goto unlock;

	error = 0;
	if (filemap_range_uptodate(mapping, iocb->ki_pos, iter, folio))
		goto unlock;

	error = -EAGAIN;
	if (iocb->ki_flags & (IOCB_NOIO | IOCB_NOWAIT | IOCB_WAITQ))
		goto unlock;

	error = filemap_read_folio(iocb->ki_filp, mapping, folio);
	goto unlock_mapping;
unlock:
	folio_unlock(folio);
unlock_mapping:
	filemap_invalidate_unlock_shared(mapping);
	if (error == AOP_TRUNCATED_PAGE)
		folio_put(folio);
	return error;
}

static int filemap_create_folio(struct file *file,
		struct address_space *mapping, pgoff_t index,
		struct folio_batch *fbatch)
{
	struct folio *folio;
	int error;

	folio = filemap_alloc_folio(mapping_gfp_mask(mapping), 0);
	if (!folio)
		return -ENOMEM;

	/*
	 * Protect against truncate / hole punch. Grabbing invalidate_lock
	 * here assures we cannot instantiate and bring uptodate new
	 * pagecache folios after evicting page cache during truncate
	 * and before actually freeing blocks.	Note that we could
	 * release invalidate_lock after inserting the folio into
	 * the page cache as the locked folio would then be enough to
	 * synchronize with hole punching. But there are code paths
	 * such as filemap_update_page() filling in partially uptodate
	 * pages or ->readahead() that need to hold invalidate_lock
	 * while mapping blocks for IO so let's hold the lock here as
	 * well to keep locking rules simple.
	 */
	filemap_invalidate_lock_shared(mapping);
	error = filemap_add_folio(mapping, folio, index,
			mapping_gfp_constraint(mapping, GFP_KERNEL));
	if (error == -EEXIST)
		error = AOP_TRUNCATED_PAGE;
	if (error)
		goto error;

	error = filemap_read_folio(file, mapping, folio);
	if (error)
		goto error;

	filemap_invalidate_unlock_shared(mapping);
	folio_batch_add(fbatch, folio);
	return 0;
error:
	filemap_invalidate_unlock_shared(mapping);
	folio_put(folio);
	return error;
}

static int filemap_readahead(struct kiocb *iocb, struct file *file,
		struct address_space *mapping, struct folio *folio,
		pgoff_t last_index)
{
	DEFINE_READAHEAD(ractl, file, &file->f_ra, mapping, folio->index);

	if (iocb->ki_flags & IOCB_NOIO)
		return -EAGAIN;
	page_cache_async_ra(&ractl, folio, last_index - folio->index);
	return 0;
}

static int filemap_get_pages(struct kiocb *iocb, struct iov_iter *iter,
		struct folio_batch *fbatch)
{
	struct file *filp = iocb->ki_filp;
	struct address_space *mapping = filp->f_mapping;
	struct file_ra_state *ra = &filp->f_ra;
	pgoff_t index = iocb->ki_pos >> PAGE_SHIFT;
	pgoff_t last_index;
	struct folio *folio;
	int err = 0;

	last_index = DIV_ROUND_UP(iocb->ki_pos + iter->count, PAGE_SIZE);
retry:
	if (fatal_signal_pending(current))
		return -EINTR;

	filemap_get_read_batch(mapping, index, last_index, fbatch);
	if (!folio_batch_count(fbatch)) {
		if (iocb->ki_flags & IOCB_NOIO)
			return -EAGAIN;
		page_cache_sync_readahead(mapping, ra, filp, index,
				last_index - index);
		filemap_get_read_batch(mapping, index, last_index, fbatch);
	}
	if (!folio_batch_count(fbatch)) {
		if (iocb->ki_flags & (IOCB_NOWAIT | IOCB_WAITQ))
			return -EAGAIN;
		err = filemap_create_folio(filp, mapping,
				iocb->ki_pos >> PAGE_SHIFT, fbatch);
		if (err == AOP_TRUNCATED_PAGE)
			goto retry;
		return err;
	}

	folio = fbatch->folios[folio_batch_count(fbatch) - 1];
	if (folio_test_readahead(folio)) {
		err = filemap_readahead(iocb, filp, mapping, folio, last_index);
		if (err)
			goto err;
	}
	if (!folio_test_uptodate(folio)) {
		if ((iocb->ki_flags & IOCB_WAITQ) &&
		    folio_batch_count(fbatch) > 1)
			iocb->ki_flags |= IOCB_NOWAIT;
		err = filemap_update_page(iocb, mapping, iter, folio);
		if (err)
			goto err;
	}

	return 0;
err:
	if (err < 0)
		folio_put(folio);
	if (likely(--fbatch->nr))
		return 0;
	if (err == AOP_TRUNCATED_PAGE)
		goto retry;
	return err;
}

/**
 * filemap_read - Read data from the page cache.
 * @iocb: The iocb to read.
 * @iter: Destination for the data.
 * @already_read: Number of bytes already read by the caller.
 *
 * Copies data from the page cache.  If the data is not currently present,
 * uses the readahead and readpage address_space operations to fetch it.
 *
 * Return: Total number of bytes copied, including those already read by
 * the caller.  If an error happens before any bytes are copied, returns
 * a negative error number.
 */
ssize_t filemap_read(struct kiocb *iocb, struct iov_iter *iter,
		ssize_t already_read)
{
	struct file *filp = iocb->ki_filp;
	struct file_ra_state *ra = &filp->f_ra;
	struct address_space *mapping = filp->f_mapping;
	struct inode *inode = mapping->host;
	struct folio_batch fbatch;
	int i, error = 0;
	bool writably_mapped;
	loff_t isize, end_offset;

	if (unlikely(iocb->ki_pos >= inode->i_sb->s_maxbytes))
		return 0;
	if (unlikely(!iov_iter_count(iter)))
		return 0;

	iov_iter_truncate(iter, inode->i_sb->s_maxbytes);
	folio_batch_init(&fbatch);

	do {
		cond_resched();

		/*
		 * If we've already successfully copied some data, then we
		 * can no longer safely return -EIOCBQUEUED. Hence mark
		 * an async read NOWAIT at that point.
		 */
		if ((iocb->ki_flags & IOCB_WAITQ) && already_read)
			iocb->ki_flags |= IOCB_NOWAIT;

		if (unlikely(iocb->ki_pos >= i_size_read(inode)))
			break;

		error = filemap_get_pages(iocb, iter, &fbatch);
		if (error < 0)
			break;

		/*
		 * i_size must be checked after we know the pages are Uptodate.
		 *
		 * Checking i_size after the check allows us to calculate
		 * the correct value for "nr", which means the zero-filled
		 * part of the page is not copied back to userspace (unless
		 * another truncate extends the file - this is desired though).
		 */
		isize = i_size_read(inode);
		if (unlikely(iocb->ki_pos >= isize))
			goto put_folios;
		end_offset = min_t(loff_t, isize, iocb->ki_pos + iter->count);

		/*
		 * Once we start copying data, we don't want to be touching any
		 * cachelines that might be contended:
		 */
		writably_mapped = mapping_writably_mapped(mapping);

		/*
		 * When a sequential read accesses a page several times, only
		 * mark it as accessed the first time.
		 */
		if (iocb->ki_pos >> PAGE_SHIFT !=
		    ra->prev_pos >> PAGE_SHIFT)
			folio_mark_accessed(fbatch.folios[0]);

		for (i = 0; i < folio_batch_count(&fbatch); i++) {
			struct folio *folio = fbatch.folios[i];
			size_t fsize = folio_size(folio);
			size_t offset = iocb->ki_pos & (fsize - 1);
			size_t bytes = min_t(loff_t, end_offset - iocb->ki_pos,
					     fsize - offset);
			size_t copied;

			if (end_offset < folio_pos(folio))
				break;
			if (i > 0)
				folio_mark_accessed(folio);
			/*
			 * If users can be writing to this folio using arbitrary
			 * virtual addresses, take care of potential aliasing
			 * before reading the folio on the kernel side.
			 */
			if (writably_mapped)
				flush_dcache_folio(folio);

			copied = copy_folio_to_iter(folio, offset, bytes, iter);

			already_read += copied;
			iocb->ki_pos += copied;
			ra->prev_pos = iocb->ki_pos;

			if (copied < bytes) {
				error = -EFAULT;
				break;
			}
		}
put_folios:
		for (i = 0; i < folio_batch_count(&fbatch); i++)
			folio_put(fbatch.folios[i]);
		folio_batch_init(&fbatch);
	} while (iov_iter_count(iter) && iocb->ki_pos < isize && !error);

	file_accessed(filp);

	return already_read ? already_read : error;
}
EXPORT_SYMBOL_GPL(filemap_read);

/**
 * generic_file_read_iter - generic filesystem read routine
 * @iocb:	kernel I/O control block
 * @iter:	destination for the data read
 *
 * This is the "read_iter()" routine for all filesystems
 * that can use the page cache directly.
 *
 * The IOCB_NOWAIT flag in iocb->ki_flags indicates that -EAGAIN shall
 * be returned when no data can be read without waiting for I/O requests
 * to complete; it doesn't prevent readahead.
 *
 * The IOCB_NOIO flag in iocb->ki_flags indicates that no new I/O
 * requests shall be made for the read or for readahead.  When no data
 * can be read, -EAGAIN shall be returned.  When readahead would be
 * triggered, a partial, possibly empty read shall be returned.
 *
 * Return:
 * * number of bytes copied, even for partial reads
 * * negative error code (or 0 if IOCB_NOIO) if nothing was read
 */
ssize_t
generic_file_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	size_t count = iov_iter_count(iter);
	ssize_t retval = 0;

	if (!count)
		return 0; /* skip atime */

	if (iocb->ki_flags & IOCB_DIRECT) {
		struct file *file = iocb->ki_filp;
		struct address_space *mapping = file->f_mapping;
		struct inode *inode = mapping->host;

		if (iocb->ki_flags & IOCB_NOWAIT) {
			if (filemap_range_needs_writeback(mapping, iocb->ki_pos,
						iocb->ki_pos + count - 1))
				return -EAGAIN;
		} else {
			retval = filemap_write_and_wait_range(mapping,
						iocb->ki_pos,
					        iocb->ki_pos + count - 1);
			if (retval < 0)
				return retval;
		}

		file_accessed(file);

		retval = mapping->a_ops->direct_IO(iocb, iter);
		if (retval >= 0) {
			iocb->ki_pos += retval;
			count -= retval;
		}
		if (retval != -EIOCBQUEUED)
			iov_iter_revert(iter, count - iov_iter_count(iter));

		/*
		 * Btrfs can have a short DIO read if we encounter
		 * compressed extents, so if there was an error, or if
		 * we've already read everything we wanted to, or if
		 * there was a short read because we hit EOF, go ahead
		 * and return.  Otherwise fallthrough to buffered io for
		 * the rest of the read.  Buffered reads will not work for
		 * DAX files, so don't bother trying.
		 */
		if (retval < 0 || !count || IS_DAX(inode))
			return retval;
		if (iocb->ki_pos >= i_size_read(inode))
			return retval;
	}

	return filemap_read(iocb, iter, retval);
}
EXPORT_SYMBOL(generic_file_read_iter);

static inline loff_t folio_seek_hole_data(struct xa_state *xas,
		struct address_space *mapping, struct folio *folio,
		loff_t start, loff_t end, bool seek_data)
{
	const struct address_space_operations *ops = mapping->a_ops;
	size_t offset, bsz = i_blocksize(mapping->host);

	if (xa_is_value(folio) || folio_test_uptodate(folio))
		return seek_data ? start : end;
	if (!ops->is_partially_uptodate)
		return seek_data ? end : start;

	xas_pause(xas);
	rcu_read_unlock();
	folio_lock(folio);
	if (unlikely(folio->mapping != mapping))
		goto unlock;

	offset = offset_in_folio(folio, start) & ~(bsz - 1);

	do {
		if (ops->is_partially_uptodate(folio, offset, bsz) ==
							seek_data)
			break;
		start = (start + bsz) & ~(bsz - 1);
		offset += bsz;
	} while (offset < folio_size(folio));
unlock:
	folio_unlock(folio);
	rcu_read_lock();
	return start;
}

static inline size_t seek_folio_size(struct xa_state *xas, struct folio *folio)
{
	if (xa_is_value(folio))
		return PAGE_SIZE << xa_get_order(xas->xa, xas->xa_index);
	return folio_size(folio);
}

/**
 * mapping_seek_hole_data - Seek for SEEK_DATA / SEEK_HOLE in the page cache.
 * @mapping: Address space to search.
 * @start: First byte to consider.
 * @end: Limit of search (exclusive).
 * @whence: Either SEEK_HOLE or SEEK_DATA.
 *
 * If the page cache knows which blocks contain holes and which blocks
 * contain data, your filesystem can use this function to implement
 * SEEK_HOLE and SEEK_DATA.  This is useful for filesystems which are
 * entirely memory-based such as tmpfs, and filesystems which support
 * unwritten extents.
 *
 * Return: The requested offset on success, or -ENXIO if @whence specifies
 * SEEK_DATA and there is no data after @start.  There is an implicit hole
 * after @end - 1, so SEEK_HOLE returns @end if all the bytes between @start
 * and @end contain data.
 */
loff_t mapping_seek_hole_data(struct address_space *mapping, loff_t start,
		loff_t end, int whence)
{
	XA_STATE(xas, &mapping->i_pages, start >> PAGE_SHIFT);
	pgoff_t max = (end - 1) >> PAGE_SHIFT;
	bool seek_data = (whence == SEEK_DATA);
	struct folio *folio;

	if (end <= start)
		return -ENXIO;

	rcu_read_lock();
	while ((folio = find_get_entry(&xas, max, XA_PRESENT))) {
		loff_t pos = (u64)xas.xa_index << PAGE_SHIFT;
		size_t seek_size;

		if (start < pos) {
			if (!seek_data)
				goto unlock;
			start = pos;
		}

		seek_size = seek_folio_size(&xas, folio);
		pos = round_up((u64)pos + 1, seek_size);
		start = folio_seek_hole_data(&xas, mapping, folio, start, pos,
				seek_data);
		if (start < pos)
			goto unlock;
		if (start >= end)
			break;
		if (seek_size > PAGE_SIZE)
			xas_set(&xas, pos >> PAGE_SHIFT);
		if (!xa_is_value(folio))
			folio_put(folio);
	}
	if (seek_data)
		start = -ENXIO;
unlock:
	rcu_read_unlock();
	if (folio && !xa_is_value(folio))
		folio_put(folio);
	if (start > end)
		return end;
	return start;
}

#ifdef CONFIG_MMU
#define MMAP_LOTSAMISS  (100)
/*
 * lock_folio_maybe_drop_mmap - lock the page, possibly dropping the mmap_lock
 * @vmf - the vm_fault for this fault.
 * @folio - the folio to lock.
 * @fpin - the pointer to the file we may pin (or is already pinned).
 *
 * This works similar to lock_folio_or_retry in that it can drop the
 * mmap_lock.  It differs in that it actually returns the folio locked
 * if it returns 1 and 0 if it couldn't lock the folio.  If we did have
 * to drop the mmap_lock then fpin will point to the pinned file and
 * needs to be fput()'ed at a later point.
 */
static int lock_folio_maybe_drop_mmap(struct vm_fault *vmf, struct folio *folio,
				     struct file **fpin)
{
	if (folio_trylock(folio))
		return 1;

	/*
	 * NOTE! This will make us return with VM_FAULT_RETRY, but with
	 * the mmap_lock still held. That's how FAULT_FLAG_RETRY_NOWAIT
	 * is supposed to work. We have way too many special cases..
	 */
	if (vmf->flags & FAULT_FLAG_RETRY_NOWAIT)
		return 0;

	*fpin = maybe_unlock_mmap_for_io(vmf, *fpin);
	if (vmf->flags & FAULT_FLAG_KILLABLE) {
		if (__folio_lock_killable(folio)) {
			/*
			 * We didn't have the right flags to drop the mmap_lock,
			 * but all fault_handlers only check for fatal signals
			 * if we return VM_FAULT_RETRY, so we need to drop the
			 * mmap_lock here and return 0 if we don't have a fpin.
			 */
			if (*fpin == NULL)
				mmap_read_unlock(vmf->vma->vm_mm);
			return 0;
		}
	} else
		__folio_lock(folio);

	return 1;
}

/*
 * Synchronous readahead happens when we don't even find a page in the page
 * cache at all.  We don't want to perform IO under the mmap sem, so if we have
 * to drop the mmap sem we return the file that was pinned in order for us to do
 * that.  If we didn't pin a file then we return NULL.  The file that is
 * returned needs to be fput()'ed when we're done with it.
 */
static struct file *do_sync_mmap_readahead(struct vm_fault *vmf)
{
	struct file *file = vmf->vma->vm_file;
	struct file_ra_state *ra = &file->f_ra;
	struct address_space *mapping = file->f_mapping;
	DEFINE_READAHEAD(ractl, file, ra, mapping, vmf->pgoff);
	struct file *fpin = NULL;
	unsigned long vm_flags = vmf->vma->vm_flags;
	unsigned int mmap_miss;

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	/* Use the readahead code, even if readahead is disabled */
	if (vm_flags & VM_HUGEPAGE) {
		fpin = maybe_unlock_mmap_for_io(vmf, fpin);
		ractl._index &= ~((unsigned long)HPAGE_PMD_NR - 1);
		ra->size = HPAGE_PMD_NR;
		/*
		 * Fetch two PMD folios, so we get the chance to actually
		 * readahead, unless we've been told not to.
		 */
		if (!(vm_flags & VM_RAND_READ))
			ra->size *= 2;
		ra->async_size = HPAGE_PMD_NR;
		page_cache_ra_order(&ractl, ra, HPAGE_PMD_ORDER);
		return fpin;
	}
#endif

	/* If we don't want any read-ahead, don't bother */
	if (vm_flags & VM_RAND_READ)
		return fpin;
	if (!ra->ra_pages)
		return fpin;

	if (vm_flags & VM_SEQ_READ) {
		fpin = maybe_unlock_mmap_for_io(vmf, fpin);
		page_cache_sync_ra(&ractl, ra->ra_pages);
		return fpin;
	}

	/* Avoid banging the cache line if not needed */
	mmap_miss = READ_ONCE(ra->mmap_miss);
	if (mmap_miss < MMAP_LOTSAMISS * 10)
		WRITE_ONCE(ra->mmap_miss, ++mmap_miss);

	/*
	 * Do we miss much more than hit in this file? If so,
	 * stop bothering with read-ahead. It will only hurt.
	 */
	if (mmap_miss > MMAP_LOTSAMISS)
		return fpin;

	/*
	 * mmap read-around
	 */
	fpin = maybe_unlock_mmap_for_io(vmf, fpin);
	ra->start = max_t(long, 0, vmf->pgoff - ra->ra_pages / 2);
	ra->size = ra->ra_pages;
	ra->async_size = ra->ra_pages / 4;
	ractl._index = ra->start;
	page_cache_ra_order(&ractl, ra, 0);
	return fpin;
}

/*
 * Asynchronous readahead happens when we find the page and PG_readahead,
 * so we want to possibly extend the readahead further.  We return the file that
 * was pinned if we have to drop the mmap_lock in order to do IO.
 */
static struct file *do_async_mmap_readahead(struct vm_fault *vmf,
					    struct folio *folio)
{
	struct file *file = vmf->vma->vm_file;
	struct file_ra_state *ra = &file->f_ra;
	DEFINE_READAHEAD(ractl, file, ra, file->f_mapping, vmf->pgoff);
	struct file *fpin = NULL;
	unsigned int mmap_miss;

	/* If we don't want any read-ahead, don't bother */
	if (vmf->vma->vm_flags & VM_RAND_READ || !ra->ra_pages)
		return fpin;

	mmap_miss = READ_ONCE(ra->mmap_miss);
	if (mmap_miss)
		WRITE_ONCE(ra->mmap_miss, --mmap_miss);

	if (folio_test_readahead(folio)) {
		fpin = maybe_unlock_mmap_for_io(vmf, fpin);
		page_cache_async_ra(&ractl, folio, ra->ra_pages);
	}
	return fpin;
}

/**
 * filemap_fault - read in file data for page fault handling
 * @vmf:	struct vm_fault containing details of the fault
 *
 * filemap_fault() is invoked via the vma operations vector for a
 * mapped memory region to read in file data during a page fault.
 *
 * The goto's are kind of ugly, but this streamlines the normal case of having
 * it in the page cache, and handles the special cases reasonably without
 * having a lot of duplicated code.
 *
 * vma->vm_mm->mmap_lock must be held on entry.
 *
 * If our return value has VM_FAULT_RETRY set, it's because the mmap_lock
 * may be dropped before doing I/O or by lock_folio_maybe_drop_mmap().
 *
 * If our return value does not have VM_FAULT_RETRY set, the mmap_lock
 * has not been released.
 *
 * We never return with VM_FAULT_RETRY and a bit from VM_FAULT_ERROR set.
 *
 * Return: bitwise-OR of %VM_FAULT_ codes.
 */
vm_fault_t filemap_fault(struct vm_fault *vmf)
{
	int error;
	struct file *file = vmf->vma->vm_file;
	struct file *fpin = NULL;
	struct address_space *mapping = file->f_mapping;
	struct inode *inode = mapping->host;
	pgoff_t max_idx, index = vmf->pgoff;
	struct folio *folio;
	vm_fault_t ret = 0;
	bool mapping_locked = false;

	max_idx = DIV_ROUND_UP(i_size_read(inode), PAGE_SIZE);
	if (unlikely(index >= max_idx))
		return VM_FAULT_SIGBUS;

	/*
	 * Do we have something in the page cache already?
	 */
	folio = filemap_get_folio(mapping, index);
	if (likely(folio)) {
		/*
		 * We found the page, so try async readahead before waiting for
		 * the lock.
		 */
		if (!(vmf->flags & FAULT_FLAG_TRIED))
			fpin = do_async_mmap_readahead(vmf, folio);
		if (unlikely(!folio_test_uptodate(folio))) {
			filemap_invalidate_lock_shared(mapping);
			mapping_locked = true;
		}
	} else {
		/* No page in the page cache at all */
		count_vm_event(PGMAJFAULT);
		count_memcg_event_mm(vmf->vma->vm_mm, PGMAJFAULT);
		ret = VM_FAULT_MAJOR;
		fpin = do_sync_mmap_readahead(vmf);
retry_find:
		/*
		 * See comment in filemap_create_folio() why we need
		 * invalidate_lock
		 */
		if (!mapping_locked) {
			filemap_invalidate_lock_shared(mapping);
			mapping_locked = true;
		}
		folio = __filemap_get_folio(mapping, index,
					  FGP_CREAT|FGP_FOR_MMAP,
					  vmf->gfp_mask);
		if (!folio) {
			if (fpin)
				goto out_retry;
			filemap_invalidate_unlock_shared(mapping);
			return VM_FAULT_OOM;
		}
	}

	if (!lock_folio_maybe_drop_mmap(vmf, folio, &fpin))
		goto out_retry;

	/* Did it get truncated? */
	if (unlikely(folio->mapping != mapping)) {
		folio_unlock(folio);
		folio_put(folio);
		goto retry_find;
	}
	VM_BUG_ON_FOLIO(!folio_contains(folio, index), folio);

	/*
	 * We have a locked page in the page cache, now we need to check
	 * that it's up-to-date. If not, it is going to be due to an error.
	 */
	if (unlikely(!folio_test_uptodate(folio))) {
		/*
		 * The page was in cache and uptodate and now it is not.
		 * Strange but possible since we didn't hold the page lock all
		 * the time. Let's drop everything get the invalidate lock and
		 * try again.
		 */
		if (!mapping_locked) {
			folio_unlock(folio);
			folio_put(folio);
			goto retry_find;
		}
		goto page_not_uptodate;
	}

	/*
	 * We've made it this far and we had to drop our mmap_lock, now is the
	 * time to return to the upper layer and have it re-find the vma and
	 * redo the fault.
	 */
	if (fpin) {
		folio_unlock(folio);
		goto out_retry;
	}
	if (mapping_locked)
		filemap_invalidate_unlock_shared(mapping);

	/*
	 * Found the page and have a reference on it.
	 * We must recheck i_size under page lock.
	 */
	max_idx = DIV_ROUND_UP(i_size_read(inode), PAGE_SIZE);
	if (unlikely(index >= max_idx)) {
		folio_unlock(folio);
		folio_put(folio);
		return VM_FAULT_SIGBUS;
	}

	vmf->page = folio_file_page(folio, index);
	return ret | VM_FAULT_LOCKED;

page_not_uptodate:
	/*
	 * Umm, take care of errors if the page isn't up-to-date.
	 * Try to re-read it _once_. We do this synchronously,
	 * because there really aren't any performance issues here
	 * and we need to check for errors.
	 */
	fpin = maybe_unlock_mmap_for_io(vmf, fpin);
	error = filemap_read_folio(file, mapping, folio);
	if (fpin)
		goto out_retry;
	folio_put(folio);

	if (!error || error == AOP_TRUNCATED_PAGE)
		goto retry_find;
	filemap_invalidate_unlock_shared(mapping);

	return VM_FAULT_SIGBUS;

out_retry:
	/*
	 * We dropped the mmap_lock, we need to return to the fault handler to
	 * re-find the vma and come back and find our hopefully still populated
	 * page.
	 */
	if (folio)
		folio_put(folio);
	if (mapping_locked)
		filemap_invalidate_unlock_shared(mapping);
	if (fpin)
		fput(fpin);
	return ret | VM_FAULT_RETRY;
}
EXPORT_SYMBOL(filemap_fault);

static bool filemap_map_pmd(struct vm_fault *vmf, struct page *page)
{
	struct mm_struct *mm = vmf->vma->vm_mm;

	/* Huge page is mapped? No need to proceed. */
	if (pmd_trans_huge(*vmf->pmd)) {
		unlock_page(page);
		put_page(page);
		return true;
	}

	if (pmd_none(*vmf->pmd) && PageTransHuge(page)) {
		vm_fault_t ret = do_set_pmd(vmf, page);
		if (!ret) {
			/* The page is mapped successfully, reference consumed. */
			unlock_page(page);
			return true;
		}
	}

	if (pmd_none(*vmf->pmd))
		pmd_install(mm, vmf->pmd, &vmf->prealloc_pte);

	/* See comment in handle_pte_fault() */
	if (pmd_devmap_trans_unstable(vmf->pmd)) {
		unlock_page(page);
		put_page(page);
		return true;
	}

	return false;
}

static struct folio *next_uptodate_page(struct folio *folio,
				       struct address_space *mapping,
				       struct xa_state *xas, pgoff_t end_pgoff)
{
	unsigned long max_idx;

	do {
		if (!folio)
			return NULL;
		if (xas_retry(xas, folio))
			continue;
		if (xa_is_value(folio))
			continue;
		if (folio_test_locked(folio))
			continue;
		if (!folio_try_get_rcu(folio))
			continue;
		/* Has the page moved or been split? */
		if (unlikely(folio != xas_reload(xas)))
			goto skip;
		if (!folio_test_uptodate(folio) || folio_test_readahead(folio))
			goto skip;
		if (!folio_trylock(folio))
			goto skip;
		if (folio->mapping != mapping)
			goto unlock;
		if (!folio_test_uptodate(folio))
			goto unlock;
		max_idx = DIV_ROUND_UP(i_size_read(mapping->host), PAGE_SIZE);
		if (xas->xa_index >= max_idx)
			goto unlock;
		return folio;
unlock:
		folio_unlock(folio);
skip:
		folio_put(folio);
	} while ((folio = xas_next_entry(xas, end_pgoff)) != NULL);

	return NULL;
}

static inline struct folio *first_map_page(struct address_space *mapping,
					  struct xa_state *xas,
					  pgoff_t end_pgoff)
{
	return next_uptodate_page(xas_find(xas, end_pgoff),
				  mapping, xas, end_pgoff);
}

static inline struct folio *next_map_page(struct address_space *mapping,
					 struct xa_state *xas,
					 pgoff_t end_pgoff)
{
	return next_uptodate_page(xas_next_entry(xas, end_pgoff),
				  mapping, xas, end_pgoff);
}

vm_fault_t filemap_map_pages(struct vm_fault *vmf,
			     pgoff_t start_pgoff, pgoff_t end_pgoff)
{
	struct vm_area_struct *vma = vmf->vma;
	struct file *file = vma->vm_file;
	struct address_space *mapping = file->f_mapping;
	pgoff_t last_pgoff = start_pgoff;
	unsigned long addr;
	XA_STATE(xas, &mapping->i_pages, start_pgoff);
	struct folio *folio;
	struct page *page;
	unsigned int mmap_miss = READ_ONCE(file->f_ra.mmap_miss);
	vm_fault_t ret = 0;

	rcu_read_lock();
	folio = first_map_page(mapping, &xas, end_pgoff);
	if (!folio)
		goto out;

	if (filemap_map_pmd(vmf, &folio->page)) {
		ret = VM_FAULT_NOPAGE;
		goto out;
	}

	addr = vma->vm_start + ((start_pgoff - vma->vm_pgoff) << PAGE_SHIFT);
	vmf->pte = pte_offset_map_lock(vma->vm_mm, vmf->pmd, addr, &vmf->ptl);
	do {
again:
		page = folio_file_page(folio, xas.xa_index);
		if (PageHWPoison(page))
			goto unlock;

		if (mmap_miss > 0)
			mmap_miss--;

		addr += (xas.xa_index - last_pgoff) << PAGE_SHIFT;
		vmf->pte += xas.xa_index - last_pgoff;
		last_pgoff = xas.xa_index;

		if (!pte_none(*vmf->pte))
			goto unlock;

		/* We're about to handle the fault */
		if (vmf->address == addr)
			ret = VM_FAULT_NOPAGE;

		do_set_pte(vmf, page, addr);
		/* no need to invalidate: a not-present page won't be cached */
		update_mmu_cache(vma, addr, vmf->pte);
		if (folio_more_pages(folio, xas.xa_index, end_pgoff)) {
			xas.xa_index++;
			folio_ref_inc(folio);
			goto again;
		}
		folio_unlock(folio);
		continue;
unlock:
		if (folio_more_pages(folio, xas.xa_index, end_pgoff)) {
			xas.xa_index++;
			goto again;
		}
		folio_unlock(folio);
		folio_put(folio);
	} while ((folio = next_map_page(mapping, &xas, end_pgoff)) != NULL);
	pte_unmap_unlock(vmf->pte, vmf->ptl);
out:
	rcu_read_unlock();
	WRITE_ONCE(file->f_ra.mmap_miss, mmap_miss);
	return ret;
}
EXPORT_SYMBOL(filemap_map_pages);

vm_fault_t filemap_page_mkwrite(struct vm_fault *vmf)
{
	struct address_space *mapping = vmf->vma->vm_file->f_mapping;
	struct folio *folio = page_folio(vmf->page);
	vm_fault_t ret = VM_FAULT_LOCKED;

	sb_start_pagefault(mapping->host->i_sb);
	file_update_time(vmf->vma->vm_file);
	folio_lock(folio);
	if (folio->mapping != mapping) {
		folio_unlock(folio);
		ret = VM_FAULT_NOPAGE;
		goto out;
	}
	/*
	 * We mark the folio dirty already here so that when freeze is in
	 * progress, we are guaranteed that writeback during freezing will
	 * see the dirty folio and writeprotect it again.
	 */
	folio_mark_dirty(folio);
	folio_wait_stable(folio);
out:
	sb_end_pagefault(mapping->host->i_sb);
	return ret;
}

const struct vm_operations_struct generic_file_vm_ops = {
	.fault		= filemap_fault,
	.map_pages	= filemap_map_pages,
	.page_mkwrite	= filemap_page_mkwrite,
};

/* This is used for a general mmap of a disk file */

int generic_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct address_space *mapping = file->f_mapping;

	if (!mapping->a_ops->readpage)
		return -ENOEXEC;
	file_accessed(file);
	vma->vm_ops = &generic_file_vm_ops;
	return 0;
}

/*
 * This is for filesystems which do not implement ->writepage.
 */
int generic_file_readonly_mmap(struct file *file, struct vm_area_struct *vma)
{
	if ((vma->vm_flags & VM_SHARED) && (vma->vm_flags & VM_MAYWRITE))
		return -EINVAL;
	return generic_file_mmap(file, vma);
}
#else
vm_fault_t filemap_page_mkwrite(struct vm_fault *vmf)
{
	return VM_FAULT_SIGBUS;
}
int generic_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	return -ENOSYS;
}
int generic_file_readonly_mmap(struct file *file, struct vm_area_struct *vma)
{
	return -ENOSYS;
}
#endif /* CONFIG_MMU */

EXPORT_SYMBOL(filemap_page_mkwrite);
EXPORT_SYMBOL(generic_file_mmap);
EXPORT_SYMBOL(generic_file_readonly_mmap);

static struct folio *do_read_cache_folio(struct address_space *mapping,
		pgoff_t index, filler_t filler, void *data, gfp_t gfp)
{
	struct folio *folio;
	int err;
repeat:
	folio = filemap_get_folio(mapping, index);
	if (!folio) {
		folio = filemap_alloc_folio(gfp, 0);
		if (!folio)
			return ERR_PTR(-ENOMEM);
		err = filemap_add_folio(mapping, folio, index, gfp);
		if (unlikely(err)) {
			folio_put(folio);
			if (err == -EEXIST)
				goto repeat;
			/* Presumably ENOMEM for xarray node */
			return ERR_PTR(err);
		}

filler:
		if (filler)
			err = filler(data, &folio->page);
		else
			err = mapping->a_ops->readpage(data, &folio->page);

		if (err < 0) {
			folio_put(folio);
			return ERR_PTR(err);
		}

		folio_wait_locked(folio);
		if (!folio_test_uptodate(folio)) {
			folio_put(folio);
			return ERR_PTR(-EIO);
		}

		goto out;
	}
	if (folio_test_uptodate(folio))
		goto out;

	if (!folio_trylock(folio)) {
		folio_put_wait_locked(folio, TASK_UNINTERRUPTIBLE);
		goto repeat;
	}

	/* Folio was truncated from mapping */
	if (!folio->mapping) {
		folio_unlock(folio);
		folio_put(folio);
		goto repeat;
	}

	/* Someone else locked and filled the page in a very small window */
	if (folio_test_uptodate(folio)) {
		folio_unlock(folio);
		goto out;
	}

	/*
	 * A previous I/O error may have been due to temporary
	 * failures.
	 * Clear page error before actual read, PG_error will be
	 * set again if read page fails.
	 */
	folio_clear_error(folio);
	goto filler;

out:
	folio_mark_accessed(folio);
	return folio;
}

/**
 * read_cache_folio - read into page cache, fill it if needed
 * @mapping:	the page's address_space
 * @index:	the page index
 * @filler:	function to perform the read
 * @data:	first arg to filler(data, page) function, often left as NULL
 *
 * Read into the page cache. If a page already exists, and PageUptodate() is
 * not set, try to fill the page and wait for it to become unlocked.
 *
 * If the page does not get brought uptodate, return -EIO.
 *
 * The function expects mapping->invalidate_lock to be already held.
 *
 * Return: up to date page on success, ERR_PTR() on failure.
 */
struct folio *read_cache_folio(struct address_space *mapping, pgoff_t index,
		filler_t filler, void *data)
{
	return do_read_cache_folio(mapping, index, filler, data,
			mapping_gfp_mask(mapping));
}
EXPORT_SYMBOL(read_cache_folio);

static struct page *do_read_cache_page(struct address_space *mapping,
		pgoff_t index, filler_t *filler, void *data, gfp_t gfp)
{
	struct folio *folio;

	folio = do_read_cache_folio(mapping, index, filler, data, gfp);
	if (IS_ERR(folio))
		return &folio->page;
	return folio_file_page(folio, index);
}

struct page *read_cache_page(struct address_space *mapping,
				pgoff_t index, filler_t *filler, void *data)
{
	return do_read_cache_page(mapping, index, filler, data,
			mapping_gfp_mask(mapping));
}
EXPORT_SYMBOL(read_cache_page);

/**
 * read_cache_page_gfp - read into page cache, using specified page allocation flags.
 * @mapping:	the page's address_space
 * @index:	the page index
 * @gfp:	the page allocator flags to use if allocating
 *
 * This is the same as "read_mapping_page(mapping, index, NULL)", but with
 * any new page allocations done using the specified allocation flags.
 *
 * If the page does not get brought uptodate, return -EIO.
 *
 * The function expects mapping->invalidate_lock to be already held.
 *
 * Return: up to date page on success, ERR_PTR() on failure.
 */
struct page *read_cache_page_gfp(struct address_space *mapping,
				pgoff_t index,
				gfp_t gfp)
{
	return do_read_cache_page(mapping, index, NULL, NULL, gfp);
}
EXPORT_SYMBOL(read_cache_page_gfp);

int pagecache_write_begin(struct file *file, struct address_space *mapping,
				loff_t pos, unsigned len, unsigned flags,
				struct page **pagep, void **fsdata)
{
	const struct address_space_operations *aops = mapping->a_ops;

	return aops->write_begin(file, mapping, pos, len, flags,
							pagep, fsdata);
}
EXPORT_SYMBOL(pagecache_write_begin);

int pagecache_write_end(struct file *file, struct address_space *mapping,
				loff_t pos, unsigned len, unsigned copied,
				struct page *page, void *fsdata)
{
	const struct address_space_operations *aops = mapping->a_ops;

	return aops->write_end(file, mapping, pos, len, copied, page, fsdata);
}
EXPORT_SYMBOL(pagecache_write_end);

/*
 * Warn about a page cache invalidation failure during a direct I/O write.
 */
void dio_warn_stale_pagecache(struct file *filp)
{
	static DEFINE_RATELIMIT_STATE(_rs, 86400 * HZ, DEFAULT_RATELIMIT_BURST);
	char pathname[128];
	char *path;

	errseq_set(&filp->f_mapping->wb_err, -EIO);
	if (__ratelimit(&_rs)) {
		path = file_path(filp, pathname, sizeof(pathname));
		if (IS_ERR(path))
			path = "(unknown)";
		pr_crit("Page cache invalidation failure on direct I/O.  Possible data corruption due to collision with buffered I/O!\n");
		pr_crit("File: %s PID: %d Comm: %.20s\n", path, current->pid,
			current->comm);
	}
}

ssize_t
generic_file_direct_write(struct kiocb *iocb, struct iov_iter *from)
{
	struct file	*file = iocb->ki_filp;
	struct address_space *mapping = file->f_mapping;
	struct inode	*inode = mapping->host;
	loff_t		pos = iocb->ki_pos;
	ssize_t		written;
	size_t		write_len;
	pgoff_t		end;

	write_len = iov_iter_count(from);
	end = (pos + write_len - 1) >> PAGE_SHIFT;

	if (iocb->ki_flags & IOCB_NOWAIT) {
		/* If there are pages to writeback, return */
		if (filemap_range_has_page(file->f_mapping, pos,
					   pos + write_len - 1))
			return -EAGAIN;
	} else {
		written = filemap_write_and_wait_range(mapping, pos,
							pos + write_len - 1);
		if (written)
			goto out;
	}

	/*
	 * After a write we want buffered reads to be sure to go to disk to get
	 * the new data.  We invalidate clean cached page from the region we're
	 * about to write.  We do this *before* the write so that we can return
	 * without clobbering -EIOCBQUEUED from ->direct_IO().
	 */
	written = invalidate_inode_pages2_range(mapping,
					pos >> PAGE_SHIFT, end);
	/*
	 * If a page can not be invalidated, return 0 to fall back
	 * to buffered write.
	 */
	if (written) {
		if (written == -EBUSY)
			return 0;
		goto out;
	}

	written = mapping->a_ops->direct_IO(iocb, from);

	/*
	 * Finally, try again to invalidate clean pages which might have been
	 * cached by non-direct readahead, or faulted in by get_user_pages()
	 * if the source of the write was an mmap'ed region of the file
	 * we're writing.  Either one is a pretty crazy thing to do,
	 * so we don't support it 100%.  If this invalidation
	 * fails, tough, the write still worked...
	 *
	 * Most of the time we do not need this since dio_complete() will do
	 * the invalidation for us. However there are some file systems that
	 * do not end up with dio_complete() being called, so let's not break
	 * them by removing it completely.
	 *
	 * Noticeable example is a blkdev_direct_IO().
	 *
	 * Skip invalidation for async writes or if mapping has no pages.
	 */
	if (written > 0 && mapping->nrpages &&
	    invalidate_inode_pages2_range(mapping, pos >> PAGE_SHIFT, end))
		dio_warn_stale_pagecache(file);

	if (written > 0) {
		pos += written;
		write_len -= written;
		if (pos > i_size_read(inode) && !S_ISBLK(inode->i_mode)) {
			i_size_write(inode, pos);
			mark_inode_dirty(inode);
		}
		iocb->ki_pos = pos;
	}
	if (written != -EIOCBQUEUED)
		iov_iter_revert(from, write_len - iov_iter_count(from));
out:
	return written;
}
EXPORT_SYMBOL(generic_file_direct_write);

ssize_t generic_perform_write(struct kiocb *iocb, struct iov_iter *i)
{
	struct file *file = iocb->ki_filp;
	loff_t pos = iocb->ki_pos;
	struct address_space *mapping = file->f_mapping;
	const struct address_space_operations *a_ops = mapping->a_ops;
	long status = 0;
	ssize_t written = 0;
	unsigned int flags = 0;

	do {
		struct page *page;
		unsigned long offset;	/* Offset into pagecache page */
		unsigned long bytes;	/* Bytes to write to page */
		size_t copied;		/* Bytes copied from user */
		void *fsdata;

		offset = (pos & (PAGE_SIZE - 1));
		bytes = min_t(unsigned long, PAGE_SIZE - offset,
						iov_iter_count(i));

again:
		/*
		 * Bring in the user page that we will copy from _first_.
		 * Otherwise there's a nasty deadlock on copying from the
		 * same page as we're writing to, without it being marked
		 * up-to-date.
		 */
		if (unlikely(fault_in_iov_iter_readable(i, bytes) == bytes)) {
			status = -EFAULT;
			break;
		}

		if (fatal_signal_pending(current)) {
			status = -EINTR;
			break;
		}

		status = a_ops->write_begin(file, mapping, pos, bytes, flags,
						&page, &fsdata);
		if (unlikely(status < 0))
			break;

		if (mapping_writably_mapped(mapping))
			flush_dcache_page(page);

		copied = copy_page_from_iter_atomic(page, offset, bytes, i);
		flush_dcache_page(page);

		status = a_ops->write_end(file, mapping, pos, bytes, copied,
						page, fsdata);
		if (unlikely(status != copied)) {
			iov_iter_revert(i, copied - max(status, 0L));
			if (unlikely(status < 0))
				break;
		}
		cond_resched();

		if (unlikely(status == 0)) {
			/*
			 * A short copy made ->write_end() reject the
			 * thing entirely.  Might be memory poisoning
			 * halfway through, might be a race with munmap,
			 * might be severe memory pressure.
			 */
			if (copied)
				bytes = copied;
			goto again;
		}
		pos += status;
		written += status;

		balance_dirty_pages_ratelimited(mapping);
	} while (iov_iter_count(i));

	return written ? written : status;
}
EXPORT_SYMBOL(generic_perform_write);

/**
 * __generic_file_write_iter - write data to a file
 * @iocb:	IO state structure (file, offset, etc.)
 * @from:	iov_iter with data to write
 *
 * This function does all the work needed for actually writing data to a
 * file. It does all basic checks, removes SUID from the file, updates
 * modification times and calls proper subroutines depending on whether we
 * do direct IO or a standard buffered write.
 *
 * It expects i_rwsem to be grabbed unless we work on a block device or similar
 * object which does not need locking at all.
 *
 * This function does *not* take care of syncing data in case of O_SYNC write.
 * A caller has to handle it. This is mainly due to the fact that we want to
 * avoid syncing under i_rwsem.
 *
 * Return:
 * * number of bytes written, even for truncated writes
 * * negative error code if no data has been written at all
 */
ssize_t __generic_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct address_space *mapping = file->f_mapping;
	struct inode 	*inode = mapping->host;
	ssize_t		written = 0;
	ssize_t		err;
	ssize_t		status;

	/* We can write back this queue in page reclaim */
	current->backing_dev_info = inode_to_bdi(inode);
	err = file_remove_privs(file);
	if (err)
		goto out;

	err = file_update_time(file);
	if (err)
		goto out;

	if (iocb->ki_flags & IOCB_DIRECT) {
		loff_t pos, endbyte;

		written = generic_file_direct_write(iocb, from);
		/*
		 * If the write stopped short of completing, fall back to
		 * buffered writes.  Some filesystems do this for writes to
		 * holes, for example.  For DAX files, a buffered write will
		 * not succeed (even if it did, DAX does not handle dirty
		 * page-cache pages correctly).
		 */
		if (written < 0 || !iov_iter_count(from) || IS_DAX(inode))
			goto out;

		pos = iocb->ki_pos;
		status = generic_perform_write(iocb, from);
		/*
		 * If generic_perform_write() returned a synchronous error
		 * then we want to return the number of bytes which were
		 * direct-written, or the error code if that was zero.  Note
		 * that this differs from normal direct-io semantics, which
		 * will return -EFOO even if some bytes were written.
		 */
		if (unlikely(status < 0)) {
			err = status;
			goto out;
		}
		/*
		 * We need to ensure that the page cache pages are written to
		 * disk and invalidated to preserve the expected O_DIRECT
		 * semantics.
		 */
		endbyte = pos + status - 1;
		err = filemap_write_and_wait_range(mapping, pos, endbyte);
		if (err == 0) {
			iocb->ki_pos = endbyte + 1;
			written += status;
			invalidate_mapping_pages(mapping,
						 pos >> PAGE_SHIFT,
						 endbyte >> PAGE_SHIFT);
		} else {
			/*
			 * We don't know how much we wrote, so just return
			 * the number of bytes which were direct-written
			 */
		}
	} else {
		written = generic_perform_write(iocb, from);
		if (likely(written > 0))
			iocb->ki_pos += written;
	}
out:
	current->backing_dev_info = NULL;
	return written ? written : err;
}
EXPORT_SYMBOL(__generic_file_write_iter);

/**
 * generic_file_write_iter - write data to a file
 * @iocb:	IO state structure
 * @from:	iov_iter with data to write
 *
 * This is a wrapper around __generic_file_write_iter() to be used by most
 * filesystems. It takes care of syncing the file in case of O_SYNC file
 * and acquires i_rwsem as needed.
 * Return:
 * * negative error code if no data has been written at all of
 *   vfs_fsync_range() failed for a synchronous write
 * * number of bytes written, even for truncated writes
 */
ssize_t generic_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file->f_mapping->host;
	ssize_t ret;

	inode_lock(inode);
	ret = generic_write_checks(iocb, from);
	if (ret > 0)
		ret = __generic_file_write_iter(iocb, from);
	inode_unlock(inode);

	if (ret > 0)
		ret = generic_write_sync(iocb, ret);
	return ret;
}
EXPORT_SYMBOL(generic_file_write_iter);

/**
 * filemap_release_folio() - Release fs-specific metadata on a folio.
 * @folio: The folio which the kernel is trying to free.
 * @gfp: Memory allocation flags (and I/O mode).
 *
 * The address_space is trying to release any data attached to a folio
 * (presumably at folio->private).
 *
 * This will also be called if the private_2 flag is set on a page,
 * indicating that the folio has other metadata associated with it.
 *
 * The @gfp argument specifies whether I/O may be performed to release
 * this page (__GFP_IO), and whether the call may block
 * (__GFP_RECLAIM & __GFP_FS).
 *
 * Return: %true if the release was successful, otherwise %false.
 */
bool filemap_release_folio(struct folio *folio, gfp_t gfp)
{
	struct address_space * const mapping = folio->mapping;

	BUG_ON(!folio_test_locked(folio));
	if (folio_test_writeback(folio))
		return false;

	if (mapping && mapping->a_ops->releasepage)
		return mapping->a_ops->releasepage(&folio->page, gfp);
	return try_to_free_buffers(&folio->page);
}
EXPORT_SYMBOL(filemap_release_folio);
