#include "async_memory_reclaim_for_cold_file_area.h"

#define BUF_PAGE_COUNT (PAGE_COUNT_IN_AREA * 8)
#define SCAN_PAGE_COUNT_ONCE (PAGE_COUNT_IN_AREA * 8)

#define FILE_AREA_REFAULT 0
#define FILE_AREA_FREE 1
#define FILE_AREA_MAPCOUNT 2
#define FILE_AREA_HOT 3

//文件page扫描过一次后，去radix tree扫描空洞page时，一次在保存file_area的radix tree上扫描的node节点个数，一个节点64个file_area
#define SCAN_FILE_AREA_NODE_COUNT 2
#define FILE_AREA_PER_NODE TREE_MAP_SIZE

//一个冷file_area，如果经过FILE_AREA_TO_FREE_AGE_DX个周期，仍然没有被访问，则释放掉file_area结构
#define MMAP_FILE_AREA_TO_FREE_AGE_DX  30
//发生refault的file_area经过FILE_AREA_REFAULT_TO_TEMP_AGE_DX个周期后，还没有被访问，则移动到file_area_temp链表
#define MMAP_FILE_AREA_REFAULT_TO_TEMP_AGE_DX 30
//普通的file_area在FILE_AREA_TEMP_TO_COLD_AGE_DX个周期内没有被访问则被判定是冷file_area，然后释放这个file_area的page
#define MMAP_FILE_AREA_TEMP_TO_COLD_AGE_DX  10//这个参数调的很小容易在file_area被内存回收后立即释放，这样测试了很多bug，先不要改

//file_area如果在 MMAP_FILE_AREA_HOT_AGE_DX 周期内被检测到访问 MMAP_FILE_AREA_HOT_DX 次，file_area被判定为热file_area
#define MMAP_FILE_AREA_HOT_DX 2
//hot链表上的file_area在MMAP_FILE_AREA_HOT_TO_TEMP_AGE_DX个周期内没有被访问，则降级到temp链表
#define MMAP_FILE_AREA_HOT_TO_TEMP_AGE_DX 10

//mapcount的file_area在MMAP_FILE_AREA_MAPCOUNT_AGE_DX个周期内不再遍历访问，降低性能损耗
#define MMAP_FILE_AREA_MAPCOUNT_AGE_DX 5
//hot链表上的file_area在MMAP_FILE_AREA_HOT_AGE_DX个周期内不再遍历访问，降低性能损耗
#define MMAP_FILE_AREA_HOT_AGE_DX 20
//free链表上的file_area在MMAP_FILE_AREA_HOT_AGE_DX个周期内不再遍历访问，降低性能损耗
#define MMAP_FILE_AREA_FREE_AGE_DX 5
//refault链表上的file_area在MMAP_FILE_AREA_HOT_AGE_DX个周期内不再遍历访问，降低性能损耗
#define MMAP_FILE_AREA_REFAULT_AGE_DX 5

//每次扫描文件file_stat的热file_area个数
#define SCAN_HOT_FILE_AREA_COUNT_ONCE 8
//每次扫描文件file_stat的mapcount file_area个数
#define SCAN_MAPCOUNT_FILE_AREA_COUNT_ONCE 8
//当扫描完一轮文件file_stat的temp链表上的file_area时，进入冷却期，在MMAP_FILE_AREA_COLD_AGE_DX个age周期内不再扫描这个文件上的file_area
#define MMAP_FILE_AREA_COLD_AGE_DX 5


unsigned int cold_mmap_file_isolate_lru_pages_and_shrink(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat,struct file_area *p_file_area,struct page *page_buf[],int cold_page_count)
{
	unsigned int isolate_pages = 0;
	int i,traverse_page_count;
	struct page *page;
	//isolate_mode_t mode = ISOLATE_UNMAPPED;
	pg_data_t *pgdat = NULL;
	unsigned int move_page_count = 0;
	struct lruvec *lruvec = NULL,*lruvec_new = NULL;
	//unsigned long nr_reclaimed = 0;

	if(shrink_page_printk_open)
		printk("1:%s file_stat:0x%llx cold_page_count:%d\n",__func__,(u64)p_file_stat,cold_page_count);

	traverse_page_count = 0;
	//对file_stat加锁
	//lock_file_stat(p_file_stat,0);
    if(0 == file_inode_lock(p_file_stat))
		return 0;

    /*执行到这里，就不用担心该inode会被其他进程iput释放掉*/

#if 0
	//如果文件inode和mapping已经释放了，则不能再使用mapping了，必须直接return
	if(file_stat_in_delete(p_file_stat) || (NULL == p_file_stat->mapping)){
		if(shrink_page_printk_open)
			printk("2:%s file_stat:0x%llx %d_0x%llx\n",__func__,(u64)p_file_stat,file_stat_in_delete(p_file_stat),(u64)p_file_stat->mapping);

		//如果异常退出，也要对page unlock
		for(i = 0; i< cold_page_count;i ++)
		{
			page = page_buf[i];
			if(page)
				unlock_page(page);
			else
				panic("%s page error\n",__func__);
		}
		goto err;
	}
#endif	

	/*read/write系统调用的pagecache的内存回收执行的cold_file_isolate_lru_pages()函数里里，对此时并发文件inode被delete做了严格防护，这里
	 * 对mamp的pagecache是否也需要防护并发inode被delete呢？突然觉得没有必要呀？因为文件还有文件页page没有被释放呀，就是这里正在回收的
	 * 文件页！这种情况文件inode可能会被delete吗？不会吧，必须得等文件的文件页全部被回收，才可能释放文件inode吧??????????????????*/
	for(i = 0; i< cold_page_count;i ++)
	{
		page = page_buf[i];
		if(shrink_page_printk_open)
			printk("3:%s file_stat:0x%llx file_area:0x%llx page:0x%llx\n",__func__,(u64)p_file_stat,(u64)p_file_area,(u64)page);

		//此时page肯定是加锁状态，否则就主动触发crash
		if(!test_bit(PG_locked,&page->flags)){
			panic("%s page:0x%llx page->flags:0x%lx\n",__func__,(u64)page,page->flags);
		}

		if(traverse_page_count++ > 32){
			traverse_page_count = 0;
			//使用 lruvec->lru_lock 锁，且有进程阻塞在这把锁上
			if(lruvec && (spin_is_contended(&lruvec->lru_lock) || need_resched())){
				spin_unlock_irq(&lruvec->lru_lock); 
			    cond_resched();
				//msleep(5);

				spin_lock_irq(&lruvec->lru_lock);
				//p_hot_cold_file_global->hot_cold_file_shrink_counter.lru_lock_contended_count ++;
			}
		}
#if 0	
		/*到这里的page，是已经pagelock的，这里就不用再pagelock了*/
		if(unlikely(pgdat != page_pgdat(page)))
		{
			//第一次进入这个if，pgdat是NULL，此时不用spin unlock，只有后续的page才需要
			if(pgdat){
				//对之前page所属pgdat进行spin unlock
				spin_unlock_irq(&pgdat->lru_lock);
				//多次开关锁次数加1
				p_hot_cold_file_global->mmap_file_lru_lock_count++;
			}
			//pgdat最新的page所属node节点对应的pgdat
			pgdat = page_pgdat(page);
			if(pgdat != p_hot_cold_file_global->p_hot_cold_file_node_pgdat[pgdat->node_id].pgdat)
				panic("pgdat not equal\n");
			//对新的page所属的pgdat进行spin lock。内核遍历lru链表都是关闭中断的，这里也关闭中断
			spin_lock_irq(&pgdat->lru_lock);
		}
#endif
				
	     if (page && !xa_is_value(page)) {
			 /*如果page映射了也表页目录，这是异常的，要给出告警信息!!!!!!!!!!!!!!!!!!!还有其他异常状态*/
			 if (unlikely(PageAnon(page))|| unlikely(PageCompound(page)) || unlikely(PageSwapBacked(page))){
				 panic("%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx error\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state,(u64)page,page->flags);
			 }
			
			//第一次循环，lruvec是NULL，则先加锁。并对lruvec赋值，这样下边的if才不会成立，然后误触发内存回收，此时还没有move page到inactive lru链表
			if(NULL == lruvec){
				lruvec_new = mem_cgroup_lruvec(page_memcg(page),page_pgdat(page));
				lruvec = lruvec_new;
				spin_lock_irq(&lruvec->lru_lock);
			}else{
				lruvec_new = mem_cgroup_lruvec(page_memcg(page),page_pgdat(page));
			}
			
			if(!PageLRU(page) || PageUnevictable(page)){
				if(shrink_page_printk_open1)
					printk("%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx flags:0x%lx LRU:%d PageUnevictable:%d\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state,(u64)page,page->flags,PageLRU(page),PageUnevictable(page));

				unlock_page(page);
				continue;
			}

			//if成立条件如果前后的两个page的lruvec不一样 或者 遍历的page数达到32，强制进行一次内存回收
			if( (move_page_count >= SWAP_CLUSTER_MAX) ||
					unlikely(lruvec != lruvec_new))
			{
				if(0 == move_page_count)
					panic("%s scan_page_count == 0 error pgdat:0x%llx lruvec:0x%llx lruvec_new:0x%llx\n",__func__,(u64)pgdat,(u64)lruvec,(u64)lruvec_new);

				//第一次进入这个if，pgdat是NULL，此时不用spin unlock，只有后续的page才需要
				if(unlikely(lruvec != lruvec_new)){
					//多次开关锁次数加1
					p_hot_cold_file_global->lru_lock_count++;
				}
				spin_unlock_irq(&lruvec->lru_lock);

				//回收inactive lru链表尾的page，这些page刚才才移动到inactive lru链表尾
				isolate_pages += shrink_inactive_list_async(move_page_count,lruvec,p_hot_cold_file_global,1,LRU_INACTIVE_FILE);

				//回收后对move_page_count清0
				move_page_count = 0;

				//lruvec赋值最新page所属的lruvec
				lruvec = lruvec_new;
				//对新的page所属的pgdat进行spin lock。内核遍历lru链表都是关闭中断的，这里也关闭中断
				spin_lock_irq(&lruvec->lru_lock);
			}

			/*这里有个很重要的隐藏点，当执行到这里时，前后挨着的page所属的lruvec必须是同一个，这样才能
			 * list_move_tail到同一个lruvec inactive lru链表尾。否则就出乱子了，把不同lruvec的page移动到同一个。保险起见，
			 * 如果出现这种情况，强制panic*/
			if(lruvec != mem_cgroup_lruvec(page_memcg(page),page_pgdat(page)))
				panic("%s lruvec not equal error pgdat:0x%llx lruvec:0x%llx lruvec_new:0x%llx\n",__func__,(u64)pgdat,(u64)lruvec,(u64)lruvec_new);

			if(PageActive(page)){
				del_page_from_lru_list(page,lruvec);
				barrier();
				//如果page在active lru链表，则清理active属性，把page从acitve链表移动到inactive链表，并令前者链表长度减1，后者链表长度加1
				ClearPageActive(page);
				barrier();
				add_page_to_lru_list_tail(page,lruvec);
			}else{
				//否则，page只是在inactive链表里移动，直接list_move即可，不用更新链表长度
				list_move_tail(&page->lru,&lruvec->lists[LRU_INACTIVE_FILE]);
			}

			//移动到inactive lru链表尾的page数加1
			move_page_count ++;
			/*这里有个问题，如果上边的if成立，触发了内核回收，当前这个page就要一直lock page，到这里才能unlock，这样
			 * 是不是lock page时间有点长。但是为了保证这个page这段时间不会被其他进程释放掉，只能一直lock page。并且
			 * 上边if里只回收32个page，还是clean page，没有io，时间很短的。*/
			unlock_page(page);
	        }
	}

	//file_stat解锁
	//unlock_file_stat(p_file_stat);
	file_inode_unlock(p_file_stat);

	//当函数退出时，如果move_page_count大于0，则强制回收这些page
	if(move_page_count > 0){
		if(lruvec)
			spin_unlock_irq(&lruvec->lru_lock);
		//回收inactive lru链表尾的page，这些page刚才才移动到inactive lru链表尾
		isolate_pages += shrink_inactive_list_async(move_page_count,lruvec,p_hot_cold_file_global,1,LRU_INACTIVE_FILE);

	}else{
		if(lruvec)
			spin_unlock_irq(&lruvec->lru_lock);
	}

	return isolate_pages;
}

//如果一个文件file_stat超过一定比例的file_area都是热的，则判定该文件file_stat是热文，件返回1是热文件
static int inline is_mmap_file_stat_hot_file(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat){
	int ret;

	//如果文件file_stat的file_area个数比较少，超过3/4的file_area是热的，则判定文件file_stat是热文件
	if(p_file_stat->file_area_count < p_hot_cold_file_global->mmap_file_area_level_for_large_file){
		//if(div64_u64((u64)p_file_stat->file_area_count*100,(u64)p_file_stat->file_area_hot_count) > 50)
		if(p_file_stat->file_area_hot_count >= (p_file_stat->file_area_count - (p_file_stat->file_area_count >> 2)))
			ret = 1;
		else
			ret = 0;
	}else{
		//否则，文件很大，则必须热file_area超过文件总file_area个数的7/8，才能判定是热文件，这个比例后续看具体情况调整吧
		if(p_file_stat->file_area_hot_count > (p_file_stat->file_area_count - (p_file_stat->file_area_count >> 3)))
			ret  = 1;
		else
			ret =  0;
	}
	return ret;
}
//当文件file_stat的file_area个数超过阀值则判定是大文件
static int inline is_mmap_file_stat_large_file(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat)
{
	if(p_file_stat->file_area_count > hot_cold_file_global_info.mmap_file_area_level_for_large_file)
		return 1;
	else
		return 0;
}
//如果一个文件file_stat超过一定比例的file_area的page都是mapcount大于1的，则判定该文件file_stat是mapcount文件，件返回1是mapcount文件
static int inline is_mmap_file_stat_mapcount_file(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat)
{
	int ret;

	//如果文件file_stat的file_area个数比较少，超过3/4的file_area是mapcount的，则判定文件file_stat是mapcount文件
	if(p_file_stat->file_area_count < p_hot_cold_file_global->mmap_file_area_level_for_large_file){
		//if(div64_u64((u64)p_file_stat->file_area_count*100,(u64)p_file_stat->file_area_hot_count) > 50)
		if(p_file_stat->mapcount_file_area_count >= (p_file_stat->file_area_count - (p_file_stat->file_area_count >> 2)))
			ret = 1;
		else
			ret = 0;
	}else{
		//否则，文件很大，则必须热file_area超过文件总file_area个数的7/8，才能判定是mapcount文件，这个比例后续看具体情况调整吧
		if(p_file_stat->mapcount_file_area_count > (p_file_stat->file_area_count - (p_file_stat->file_area_count >> 3)))
			ret  = 1;
		else
			ret =  0;
	}
	return ret;
}
#ifdef USE_KERNEL_SHRINK_INACTIVE_LIST
//mmap的文件页page，内存回收失败，测试发现都是被访问页表pte置位了，则把这些page移动到file_stat->refault链表
static int  solve_reclaim_fail_page(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat *p_file_stat,struct list_head *page_list)
{
	struct page *page;
	pgoff_t last_index,area_index_for_page;
	struct file_area *p_file_area;
	void **page_slot_in_tree = NULL;
	struct hot_cold_file_area_tree_node *parent_node;

	last_index = (unsigned long)-1;
	list_for_each_entry(page,page_list,lru){

		area_index_for_page = page->index >> PAGE_COUNT_IN_AREA_SHIFT;
		//前后两个page都属于同一个file_area
		if(last_index == area_index_for_page)
			continue;

		last_index = area_index_for_page;
		parent_node = hot_cold_file_area_tree_lookup(&p_file_stat->hot_cold_file_area_tree_root_node,area_index_for_page,&page_slot_in_tree);
		if(IS_ERR(parent_node) || NULL == *page_slot_in_tree){
			panic("2:%s hot_cold_file_area_tree_lookup_and_create fail parent_node:0x%llx page_slot_in_tree:0x%llx\n",__func__,(u64)parent_node,(u64)page_slot_in_tree);
		}
		p_file_area = (struct file_area *)(*page_slot_in_tree);
		/*有可能前边的循环已经把这个file_area移动到refault链表了，那此时if不成立*/
		if(file_area_in_free_list(p_file_area)){
			if(file_area_in_free_list_error(p_file_area)){
				panic("%s file_area:0x%llx status:%d not in file_area_free\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
			}

			/*file_area的page在内存回收时被访问了，file_area移动到refault链表。但如果page的mapcount大于1，那要移动到file_area_mapcount链表*/
			if(page_mapcount(page) == 1){
			    clear_file_area_in_free_list(p_file_area);
			    set_file_area_in_refault_list(p_file_area);
			    list_move(&p_file_area->file_area_list,&p_file_stat->file_area_refault);
				if(shrink_page_printk_open1)
					printk("%s page:0x%llx file_area:0x%llx status:%d move to refault list\n",__func__,(u64)page,(u64)p_file_area,p_file_area->file_area_state);
			}
			else{
				p_file_stat->mapcount_file_area_count ++;
			    //file_area的page的mapcount大于1，则把file_area移动到file_stat->file_area_mapcount链表
			    clear_file_area_in_free_list(p_file_area);
			    set_file_area_in_mapcount_list(p_file_area);
			    list_move(&p_file_area->file_area_list,&p_file_stat->file_area_mapcount);
				if(shrink_page_printk_open1)
					printk("%s page:0x%llx file_area:0x%llx status:%d move to mapcount list\n",__func__,(u64)page,(u64)p_file_area,p_file_area->file_area_state);

				/*如果文件file_stat的mapcount的file_area个数超过阀值，则file_stat被判定为mapcount file_stat而移动到
			    *global mmap_file_stat_mapcount_head链表。但前提file_stat必须在temp_file链表或temp_large_file链表*/
				if(is_mmap_file_stat_mapcount_file(p_hot_cold_file_global,p_file_stat) && file_stat_in_file_stat_temp_head_list(p_file_stat)){
					 if(file_stat_in_file_stat_temp_head_list_error(p_file_stat))
						 panic("%s file_stat:0x%llx status error:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

					 clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
					 set_file_stat_in_mapcount_file_area_list(p_file_stat);
					 list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_mapcount_head);
					 p_hot_cold_file_global->mapcount_mmap_file_stat_count ++;
					 if(shrink_page_printk_open1)
						 printk("%s file_stat:0x%llx status:0x%llx is mapcount file\n",__func__,(u64)p_file_stat,(u64)p_file_stat->file_stat_status);
				}
			}
		}
	}
	return 0;
}
#endif
int  cold_mmap_file_stat_delete(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat_del)
{  

	//spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);-----有了global->mmap_file_stat_uninit_head链表后，从global temp删除file_stat，不用再加锁

	//p_file_stat_del->mapping = NULL;多余操作
	clear_file_stat_in_file_stat_temp_head_list(p_file_stat_del);
	list_del(&p_file_stat_del->hot_cold_file_list);
	//差点忘了释放file_stat结构，不然就内存泄漏了!!!!!!!!!!!!!!
	kmem_cache_free(p_hot_cold_file_global->file_stat_cachep,p_file_stat_del);
	hot_cold_file_global_info.mmap_file_stat_count --;

	//spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);

	return 0;
}
EXPORT_SYMBOL(cold_mmap_file_stat_delete);

static  unsigned int check_one_file_area_cold_page_and_clear(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat,struct file_area *p_file_area,struct page *page_buf[],int *cold_page_count)
{
	unsigned long vm_flags;
	int ret = 0;
	struct page *page;
	struct folio *folio;
	unsigned cold_page_count_temp = 0;
	int i,j;
	struct address_space *mapping = p_file_stat->mapping;
	int file_area_cold = 0;
	struct page *pages[PAGE_COUNT_IN_AREA];
	int mapcount_file_area = 0;
	int file_area_is_hot = 0;

	//file_area已经很长一段时间没有被访问则file_area_cold置1，只有在这个大前提下，file_area的page pte没有被访问，才能回收page
	if(p_hot_cold_file_global->global_age - p_file_area->file_area_age >  MMAP_FILE_AREA_TEMP_TO_COLD_AGE_DX)
		file_area_cold = 1;

	if(cold_page_count)
		cold_page_count_temp = *cold_page_count;

	if(shrink_page_printk_open)
		printk("1:%s file_stat:0x%llx file_area:0x%llx get %d page\n",__func__,(u64)p_file_stat,(u64)p_file_area,ret);

	if(0 == file_area_have_page(p_file_area))
	    goto out; 

	//ret必须清0，否则会影响下边ret += page_referenced_async的ret大于0，误判page被访问pte置位了
	ret = 0;
	for(i = 0;i < PAGE_COUNT_IN_AREA;i ++){
		//page = xa_load(&mapping->i_pages, p_file_area->start_index + i);
		folio = p_file_area->pages[i];
		page = &folio->page;
		/*这里判断并清理 映射page的页表页目录pte的access bit，是否有必要对page lock_page加锁呢?需要加锁*/
		if (page && !xa_is_value(page)) {
			/*对page上锁，上锁失败就休眠，这里回收mmap的文件页的异步内存回收线程，这里没有加锁且对性能没有要求，可以休眠
			 *到底用lock_page还是trylock_page？如果trylock_page失败的话，说明这个page最近被访问了，那肯定不是冷page，就不用执行
			 *下边的page_referenced检测page的 pte了，浪费性能。??????????????????????????????????????????????????
			 *为什么用trylock_page呢？因为page_lock了实际有两种情况 1：其他进程访问这个page然后lock_page，2：其他进程内存回收
			 *这个page然后lock_pagea。后者page并不是被其他进程被访问而lock了！因此只能用lock_page了，然后再
			 *page_referenced判断page pte，因为这个page可能被其他进程内存回收而lock_page，并不是被访问了lock_page
			 */
			if(shrink_page_printk_open)
				printk("2:%s page:0x%llx index:%ld %ld_%d\n",__func__,(u64)page,page->index,p_file_area->start_index,i);
			lock_page(page);
			//if(trylock_page(page))------不要删
			{
				/*如果page被其他进程回收了，if不成立，这些就不再对该file_area的其他page进行内存回收了，其实
				 *也可以回收，但是处理起来很麻烦，后期再考虑优化优化细节吧!!!!!!!!!!!!!!!!!!!!!!*/
				if(page->mapping != mapping){
					if(shrink_page_printk_open1)
						printk("3:%s file_stat:0x%llx file_area:0x%llx status:0x%x page->mapping != mapping!!!!!!!!!\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

					unlock_page(page);
					continue;
				}
				/*如果page不是mmap的要跳过。一个文件可能是cache文件，同时也被mmap映射，因此这类的文件页page可能不是mmap的，只是cache page
				 *这个判断必须放到lock_page后边*/
				if (!page_mapped(page)){
					unlock_page(page);
					if(shrink_page_printk_open1)
						printk("4:%s file_stat:0x%llx file_area:0x%llx status:0x%x page:0x%llx not in page_mapped error!!!!!!\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state,(u64)page);

					continue;
				}
				
				if(0 == mapcount_file_area && page_mapcount(page) > 1)
					mapcount_file_area = 1;

				//检测映射page的页表pte access bit是否置位了，是的话返回1并清除pte access bit。错了，不是返回1，是反应映射page的进程个数
				/*page_referenced函数第2个参数是0里边会自动执行lock page()。这个到底要传入folio还是page????????????????????????????????*/
				ret += folio_referenced(page_folio(page), 1, page_memcg(page),&vm_flags);
				
				if(shrink_page_printk_open)
					printk("5:%s file_stat:0x%llx file_area:0x%llx page:0x%llx index:%ld file_area_cold:%d cold_page_count:%d ret:%d page_mapcount:%d access_count:%d\n",__func__,(u64)p_file_stat,(u64)p_file_area,(u64)page,page->index,file_area_cold,cold_page_count == NULL ?-1:*cold_page_count,ret,page_mapcount(page),file_area_access_count_get(p_file_area));

				/*ret大于0说明page最近被访问了，不是冷page，则赋值全局age*/
				if(ret > 0){
					unlock_page(page);
					//本次file_area已经被判定为热file_area了，continue然后遍历下一个page
					if(file_area_is_hot)
						continue;
					file_area_is_hot = 1;

					//不能放在这里，这样二者就相等了,if(p_hot_cold_file_global->global_age - p_file_area->file_area_age <= MMAP_FILE_AREA_HOT_AGE_DX)永远成立
					//p_file_area->file_area_age = p_hot_cold_file_global->global_age;

					/*file_area必须在temp_list链表再令file_area的access_count加1，如果在固定周期内file_area的page被访问次数超过阀值，就判定为热file_area。
					 *file_area可能也在refault_list、free_list也会执行到这个函数，要过滤掉*/
					if(file_area_in_temp_list(p_file_area)){
						//file_area如果在 MMAP_FILE_AREA_HOT_AGE_DX 周期内被检测到访问 MMAP_FILE_AREA_HOT_DX 次，file_area被判定为热file_area
						if(p_hot_cold_file_global->global_age - p_file_area->file_area_age <= MMAP_FILE_AREA_HOT_AGE_DX){

						    //file_area的page被访问了，file_area的access_count加1
						    file_area_access_count_add(p_file_area,1);
							//在规定周期内file_area被访问次数大于MMAP_FILE_AREA_HOT_DX则file_area被判定为热file_area
							if(file_area_access_count_get(p_file_area) > MMAP_FILE_AREA_HOT_DX){
								//被判定为热file_area后，对file_area的access_count清0
								file_area_access_count_clear(p_file_area);

								//file_stat->temp 链表上的file_area个数减1
								p_file_stat->file_area_count_in_temp_list --;
								//file_area移动到hot链表
								clear_file_area_in_temp_list(p_file_area);
								set_file_area_in_hot_list(p_file_area);
								list_move(&p_file_area->file_area_list,&p_file_stat->file_area_hot);
								//该文件的热file_area数加1
								p_file_stat->file_area_hot_count ++;
								if(shrink_page_printk_open)
									printk("6:%s file_stat:0x%llx file_area:0x%llx is hot status:0x%x\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

								//如果文件的热file_area个数超过阀值则被判定为热文件，文件file_stat移动到global mmap_file_stat_hot_head链表
								if(is_mmap_file_stat_hot_file(p_hot_cold_file_global,p_file_stat) && file_stat_in_file_stat_temp_head_list(p_file_stat)){
									clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
									set_file_stat_in_file_stat_hot_head_list(p_file_stat);
									list_move(&p_file_stat->hot_cold_file_list,&hot_cold_file_global_info.mmap_file_stat_hot_head);
									hot_cold_file_global_info.hot_mmap_file_stat_count ++;
									if(shrink_page_printk_open)
										printk("7:%s file_stat:0x%llx status:0x%llx is hot file\n",__func__,(u64)p_file_stat,(u64)p_file_stat->file_stat_status);
								}
							}
						}else{
							//超过MMAP_FILE_AREA_HOT_AGE_DX个周期后对file_area访问计数清0
							file_area_access_count_clear(p_file_area);
						}
					}

					p_file_area->file_area_age = p_hot_cold_file_global->global_age;
					
					/*这里非常重要。当file_area的一个page发现最近访问过，不能break跳出循环。而是要继续循环把file_area剩下的page也执行
					 *page_referenced()清理掉page的pte access bit。否则，这些pte access bit置位的page会对file_area接下来的冷热造成
					 *重大误判。比如，file_area对应page0~page3，page的pte access bit全置位了。在global_age=1时，执行到该函数，这个for循环
					 *里执行page_referenced()判断出file_area的page0的pte access bit置位了，判断这个file_area最近访问过，然后自动清理掉page的
					 *pte access bit。等global_age=8,10,15时，依次又在该函数的for循环判断出page1、page2、page3的pte access bit置位了。这不仅
					 *导致误判该file_area是热的！实际情况是，page0~page3在global_age=1被访问过一次后就没有再被访问了，等到global_age=15正常
					 *要被判定为冷file_area而回收掉page。但实际却错误连续判定这个file_area一直被访问。解决方法注释掉break，换成continue，这样在
					 *global_age=1时，就会把page0~page3的pte access bit全清0，就不会影响后续判断了。但是这样性能损耗会增大，后续有打算
					 *只用file_area里的1个page判断冷热，不在扫描其他page*/
					//break;
					continue;
				}else{
					/*否则，file_area的page没有被访问，要不要立即就对file_area的access_count清0??????? 修改成，如过了规定周期file_area的page依然没被访问再对
					 *file_area的access_count清0*/
					if(file_area_in_temp_list(p_file_area) && (p_hot_cold_file_global->global_age - p_file_area->file_area_age > MMAP_FILE_AREA_HOT_AGE_DX)){
						file_area_access_count_clear(p_file_area);
					}
				}

				/*cold_page_count不是NULL说明此时遍历的是file_stat->file_area_temp链表上的file_area。否则，遍历的是
				 *file_stat->file_area_refault和file_stat->file_area_free_temp链表上的file_area，使用完page就需要unlock_page*
				 *file_area_cold是1说明此file_area是冷的，file_area的page也没有被访问，然后才回收这个file_area的page*/
				if(cold_page_count != NULL && file_area_cold){
					if(*cold_page_count < BUF_PAGE_COUNT){

						if(p_hot_cold_file_global->global_age - p_file_area->file_area_age <  MMAP_FILE_AREA_TEMP_TO_COLD_AGE_DX - 2)
							panic("%s file_stat:0x%llx status:0x%llx is hot ,can not reclaim\n",__func__,(u64)p_file_stat,(u64)p_file_stat->file_stat_status);

						//冷page保存到page_buf[]，然后参与内存回收
						page_buf[*cold_page_count] = page;
						*cold_page_count = *cold_page_count + 1;
					}
					else
						panic("%s %d error\n",__func__,*cold_page_count);
				}else{
					unlock_page(page);
				}
		 }
			/*-------很重要，不要删
			  else{
			//到这个分支，说明page被其他先lock了。1：其他进程访问这个page然后lock_page，2：其他进程内存回收这个page然后lock_pagea。
			//到底要不要令ret加1呢？想来想去不能，于是上边把trylock_page(page)改成lock_page
			//ret += 1;
			}*/
		}
	}
   
	//必须是处于global temp链表上的file_stat->file_area_temp 链表上的file_area再判断是否是mapcountfile_area
	if(file_stat_in_file_stat_temp_head_list(p_file_stat) && file_area_in_temp_list(p_file_area)){
		/*如果上边for循环遍历的file_area的page的mapcount都是1，且file_area的page上边没有遍历完，则这里继续遍历完剩余的page*/
		while(0 == mapcount_file_area && i < PAGE_COUNT_IN_AREA){
			page= pages[i];
			if (page && !xa_is_value(page) && page_mapped(page) && page_mapcount(page) > 1){
				mapcount_file_area = 1;
			}
			i ++;
		}
		if(mapcount_file_area){
			//file_stat->temp 链表上的file_area个数减1
			p_file_stat->file_area_count_in_temp_list --;
			//文件file_stat的mapcount的file_area个数加1
			p_file_stat->mapcount_file_area_count ++;
			//file_area的page的mapcount大于1，则把file_area移动到file_stat->file_area_mapcount链表
			clear_file_area_in_temp_list(p_file_area);
			set_file_area_in_mapcount_list(p_file_area);
			list_move(&p_file_area->file_area_list,&p_file_stat->file_area_mapcount);
			if(shrink_page_printk_open)
				printk("8:%s file_stat:0x%llx file_area:0x%llx state:0x%x temp to mapcount\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

			/*如果文件file_stat的mapcount的file_area个数超过阀值，则file_stat被判定为mapcount file_stat而移动到
			 *global mmap_file_stat_mapcount_head链表。但前提file_stat必须在temp_file链表或temp_large_file链表*/
			if(is_mmap_file_stat_mapcount_file(p_hot_cold_file_global,p_file_stat) /*&& file_stat_in_file_stat_temp_head_list(p_file_stat)*/){
				 if(file_stat_in_file_stat_temp_head_list_error(p_file_stat))
					 panic("%s file_stat:0x%llx status error:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

				 clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
				 set_file_stat_in_mapcount_file_area_list(p_file_stat);
				 list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_mapcount_head);
				 p_hot_cold_file_global->mapcount_mmap_file_stat_count ++;
				 if(shrink_page_printk_open1)
					 printk("9:%s file_stat:0x%llx status:0x%llx is mapcount file\n",__func__,(u64)p_file_stat,(u64)p_file_stat->file_stat_status);
			}
		}
    }

	/*到这里有这些可能
	 *1: file_area的page都是冷的，ret是0
	 *2: file_area的page有些被访问了，ret大于0
	 *3：file_area的page都是冷的，但是有些page前边trylock_page失败了，ret大于0。这种情况目前已经不可能了
	 */
	//历的是file_stat->file_area_temp链表上的file_area是if才成立
	if(ret > 0 && cold_page_count != NULL && file_area_cold){
		/*走到这里，说明file_area的page可能是热的，或者page_lock失败，那就不参与内存回收了。那就要对已加锁的page解锁*/
		//不回收该file_area的page，恢复cold_page_count
		*cold_page_count = cold_page_count_temp;
		/*解除上边加锁的page lock，cold_page_count ~ cold_page_count+i 的page上边加锁了，这里解锁*/
		for(j = 0 ;j < i;j++){
			page = page_buf[*cold_page_count + j];
			if(page){
				if(shrink_page_printk_open1)
					printk("10:%s file_stat:0x%llx file_area:0x%llx cold_page_count:%d page:0x%llx\n",__func__,(u64)p_file_stat,(u64)p_file_area,*cold_page_count,(u64)page);

				unlock_page(page);
			}
		}
	}
out:
	//返回值是file_area里4个page是热page的个数
	return ret;
}
/*1:遍历file_stat->file_area_mapcount上的file_area，如果file_area的page的mapcount都是1，file_area不再是mapcount file_area，则降级到file_stat->temp链表
 *2:遍历file_stat->file_area_hot、refault上的file_area，如果长时间不被访问了，则降级到file_stat->temp链表
 *3:遍历file_stat->file_area_free链表上的file_area，如果对应page还是长时间不访问则释放掉file_area，如果被访问了则升级到file_stat->temp链表
 */

//static int reverse_file_area_mapcount_and_hot_list(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat,struct list_head *file_area_list_head,int traversal_max_count,char type,int age_dx)//file_area_list_head 是p_file_stat->file_area_mapcount 或 p_file_stat->file_area_hot链表
static int reverse_other_file_area_list(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat,struct list_head *file_area_list_head,int traversal_max_count,char type,int age_dx)//file_area_list_head 是p_file_stat->file_area_mapcount、hot、refault、free链表
{
	unsigned int scan_file_area_count = 0;
	struct file_area *p_file_area,*p_file_area_temp;
	int i,ret;
	LIST_HEAD(file_area_list);
	struct page *page;
	struct folio *folio;

	list_for_each_entry_safe_reverse(p_file_area,p_file_area_temp,file_area_list_head,file_area_list){//从链表尾开始遍历
		//如果file_area_list_head 链表尾的file_area在规定周期内不再遍历，降低性能损耗。链表尾的file_area的file_area_access_age更小，
		//它的file_area_access_age与global_age相差小于age_dx，链表头的更小于
		if(p_hot_cold_file_global->global_age - p_file_area->file_area_access_age <= age_dx){
			if(shrink_page_printk_open)
				printk("1:%s file_stat:0x%llx type:%d  global_age:%ld file_area_access_age:%d age_dx:%d\n",__func__,(u64)p_file_stat,type,p_hot_cold_file_global->global_age,p_file_area->file_area_access_age,age_dx);

			break;
		}
		if(scan_file_area_count ++ > traversal_max_count)
			break;

		if(FILE_AREA_MAPCOUNT == type){
			if(!file_area_in_mapcount_list(p_file_area) || file_area_in_mapcount_list_error(p_file_area))
				panic("%s file_area:0x%llx status:%d not in file_area_mapcount\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
#if 0
			/*存在一种情况，file_area的page都是非mmap的，普通文件页，这样该函数也会返回0!!!!!!!!!!!!!!!!*/
			memset(pages,0,PAGE_COUNT_IN_AREA*sizeof(struct page *));
			//获取p_file_area对应的文件页page指针并保存到pages数组
			ret = get_page_from_file_area(p_file_stat,p_file_area->start_index,pages);
#endif			
			if(shrink_page_printk_open1)
				printk("1:%s file_stat:0x%llx file_area:0x%llx get %d page--------->\n",__func__,(u64)p_file_stat,(u64)p_file_area,ret);

			//file_area被遍历到时记录当时的global_age，不管此时file_area的page是否被访问pte置位了
			p_file_area->file_area_access_age = p_hot_cold_file_global->global_age;
#if 0			
			//这个file_area没有page，直接遍历下一个file_area
			if(ret <= 0)
				continue;
#endif				

	        if(0 == file_area_have_page(p_file_area))
	            continue; 

			/*存在一种情况，file_area的page都是非mmap的，普通文件页!!!!!!!!!!!!!!!!!!!*/
			for(i = 0;i < ret;i ++){
				folio = p_file_area->pages[i];
				page = &folio->page;
				if(page_mapcount(page) > 1)
					break;
			}
			//if成立说明file_area的page的mapcount都是1，file_area不再是mapcount file_area，则降级到temp_list链表头
			if(i == ret){
				//file_stat->refault、free、hot、mapcount链表上的file_area移动到file_stat->temp链表时要先对file_area->file_area_access_age清0，原因看定义
				p_file_area->file_area_access_age = 0;
				clear_file_area_in_mapcount_list(p_file_area);
				set_file_area_in_temp_list(p_file_area);
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
				//在file_stat->file_area_temp链表的file_area个数加1
				p_file_stat->file_area_count_in_temp_list ++;
				//在file_stat->file_area_mapcount链表的file_area个数减1
				p_file_stat->mapcount_file_area_count --;
			}
			else{
				/*否则file_area移动到file_area_list临时链表。但要防止前边file_area被移动到其他file_stat的链表了，此时就不能再把该file_area
				 *移动到file_area_list临时链表，否则该函数最后会把file_area再移动到老的file_stat链表，file_area的状态和所处链表就错乱了，会crash*/
				if(file_area_in_mapcount_list(p_file_area))
				    list_move(&p_file_area->file_area_list,&file_area_list);
				else
					printk("%s %d file_area:0x%llx status:%d changed\n",__func__,__LINE__,(u64)p_file_area,p_file_area->file_area_state);
			}

		}else if(FILE_AREA_HOT == type){
			if(!file_area_in_hot_list(p_file_area) || file_area_in_hot_list_error(p_file_area))
				panic("%s file_area:0x%llx status:%d not in file_area_hot\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			//file_area被遍历到时记录当时的global_age，不管此时file_area的page是否被访问pte置位了
			p_file_area->file_area_access_age = p_hot_cold_file_global->global_age;

			//检测file_area的page最近是否被访问了
			ret = check_one_file_area_cold_page_and_clear(p_hot_cold_file_global,p_file_stat,p_file_area,NULL,NULL);
			//file_area的page被访问了，依然停留在hot链表
			if(ret > 0){
				/*否则file_area移动到file_area_list临时链表。但要防止前边check_one_file_area_cold_page_and_clear()函数file_area被
				 *移动到其他file_stat的链表了，此时就不能再把该file_area移动到file_area_list临时链表，
				 否则该函数最后会把file_area再移动到老的file_stat链表，file_area的状态和所处链表就错乱了，会crash*/
				if(file_area_in_hot_list(p_file_area))
				    list_move(&p_file_area->file_area_list,&file_area_list);
				else
				    printk("%s %d file_area:0x%llx status:%d changed\n",__func__,__LINE__,(u64)p_file_area,p_file_area->file_area_state);

			}
			//file_area在MMAP_FILE_AREA_HOT_TO_TEMP_AGE_DX个周期内没有被访问，则降级到temp链表
			else if(p_hot_cold_file_global->global_age - p_file_area->file_area_age >MMAP_FILE_AREA_HOT_TO_TEMP_AGE_DX){
				//file_stat->refault、free、hot、mapcount链表上的file_area移动到file_stat->temp链表时要先对file_area->file_area_access_age清0，原因看定义
				p_file_area->file_area_access_age = 0;

				clear_file_area_in_hot_list(p_file_area);
				set_file_area_in_temp_list(p_file_area);
				barrier();
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
				//在file_stat->file_area_temp链表的file_area个数加1
				p_file_stat->file_area_count_in_temp_list ++;
				//在file_stat->file_area_hot链表的file_area个数减1
				p_file_stat->file_area_hot_count --;
			}
		}
		/*遍历file_stat->file_area_refault链表上的file_area，如果长时间没访问，要把file_area移动回file_stat->file_area_temp链表*/
		else if(FILE_AREA_REFAULT == type ){
			if(!file_area_in_refault_list(p_file_area) || file_area_in_refault_list_error(p_file_area))
				panic("%s file_area:0x%llx status:%d not in file_area_refault\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			//file_area被遍历到时记录当时的global_age，不管此时file_area的page是否被访问pte置位了
			p_file_area->file_area_access_age = p_hot_cold_file_global->global_age;

			//检测file_area的page最近是否被访问了
			ret = check_one_file_area_cold_page_and_clear(p_hot_cold_file_global,p_file_stat,p_file_area,NULL,NULL);
			if(ret > 0){
				/*否则file_area移动到file_area_list临时链表。但要防止前边check_one_file_area_cold_page_and_clear()函数file_area被
				 *移动到其他file_stat的链表了，此时就不能再把该file_area移动到file_area_list临时链表，
				 否则该函数最后会把file_area再移动到老的file_stat链表，file_area的状态和所处链表就错乱了，会crash*/
				if(file_area_in_refault_list(p_file_area))
				    list_move(&p_file_area->file_area_list,&file_area_list);
				else
				    printk("%s %d file_area:0x%llx status:%d changed\n",__func__,__LINE__,(u64)p_file_area,p_file_area->file_area_state);

			}
			//file_area在MMAP_FILE_AREA_REFAULT_TO_TEMP_AGE_DX个周期内没有被访问，则降级到temp链表
			else if(p_hot_cold_file_global->global_age - p_file_area->file_area_age >MMAP_FILE_AREA_REFAULT_TO_TEMP_AGE_DX){
				//file_stat->refault、free、hot、mapcount链表上的file_area移动到file_stat->temp链表时要先对file_area->file_area_access_age清0，原因看定义
				p_file_area->file_area_access_age = 0;

				clear_file_area_in_refault_list(p_file_area);
				set_file_area_in_temp_list(p_file_area);
				barrier();
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
				//在file_stat->file_area_temp链表的file_area个数加1
				p_file_stat->file_area_count_in_temp_list ++;
			}
		}
		/*遍历file_stat->file_area_free_temp链表上file_area，如果长时间不被访问则释放掉file_area结构。如果短时间被访问了，则把file_area移动到
		 *file_stat->file_area_refault链表，如果过了很长时间被访问了，则把file_area移动到file_stat->file_area_temp链表*/
		else if(FILE_AREA_FREE == type){

			if(!file_area_in_free_list(p_file_area) || file_area_in_free_list_error(p_file_area))
				panic("%s file_area:0x%llx status:%d not in file_area_free\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			//file_area被遍历到时记录当时的global_age，不管此时file_area的page是否被访问pte置位了
			p_file_area->file_area_access_age = p_hot_cold_file_global->global_age;

			//检测file_area的page最近是否被访问了
			ret = check_one_file_area_cold_page_and_clear(p_hot_cold_file_global,p_file_stat,p_file_area,NULL,NULL);
			if(0 == ret){
				//file_stat->file_area_free_temp链表上file_area，如果长时间不被访问则释放掉file_area结构，里边有把file_area从链表剔除的代码
				if(p_hot_cold_file_global->global_age - p_file_area->file_area_age > MMAP_FILE_AREA_TO_FREE_AGE_DX){
					clear_file_area_in_free_list(p_file_area);
					cold_file_area_detele_quick(p_hot_cold_file_global,p_file_stat,p_file_area);
				}else{
					/*否则file_area移动到file_area_list临时链表。但要防止前边check_one_file_area_cold_page_and_clear()函数file_area被
					 *移动到其他file_stat的链表了，此时就不能再把该file_area移动到file_area_list临时链表，
					 否则该函数最后会把file_area再移动到老的file_stat链表，file_area的状态和所处链表就错乱了，会crash*/
					if(file_area_in_free_list(p_file_area))
						list_move(&p_file_area->file_area_list,&file_area_list);
					else
						printk("%s %d file_area:0x%llx status:%d changed\n",__func__,__LINE__,(u64)p_file_area,p_file_area->file_area_state);
				}
			}else{
				if(0 == p_file_area->shrink_time)
					panic("%s file_stat:0x%llx status:0x%lx p_file_area->shrink_time == 0\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

				//file_area的page被访问而pte置位了，则file_area->file_area_age记录当时的全局age。然后把file_area移动到file_stat->refault或temp链表
				//在check_one_file_area_cold_page_and_clear函数如果page被访问过，就会对file_area->file_area_age赋值，这里就不用再赋值了
				//p_file_area->file_area_age = p_hot_cold_file_global->global_age;

				clear_file_area_in_free_list(p_file_area);
				/*file_stat->file_area_free_temp链表上file_area，短时间被访问了，则把file_area移动到file_stat->file_area_refault链表。否则
				 *移动到file_stat->file_area_temp链表*/
				if(p_file_area->shrink_time && (ktime_to_ms(ktime_get()) - (p_file_area->shrink_time << 10) < 130000)){
					set_file_area_in_refault_list(p_file_area);
					barrier();
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_refault);	
				}
				else{
					//file_stat->refault、free、hot、mapcount链表上的file_area移动到file_stat->temp链表时要先对file_area->file_area_access_age清0
					p_file_area->file_area_access_age = 0;
					set_file_area_in_temp_list(p_file_area);
					barrier();
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
					//在file_stat->file_area_temp链表的file_area个数加1
					p_file_stat->file_area_count_in_temp_list ++;
				}
				p_file_area->shrink_time = 0;
			}
		}

		//在把file_area移动到其他链表后，file_area_list_head链表可能是空的，没有file_area了，就结束遍历。其实这个判断list_for_each_entry_safe_reverse也有
		if(list_empty(file_area_list_head)){
			break;
		}
	}
	//如果file_area_list临时链表上有file_area，则要移动到 file_area_list_head链表头，最近遍历过的file_area移动到链表头
	if(!list_empty(&file_area_list)){
		list_splice(&file_area_list,file_area_list_head);
	}
	if(shrink_page_printk_open1)
		printk("2:%s file_stat:0x%llx type:%d scan_file_area_count:%d\n",__func__,(u64)p_file_stat,type,scan_file_area_count);
	return scan_file_area_count;
}

#if 0 //这段代码不要删除，有重要价值
/*遍历file_stat->file_area_refault和file_stat->file_area_free_temp链表上的file_area，根据具体情况处理*/
static int reverse_file_area_refault_and_free_list(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat,struct file_area **file_area_last,struct list_head *file_area_list_head,int traversal_max_count,char type)
{//file_area_list_head 是file_stat->file_area_refault或file_stat->file_area_free_temp链表头

	int ret;
	unsigned int scan_file_area_count = 0;
	struct file_area *p_file_area,*p_file_area_temp;
	char delete_file_area_last = 0;

	printk("1:%s file_stat:0x%llx file_area_last:0x%llx type:%d\n",__func__,(u64)p_file_stat,(u64)*file_area_last,type);
	if(*file_area_last){//本次从上一轮扫描打断的file_area继续遍历
		p_file_area = *file_area_last;
	}
	else{
		//第一次从链表尾的file_area开始遍历
		p_file_area = list_last_entry(file_area_list_head,struct file_area,file_area_list);
		*file_area_last = p_file_area;
	}

	do {
		/*查找file_area在file_stat->file_area_temp链表上一个file_area。如果p_file_area不是链表头的file_area，直接list_prev_entry
		 * 找到上一个file_area。如果p_file_stat是链表头的file_area，那要跳过链表过，取出链表尾的file_area*/
		if(!list_is_first(&p_file_area->file_area_list,file_area_list_head))
			p_file_area_temp = list_prev_entry(p_file_area,file_area_list);
		else
			p_file_area_temp = list_last_entry(file_area_list_head,struct file_area,file_area_list);

		//检测file_area的page最近是否被访问了
		ret = check_one_file_area_cold_page_and_clear(p_hot_cold_file_global,p_file_stat,p_file_area,NULL,NULL);

		/*遍历file_stat->file_area_refault链表上的file_area，如果长时间没访问，要把file_area移动回file_stat->file_area_temp链表*/
		if(FILE_AREA_REFAULT == type ){
			if(!file_area_in_refault_list(p_file_area) || file_area_in_refault_list_error(p_file_area))
				panic("%s file_area:0x%llx status:%d not in file_area_refault\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			if(ret > 0){
				p_file_area->file_area_age = p_hot_cold_file_global->global_age;
			}
			//file_area在MMAP_FILE_AREA_REFAULT_TO_TEMP_AGE_DX个周期内没有被访问，则降级到temp链表
			else if(p_hot_cold_file_global->global_age - p_file_area->file_area_age >MMAP_FILE_AREA_REFAULT_TO_TEMP_AGE_DX){
				//这段if判断代码的原因分析见check_file_area_cold_page_and_clear()函数
				if(*file_area_last == p_file_area){
					*file_area_last = p_file_area_temp;
					delete_file_area_last = 1;
				}
				clear_file_area_in_refault_list(p_file_area);
				set_file_area_in_temp_list(p_file_area);
				barrier();
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
				//在file_stat->file_area_temp链表的file_area个数加1
				p_file_stat->file_area_count_in_temp_list ++;
			}
		}
		/*遍历file_stat->file_area_free_temp链表上file_area，如果长时间不被访问则释放掉file_area结构。如果短时间被访问了，则把file_area移动到
		 *file_stat->file_area_refault链表，如果过了很长时间被访问了，则把file_area移动到file_stat->file_area_temp链表*/
		else if(FILE_AREA_FREE == type){

			if(!file_area_in_free_list(p_file_area) || file_area_in_free_list_error(p_file_area))
				panic("%s file_area:0x%llx status:%d not in file_area_free\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

			if(0 == ret){
				//file_stat->file_area_free_temp链表上file_area，如果长时间不被访问则释放掉file_area结构，里边有把file_area从链表剔除的代码
				if(p_hot_cold_file_global->global_age - p_file_area->file_area_age >  MMAP_FILE_AREA_TEMP_TO_COLD_AGE_DX){
					//这段if代码的原因分析见check_file_area_cold_page_and_clear()函数
					if(*file_area_last == p_file_area){
						*file_area_last = p_file_area_temp;
						delete_file_area_last = 1;
					}
					clear_file_area_in_free_list(p_file_area);
					cold_file_area_detele_quick(p_hot_cold_file_global,p_file_stat,p_file_area);
				}
			}else{
				if(0 == p_file_area->shrink_time)
					panic("%s file_stat:0x%llx status:0x%lx p_file_area->shrink_time == 0\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

				//这段if代码的原因分析见check_file_area_cold_page_and_clear()函数
				if(*file_area_last == p_file_area){
					*file_area_last = p_file_area_temp;
					delete_file_area_last = 1;
				}

				p_file_area->file_area_age = p_hot_cold_file_global->global_age;
				clear_file_area_in_free_list(p_file_area);
				/*file_stat->file_area_free_temp链表上file_area，短时间被访问了，则把file_area移动到file_stat->file_area_refault链表。否则
				 *移动到file_stat->file_area_temp链表*/
				if(p_file_area->shrink_time && (ktime_to_ms(ktime_get()) - (p_file_area->shrink_time << 10) < 5000)){
					set_file_area_in_refault_list(p_file_area);
					barrier();
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_refault);	
				}
				else{
					set_file_area_in_temp_list(p_file_area);
					barrier();
					list_move(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
					//在file_stat->file_area_temp链表的file_area个数加1
					p_file_stat->file_area_count_in_temp_list ++;
				}
				p_file_area->shrink_time = 0;
			}
		}

		//下一个扫描的file_area
		p_file_area = p_file_area_temp;

		//超过本轮扫描的最大file_area个数则结束本次的遍历
		if(scan_file_area_count > traversal_max_count)
			break;
		scan_file_area_count ++;

		if(0 == delete_file_area_last && p_file_area == *file_area_last)
			break;
		else if(delete_file_area_last)
			delete_file_area_last = 0;

		/*这里退出循环的条件，不能碰到链表头退出，是一个环形队列的遍历形式,以下两种情况退出循环
		 *1：上边的 遍历指定数目的file_area后，强制结束遍历
		 *2：这里的while，本次循环处理到file_area已经是第一次循环处理过了，相当于重复了
		 *3: 添加!list_empty(&file_area_list_head)判断，详情见check_file_area_cold_page_and_clear()分析
		 */
		//}while(p_file_area != *file_area_last && !list_empty(file_area_list_head));
    }while(!list_empty(file_area_list_head));

	if(!list_empty(file_area_list_head)){
		/*下个周期直接从file_area_last指向的file_area开始扫描*/
		if(!list_is_first(&p_file_area->file_area_list,file_area_list_head))
			*file_area_last = list_prev_entry(p_file_area,file_area_list);
		else
			*file_area_last = list_last_entry(file_area_list_head,struct file_area,file_area_list);
	}else{
		/*这里必须对file_area_last清NULL，否则下次执行该函数，file_area_last指向的是一个非法的file_area，从而触发crash。比如
		 *file_stat->file_area_free链表只有一个file_area，因为长时间不被访问，执行cold_file_area_detele_quick()释放了。但是释放
		 前，先执行*file_area_last = p_file_area_temp赋值，这个赋值令*file_area_last指向刚释放的file_area，因为p_file_area_temp
		 指向释放的file_area，file_stat->file_area_free链表只有这一个file_area！继续，释放唯一的file_area后，此时file_stat->file_area_free链表空
		 (即file_area_list_head链表空)，则跳出while循环。然后 *file_area_last 还是指向刚释放file_area。下次执行该函数，使用 *file_area_last
		 这个指向的已经释放的file_aera，就会crash*/
		*file_area_last = NULL;
	}

    return scan_file_area_count;
}
#endif

#if 0 //这段源码不要删除，牵涉到一个内存越界的重大bug!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
/*文件的radix tree在遍历完一次所有的page后，可能存在空洞，于是后续还要再遍历文件的radix tree获取之前没有遍历到的page*/
static unsigned int reverse_file_stat_radix_tree_hole(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat)
{
	int i,j,k;
	struct hot_cold_file_area_tree_node *parent_node;
	void **page_slot_in_tree = NULL;
	unsigned int area_index_for_page;
	int ret = 0;
	struct page *page;
	unsigned int file_page_count = p_file_stat->mapping->host->i_size >> PAGE_SHIFT;//除以4096
	unsigned int start_page_index = 0;
	struct address_space *mapping = p_file_stat->mapping;

	//p_file_stat->traverse_done是0，说明还没有遍历完一次文件的page，那先返回
	if(0 == p_file_stat->traverse_done)
		return ret;

	printk("1:%s file_stat:0x%llx file_stat->last_index:0x%d\n",__func__,(u64)p_file_stat,p_file_stat->last_index);

	//p_file_stat->last_index初值是0，后续依次是64*1、64*2、64*3等等，
	area_index_for_page = p_file_stat->last_index;

	//一次遍历SCAN_FILE_AREA_NODE_COUNT个node，一个node 64个file_area
	for(i = 0;i < SCAN_FILE_AREA_NODE_COUNT;i++){
		/*查找索引0、64*1、64*2、64*3等等的file_area的地址，保存到page_slot_in_tree。page_slot_in_tree指向的是每个node节点的第一个file_area，
		 *每个node节点一共64个file_area，都保存在node节点的slots[64]数组。下边的for循环一次查找node->slots[0]~node->slots[63]，如果是NULL，
		 *说明还没有分配file_area，是空洞，那就分配file_area并添加到radix tree。否则说明file_area已经分配了，就不用再管了*/
		parent_node = hot_cold_file_area_tree_lookup_and_create(&p_file_stat->hot_cold_file_area_tree_root_node,area_index_for_page,&page_slot_in_tree);
		if(IS_ERR(parent_node)){
			ret = -1;
			printk("%s hot_cold_file_area_tree_lookup_and_create fail\n",__func__);
			goto out;
		}
		/*一个node FILE_AREA_PER_NODE(64)个file_area。下边靠着page_slot_in_tree++依次遍历这64个file_area，如果*page_slot_in_tree
		 *是NULL，说明是空洞file_area，之前这个file_area对应的page没有分配，也没有分配file_area，于是按照area_index_for_page<<PAGE_COUNT_IN_AREA_SHIFT
		 *这个page索引，在此查找page是否分配了，是的话就分配file_area*/
		for(j = 0;j < FILE_AREA_PER_NODE - 1;){

			printk("2:%s file_stat:0x%llx i:%d j:%d page_slot_in_tree:0x%llx\n",__func__,(u64)p_file_stat,i,j,(u64)(*page_slot_in_tree));
			//如果是空树，parent_node是NULL，page_slot_in_tree是NULL，*page_slot_in_tree会导致crash
			if(NULL == *page_slot_in_tree){
				//第一次area_index_for_page是0时，start_page_index取值，依次是 0*4 、1*4、2*4、3*4....63*4
				start_page_index = (area_index_for_page + j) << PAGE_COUNT_IN_AREA_SHIFT;
				//page索引超过文件最大page数，结束遍历
				if(start_page_index > file_page_count){
					printk("3:%s start_page_index:%d > file_page_count:%d\n",__func__,start_page_index,file_page_count);
					ret = 1;
					goto out;
				}
				for(k = 0;k < PAGE_COUNT_IN_AREA;k++){
					/*这里需要优化，遍历一次radix tree就得到4个page，完全可以实现的，节省性能$$$$$$$$$$$$$$$$$$$$$$$$*/
					page = xa_load(&mapping->i_pages, start_page_index + k);
					//空洞file_area的page被分配了，那就分配file_area
					if (page && !xa_is_value(page) && page_mapped(page)) {

						//分配file_area并初始化，成功返回0，函数里边把新分配的file_area赋值给*page_slot_in_tree，即在radix tree的槽位
						if(file_area_alloc_and_init(parent_node,page_slot_in_tree,page->index >> PAGE_COUNT_IN_AREA_SHIFT,p_file_stat) < 0){
							ret = -1;
							goto out;
						}
						printk("3:%s file_stat:0x%llx alloc file_area:0x%llx\n",__func__,(u64)p_file_stat,(u64)(*page_slot_in_tree));
						/*4个连续的page只要有一个在radix tree找到，分配file_area,之后就不再查找其他page了*/
						break;
					}
				}
				printk("3:%s start_page_index:%d > file_page_count:%d\n",__func__,start_page_index,file_page_count);
				ret = 1;
				goto out;
			}
			for(k = 0;k < PAGE_COUNT_IN_AREA;k++){
				/*这里需要优化，遍历一次radix tree就得到4个page，完全可以实现的，节省性能$$$$$$$$$$$$$$$$$$$$$$$$*/
				page = xa_load(&mapping->i_pages, start_page_index + k);
				//空洞file_area的page被分配了，那就分配file_area
				if (page && !xa_is_value(page) && page_mapped(page)) {

					//分配file_area并初始化，成功返回0，函数里边把新分配的file_area赋值给*page_slot_in_tree，即在radix tree的槽位
					if(file_area_alloc_and_init(parent_node,page_slot_in_tree,page->index >> PAGE_COUNT_IN_AREA_SHIFT,p_file_stat) < 0){
						ret = -1;
						goto out;
					}
					printk("3:%s file_stat:0x%llx alloc file_area:0x%llx\n",__func__,(u64)p_file_stat,(u64)(*page_slot_in_tree));
					/*4个连续的page只要有一个在radix tree找到，分配file_area,之后就不再查找其他page了*/
					break;
				}
			}
		}
		/*这里有个重大bug，当保存file_area的radix tree的file_area全被释放了，是个空树，此时area_index_for_page指向的是radix tree的根节点的指针的地址，
		 * 即area_index_for_page指向 p_file_stat->hot_cold_file_area_tree_root_node->root_node的地址，然后这里的page_slot_in_tree ++就有问题了。
		 * 原本的设计，area_index_for_page最初指向的是node节点node->slot[64]数组槽位0的slot的地址，然后page_slot_in_tree++依次指向槽位0到槽位63
		 * 的地址。然后看*page_slot_in_tree是否是NULL，是的话说明file_area已经分配。否则说明是空洞file_area，那就要执行xa_load()探测对应索引
		 * 的文件页是否已经分配并插入radix tree(保存page指针的radix tree)了，是的话就file_area_alloc_and_init分配file_area并保存到
		 * page_slot_in_tree指向的保存file_area的radix tree。..........但是，现在保存file_area的radix tree，是个空树，area_index_for_page
		 * 经过上边hot_cold_file_area_tree_lookup探测索引是0的file_area后，指向的是该radix tree的根节点指针的地址，
		 * 即p_file_stat->hot_cold_file_area_tree_root_node->root_node的地址。没办法，这是radix tree的特性，如果只有一个索引是0的成员，该成员
		 * 就是保存在radix tree的根节点指针里。如此，page_slot_in_tree ++就内存越界了，越界到p_file_stat->hot_cold_file_area_tree_root_node
		 * 成员的后边，即p_file_stat的file_stat_lock、max_file_area_age、recent_access_age等成员里，然后对应page分配的话，就要创建新的
		 * file_area并保存到 p_file_stat的file_stat_lock、max_file_area_age、recent_access_age里，导致这些应该是0的成员但却很大。
		 *
		 * 解决办法是，如果该radix tree是空树，先xa_load()探测索引是0的file_aera对应的索引是0~3的文件页page是否分配了，是的话就创建file_area并保存到
		 * radix tree的p_file_stat->hot_cold_file_area_tree_root_node->root_node。然后不令page_slot_in_tree ++，而是xa_load()探测索引是1的file_aera
		 * 对应的索引是4~7的文件页page是否分配了，是的话，直接执行hot_cold_file_area_tree_lookup_and_create创建这个file_area，不是探测结束。
		 * 并且要令p_file_stat->last_index恢复0，这样下次执行该函数还是从索引是0的file_area开始探测，然后探测索引是1的file_area对应的文件页是否分配了。
		 * 这样有点啰嗦，并且会重复探测索引是0的file_area。如果索引是1的file_area的文件页page没分配，那索引是2的file_area的文件页page被分配了。
		 * 现在的代码就有问题了，不会针对这这种情况分配索引是2的file_area*/
		//page_slot_in_tree ++;

		if((NULL == parent_node) && (0 == j) && (0 == area_index_for_page)){
			printk("4:%s file_stat:0x%llx page_slot_in_tree:0x%llx_0x%llx j:%d\n",__func__,(u64)p_file_stat,(u64)page_slot_in_tree,(u64)&p_file_stat->hot_cold_file_area_tree_root_node.root_node,j);
			for(k = 0;k < PAGE_COUNT_IN_AREA;k++){
				/*探测索引是1的file_area对应的文件页page是否分配了，是的话就创建该file_area并插入radix tree*/
				page = xa_load(&mapping->i_pages, PAGE_COUNT_IN_AREA + k);
				if (page && !xa_is_value(page) && page_mapped(page)) {
					//此时file_area的radix tree还是空节点，现在创建根节点node，函数返回后page_slot_in_tree指向的是根节点node->slots[]数组槽位1的地址，下边
					//file_area_alloc_and_init再分配索引是1的file_area并添加到插入radix tree，再赋值高node->slots[1]，即槽位1
					parent_node = hot_cold_file_area_tree_lookup_and_create(&p_file_stat->hot_cold_file_area_tree_root_node,1,&page_slot_in_tree);
					if(IS_ERR(parent_node)){
						ret = -1;
						printk("%s hot_cold_file_area_tree_lookup_and_create fail\n",__func__);
						goto out;
					}

					if(NULL == parent_node || *page_slot_in_tree != NULL){
						panic("%s parent_node:0x%llx *page_slot_in_tree:0x%llx\n",__func__,(u64)parent_node,(u64)(*page_slot_in_tree));
					}
					//分配file_area并初始化，成功返回0，函数里边把新分配的file_area赋值给*page_slot_in_tree，即在radix tree的槽位
					if(file_area_alloc_and_init(parent_node,page_slot_in_tree,1,p_file_stat) < 0){
						ret = -1;
						goto out;
					}
					printk("5:%s file_stat:0x%llx alloc file_area:0x%llx\n",__func__,(u64)p_file_stat,(u64)(*page_slot_in_tree));
					/*4个连续的page只要有一个在radix tree找到，分配file_area,之后就不再查找其他page了*/
					break;
				}
			}

			/*如果parent_node不是NULL。说明上边索引是0的file_area的对应文件页page分配了，创建的根节点，parent_node就是这个根节点。并且，令j加1.
			 *这样下边再就j加1，j就是2了，page_slot_in_tree = parent_node.slots[j]指向的是索引是2的file_area，然后探测对应文件页是否分配了。
			 *因为索引是0、1的file_area已经探测过了。如果 parent_node是NULL，那说明索引是1的file_area对应的文件页page没分配，上边也没有创建
			 *根节点。于是令p_file_stat->last_index清0，直接goto out，这样下次执行该函数，还是从索引是0的file_area开始探测。这样有个问题，如经
			 *索引是1的file_area对应文件页没分配，这类直接goto out了，那索引是2的file_area对应的文件页分配，就不理会了。这种可能是存在的！索引
			 *是2的file_area的文件页page也应该探测呀，后溪再改进吧
			 */
			if(parent_node){
				j ++;
			}else{
				p_file_stat->last_index = 0;
				goto out;
			}
		}
		//j加1令page_slot_in_tree指向下一个file_area
		j++;
		//不用page_slot_in_tree ++了，虽然性能好点，但是内存越界了也不知道。page_slot_in_tree指向下一个槽位的地址
		page_slot_in_tree = &parent_node->slots[j];
#ifdef __LITTLE_ENDIAN//这个判断下端模式才成立
		if((u64)page_slot_in_tree < (u64)(&parent_node->slots[0]) || (u64)page_slot_in_tree > (u64)(&parent_node->slots[TREE_MAP_SIZE])){		
			panic("%s page_slot_in_tree:0x%llx error 0x%llx_0x%llx\n",__func__,(u64)page_slot_in_tree,(u64)(&parent_node->slots[0]),(u64)(&parent_node->slots[TREE_MAP_SIZE]));
		}
#endif	
		//page_slot_in_tree ++;
	    //area_index_for_page的取值，0，后续依次是64*1、64*2、64*3等等，
	    area_index_for_page += FILE_AREA_PER_NODE;
    }
	//p_file_stat->last_index记录下次要查找的第一个node节点的file_area的索引
	p_file_stat->last_index = area_index_for_page;
out:
	if(start_page_index > file_page_count){
		//p_file_stat->last_index清0，下次从头开始扫描文件页
		p_file_stat->last_index = 0;
	}
	return ret;
}
#endif

#if 0
/*跟reverse_file_stat_radix_tree_hole函数一样，现在不需要了*/
static int check_page_exist_and_create_file_area(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat *p_file_stat,struct hot_cold_file_area_tree_node **_parent_node,void ***_page_slot_in_tree,unsigned int area_index)
{
	/*空树时函数返回NULL并且page_slot_in_tree指向root->root_node的地址。当传入索引很大找不到file_area时，函数返回NULL并且page_slot_in_tree不会被赋值(保持原值NULL)*/

	int ret = 0;
	int k;
	pgoff_t start_page_index;
	struct page *page;
	struct page *pages[PAGE_COUNT_IN_AREA];

	//struct address_space *mapping = p_file_stat->mapping;
	void **page_slot_in_tree = *_page_slot_in_tree;
	struct hot_cold_file_area_tree_node *parent_node = *_parent_node;
	//file_area的page有一个mapcount大于1，则是mapcount file_area，再把mapcount_file_area置1
	bool mapcount_file_area = 0;
	struct file_area *p_file_area = NULL;

	start_page_index = (area_index) << PAGE_COUNT_IN_AREA_SHIFT;
    
	memset(pages,0,PAGE_COUNT_IN_AREA*sizeof(struct page *));
	//获取p_file_area对应的文件页page指针并保存到pages数组
	ret = get_page_from_file_area(p_file_stat,start_page_index,pages);
	if(shrink_page_printk_open)
		printk("1:%s file_stat:0x%llx start_page_index:%ld get %d page\n",__func__,(u64)p_file_stat,start_page_index,ret);

	if(ret <= 0)
	    goto out; 

	/*探测area_index对应file_area索引的page是否分配了，分配的话则分配对应的file_area。但是如果父节点不存在，需要先分配父节点*/
	for(k = 0;k < PAGE_COUNT_IN_AREA;k++){
		/*探测索引是1的file_area对应的文件页page是否分配了，是的话就创建该file_area并插入radix tree*/
		//page = xa_load(&mapping->i_pages, start_page_index + k);
		page= pages[k];
		//area_index对应file_area索引的page存在
		if (page && !xa_is_value(page) && page_mapped(page)){

			if( 0 == mapcount_file_area && page_mapcount(page) > 1)
				mapcount_file_area = 1;

			//父节点不存在则创建父节点，并令page_slot_in_tree指向area_index索引对应file_area在父节点的槽位parent_node.slots[槽位索引]槽位地址
			if(NULL == parent_node){//parent_node是NULL，page_slot_in_tree一定也是NULL
				parent_node = hot_cold_file_area_tree_lookup_and_create(&p_file_stat->hot_cold_file_area_tree_root_node,area_index,&page_slot_in_tree);
				if(IS_ERR(parent_node)){
					ret = -1;
					printk("%s hot_cold_file_area_tree_lookup_and_create fail\n",__func__);
					goto out;
				}

			}
			/*到这里，page_slot_in_tree一定不是NULL，是则触发crash。如果parent_node是NULL是有可能的，当radix tree是空树时。查找索引是0的file_area
			 *时，parent_node是NULL，page_slot_in_tree指向p_file_stat->hot_cold_file_area_tree_root_node->root_node的地址。否则，其他情况触发crash*/
			if((area_index != 0 && NULL == parent_node) || (NULL == page_slot_in_tree)){
				panic("%s parent_node:0x%llx *page_slot_in_tree:0x%llx\n",__func__,(u64)parent_node,(u64)(*page_slot_in_tree));
			}
			p_file_area = file_area_alloc_and_init(parent_node,page_slot_in_tree,area_index,p_file_stat);
			//分配file_area并初始化，成功返回0，函数里边把新分配的file_area赋值给*page_slot_in_tree，即在radix tree的槽位
			if(NULL == p_file_area){
				ret = -1;
				goto out;
			}
			//在file_stat->file_area_temp链表的file_area个数加1
			p_file_stat->file_area_count_in_temp_list ++;

			ret = 1;
			//令_page_slot_in_tree指向新分配的file_area在radix tree的parent_node.slots[槽位索引]槽位地址
			if(NULL == *_page_slot_in_tree)
				*_page_slot_in_tree = page_slot_in_tree;

			//新分配的parent_node赋值给*_parent_node
			if(NULL == *_parent_node)
				*_parent_node = parent_node;

			//只要有一个page在radix tree找到，分配file_area,之后就不再查找其他page了
			break;
		}
	}

	/*如果上边for循环遍历的file_area的page的mapcount都是1，且file_area的page上边没有遍历完，则这里继续遍历完剩余的page*/
	while(0 == mapcount_file_area && k < PAGE_COUNT_IN_AREA){
		page= pages[k];
		if (page && !xa_is_value(page) && page_mapped(page) && page_mapcount(page) > 1){
			mapcount_file_area = 1;
		}
		k ++;
	}
	if(mapcount_file_area){
		//file_stat->temp 链表上的file_area个数减1
		p_file_stat->file_area_count_in_temp_list --;
		//文件file_stat的mapcount的file_area个数加1
		p_file_stat->mapcount_file_area_count ++;
		//file_area的page的mapcount大于1，则把file_area移动到file_stat->file_area_mapcount链表
		clear_file_area_in_temp_list(p_file_area);
		set_file_area_in_mapcount_list(p_file_area);
		list_move(&p_file_area->file_area_list,&p_file_stat->file_area_mapcount);
		if(shrink_page_printk_open)
			printk("5:%s file_stat:0x%llx file_area:0x%llx state:0x%x is mapcount file_area\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);

		/*如果文件file_stat的mapcount的file_area个数超过阀值，则file_stat被判定为mapcount file_stat而移动到
		 *global mmap_file_stat_mapcount_head链表。但前提file_stat必须在temp_file链表或temp_large_file链表*/
		if(is_mmap_file_stat_mapcount_file(p_hot_cold_file_global,p_file_stat) && file_stat_in_file_stat_temp_head_list(p_file_stat)){
			 if(file_stat_in_file_stat_temp_head_list_error(p_file_stat))
				 panic("%s file_stat:0x%llx status error:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

			 clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
			 set_file_stat_in_mapcount_file_area_list(p_file_stat);
			 list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_mapcount_head);
		 	 p_hot_cold_file_global->mapcount_mmap_file_stat_count ++;
			 if(shrink_page_printk_open1)
				 printk("6:%s file_stat:0x%llx status:0x%llx is mapcount file\n",__func__,(u64)p_file_stat,(u64)p_file_stat->file_stat_status);
		}
	}

out:
	return ret;
}

/*reverse_file_stat_radix_tree_hole()最初是为了ko模式异步内存回收设计的:保存file_area指针的是radix tree，保存page指针的
 *是另一个xarray tree，二者独立。因此，需要时不时的执行该函数，遍历xarray tree，探测一个个page，看哪些page还没有创建file_area。
  没有的话则创建file_area并添加到file_stat->temp链表，后续就可以探测这些page冷热，参与内存回收。

  然而，现在file_area和page是一体了，file_area指针保存在原保存page指针的xarray tree，page指针保存在file_area->pages[]数组。
  后续，如果该mmap文件访问任意一个新的page，先分配一个page，最后执行到 __filemap_add_folio->__filemap_add_folio_for_file_area()
  ->file_area_alloc_and_init()函数，必然分配file_area并添加到file_stat->temp链表，然后把新的page保存到file_area->pages[]
  数组。也就是说，现在不用再主动执行reverse_file_stat_radix_tree_hole()探测空洞page然后创建file_area了。内核原有机制就可以
  保证mmap的文件一旦有新的page，自己创建file_area并添加到file_stat->temp链表。后续异步内存回收线程就可以探测该file_area的
  冷热page，然后内存回收。
*/
/*文件的radix tree在遍历完一次所有的page后，可能存在空洞，于是后续还要再遍历文件的radix tree获取之前没有遍历到的page*/
static unsigned int reverse_file_stat_radix_tree_hole(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat)
{
	int i,j;
	struct hot_cold_file_area_tree_node *parent_node;
	void **page_slot_in_tree = NULL;
	unsigned int base_area_index;
	int ret = 0;
	unsigned int file_page_count = p_file_stat->mapping->host->i_size >> PAGE_SHIFT;//除以4096
	unsigned int start_page_index = 0;
	struct file_area *p_file_area;

	//p_file_stat->traverse_done是0，说明还没有遍历完一次文件的page，那先返回
	if(0 == p_file_stat->traverse_done)
		return ret;
	if(shrink_page_printk_open)
		printk("1:%s file_stat:0x%llx file_stat->last_index:%ld\n",__func__,(u64)p_file_stat,p_file_stat->last_index);

	//p_file_stat->last_index初值是0，后续依次是64*1、64*2、64*3等等，
	base_area_index = p_file_stat->last_index;
	//一次遍历SCAN_FILE_AREA_NODE_COUNT个node，一个node 64个file_area
	for(i = 0;i < SCAN_FILE_AREA_NODE_COUNT;i++){
		//每次必须对page_slot_in_tree赋值NULL，下边hot_cold_file_area_tree_lookup()如果没找到对应索引的file_area，page_slot_in_tree还是NULL
		page_slot_in_tree = NULL;
		j = 0;

		/*查找索引0、64*1、64*2、64*3等等的file_area的地址，保存到page_slot_in_tree。page_slot_in_tree指向的是每个node节点的第一个file_area，
		 *每个node节点一共64个file_area，都保存在node节点的slots[64]数组。下边的for循环一次查找node->slots[0]~node->slots[63]，如果是NULL，
		 *说明还没有分配file_area，是空洞，那就分配file_area并添加到radix tree。否则说明file_area已经分配了，就不用再管了*/

		/*不能用hot_cold_file_area_tree_lookup_and_create，如果是空树，但是去探测索引是1的file_area，此时会直接分配索引是1的file_area对应的node节点
		 *并插入radix tree，注意是分配node节点。而根本不管索引是1的file_area对应的文件页page是否分配了。这样会分配很多没用的node节点，而不管对应索引的
		 *file_area的文件页是否分配了，浪费内存。这里只能探测file_area是否存在，不能node节点*/
		//parent_node = hot_cold_file_area_tree_lookup_and_create(&p_file_stat->hot_cold_file_area_tree_root_node,base_area_index,&page_slot_in_tree);

		/*空树时函数返回NULL并且page_slot_in_tree指向root->root_node的地址。当传入索引很大找不到file_area时，函数返回NULL并且page_slot_in_tree不会被赋值(保持原值NULL)*/
		parent_node = hot_cold_file_area_tree_lookup(&p_file_stat->hot_cold_file_area_tree_root_node,base_area_index,&page_slot_in_tree);
		if(IS_ERR(parent_node)){
			ret = -1;
			printk("2:%s hot_cold_file_area_tree_lookup_and_create fail\n",__func__);
			goto out;
		}
		/*一个node FILE_AREA_PER_NODE(64)个file_area。下边靠着page_slot_in_tree++依次遍历这64个file_area，如果*page_slot_in_tree
		 *是NULL，说明是空洞file_area，之前这个file_area对应的page没有分配，也没有分配file_area，于是按照base_area_index<<PAGE_COUNT_IN_AREA_SHIFT
		 *这个page索引，在此查找page是否分配了，是的话就分配file_area*/
		while(1){
			/* 1：hot_cold_file_area_tree_lookup中找到对应索引的file_area，parent_node非NULL，page_slot_in_tree和*page_slot_in_tree都非NULL
			 * 2：hot_cold_file_area_tree_lookup中没找到对应索引的file_area，但是父节点存在，parent_node非NULL，page_slot_in_tree非NULL，*page_slot_in_tree是NULL
			 * 3：hot_cold_file_area_tree_lookup中没找到对应索引的file_area，父节点也不存在，parent_node是NULL，page_slot_in_tree是NULL，此时不能出现*page_slot_in_tree
			 * 另外，如果radix tree是空树，lookup索引是0的file_area后，page_slot_in_tree指向p_file_stat->hot_cold_file_area_tree_root_node->root_node的地址，
			 *    parent_node是NULL，page_slot_in_tree和*page_slot_in_tree都非NULL
			 *
			 * 简单说，
			 * 情况1：只要待查找索引的file_area的父节点存在，parent_node不是NULL，page_slot_in_tree也一定不是NULL，page_slot_in_tree指向保存
			 * file_area指针在父节点的槽位地址，即parent_node.slot[槽位索引]的地址。如果file_area存在则*page_slot_in_tree非NULL，否则*page_slot_in_tree是NULL
			 * 情况2：待查找的file_area的索引太大，没找到父节点，parent_node是NULL，page_slot_in_tree也是NULL，此时不能用*page_slot_in_tree，会crash
			 * 情况3：radix tree是空树，lookup索引是0的file_area后， parent_node是NULL，page_slot_in_tree非NULL，指向p_file_stat->hot_cold_file_area_tree_root_node->root_node的地址
			 * */

			start_page_index = (base_area_index + j) << PAGE_COUNT_IN_AREA_SHIFT;
			if(start_page_index >= file_page_count){
				if(shrink_page_printk_open)
					printk("3:%s start_page_index:%d >= file_page_count:%d\n",__func__,start_page_index,file_page_count);

				goto out;
			}
			if(shrink_page_printk_open){
				if(page_slot_in_tree)
					printk("4:%s file_stat:0x%llx i:%d j:%d start_page_index:%d base_area_index:%d parent_node:0x%llx page_slot_in_tree:0x%llx *page_slot_in_tree:0x%llx\n",__func__,(u64)p_file_stat,i,j,start_page_index,base_area_index,(u64)parent_node,(u64)page_slot_in_tree,(u64)(*page_slot_in_tree));
				else
					printk("4:%s file_stat:0x%llx i:%d j:%d start_page_index:%d base_area_index:%d parent_node:0x%llx page_slot_in_tree:0x%llx\n",__func__,(u64)p_file_stat,i,j,start_page_index,base_area_index,(u64)parent_node,(u64)page_slot_in_tree);
			}

			/* (NULL == page_slot_in_tree)：对应情况2，radix tree现在节点太少，待查找的file_area索引太大找不到父节点和file_area的槽位，
			 * parent_node 和 page_slot_in_tree都是NULL。那就执行check_page_exist_and_create_file_area()分配父节点 parent_node，并令page_slot_in_tree指向
			 * parent_node->slots[槽位索引]槽位，然后分配对应索引的file_area并保存到parent_node->slots[槽位索引]
			 *
			 * (NULL!= page_slot_in_tree && NULL == *page_slot_in_tree)对应情况1和情况3。情况1：找到对应索引的file_area的槽位，即parent_node.slot[槽位索引]，
			 * parent_node 和 page_slot_in_tree都非NULL，但*page_slot_in_tree是NULL，那就执行check_page_exist_and_create_file_area()分配对应索引的file_area结构
			 * 并保存到parent_node.slot[槽位索引]。 情况3：radix tree是空树，lookup索引是0的file_area后， parent_node是NULL，page_slot_in_tree非NULL，
			 * page_slot_in_tree指向p_file_stat->hot_cold_file_area_tree_root_node->root_node的地址，但如果*page_slot_in_tree是NULL，说明file_area没有分配，
			 * 那就执行check_page_exist_and_create_file_area()分配索引是0的file_area并保存到 p_file_stat->hot_cold_file_area_tree_root_node->root_node。
			 *
			 * */
			if((NULL == page_slot_in_tree)  || (NULL!= page_slot_in_tree && NULL == *page_slot_in_tree)){
				ret = check_page_exist_and_create_file_area(p_hot_cold_file_global,p_file_stat,&parent_node,&page_slot_in_tree,base_area_index + j);
				if(ret < 0){
					printk("5:%sheck_page_exist_and_create_file_area fail\n",__func__);
					goto out;
				}else if(ret >0){
					//ret 大于0说明上边创建了file_area或者node节点，这里再打印出来
					if(shrink_page_printk_open)
						printk("6:%s file_stat:0x%llx i:%d j:%d start_page_index:%d base_area_index:%d parent_node:0x%llx page_slot_in_tree:0x%llx *page_slot_in_tree:0x%llx\n",__func__,(u64)p_file_stat,i,j,start_page_index,base_area_index,(u64)parent_node,(u64)page_slot_in_tree,(u64)(*page_slot_in_tree));
				}
			}

			if(page_slot_in_tree){
				barrier();
				if(*page_slot_in_tree){
					p_file_area = (struct file_area *)(*page_slot_in_tree);
					//file_area自身保存的索引数据 跟所在radix tree的槽位位置不一致，触发crash
					if((p_file_area->start_index >>PAGE_COUNT_IN_AREA_SHIFT) != base_area_index + j)
						panic("%s file_area index error!!!!!! file_stat:0x%llx p_file_area:0x%llx p_file_area->start_index:%ld base_area_index:%d j:%d\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->start_index,base_area_index,j);

				}
			}
			/*
			//情况1：只要待查找索引的file_area的父节点存在，parent_node不是NULL，page_slot_in_tree也一定不是NULL
			if(parent_node){
			    //待查找索引的file_area不存在，则探测它对应的page是否存在，存在的话则分配file_area
			    if(NULL == *page_slot_in_tree){
					if(check_page_exist_and_create_file_area(p_hot_cold_file_global,p_file_stat,&parent_node,&page_slot_in_tree,base_area_index + j) < 0){
			           goto out;
			        }
			    }
			    //待查找索引的file_area存在，什么都不用干
			    else{}
			}
			else
			{
				//情况2：待查找的file_area的索引太大，没找到父节点，parent_node是NULL，page_slot_in_tree也是NULL，此时不能用*page_slot_in_tree，会crash
				if(NULL == page_slot_in_tree){
					if(check_page_exist_and_create_file_area(p_hot_cold_file_global,p_file_stat,&parent_node,&page_slot_in_tree,base_area_index+j) < 0){
					    goto out;
					}
					//到这里，如果指定索引的file_area的page存在，则创建父节点和file_area，parent_node和page_slot_in_tree不再是NULL，*ppage_slot_in_tree也非NULL
				}
				//情况3：radix tree是空树，lookup索引是0的file_area后， parent_node是NULL，page_slot_in_tree非NULL，指向*p_file_stat->hot_cold_file_area_tree_root_node->root_node的地址
				else{
					if((0 == j) && (0 == base_area_index)&& (page_slot_in_tree ==  &p_file_stat->hot_cold_file_area_tree_root_node.root_node)){
						//如果索引是0的file_area不存在，则探测对应page是否存在，存在的话创建索引是0的file_area，不用创建父节点，file_area指针保存在p_file_stat->hot_cold_file_area_tree_root_node->root_node
						if(NULL == *page_slot_in_tree){
							if(check_page_exist_and_create_file_area(p_hot_cold_file_global,p_file_stat,&parent_node,&page_slot_in_tree,base_area_index + j) < 0){
								goto out;
							}
						}
					}else{
						if(check_page_exist_and_create_file_area(p_hot_cold_file_global,p_file_stat,&parent_node,&page_slot_in_tree,base_area_index + j) < 0){
						    goto out;
						}
						//这里可能进入，空树时，探测索引很大的file_area
						printk("%s j:%d base_area_index:%d page_slot_in_tree:0x%llx_0x%llx error!!!!!!!!!\n",__func__,j,base_area_index,(u64)page_slot_in_tree,(u64)&p_file_stat->hot_cold_file_area_tree_root_node.root_node);
					}
				}
			}
			*/			
			//依次只能遍历FILE_AREA_PER_NODE 个file_area
			if(j >= FILE_AREA_PER_NODE - 1)
				break;

			//j加1令page_slot_in_tree指向下一个file_area
			j++;
			if(parent_node){
				//不用page_slot_in_tree ++了，虽然性能好点，但是内存越界了也不知道。page_slot_in_tree指向下一个槽位的地址
				page_slot_in_tree = &parent_node->slots[j];
#ifdef __LITTLE_ENDIAN//这个判断下端模式才成立
				if((u64)page_slot_in_tree < (u64)(&parent_node->slots[0]) || (u64)page_slot_in_tree > (u64)(&parent_node->slots[TREE_MAP_SIZE])){		
					panic("%s page_slot_in_tree:0x%llx error 0x%llx_0x%llx\n",__func__,(u64)page_slot_in_tree,(u64)(&parent_node->slots[0]),(u64)(&parent_node->slots[TREE_MAP_SIZE]));
				}
#endif
			}else{
				/*到这里，应该radix tree是空树时才成立，要令page_slot_in_tree指向NULL，否则当前这个for循环的page_slot_in_tree值会被错误用到下个循环*/
				page_slot_in_tree = NULL;
			}
		}
		//base_area_index的取值，0，后续依次是64*1、64*2、64*3等等，
		base_area_index += FILE_AREA_PER_NODE;
	}
	//p_file_stat->last_index记录下次要查找的第一个node节点的file_area的索引
	p_file_stat->last_index = base_area_index;
out:
	if((ret >= 0) && ((base_area_index +j) << PAGE_COUNT_IN_AREA_SHIFT >= file_page_count)){
		if(shrink_page_printk_open1)
			printk("7:%s last_index = 0 last_index:%ld base_area_index:%d j:%d file_page_count:%d\n",__func__,p_file_stat->last_index,base_area_index,j,file_page_count);

		//p_file_stat->last_index清0，下次从头开始扫描文件页
		p_file_stat->last_index = 0;
	}
	return ret;
}
#endif
/*1:先file_stat->file_area_temp链表尾巴遍历file_area，如果在规定周期被访问次数超过阀值，则判定为热file_area而移动
 * file_stat->file_area_hot链表。如果file_stat->file_area_hot链表的热file_area超过阀值则该文件被判定为热文件，file_stat移动到
 * global hot链表。
 *2:如果ile_stat->file_area_temp上file_area长时间不被访问，则释放掉file_area的page，并把file_area移动到file_stat->file_area_free链表
 *  file_stat->file_area_free 和 file_stat->file_area_free_temp 在这里一个意思。
 *3:遍历file_stat->file_area_refault、hot、mapcount、free链表上file_area，处理各不相同，升级或降级到file_stat->temp链表，或者
 *  释放掉file_area，具体看源码吧
 *4:如果file_stat->file_area_temp链表上的file_area遍历了一遍，则进入冷却期。在N个周期内，不在执行该函数遍历该文件
 *file_stat->temp、refault、free、mapcount、hot 链表上file_area。file_stat->file_area_temp链表上的file_area遍历了一遍，导致文件进入
 *冷冻期，此时页牵连无法遍历到该文件file_stat->refault、free、mapcount、hot 链表上file_area，这合理吗。当然可以分开遍历，但是目前
 *觉得没必要，因为file_stat->refault、free、mapcount、hot 链表上file_area也有冷冻期，这个冷冻期还更长，是N的好几倍。因此不会影响，还降低性能损耗
 *5:遍历文件file_stat的原生radix tree，是否存在空洞file_area，是的话则为遍历到的新的文件页创建file_area
 */
static unsigned int check_file_area_cold_page_and_clear(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat,unsigned int scan_file_area_max,unsigned int *already_scan_file_area_count)
{
	struct page *page_buf[BUF_PAGE_COUNT];
	int cold_page_count = 0,cold_page_count_last;
	int ret = 0;
	struct file_area *p_file_area,*p_file_area_temp;
	char delete_file_area_last = 0;
	unsigned int reclaimed_pages = 0;
	unsigned int isolate_pages = 0;
	LIST_HEAD(file_area_temp_head);
	memset(page_buf,0,BUF_PAGE_COUNT*sizeof(struct page*));

	/*注意，执行该函数的file_stat都是处于global temp链表的，file_stat->file_area_temp和 file_stat->file_area_mapcount 链表上都有file_area,mapcount的file_area
	 *变多了，达到了该文件要变成mapcount文件的阀值。目前在下边的check_one_file_area_cold_page_and_clear函数里，只会遍历file_stat->file_area_mapcount 链表上
	 *的file_area，如果不是mapcount了，那就降级到file_stat->file_area_temp链表。没有遍历file_stat->file_area_temp链表上的file_area，如果对应page的mapcount大于1
	 *了，再把file_area升级到file_stat->file_area_mapcount链表。如果mapcount的file_area个数超过阀值，那就升级file_stat到mapcount文件。这个有必要做吗???????，
	 *想想还是做吧，否则这种file_area的page回收时很容易失败*/
	if(shrink_page_printk_open)
		printk("1:%s file_stat:0x%llx file_area_last:0x%llx file_area_count_in_temp_list:%d file_area_hot_count:%d mapcount_file_area_count:%d file_area_count:%d\n",__func__,(u64)p_file_stat,(u64)p_file_stat->file_area_last,p_file_stat->file_area_count_in_temp_list,p_file_stat->file_area_hot_count,p_file_stat->mapcount_file_area_count,p_file_stat->file_area_count);

	if(p_file_stat->file_area_last){//本次从上一轮扫描打断的file_stat继续遍历
		p_file_area = p_file_stat->file_area_last;
	}
	else{
		//第一次从链表尾的file_stat开始遍历。或者新的冷却期开始后也是
		p_file_area = list_last_entry(&p_file_stat->file_area_temp,struct file_area,file_area_list);
		p_file_stat->file_area_last = p_file_area;
	}

	while(!list_empty(&p_file_stat->file_area_temp)){

		if(!file_area_in_temp_list(p_file_area) || file_area_in_temp_list_error(p_file_area))
			panic("%s file_area:0x%llx status:%d not in file_area_temp\n",__func__,(u64)p_file_area,p_file_area->file_area_state);

		/*查找file_area在file_stat->file_area_temp链表上一个file_area。如果p_file_area不是链表头的file_area，直接list_prev_entry
		 * 找到上一个file_area。如果p_file_stat是链表头的file_area，那要跳过链表过，取出链表尾的file_area*/
		if(!list_is_first(&p_file_area->file_area_list,&p_file_stat->file_area_temp)){
			p_file_area_temp = list_prev_entry(p_file_area,file_area_list);
		}
		else{
			//从链表尾遍历完一轮file_area了，文件file_stat要进入冷却期
			p_file_stat->cooling_off_start = 1;
			//记录此时的全局age
			p_file_stat->cooling_off_start_age = p_hot_cold_file_global->global_age;
			if(shrink_page_printk_open)
				printk("1_1:%s file_stat:0x%llx cooling_off_start age:%ld\n",__func__,(u64)p_file_stat,p_file_stat->cooling_off_start_age);

			p_file_area_temp = list_last_entry(&p_file_stat->file_area_temp,struct file_area,file_area_list);
		}

		/*遍历file_stat->file_area_temp，查找冷的file_area*/
		cold_page_count_last = cold_page_count;
		if(shrink_page_printk_open)
			printk("2:%s file_stat:0x%llx file_area:0x%llx index:%ld scan_file_area_count_temp_list:%d file_area_count_in_temp_list:%d\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->start_index,p_file_stat->scan_file_area_count_temp_list,p_file_stat->file_area_count_in_temp_list);
	
	    //这个错误赋值会影响到file_stat->access_count，导致误判为热file_area	
		//p_file_area->file_area_access_age = p_hot_cold_file_global->global_age;
		ret = check_one_file_area_cold_page_and_clear(p_hot_cold_file_global,p_file_stat,p_file_area,page_buf,&cold_page_count);
		/*到这里有两种可能
		 *1: file_area的page都是冷的，ret是0
		 *2: file_area的page有些被访问了，ret大于0
		 *3：file_area的page都是冷的，但是有些page前边trylock_page失败了，ret大于0,这种情况后期得再优化优化细节!!!!!!!!!!!!!
		 */
		if(0 == ret){
			/*加下边这个if判断，是因为之前设计的以p_file_stat->file_area_last为基准的循环遍历链表有个重大bug：while循环第一次
			 *遍历p_file_area时，p_file_area和p_file_stat->file_area_last是同一个。而如果p_file_area是冷的，并且本次它的page的
			 *pte页表也没访问，那就要把file_area移动到file_stat->file_area_free_temp链表。这样后续这个while就要陷入死循环了，
			 *因为p_file_stat->file_area_last指向的file_area移动到file_stat->file_area_free_temp链表了。而p_file_stat一个个
			 *指向file_stat->file_area_temp链表的file_area。下边的while(p_file_area != p_file_stat->file_area_last)永远不成立
			 *并且上边check_one_file_area_cold_page_and_clear()里传入的file_area是重复的，会导致里边重复判断page和lock_page，
			 然后就会出现进程hung在lock_page里，因为重复lock_page。解决办法是

			 1：凡是p_file_area和p_file_stat->file_area_last是同一个，一定要更新p_file_stat->file_area_last为p_file_area在
			 file_stat->file_area_temp链表的前一个file_area。
			 2：下边else分支的file_area不太冷但它的page是冷的情况，要把file_area从file_stat->file_area_temp链表移除，并移动到
			 file_area_temp_head临时链表。while循环结束时再把这些file_area移动到file_stat->file_area_temp链表尾。这样避免这个
			 while循环里，重复遍历这种file_area，重复lock_page 对应的page，又要hung
			 3：while循环的退出条件加个 !list_empty(file_stat->file_area_temp)。这样做的目的是，如果file_stat->file_area_temp链表
			 只有一个file_area，而它和它的page都是冷的，它移动到ile_stat->file_area_free_temp链表后，p_file_stat->file_area_last
			 指向啥呢？这个链表没有成员了！只能靠!list_empty(file_stat->file_area_temp)退出while循环

			 注意，还有一个隐藏bug，当下边的if成立时，这个while循环就立即退出了，不会接着遍历了。因为if成立时，
			 p_file_stat->file_area_last = p_file_area_temp，二者相等，然后下边执行p_file_area = p_file_area_temp 后，就导致
			 p_file_stat->file_area_last 和 p_file_area 也相等了，while(p_file_area != p_file_stat->file_area_last)就不成立了。
			 解决办法时，当发现该if成立，令 delete_file_area_last置1，然后下边跳出while循环的条件改为
			 if(0 == delete_file_area_last && p_file_area != p_file_stat->file_area_last) break。就是说，当发现
			 本次要移动到其他链表的p_file_area是p_file_stat->file_area_last时，令p_file_stat->file_area_last指向p_file_area在
			 file_stat->file_area_temp链表的上一个file_area(即p_file_area_temp)后，p_file_area也会指向这个file_area，此时不能
			 跳出while循环，p_file_area此时的新file_area还没使用过呢！
			 **/
			if(!file_area_in_temp_list(p_file_area) && (p_file_area == p_file_stat->file_area_last)){
				p_file_stat->file_area_last = p_file_area_temp;
				delete_file_area_last = 1;
			}

			/*二者不相等，说明file_area是冷的，并且它的page的pte本次检测也没被访问，这种情况才回收这个file_area的page*/
			if(cold_page_count_last != cold_page_count)
			{
				//处于file_stat->tmep链表上的file_area，移动到其他链表时，要先对file_area的access_count清0，否则会影响到
				//file_area->file_area_access_age变量，因为file_area->access_count和file_area_access_age是共享枚举变量
				file_area_access_count_clear(p_file_area);
				//file_stat->temp 链表上的file_area个数减1
				p_file_stat->file_area_count_in_temp_list --;

				clear_file_area_in_temp_list(p_file_area);
				/*设置file_area处于file_stat的free_temp_list链表。这里设定，不管file_area处于file_stat->file_area_free_temp还是
				 *file_stat->file_area_free链表，都是file_area_in_free_list状态，没有必要再区分二者*/
				set_file_area_in_free_list(p_file_area);
				/*冷file_area移动到file_area_free_temp链表参与内存回收。移动到 file_area_free_temp链表的file_area也要每个周期都扫描。
				 *1：如果对应文件页长时间不被访问，那就释放掉file_area
				 *2：如果对应page内存回收又被访问了，file_area就要移动到refault链表，长时间不参与内存回收
				 *3：如果refault链表上的file_area的page长时间不被访问，file_area就要降级到temp链表
				 *4：如果文件file_stat的file_area全被释放了，file_stat要移动到 zero_file_area链表，然后释放掉file_stat结构
				 *5：在驱动卸载时，要释放掉所有的file_stat和file_area*/
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_free_temp);
				//记录file_area参与内存回收的时间
				p_file_area->shrink_time = ktime_to_ms(ktime_get()) >> 10;
			}else{
				/*如果file_area的page没被访问，但是file_area还不是冷的，file_area不太冷，则把file_area先移动到临时链表，然后该函数最后再把
				 *该临时链表上的不太冷file_area同统一移动到file_stat->file_area_temp链表尾。这样做的目的是，避免该while循环里重复遍历到
				 *这些file_area*/
				//list_move_tail(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
				list_move(&p_file_area->file_area_list,&file_area_temp_head);
			}
		}else if(ret > 0){
			/*如果file_area的page被访问了，则把file_area移动到链表头-------这个操作就多余了，去掉，只要把不太冷的file_area移动到
			  file_stat->file_area_temp链表尾就行了，这样就达到目的：链表尾是冷file_area，链表头是热file_area*/
			//list_move(&p_file_area->file_area_list,&p_file_stat->file_area_temp);

			/*如果file_area被判定是热file_area等原因而移动到了其他链表，并且file_area_in_temp_list(p_file_area)成立，
			 *并且，p_file_area是p_file_stat->file_area_last，要强制更新p_file_stat->file_area_last为p_file_area_temp。
			 *因为此时这个p_file_stat->file_area_last已经不再处于temp链表了，可能会导致死循环。原因上边友分析*/
			if(!file_area_in_temp_list(p_file_area) && (p_file_area == p_file_stat->file_area_last)){
				p_file_stat->file_area_last = p_file_area_temp;
				delete_file_area_last = 1;
			}
		}

		/*1:凑够BUF_PAGE_COUNT个要回收的page，if成立，则开始隔离page、回收page
		 *2:page_buf剩余的空间不足容纳PAGE_COUNT_IN_AREA个page，if也成立，否则下个循环执行check_one_file_area_cold_page_and_clear函数
		 *向page_buf保存PAGE_COUNT_IN_AREA个page，将导致内存溢出*/
		if(cold_page_count >= BUF_PAGE_COUNT || (BUF_PAGE_COUNT - cold_page_count <=  PAGE_COUNT_IN_AREA)){

		    isolate_pages += cold_mmap_file_isolate_lru_pages_and_shrink(p_hot_cold_file_global,p_file_stat,p_file_area,page_buf,cold_page_count);
		    reclaimed_pages = p_hot_cold_file_global->hot_cold_file_shrink_counter.mmap_free_pages_count;
        	cold_page_count = 0;
			if(shrink_page_printk_open)
				printk("3:%s file_stat:0x%llx reclaimed_pages:%d isolate_pages:%d\n",__func__,(u64)p_file_stat,reclaimed_pages,isolate_pages);
		}

		/*下一个扫描的file_area。这个对p_file_area赋值p_file_area_temp，要放到if(*already_scan_file_area_count > scan_file_area_max) break;
		 *跳出while循环前边。否则会存在这样一种问题，前边p_file_area不是太冷而移动到了file_area_temp_head链表头，然后下边break跳出，
		 *p_file_area此时指向的是file_area已经移动到file_area_temp_head链表头链表头了，且这个链表只有这一个file_area。然后下边执行
		 *p_file_stat->file_area_last = list_prev_entry(p_file_area,file_area_list) 对p_file_stat->file_area_last赋值时，
		 *p_file_stat->file_area_last指向file_area就是file_area_temp_head链表头了。下次执行这个函数时，使用p_file_stat->file_area_last
		 *指向的file_area时非法的了*/
		p_file_area = p_file_area_temp;

		//异步内存回收线程本次运行扫描的总file_area个数加1，
		*already_scan_file_area_count = *already_scan_file_area_count + 1;
		//文件file_stat已经扫描的file_area个数加1
		p_file_stat->scan_file_area_count_temp_list ++;

		//超过本轮扫描的最大file_area个数则结束本次的遍历
		if(*already_scan_file_area_count >= scan_file_area_max)
			break;

		/*文件file_stat已经扫描的file_area个数超过file_stat->file_area_temp 链表的总file_area个数，停止扫描该文件的file_area。
		 *然后才会扫描global->mmap_file_stat_temp_head或mmap_file_stat_temp_large_file_head链表上的下一个文件file_stat的file_area
		 *文件file_stat进入冷却期if也成。其实这两个功能重复了，本质都表示遍历完file_stat->temp链表上的file_area*/
		if(/*p_file_stat->scan_file_area_count_temp_list >= p_file_stat->file_area_count_in_temp_list ||*/ p_file_stat->cooling_off_start){
			//文件扫描的file_area个数清0，下次轮到扫描该文件的file_area时，才能继续扫描
			p_file_stat->scan_file_area_count_temp_list = 0;
			ret = 1;
			break;
		}

		if(0 == delete_file_area_last && p_file_area == p_file_stat->file_area_last){
			ret = 1;
			break;
		}
		else if(1 == delete_file_area_last)
			delete_file_area_last = 0;


	/*这里退出循环的条件，不能碰到链表头退出，是一个环形队列的遍历形式,以下两种情况退出循环
	 *1：上边的 遍历指定数目的file_area后，强制结束遍历
	 *2：这里的while，本次循环处理到file_area已经是第一次循环处理过了，相当于重复了
	 */
	//}while(p_file_area != p_file_stat->file_area_last);
	//}while(p_file_area != p_file_stat->file_area_last  && !list_empty(&p_file_stat->file_area_temp));
	//}while(!list_empty(&p_file_stat->file_area_temp));
	}

	/*如果到这里file_stat->file_area_temp链表时空的，说明上边的file_area都被遍历过了，那就令p_file_stat->file_area_last = NULL。
	 *否则令p_file_stat->file_area_last指向本次最后在file_stat->file_area_temp链表上遍历的file_area的上一个file_area*/
	if(!list_empty(&p_file_stat->file_area_temp)){
		/*下个周期直接从p_file_stat->file_area_last指向的file_area开始扫描*/
		if(!list_is_first(&p_file_area->file_area_list,&p_file_stat->file_area_temp))
			p_file_stat->file_area_last = list_prev_entry(p_file_area,file_area_list);
		else
			p_file_stat->file_area_last = list_last_entry(&p_file_stat->file_area_temp,struct file_area,file_area_list);
	}else{
		p_file_stat->file_area_last = NULL;
		//当前文件file_stat->file_area_temp上的file_area扫描完了，需要扫描下一个文件了
		ret = 1;
	}

	if(!list_empty(&file_area_temp_head))
		//把本次扫描的暂存在file_area_temp_head临时链表上的不太冷的file_area移动到file_stat->file_area_temp链表尾
		list_splice_tail(&file_area_temp_head,&p_file_stat->file_area_temp);

    if(shrink_page_printk_open)
	    printk("4:%s file_stat:0x%llx cold_page_count:%d\n",__func__,(u64)p_file_stat,cold_page_count);

	//如果本次对文件遍历结束后，有未达到BUF_PAGE_COUNT数目要回收的page，这里就隔离+回收这些page
	if(cold_page_count){
		isolate_pages += cold_mmap_file_isolate_lru_pages_and_shrink(p_hot_cold_file_global,p_file_stat,p_file_area,page_buf,cold_page_count);
		reclaimed_pages = p_hot_cold_file_global->hot_cold_file_shrink_counter.mmap_free_pages_count;
    	if(shrink_page_printk_open)
			printk("5:%s %s file_stat:0x%llx reclaimed_pages:%d isolate_pages:%d\n",__func__,p_file_stat->file_name,(u64)p_file_stat,reclaimed_pages,isolate_pages);
	}

    /*遍历file_stat->file_area_free_temp链表上已经释放page的file_area，如果长时间还没被访问，那就释放掉file_area。
	 *否则访问的话，要把file_area移动到file_stat->file_area_refault或file_area_temp链表。是否一次遍历完file_area_free_temp
	 *链表上所有的file_area呢？那估计会很损耗性能，因为要检测这些file_area的page映射页表的pte，这样太浪费性能了！
	 *也得弄个file_area_last，每次只遍历file_area_free_temp链表上几十个file_area，file_area_last记录最后一次的
	 *file_area的上一个file_area，下次循环直接从file_area_last指向file_area开始遍历。这样就不会重复遍历file_area，
	 *也不会太浪费性能*/

/*
#if 0
    //list_for_each_entry_safe_reverse(p_file_area,tmp_file_area,&p_file_stat->file_area_free_temp,file_area_list)
	if(!list_empty(&p_file_stat->file_area_free_temp)){
		reverse_file_area_refault_and_free_list(p_hot_cold_file_global,p_file_stat,&p_file_stat->file_area_refault_last,&p_file_stat->file_area_free_temp,32,FILE_AREA_FREE);
	}
	//遍历file_stat->file_area_refault链表上file_area，如果长时间不被访问，要降级到file_stat->file_area_temp链表
	//list_for_each_entry_safe_reverse(p_file_area,tmp_file_area,&p_file_stat->file_area_refault,file_area_list)
	if(!list_empty(&p_file_stat->file_area_refault)){
		reverse_file_area_refault_and_free_list(p_hot_cold_file_global,p_file_stat,&p_file_stat->file_area_free_last,&p_file_stat->file_area_refault,16,FILE_AREA_REFAULT);
	}
	//遍历file_stat->file_area_mapcount上的file_area，如果file_area的page的mapcount都是1，file_area不再是mapcount file_area，则降级到temp_list
	if(!list_empty(&p_file_stat->file_area_mapcount)){
		reverse_file_area_mapcount_and_hot_list(p_hot_cold_file_global,p_file_stat,&p_file_stat->file_area_mapcount,8,FILE_AREA_MAPCOUNT,MMAP_FILE_AREA_MAPCOUNT_AGE_DX);
	}
	//遍历file_stat->file_area_hot上的file_area，如果长时间不被访问了，则降级到temp_list
	if(!list_empty(&p_file_stat->file_area_hot)){
		reverse_file_area_mapcount_and_hot_list(p_hot_cold_file_global,p_file_stat,&p_file_stat->file_area_hot,8,FILE_AREA_HOT,MMAP_FILE_AREA_HOT_AGE_DX);
	}	
*/
//#else
	/*遍历file_stat->file_area_free链表上file_area，如果长时间不被访问则释放掉，如果被访问了则升级到file_stat->file_area_refault或temp链表*/
	if(!list_empty(&p_file_stat->file_area_free_temp)){
		reverse_other_file_area_list(p_hot_cold_file_global,p_file_stat,&p_file_stat->file_area_free_temp,32,FILE_AREA_FREE,MMAP_FILE_AREA_FREE_AGE_DX);
	}
	/*遍历file_stat->file_area_refault链表上file_area，如果长时间不被访问，要降级到file_stat->file_area_temp链表*/
	if(!list_empty(&p_file_stat->file_area_refault)){
		reverse_other_file_area_list(p_hot_cold_file_global,p_file_stat,&p_file_stat->file_area_refault,16,FILE_AREA_REFAULT,MMAP_FILE_AREA_REFAULT_AGE_DX);
	}
	//遍历file_stat->file_area_mapcount上的file_area，如果file_area的page的mapcount都是1，file_area不再是mapcount file_area，则降级到temp_list
	if(!list_empty(&p_file_stat->file_area_mapcount)){
		reverse_other_file_area_list(p_hot_cold_file_global,p_file_stat,&p_file_stat->file_area_mapcount,8,FILE_AREA_MAPCOUNT,MMAP_FILE_AREA_MAPCOUNT_AGE_DX);
	}
	//遍历file_stat->file_area_hot上的file_area，如果长时间不被访问了，则降级到temp_list
	if(!list_empty(&p_file_stat->file_area_hot)){
		reverse_other_file_area_list(p_hot_cold_file_global,p_file_stat,&p_file_stat->file_area_hot,8,FILE_AREA_HOT,MMAP_FILE_AREA_HOT_AGE_DX);
	}
//#endif

#if 0//不要删除，关键代码
	/*文件的radix tree在遍历完一次所有的page后，可能存在空洞，于是后续还要再遍历文件的radix tree获取之前没有遍历到的page*/
	reverse_file_stat_radix_tree_hole(p_hot_cold_file_global,p_file_stat);
#endif

    if(shrink_page_printk_open1)
	    printk("%s %s file_stat:0x%llx already_scan_file_area_count:%d reclaimed_pages:%d isolate_pages:%d\n",__func__,p_file_stat->file_name,(u64)p_file_stat,*already_scan_file_area_count,reclaimed_pages,isolate_pages);
	return ret;
}
#if 0 //这个函数的作用已经分拆了，第一次扫描文件page并创建file_area的代码已经移动到scan_uninit_file_stat()函数了
/*
 * 1：如果还没有遍历过file_stat对应的文件的radix tree，先遍历一遍radix tree，得到page，分配file_area并添加到file_stat->file_area_temp链表头，
 *    还把file_area保存在radix tree
 * 2：如果已经遍历完一次文件的radix tree，则开始遍历file_stat->file_area_temp链表上的file_area的page，如果page被访问了则把file_area移动到
 * file_stat->file_area_temp链表头。如果file_area的page长时间不被访问，把file_area移动到file_stat->file_area_free链表，则回收这些page
 * 
 * 2.1：遍历file_stat->file_area_free链表上的file_area，如果对应page被访问了则file_area移动到file_stat->file_area_refault链表;
 *      如果对应page长时间不被访问则释放掉file_area
 * 2.2：如果file_stat->file_area_refault链表上file_area的page如果长时间不被访问，则移动回file_stat->file_area_temp链表
 *
 * 2.3：文件可能有page没有被file_area控制，存在空洞。就是说有些文件页page最近被访问了，才分配并加入radix tree，这些page还没有分配
 *      对应的file_area。于是遍历文件file_stat保存file_area的radix tree，找到没有file_area的槽位，计算出本应该保存在这个槽位的file_area
 *      对应的page的索引，再去保存page的radix tree的查找这个page是否分配了，是的话就分配file_area并添加到file_stat->file_area_temp链表头
 * */
static unsigned int traverse_mmap_file_stat_get_cold_page(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat,unsigned int scan_file_area_max,unsigned int *already_scan_file_area_count)
{
	int i,k;
	struct hot_cold_file_area_tree_node *parent_node;
	void **page_slot_in_tree;
	unsigned int area_index_for_page;
	int ret = 0;
	struct page *page;
	unsigned int file_page_count = p_file_stat->mapping->host->i_size >> PAGE_SHIFT;//除以4096
	struct address_space *mapping = p_file_stat->mapping;

	printk("1:%s file_stat:0x%llx file_stat->last_index:%d file_area_count:%d traverse_done:%d\n",__func__,(u64)p_file_stat,p_file_stat->last_index,p_file_stat->file_area_count,p_file_stat->traverse_done);
	if(p_file_stat->max_file_area_age || p_file_stat->recent_access_age || p_file_stat->hot_file_area_cache[0] || p_file_stat->hot_file_area_cache[1] ||p_file_stat->hot_file_area_cache[2]){
		panic("file_stat error\n");
	}

	/*p_file_stat->traverse_done非0，说明还没遍历完一次文件radix tree上所有的page，那就遍历一次，每4个page分配一个file_area*/
	if(0 == p_file_stat->traverse_done){
		/*第一次扫描文件的page，每个周期扫描SCAN_PAGE_COUNT_ONCE个page，一直到扫描完所有的page。4个page一组，每组分配一个file_area结构*/
		for(i = 0;i < SCAN_PAGE_COUNT_ONCE >> PAGE_COUNT_IN_AREA_SHIFT;i++){
			for(k = 0;k < PAGE_COUNT_IN_AREA;k++){
				/*这里需要优化，遍历一次radix tree就得到4个page，完全可以实现的，节省性能$$$$$$$$$$$$$$$$$$$$$$$$*/
				page = xa_load(&mapping->i_pages, p_file_stat->last_index + k);
				if (page && !xa_is_value(page) && page_mapped(page)) {
					area_index_for_page = page->index >> PAGE_COUNT_IN_AREA_SHIFT;
					parent_node = hot_cold_file_area_tree_lookup_and_create(&p_file_stat->hot_cold_file_area_tree_root_node,area_index_for_page,&page_slot_in_tree);
					if(IS_ERR(parent_node)){
						ret = -1;
						printk("%s hot_cold_file_area_tree_lookup_and_create fail\n",__func__);
						goto out;
					}
					if(NULL == *page_slot_in_tree){
						//分配file_area并初始化，成功返回0
						if(file_area_alloc_and_init(parent_node,page_slot_in_tree,area_index_for_page,p_file_stat) < 0){
							ret = -1;
							goto out;
						}
					}
					else{
						printk("%s file_stat:0x%llx file_area index:%d_%ld already alloc\n",__func__,(u64)p_file_stat,area_index_for_page,page->index);
					}
					/*4个连续的page只要有一个在radix tree找到，分配file_area,之后就不再查找其他page了*/
					break;
				}
			}
			p_file_stat->last_index += PAGE_COUNT_IN_AREA;
		}
		//p_file_stat->last_index += SCAN_PAGE_COUNT_ONCE;
		//if成立说明整个文件的page都扫描完了
		if(p_file_stat->last_index >= file_page_count){
			p_file_stat->traverse_done = 1;
			//file_stat->last_index清0
			p_file_stat->last_index = 0;
		}

		ret = 1;
	}else{
		/*到这个分支，文件的所有文件页都遍历了一遍。那就开始回收这个文件的文件页page了。但是可能存在空洞，上边的遍历就会不完整，有些page
		 * 还没有分配，那这里除了内存回收外，还得遍历文件文件的radix tree，找到之前没有映射的page，但是这样太浪费性能了。于是遍历保存file_area
		 * 的radix tree，找到空洞file_area，这些file_area对应的page还没有被管控起来。$$$$$$$$$$$$$$$$$$$$$$$$$$*/
		p_file_stat->traverse_done ++;

		if(!list_empty(&p_file_stat->file_area_temp))
			check_file_area_cold_page_and_clear(p_hot_cold_file_global,p_file_stat,scan_file_area_max,already_scan_file_area_count);
	}
out:
	return ret;
}
#endif

static int traverse_mmap_file_stat_get_cold_page(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat,unsigned int scan_file_area_max,unsigned int *already_scan_file_area_count)
{
	int ret;
	if(shrink_page_printk_open)
		printk("1:%s file_stat:0x%llx file_stat->last_index:%ld file_area_count:%d traverse_done:%d\n",__func__,(u64)p_file_stat,p_file_stat->last_index,p_file_stat->file_area_count,p_file_stat->traverse_done);

	if(p_file_stat->max_file_area_age/* || p_file_stat->hot_file_area_cache[0] || p_file_stat->hot_file_area_cache[1] ||p_file_stat->hot_file_area_cache[2]*/){
		panic("file_stat error p_file_stat:0x%llx\n",(u64)p_file_stat);
	}

	/*到这个分支，文件的所有文件页都遍历了一遍。那就开始回收这个文件的文件页page了。但是可能存在空洞，上边的遍历就会不完整，有些page
	 * 还没有分配，那这里除了内存回收外，还得遍历文件文件的radix tree，找到之前没有映射的page，但是这样太浪费性能了。于是遍历保存file_area
	 * 的radix tree，找到空洞file_area，这些file_area对应的page还没有被管控起来*/

	//令inode引用计数加1，防止遍历该文件的radix tree时文件inode被释放了
	if(file_inode_lock(p_file_stat) == 0)
	{
		printk("%s file_stat:0x%llx status 0x%lx inode lock fail\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
		return -1;
	}
	ret = check_file_area_cold_page_and_clear(p_hot_cold_file_global,p_file_stat,scan_file_area_max,already_scan_file_area_count);
	//令inode引用计数减1
	file_inode_unlock(p_file_stat);

	//返回值是1是说明当前这个file_stat的file_area已经全扫描完了，则扫描该file_stat在global->mmap_file_stat_temp_large_file_head或global->mmap_file_stat_temp_head链表上的上一个file_stat的file_area
	return ret;
}

/*
目前遍历各种global 链表上的file_stat，或者file_stat链表上的file_area，有两种形式。
1:比如 check_file_area_cold_page_and_clear()函数，遍历file_stat->temp 链表上的file_area，从链表尾到头遍历，每次定义一个file_stat->file_area_last指针。它指向一个文件file_stat->temp链表上每轮遍历的最后一个file_area。下轮再次遍历这个文件file_stat->temp链表上的file_area时，直接从file_stat->file_area_last指针指向的file_area开始遍历就行。这种形式的好处是，当遍历到热file_area，留在链表头，遍历到冷file_area留在链表尾巴，冷file_area都聚集在file_stat->temp 链表尾。而当遍历完一轮file_stat->temp 链表上的file_area时，file_stat冷却N个周期后才能再次遍历file_stat->temp 链表上的file_area。等N个周期后，继续从file_stat->temp 链表尾遍历file_area，只用遍历链表尾的冷file_area后，就可以结束遍历，进入冷却期。这样就可以大大降级性能损耗，因为不用遍历整个file_stat->temp链表。这个方案的缺点时，在文件file_stat进去冷却期后，N个周期内，不再遍历file_stat->temp 链表上的file_area，也牵连到不能遍历 file_stat->free、refault、hot、mapcount 链表上的file_area。因为check_file_area_cold_page_and_clear()函数中，这些链表上的file_area是连续遍历的。后期可以考虑把遍历file_stat->temp 链表上的file_area 和 遍历 file_stat->free、refault、hot、mapcount 链表上的file_area 分开????????????????????????????????其实也没必要分开，file_stat的冷却期N，也不会太长，而file_stat->free、refault、hot、mapcount  链表上的file_area 的page都比较特殊，根本不用频繁遍历，N个周期不遍历也没啥事，反而能降低性能损耗。

2:比如 reverse_file_area_mapcount_and_hot_list 函数，每次都从file_stat->file_area_mapcount链表尾遍历一定数据的file_area，并记录当时的global age，然后移动到链表头。下次还是从file_stat->file_area_mapcount链表尾开始遍历file_area，如果file_area的age与gloabl age小于M，结束遍历。就是说，这些链表上的file_area必须间隔M个周期才能再遍历一次，降级性能损耗。这种的优点是设计简单，易于理解，但是不能保证链表尾的都是冷file_area。
 * */

#if 0 //下边的代码很有意义，不要删除，犯过很多错误
static int get_file_area_from_mmap_file_stat_list(struct hot_cold_file_global *p_hot_cold_file_global,unsigned int scan_file_area_max,unsigned int scan_file_stat_max,struct list_head *file_stat_temp_head)//file_stat_temp_head链表来自 global->mmap_file_stat_temp_head 和 global->mmap_file_stat_temp_large_file_head 链表
{
	struct file_stat * p_file_stat = NULL,*p_file_stat_temp = NULL;
	unsigned int scan_file_area_count  = 0;
	unsigned int scan_file_stat_count  = 0;
	unsigned int free_pages = 0;
	int ret = 0;
	char delete_file_stat_last = 0;
	char scan_next_file_stat = 0;

	if(list_empty(file_stat_temp_head))
		return ret;

	printk("1:%s file_stat_last:0x%llx\n",__func__,(u64)p_hot_cold_file_global->file_stat_last);
	if(p_hot_cold_file_global->file_stat_last){//本次从上一轮扫描打断的file_stat继续遍历
		p_file_stat = p_hot_cold_file_global->file_stat_last;
	}
	else{
		//第一次从链表尾的file_stat开始遍历
		p_file_stat = list_last_entry(file_stat_temp_head,struct file_stat,hot_cold_file_list);
		p_hot_cold_file_global->file_stat_last = p_file_stat;
	}	

	do{
		/*加个panic判断，如果p_file_stat是链表头p_hot_cold_file_global->mmap_file_stat_temp_head，那就触发panic*/

		/*查找file_stat在global->mmap_file_stat_temp_head链表上一个file_stat。如果p_file_stat不是链表头的file_stat，直接list_prev_entry
		 * 找到上一个file_stat。如果p_file_stat是链表头的file_stat，那要跳过链表过，取出链表尾的file_stat*/
		if(!list_is_first(&p_file_stat->hot_cold_file_list,file_stat_temp_head))
			p_file_stat_temp = list_prev_entry(p_file_stat,hot_cold_file_list);
		else
			p_file_stat_temp = list_last_entry(file_stat_temp_head,struct file_stat,hot_cold_file_list);

		if(!file_stat_in_file_stat_temp_head_list(p_file_stat) || file_stat_in_file_stat_temp_head_list_error(p_file_stat)){
			panic("%s file_stat:0x%llx not int file_stat_temp_head status:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
		}


		/*因为存在一种并发，1：文件mmap映射分配file_stat向global mmap_file_stat_temp_head添加，并赋值mapping->rh_reserved1=p_file_stat，
		 * 2：这个文件cache读写执行hot_file_update_file_status()，分配file_stat向global file_stat_temp_head添加,并赋值
		 * mapping->rh_reserved1=p_file_stat。因为二者流程并发执行，因为mapping->rh_reserved1是NULL，导致两个流程都分配了file_stat并赋值
		 * 给mapping->rh_reserved1。因此这里的file_stat可能就是cache读写产生的，目前先暂定把mapping->rh_reserved1清0，让下次文件cache读写
		 * 再重新分配file_stat并赋值给mapping->rh_reserved1。这个问题没有其他并发问题，无非就是分配两个file_stat都赋值给mapping->rh_reserved1。
		 *
		 * 还有一点，异步内存回收线程walk_throuth_all_file_area()回收cache文件的page时，从global temp链表遍历file_stat时，要判断
		 * if(file_stat_in_mmap_file(p_file_stat))，是的话也要p_file_stat->mapping->rh_reserved1 = 0并跳过这个file_stat
		 *
		 * 这个问题有个解决方法，就是mmap文件分配file_stat 和cache文件读写分配file_stat，都是用global_lock锁，现在用的是各自的锁。
		 * 这样就避免了分配两个file_stat并都赋值给mapping->rh_reserved1
		 * */
		if(file_stat_in_cache_file(p_file_stat)){
			/*如果p_file_stat从从global mmap_file_stat_temp_head链表剔除，且与p_hot_cold_file_global->file_stat_last指向同一个file_stat。
			 *那要把p_file_stat在global mmap_file_stat_temp_head链表的上一个file_stat(即p_file_stat_temp)赋值给p_hot_cold_file_global->file_stat_last。
			 *否则，会导致下边的while(p_file_stat != p_hot_cold_file_global->file_stat_last)永远不成立,陷入死循环,详解见check_file_area_cold_page_and_clear()*/
			if(p_hot_cold_file_global->file_stat_last == p_file_stat){
				p_hot_cold_file_global->file_stat_last = p_file_stat_temp;
				delete_file_stat_last = 1;
			}
			spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
			clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
			set_file_stat_in_delete(p_file_stat);
			p_file_stat->mapping->rh_reserved1 = 0;
			list_del(&p_file_stat->hot_cold_file_list);
			spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);

			/*释放掉file_stat的所有file_area，最后释放掉file_stat。但释放file_stat用的还是p_hot_cold_file_global->global_lock锁防护
			 *并发，这点后期需要改进!!!!!!!!!!!!!!!!!!!!!!!!!*/
			cold_file_stat_delete_all_file_area(p_hot_cold_file_global,p_file_stat);
			printk("%s p_file_stat:0x%llx status:0x%lx in_cache_file\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
			//p_file_stat = p_file_stat_temp;
			//continue;
			goto next;
		}else if(file_stat_in_delete(p_file_stat)){
			printk("%s p_file_stat:0x%llx status:0x%lx in_delete\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
			//上边有解释
			if(p_hot_cold_file_global->file_stat_last == p_file_stat){
				p_hot_cold_file_global->file_stat_last = p_file_stat_temp;
				delete_file_stat_last = 1;
			}

			/*释放掉file_stat的所有file_area，最后释放掉file_stat*/
			cold_file_stat_delete_all_file_area(p_hot_cold_file_global,p_file_stat);
			//p_file_stat = p_file_stat_temp;
			//continue;
			goto next;
		}

		/*针对0个file_area的file_stat，不能把它移动到mmap_file_stat_zero_file_area_head链表，然后释放掉file_stat。因为如果后续这个文件file_stat
		 *又有文件页page被访问并分配，建立页表页目录映射，我的代码感知不到这点，但是file_stat已经释放了。这种情况下的文件页就无法被内存回收了!
		 *那什么情况下才能释放file_stat呢？在unmap 文件时，可以释放file_stat吗？可以，但是需要保证在unmap且没有其他进程mmap映射这个文件时，
		 *才能unmap时释放掉file_stat结构。这样稍微有点麻烦！还有一种情况可以释放file_stat，就是文件indoe被释放时，这种情况肯定可以释放掉
		 *file_stat结构*/
		if(p_file_stat->file_area_count == 0){
			/*spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
			  clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
			  set_file_stat_in_zero_file_area_list(p_file_stat);
			  list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_zero_file_area_head);
			  spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);
			  goto next;*/
		}

		ret = traverse_mmap_file_stat_get_cold_page(p_hot_cold_file_global,p_file_stat,scan_file_area_max,&scan_file_area_count);
		//返回值是1是说明当前这个file_stat的temp链表上的file_area已经全扫描完了，则扫描该file_stat在global->mmap_file_stat_temp_large_file_head或global->mmap_file_stat_temp_head链表上的上一个file_stat的file_area
		if(ret > 0){
			scan_next_file_stat = 1; 
		}else if(ret < 0){
			return -1;
		}

#if 0 
		//---------------------重大后期改进
		/*ret是1说明这个file_stat还没有从radix tree遍历完一次所有的page，那就把file_stat移动到global mmap_file_stat_temp_head链表尾
		//这样下个周期还能继续扫描这个文件radix tree的page--------------这个解决方案不行，好的解决办法是每次设定最多遍历新文件的page
		//的个数，如果本次这个文件没有遍历完，下次也要从这个文件继续遍历。
		//有个更好的解决方案，新的文件的file_stat要添加到global mmap_temp_not_done链表，只有这个文件的page全遍历完一次，再把这个file_stat
		//移动到global mmap_file_stat_temp_head链表。异步内存回收线程每次两个链表上的文件file_stat按照一定比例都分开遍历，谁也不影响谁*/
		if(1 == ret){
			if(!list_is_last(&p_file_stat->hot_cold_file_list,file_stat_temp_head)){
				spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
				list_move_tail(&p_file_stat->hot_cold_file_list,file_stat_temp_head);
				spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);
			}
		}
#endif

next:
		//只有当前的file_stat的temp链表上的file_area扫描完，才能扫描下一个file_stat
		if(scan_next_file_stat){
			printk("%s p_file_stat:0x%llx file_area:%d scan complete,next scan file_stat:0x%llx\n",__func__,(u64)p_file_stat,p_file_stat->file_area_count,(u64)p_file_stat_temp);
			//下一个file_stat
			//p_file_stat = p_file_stat_temp;
			scan_file_stat_count ++;
		}

		//遍历指定数目的file_stat和file_area后，强制结束遍历
		if(scan_file_area_count >= scan_file_area_max || scan_file_stat_count  >= scan_file_stat_max){
			printk("%s scan_file_area_count:%d scan_file_stat_count:%d exceed max\n",__func__,scan_file_area_count,scan_file_stat_count);
			break;
		}

		if(0 == delete_file_stat_last && p_file_stat == p_hot_cold_file_global->file_stat_last){
			printk("%s p_file_stat:0x%llx == p_hot_cold_file_global->file_stat_last\n",__func__,(u64)p_file_stat);
			break;
		}
		else if(delete_file_stat_last)
			delete_file_stat_last = 0;

		//在scan_next_file_stat时把p_file_stat = p_file_stat_temp赋值放到下边。因为，如果上边可能break了，
		//而p_file_stat = p_file_stat_temp已经赋值过了，但这个file_stat根本没扫描。接着跳出while循环，
		//下边对p_hot_cold_file_global->file_stat_last新的file_stat，而当前的file_stat就漏掉扫描了!!!!!!!!
		if(scan_next_file_stat){
			//下一个file_stat
			p_file_stat = p_file_stat_temp;
		}


	/*这里退出循环的条件，不能碰到链表头退出，是一个环形队列的遍历形式。主要原因是不想模仿read/write文件页的内存回收思路：
	 *先加锁从global temp链表隔离几十个文件file_stat，清理file_stat的状态，然后内存回收。内存回收后再把file_stat加锁
	 *移动到global temp链表头。这样太麻烦了，还浪费性能。针对mmap文件页的内存回收，不用担心并发问题，不用这么麻烦
	 *
	 * 以下两种情况退出循环
	 *1：上边的 遍历指定数目的file_stat和file_area后，强制结束遍历
	 *2：这里的while，本次循环处理到file_stat已经是第一次循环处理过了，相当于重复了
	 *3：添加!list_empty(file_stat_temp_head)判断，原理分析在check_file_area_cold_page_and_clear()函数
	 */
	//}while(p_file_stat != p_hot_cold_file_global->file_stat_last);
	//}while(p_file_stat != p_hot_cold_file_global->file_stat_last && !list_empty(file_stat_temp_head));
	}while(!list_empty(file_stat_temp_head));

	/*scan_next_file_stat如果是1，说明当前文件file_stat的temp链表上已经扫描的file_area个数超过该文件temp链表的总file_area个数，
	 *然后才能更新p_hot_cold_file_global->file_stat_last，这样下次才能扫描该file_stat在global->mmap_file_stat_temp_large_file_head
	 *或global->mmap_file_stat_temp_head链表上的上一个file_stat*/
	if(1 == scan_next_file_stat){
		if(!list_empty(file_stat_temp_head)){
			/*p_hot_cold_file_global->file_stat_last指向_hot_cold_file_global->file_stat_temp_head链表上一个file_area，下个周期
			 *直接从p_hot_cold_file_global->file_stat_last指向的file_stat开始扫描*/
			if(!list_is_first(&p_file_stat->hot_cold_file_list,file_stat_temp_head))
				p_hot_cold_file_global->file_stat_last = list_prev_entry(p_file_stat,hot_cold_file_list);
			else
				p_hot_cold_file_global->file_stat_last = list_last_entry(file_stat_temp_head,struct file_stat,hot_cold_file_list);
		}else{
			p_hot_cold_file_global->file_stat_last = NULL;
		}
	}
	//err:
	return free_pages;
}
#endif

static int get_file_area_from_mmap_file_stat_list(struct hot_cold_file_global *p_hot_cold_file_global,unsigned int scan_file_area_max,unsigned int scan_file_stat_max,struct list_head *file_stat_temp_head)//file_stat_temp_head链表来自 global->mmap_file_stat_temp_head 和 global->mmap_file_stat_temp_large_file_head 链表
{
	struct file_stat * p_file_stat = NULL,*p_file_stat_temp = NULL;
	unsigned int scan_file_area_count  = 0;
	unsigned int scan_file_stat_count  = 0;
	unsigned int free_pages = 0;
	int ret = 0;
	LIST_HEAD(file_stat_list);

	if(list_empty(file_stat_temp_head))
		return ret;
	if(shrink_page_printk_open)
		printk("1:%s file_stat_last:0x%llx\n",__func__,(u64)p_hot_cold_file_global->file_stat_last);

	//每次都从链表尾开始遍历
	list_for_each_entry_safe_reverse(p_file_stat,p_file_stat_temp,file_stat_temp_head,hot_cold_file_list)
	{
		/*如果p_file_stat是链表头p_hot_cold_file_global->mmap_file_stat_temp_head，那就触发panic*/
		if(!file_stat_in_file_stat_temp_head_list(p_file_stat) || file_stat_in_file_stat_temp_head_list_error(p_file_stat)){
			panic("%s file_stat:0x%llx not int file_stat_temp_head status:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
		}
		
		//遍历指定数目的file_stat和file_area后，强制结束遍历。包括遍历delete等文件
		if(scan_file_area_count >= scan_file_area_max){
			if(shrink_page_printk_open)
				printk("%s scan_file_area_count:%d scan_file_stat_count:%d exceed max\n",__func__,scan_file_area_count,scan_file_stat_count);

			break;
		}
		scan_file_stat_count ++;

		/*因为存在一种并发，1：文件mmap映射分配file_stat向global mmap_file_stat_temp_head添加，并赋值mapping->rh_reserved1=p_file_stat，
		 * 2：这个文件cache读写执行hot_file_update_file_status()，分配file_stat向global file_stat_temp_head添加,并赋值
		 * mapping->rh_reserved1=p_file_stat。因为二者流程并发执行，因为mapping->rh_reserved1是NULL，导致两个流程都分配了file_stat并赋值
		 * 给mapping->rh_reserved1。因此这里的file_stat可能就是cache读写产生的，目前先暂定把mapping->rh_reserved1清0，让下次文件cache读写
		 * 再重新分配file_stat并赋值给mapping->rh_reserved1。这个问题没有其他并发问题，无非就是分配两个file_stat都赋值给mapping->rh_reserved1。
		 *
		 * 还有一点，异步内存回收线程walk_throuth_all_file_area()回收cache文件的page时，从global temp链表遍历file_stat时，要判断
		 * if(file_stat_in_mmap_file(p_file_stat))，是的话也要p_file_stat->mapping->rh_reserved1 = 0并跳过这个file_stat
		 *
		 * 这个问题有个解决方法，就是mmap文件分配file_stat 和cache文件读写分配file_stat，都是用global_lock锁，现在用的是各自的锁。
		 * 这样就避免了分配两个file_stat并都赋值给mapping->rh_reserved1
		 * */
		if(file_stat_in_cache_file(p_file_stat)){
			spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
			clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
			set_file_stat_in_delete(p_file_stat);
			p_file_stat->mapping->rh_reserved1 = 0;
			list_del(&p_file_stat->hot_cold_file_list);
			spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);

			/*释放掉file_stat的所有file_area，最后释放掉file_stat。但释放file_stat用的还是p_hot_cold_file_global->global_lock锁防护
			 *并发，这点后期需要改进!!!!!!!!!!!!!!!!!!!!!!!!!*/
			cold_file_stat_delete_all_file_area(p_hot_cold_file_global,p_file_stat);
			printk("%s p_file_stat:0x%llx status:0x%lx in_cache_file\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
			continue;
		}else if(file_stat_in_delete(p_file_stat)){
			printk("%s p_file_stat:0x%llx status:0x%lx in_delete\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
			
			/*释放掉file_stat的所有file_area，最后释放掉file_stat*/
			cold_file_stat_delete_all_file_area(p_hot_cold_file_global,p_file_stat);
			continue;
		}

		//如果文件file_stat还在冷却期，不扫描这个文件file_stat->temp链表上的file_area，只是把file_stat移动到file_stat_list临时链表
		if(p_file_stat->cooling_off_start){
			if(p_hot_cold_file_global->global_age - p_file_stat->cooling_off_start_age < MMAP_FILE_AREA_COLD_AGE_DX){
			    list_move(&p_file_stat->hot_cold_file_list,&file_stat_list);
			    continue;
			}
			else{
			    p_file_stat->cooling_off_start = 0;
			}
		}

		/*针对0个file_area的file_stat，不能把它移动到mmap_file_stat_zero_file_area_head链表，然后释放掉file_stat。因为如果后续这个文件file_stat
		 *又有文件页page被访问并分配，建立页表页目录映射，我的代码感知不到这点，但是file_stat已经释放了。这种情况下的文件页就无法被内存回收了!
		 *那什么情况下才能释放file_stat呢？在unmap 文件时，可以释放file_stat吗？可以，但是需要保证在unmap且没有其他进程mmap映射这个文件时，
		 *才能unmap时释放掉file_stat结构。这样稍微有点麻烦！还有一种情况可以释放file_stat，就是文件indoe被释放时，这种情况肯定可以释放掉
		 *file_stat结构*/
		if(p_file_stat->file_area_count == 0){//这段代码比较重要不要删除---------------------------
			/*spin_lock(&p_hot_cold_file_global->mmap_file_global_lock);
			  clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
			  set_file_stat_in_zero_file_area_list(p_file_stat);
			  list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_zero_file_area_head);
			  spin_unlock(&p_hot_cold_file_global->mmap_file_global_lock);
			  goto next;*/
		}

		ret = traverse_mmap_file_stat_get_cold_page(p_hot_cold_file_global,p_file_stat,scan_file_area_max,&scan_file_area_count);
		//返回值是1是说明当前这个file_stat的temp链表上的file_area已经全扫描完了，则扫描该file_stat在global->mmap_file_stat_temp_large_file_head或global->mmap_file_stat_temp_head链表上的上一个file_stat的file_area
		if(ret < 0){
			return -1;
		}

		/*到这里，只有几种情况
		 *1：当前文件p_file_stat->temp链表上的file_area扫描了一遍，ret是1，此时需要把p_file_stat移动到file_stat_list临时链表，然后下轮for循环扫描下一个文件
		 *2：当前文件p_file_stat->temp链表上的file_area太多了，已经扫描的file_area个数超过scan_file_stat_max，break退出，下次执行该函数还要继续扫描p_file_stat这个文件
		 * */

		//只有当前的file_stat的temp链表上的file_area扫描完，才能扫描下一个file_stat
		if(ret > 0){
			/*遍历过的文件file_stat移动到file_stat_list临时链表。但可能这个file_stat因为热file_area增多而变成了热file_area而移动到了global hot链表。
			 *此时这里再把这个热file_area移动到file_stat_list临时链表，该函数最后再把它移动到global temp链表，那该热file_stat处于的链表就错了，会crash
			 *解决办法是限制这个file_stat必须处于global temp链表才能移动到file_stat_list临时链表*/
			if(file_stat_in_file_stat_temp_head_list(p_file_stat))
			    list_move(&p_file_stat->hot_cold_file_list,&file_stat_list);
			if(shrink_page_printk_open)
				printk("%s p_file_stat:0x%llx file_area:%d scan complete,next scan file_stat:0x%llx\n",__func__,(u64)p_file_stat,p_file_stat->file_area_count,(u64)p_file_stat_temp);
		}else if(scan_file_area_count  >= scan_file_area_max){
			if(shrink_page_printk_open)
				printk("%s scan_file_area_count:%d scan_file_stat_count:%d exceed max\n",__func__,scan_file_area_count,scan_file_stat_count);

			break;
		}else{
			panic("%s file_stat:0x%llx status:0x%lx exception scan_file_area_count:%d scan_file_stat_count:%d ret:%d\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status,scan_file_area_count,scan_file_stat_count,ret);
		}
    }

	//如果file_stat_list临时链表还有file_stat，则把这些file_stat移动到global temp链表头，下轮循环就能从链表尾巴扫描还没有扫描的file_stat了
	if(!list_empty(&file_stat_list)){
		list_splice(&file_stat_list,file_stat_temp_head);
	}
	return free_pages;
}

/*scan_uninit_file_stat()最初是为了ko模式异步内存回收设计的:保存file_area指针的是radix tree，保存page指针的
 *是另一个xarray tree，二者独立。因此，要先执行该函数，遍历xarray tree，探测一个个page，有page的则创建file_area。
  并添加到file_stat->temp链表，后续就可以探测这些page冷热，参与内存回收。

  然而，现在file_area和page是一体了，file_area指针保存在原保存page指针的xarray tree，page指针保存在file_area->pages[]数组。
  后续，如果该mmap文件访问任意一个新的page，先分配一个page，最后执行到 __filemap_add_folio->__filemap_add_folio_for_file_area()
  ->file_area_alloc_and_init()函数，必然分配file_area并添加到file_stat->temp链表，然后把新的page保存到file_area->pages[]
  数组。也就是说，现在不用再主动执行scan_uninit_file_stat()探测page然后创建file_area了。内核原有机制就可以
  保证mmap的文件一旦有新的page，自己创建file_area并添加到file_stat->temp链表。后续异步内存回收线程就可以探测该file_area的
  冷热page，然后内存回收。

  但是这就引入了一个新的问题，并发:异步内存回收线程对file_area、file_stat的链表add/del操作都会跟
  __filemap_add_folio_for_file_area()中也会对file_area、file_stat的链表add操作形成并发
*/
#if 0
//扫描global mmap_file_stat_uninit_head链表上的file_stat的page，page存在的话则创建file_area，否则一直遍历完这个文件的所有page，才会遍历下一个文件
static int scan_uninit_file_stat(struct hot_cold_file_global *p_hot_cold_file_global,struct list_head *mmap_file_stat_uninit_head,unsigned int scan_page_max)
{
	int k;
	struct hot_cold_file_area_tree_node *parent_node;
	void **page_slot_in_tree;
	unsigned int area_index_for_page;
	int ret = 0;
	struct page *page;
	struct page *pages[PAGE_COUNT_IN_AREA];
	struct address_space *mapping;
	unsigned int scan_file_area_max = scan_page_max >> PAGE_COUNT_IN_AREA_SHIFT;
	unsigned int scan_file_area_count = 0;
	struct file_stat *p_file_stat,*p_file_stat_temp;
	unsigned int file_page_count;
	char mapcount_file_area = 0;
	struct file_area *p_file_area = NULL;

	list_for_each_entry_safe_reverse(p_file_stat,p_file_stat_temp,mmap_file_stat_uninit_head,hot_cold_file_list){
		if(p_file_stat->file_stat_status != (1 << F_file_stat_in_mmap_file)){
			/*实际测试这里遇到过file_stat in delte，则把file_stat移动到global mmap_file_stat_temp_head链表尾，
			 *稍后get_file_area_from_mmap_file_stat_list()函数就会把这个delete的file_stat释放掉*/
			if(file_stat_in_delete(p_file_stat)){
				spin_lock(&hot_cold_file_global_info.mmap_file_global_lock);
				list_move_tail(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_temp_head);
				spin_unlock(&hot_cold_file_global_info.mmap_file_global_lock);
				printk("%s file_stat:0x%llx status:0x%lx in delete\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
				continue;
			}
			else
				panic("%s file_stat:0x%llx status error:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);
		}
		mapping = p_file_stat->mapping;
		file_page_count = p_file_stat->mapping->host->i_size >> PAGE_SHIFT;//除以4096

		if(shrink_page_printk_open)
			printk("1:%s scan file_stat:0x%llx\n",__func__,(u64)p_file_stat);

		/*这个while循环扫一个文件file_stat的page，存在的话则创建file_area。有下边这几种情况
		 *1:文件page太多，扫描的file_area超过mac，文件的page还没扫描完，直接break，下次执行函数还扫描这个文件，直到扫描完
		 *2:文件page很少，扫描的file_area未超过max就break，于是把file_stat移动到global->mmap_file_stat_temp_large_file_head或
		 *  global->mmap_file_stat_temp_head链表。这个file_stat就从global->mmap_file_stat_uninit_head链表尾剔除了，然后扫描第2个文件file_stat*/
		while(scan_file_area_count++ < scan_file_area_max){

			memset(pages,0,PAGE_COUNT_IN_AREA*sizeof(struct page *));
			//获取p_file_stat->last_index对应的PAGE_COUNT_IN_AREA文件页page指针并保存到pages数组
			ret = get_page_from_file_area(p_file_stat,p_file_stat->last_index,pages);

			if(shrink_page_printk_open)
				printk("2:%s file_stat:0x%llx start_page_index:%ld get %d page file_area_count_in_temp_list:%d\n",__func__,(u64)p_file_stat,p_file_stat->last_index,ret,p_file_stat->file_area_count_in_temp_list);
			/*遇到一个重大问题，上边打印"file_stat:0xffff8c9f5fbb1320 start_page_index:464 get 0 page"，然后之后每次执行该函数，都从global mmap_file_stat_uninit_head
			 *链表尾遍历file_stat:0xffff8c9f5fbb1320的起始索引是464的4个page，但这些page都没有，于是ret是0，这导致直接goto out。回收每次就陷入了死循环，无法遍历
			 *global mmap_file_stat_uninit_head链表尾其他file_stat，以及file_stat:0xffff8c9f5fbb1320 start_page_index:464 索引后边的page。简单说，因为一个文件
			 *file_stat的page存在空洞，导致每次执行到该函数都都一直遍历这个文件的空洞page，陷入死循环。解决方法是，遇到文件空洞page，ret是0，继续遍历下一个后边的page
			 *避免陷入死循环*/
			if(0 == ret){
			    p_file_stat->last_index += PAGE_COUNT_IN_AREA;
				if(p_file_stat->last_index >= file_page_count){
				    goto complete;
				}

				continue;
			}
			if(ret < 0){
				printk("2_1:%s file_stat:0x%llx start_page_index:%ld get %d fail\n",__func__,(u64)p_file_stat,p_file_stat->last_index,ret);
				goto out; 
			}

			mapcount_file_area = 0;
			p_file_area = NULL;
			/*第一次扫描文件的page，每个周期扫描SCAN_PAGE_COUNT_ONCE个page，一直到扫描完所有的page。4个page一组，每组分配一个file_area结构*/
			for(k = 0;k < PAGE_COUNT_IN_AREA;k++){
				/*这里需要优化，遍历一次radix tree就得到4个page，完全可以实现的，节省性能$$$$$$$$$$$$$$$$$$$$$$$$*/
				//page = xa_load(&mapping->i_pages, p_file_stat->last_index + k);
				page = pages[k];
				if (page && !xa_is_value(page) && page_mapped(page)) {
					//mapcount file_area
					if(0 == mapcount_file_area && page_mapcount(page) > 1){
						mapcount_file_area = 1;
					}

					area_index_for_page = page->index >> PAGE_COUNT_IN_AREA_SHIFT;
					page_slot_in_tree = NULL;
					parent_node = hot_cold_file_area_tree_lookup_and_create(&p_file_stat->hot_cold_file_area_tree_root_node,area_index_for_page,&page_slot_in_tree);
					if(IS_ERR(parent_node)){
						ret = -1;
						printk("3:%s hot_cold_file_area_tree_lookup_and_create fail\n",__func__);
						goto out;
					}
					if(NULL == *page_slot_in_tree){
						//分配file_area并初始化，成功返回非NULL
						p_file_area = file_area_alloc_and_init(parent_node,page_slot_in_tree,area_index_for_page,p_file_stat);
						if(p_file_area == NULL){
							ret = -1;
							goto out;
						}
					}
					else{
						panic("4:%s file_stat:0x%llx file_area index:%d_%ld 0x%llx already alloc!!!!!!!!!!!!!!!!\n",__func__,(u64)p_file_stat,area_index_for_page,page->index,(u64)(*page_slot_in_tree));
					}
					//file_stat->temp 链表上的file_area个数加1
					p_file_stat->file_area_count_in_temp_list ++;
					/*4个连续的page只要有一个在radix tree找到，分配file_area,之后就不再查找其他page了*/
					break;
				}else{
					if(shrink_page_printk_open1)
						printk("4_1:%s file_stat:0x%llx start_page_index:%ld page:0x%llx error\n",__func__,(u64)p_file_stat,p_file_stat->last_index,(u64)page);
				}
			}

			/*如果上边for循环遍历的file_area的page的mapcount都是1，且file_area的page上边没有遍历完，则这里继续遍历完剩余的page*/
			while(0 == mapcount_file_area && k < PAGE_COUNT_IN_AREA){
				page= pages[k];
				if (page && !xa_is_value(page) && page_mapped(page) && page_mapcount(page) > 1){
					mapcount_file_area = 1;
				}
				k ++;
			}
			if(mapcount_file_area){
				if(!file_area_in_temp_list(p_file_area) || file_area_in_temp_list_error(p_file_area)){
					panic("%s file_area:0x%llx status:%d not in file_area_temp\n",__func__,(u64)p_file_area,p_file_area->file_area_state);
				}

				//文件file_stat的mapcount的file_area个数加1
				p_file_stat->mapcount_file_area_count ++;
				//file_stat->temp 链表上的file_area个数减1
				p_file_stat->file_area_count_in_temp_list --;
				//file_area的page的mapcount大于1，则把file_area移动到file_stat->file_area_mapcount链表
				clear_file_area_in_temp_list(p_file_area);
				set_file_area_in_mapcount_list(p_file_area);
				list_move(&p_file_area->file_area_list,&p_file_stat->file_area_mapcount);
				if(shrink_page_printk_open)
					printk("5:%s file_stat:0x%llx file_area:0x%llx state:0x%x is mapcount file_area\n",__func__,(u64)p_file_stat,(u64)p_file_area,p_file_area->file_area_state);
			}

			//每扫描1个file_area，p_file_stat->last_index加PAGE_COUNT_IN_AREA
			p_file_stat->last_index += PAGE_COUNT_IN_AREA;

			//if成立说明整个文件的page都扫描完了
			if(p_file_stat->last_index >= file_page_count){
complete:				
				if(shrink_page_printk_open1)
					printk("6:%s file_stat:0x%llx %s all page scan complete p_file_stat->last_index:%ld file_page_count:%d\n",__func__,(u64)p_file_stat,p_file_stat->file_name,p_file_stat->last_index,file_page_count);

				//p_file_stat->traverse_done = 1;

				//对file_stat->last_index清0，后续有用于保存最近一次扫描的file_area的索引
				p_file_stat->last_index = 0;
				//在文件file_stat移动到temp链表时，p_file_stat->file_area_count_in_temp_list是文件的总file_area个数
				//p_file_stat->file_area_count_in_temp_list = p_file_stat->file_area_count;//上边已经加1了

				/*文件的page扫描完了，把file_stat从global mmap_file_stat_uninit_head链表移动到global mmap_file_stat_temp_head或
				 *mmap_file_stat_temp_large_file_head。这个过程必须加锁，因为与add_mmap_file_stat_to_list()存在并发修改global mmap_file_stat_uninit_head
				 *链表的情况。后续file_stat再移动到大文件、zero_file_area等链表，就不用再加锁了，完全是异步内存回收线程的单线程操作*/
				spin_lock(&hot_cold_file_global_info.mmap_file_global_lock);

				/*新分配的file_stat必须设置in_file_stat_temp_head_list链表。这个设置file_stat状态的操作必须放到 把file_stat添加到
				 *tmep链表前边，还要加内存屏障。否则会出现一种极端情况，异步内存回收线程从temp链表遍历到这个file_stat，
				 *但是file_stat还没有设置为in_temp_list状态。这样有问题会触发panic。因为mmap文件异步内存回收线程，
				 *从temp链表遍历file_stat没有mmap_file_global_lock加锁，所以与这里存在并发操作。而针对cache文件，异步内存回收线程
				 *从global temp链表遍历file_stat，全程global_lock加锁，不会跟向global temp链表添加file_stat存在方法，但最好改造一下*/
				set_file_stat_in_file_stat_temp_head_list(p_file_stat);
				smp_wmb();

				if(is_mmap_file_stat_large_file(p_hot_cold_file_global,p_file_stat)){
					set_file_stat_in_large_file(p_file_stat);
					list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_temp_large_file_head);
				}
				else
					list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_temp_head);

				spin_unlock(&hot_cold_file_global_info.mmap_file_global_lock);
	
				/*如果文件file_stat的mapcount的file_area个数超过阀值，则file_stat被判定为mapcount file_stat而移动到
				 *global mmap_file_stat_mapcount_head链表。但前提file_stat必须在temp_file链表或temp_large_file链表*/
				if(is_mmap_file_stat_mapcount_file(p_hot_cold_file_global,p_file_stat) && file_stat_in_file_stat_temp_head_list(p_file_stat)){
					if(file_stat_in_file_stat_temp_head_list_error(p_file_stat))
						panic("%s file_stat:0x%llx status error:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

					clear_file_stat_in_file_stat_temp_head_list(p_file_stat);
					set_file_stat_in_mapcount_file_area_list(p_file_stat);
					list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_mapcount_head);
					p_hot_cold_file_global->mapcount_mmap_file_stat_count ++;
					if(shrink_page_printk_open)
						printk("6:%s file_stat:0x%llx status:0x%llx is mapcount file\n",__func__,(u64)p_file_stat,(u64)p_file_stat->file_stat_status);
				}

				break;
			}
		}

		//如果扫描的文件页page数达到本次的限制，结束本次的scan
		if(scan_file_area_count >= scan_file_area_max){
			break;
		}
	}
out:
	return ret;
}
#endif
static int scan_mmap_mapcount_file_stat(struct hot_cold_file_global *p_hot_cold_file_global,unsigned int scan_file_area_max)
{
	struct file_stat *p_file_stat,*p_file_stat_temp;
	unsigned int mapcount_file_area_count_origin;
	unsigned int scan_file_area_count = 0;
	char file_stat_change = 0;
	LIST_HEAD(file_stat_list);

	//每次都从链表尾开始遍历
	list_for_each_entry_safe_reverse(p_file_stat,p_file_stat_temp,&p_hot_cold_file_global->mmap_file_stat_mapcount_head,hot_cold_file_list){
		if(!file_stat_in_mapcount_file_area_list(p_file_stat) || file_stat_in_mapcount_file_area_list_error(p_file_stat))
			panic("%s file_stat:0x%llx not in_mapcount_file_area_list status:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

		//遍历file_stat->file_area_mapcount上的file_area，如果file_area的page的mapcount都是1，file_area不再是mapcount file_area，则降级到temp_list
		if(!list_empty(&p_file_stat->file_area_mapcount)){
			mapcount_file_area_count_origin = p_file_stat->mapcount_file_area_count;
			file_stat_change = 0;

			scan_file_area_count += reverse_other_file_area_list(p_hot_cold_file_global,p_file_stat,&p_file_stat->file_area_mapcount,SCAN_MAPCOUNT_FILE_AREA_COUNT_ONCE,FILE_AREA_MAPCOUNT,MMAP_FILE_AREA_MAPCOUNT_AGE_DX);

			if(mapcount_file_area_count_origin != p_file_stat->mapcount_file_area_count){
				//文件file_stat的mapcount的file_area个数减少到阀值以下了，降级到普通文件
				if(0 == is_mmap_file_stat_mapcount_file(p_hot_cold_file_global,p_file_stat)){
					clear_file_stat_in_mapcount_file_area_list(p_file_stat);
					set_file_stat_in_file_stat_temp_head_list(p_file_stat);
					if(is_mmap_file_stat_large_file(p_hot_cold_file_global,p_file_stat)){//大文件
						list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_temp_large_file_head);
					}
					else{//普通文件
						list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_temp_head);
					}
					p_hot_cold_file_global->mapcount_mmap_file_stat_count --;
					file_stat_change = 1;
					if(shrink_page_printk_open1)
						printk("1:%s file_stat:0x%llx status:0x%llx  mapcount to temp file\n",__func__,(u64)p_file_stat,(u64)p_file_stat->file_stat_status);
				}
			}
		}

		/*file_stat未发生变化，先移动到file_stat_list临时链表。如果此时global mmap_file_stat_mapcount_head链表没有file_stat了，
		  则p_file_stat_temp指向链表头，下次循环直接break跳出*/
		if(0 == file_stat_change)
			list_move(&p_file_stat->hot_cold_file_list,&file_stat_list);

		//超出扫描的file_area上限，break
		if(scan_file_area_count > scan_file_area_max){
			break;
		}
	}

	//如果file_stat_list临时链表还有file_stat，则把这些file_stat移动到global mmap_file_stat_hot_head链表头，下轮循环就能从链表尾巴扫描还没有扫描的file_stat了
	if(!list_empty(&file_stat_list)){
		list_splice(&file_stat_list,&p_hot_cold_file_global->mmap_file_stat_mapcount_head);
	}

	return scan_file_area_count;
}
static int scan_mmap_hot_file_stat(struct hot_cold_file_global *p_hot_cold_file_global,unsigned int scan_file_area_max)
{
	struct file_stat *p_file_stat,*p_file_stat_temp;
	unsigned int file_area_hot_count_origin;
	unsigned int scan_file_area_count = 0;
	char file_stat_change = 0;
	LIST_HEAD(file_stat_list);


	list_for_each_entry_safe_reverse(p_file_stat,p_file_stat_temp,&p_hot_cold_file_global->mmap_file_stat_hot_head,hot_cold_file_list){
		if(!file_stat_in_file_stat_hot_head_list(p_file_stat) || file_stat_in_file_stat_hot_head_list_error(p_file_stat))
			panic("%s file_stat:0x%llx not in_file_stat_hot_head_list status:0x%lx\n",__func__,(u64)p_file_stat,p_file_stat->file_stat_status);

		//遍历file_stat->file_area_hot上的file_area，如果长时间不被访问了，则降级到temp_list
		if(!list_empty(&p_file_stat->file_area_hot)){
			file_area_hot_count_origin = p_file_stat->file_area_hot_count;

			scan_file_area_count += reverse_other_file_area_list(p_hot_cold_file_global,p_file_stat,&p_file_stat->file_area_hot,SCAN_HOT_FILE_AREA_COUNT_ONCE,FILE_AREA_HOT,MMAP_FILE_AREA_HOT_AGE_DX);

			if(file_area_hot_count_origin != p_file_stat->file_area_hot_count){
				//文件file_stat的mapcount的file_area个数减少到阀值以下了，降级到普通文件
				if(0 == is_mmap_file_stat_hot_file(p_hot_cold_file_global,p_file_stat)){
					clear_file_stat_in_file_stat_hot_head_list(p_file_stat);
					set_file_stat_in_file_stat_temp_head_list(p_file_stat);
					if(is_mmap_file_stat_large_file(p_hot_cold_file_global,p_file_stat)){//大文件
						list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_temp_large_file_head);
					}
					else{//普通文件
						list_move(&p_file_stat->hot_cold_file_list,&p_hot_cold_file_global->mmap_file_stat_temp_head);
					}
					p_hot_cold_file_global->hot_mmap_file_stat_count --;
					file_stat_change = 1;
					if(shrink_page_printk_open1)
						printk("1:%s file_stat:0x%llx status:0x%llx  hot to temp file\n",__func__,(u64)p_file_stat,(u64)p_file_stat->file_stat_status);
				}
			}
		}

		/*file_stat未发生变化，先移动到file_stat_list临时链表。如果此时global mmap_file_stat_mapcount_head链表没有file_stat了，
		  则p_file_stat_temp指向链表头，下次循环直接break跳出*/
		if(0 == file_stat_change)
			list_move(&p_file_stat->hot_cold_file_list,&file_stat_list);

		//超出扫描的file_area上限，break
		if(scan_file_area_count > scan_file_area_max){
			break;
		}
	}

	//如果file_stat_list临时链表还有file_stat，则把这些file_stat移动到global mmap_file_stat_hot_head链表头，下轮循环就能从链表尾巴扫描还没有扫描的file_stat了
	if(!list_empty(&file_stat_list)){
		list_splice(&file_stat_list,&p_hot_cold_file_global->mmap_file_stat_hot_head);
	}
	return scan_file_area_count;
}
int walk_throuth_all_mmap_file_area(struct hot_cold_file_global *p_hot_cold_file_global)
{
	int ret;
	unsigned int scan_file_area_max,scan_file_stat_max;
	if(shrink_page_printk_open)
		printk("%s mmap_file_stat_count:%d mapcount_mmap_file_stat_count:%d hot_mmap_file_stat_count:%d\n",__func__,p_hot_cold_file_global->mmap_file_stat_count,p_hot_cold_file_global->mapcount_mmap_file_stat_count,p_hot_cold_file_global->hot_mmap_file_stat_count);

#if 0	
	//扫描global mmap_file_stat_uninit_head链表上的file_stat
	ret = scan_uninit_file_stat(p_hot_cold_file_global,&p_hot_cold_file_global->mmap_file_stat_uninit_head,512);
	if(ret < 0)
		return ret;
#endif

	//扫描大文件file_area
	scan_file_stat_max = 16;
	scan_file_area_max = 256;
	ret = get_file_area_from_mmap_file_stat_list(p_hot_cold_file_global,scan_file_area_max,scan_file_stat_max,&p_hot_cold_file_global->mmap_file_stat_temp_large_file_head);
	if(ret < 0)
		return ret;

	//扫描小文件file_area
	scan_file_stat_max = 32;
	scan_file_area_max = 128;
	ret = get_file_area_from_mmap_file_stat_list(p_hot_cold_file_global,scan_file_area_max,scan_file_stat_max,&p_hot_cold_file_global->mmap_file_stat_temp_head);
	if(ret < 0)
		return ret;

	scan_file_area_max = 32;
	//扫描热文件的file_area
	ret = scan_mmap_hot_file_stat(p_hot_cold_file_global,scan_file_area_max);
	if(ret < 0)
		return ret;

	scan_file_area_max = 32;
	//扫描mapcount文件的file_area
	ret = scan_mmap_mapcount_file_stat(p_hot_cold_file_global,scan_file_area_max);

	return ret;
}
EXPORT_SYMBOL(walk_throuth_all_mmap_file_area);
