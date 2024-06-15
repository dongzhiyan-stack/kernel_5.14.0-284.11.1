#ifndef _ASYNC_MEMORY_RECLAIM_BASH_H_
#define _ASYNC_MEMORY_RECLAIM_BASH_H_
#include <linux/mm.h>

#define ASYNC_MEMORY_RECLAIM_IN_KERNEL

//一个file_stat结构里缓存的热file_area结构个数
#define FILE_AREA_CACHE_COUNT 3
//置1才允许异步内存回收
#define ASYNC_MEMORY_RECLAIM_ENABLE 0
//置1说明说明触发了drop_cache，此时禁止异步内存回收线程处理gloabl drop_cache_file_stat_head链表上的file_stat
#define ASYNC_DROP_CACHES 1
//异步内存回收周期，单位s
#define ASYNC_MEMORY_RECLIAIM_PERIOD 60
//最大文件名字长度
#define MAX_FILE_NAME_LEN 100
//当一个文件file_stat长时间不被访问，释放掉了所有的file_area，再过FILE_STAT_DELETE_AGE_DX个周期，则释放掉file_stat结构
#define FILE_STAT_DELETE_AGE_DX  10
//一个 file_area 包含的page数，默认4个
#define PAGE_COUNT_IN_AREA_SHIFT 2
#define PAGE_COUNT_IN_AREA (1UL << PAGE_COUNT_IN_AREA_SHIFT)//4
#define PAGE_COUNT_IN_AREA_MASK (PAGE_COUNT_IN_AREA - 1)//0x3

#define TREE_MAP_SHIFT	6
#define TREE_MAP_SIZE	(1UL << TREE_MAP_SHIFT)
#define TREE_MAP_MASK (TREE_MAP_SIZE - 1)
#define TREE_ENTRY_MASK 3
#define TREE_INTERNAL_NODE 1

/*热file_area经过FILE_AREA_HOT_to_TEMP_AGE_DX个周期后，还没有被访问，则移动到file_area_temp链表*/
#define FILE_AREA_HOT_to_TEMP_AGE_DX  5
/*发生refault的file_area经过FILE_AREA_REFAULT_TO_TEMP_AGE_DX个周期后，还没有被访问，则移动到file_area_temp链表*/
#define FILE_AREA_REFAULT_TO_TEMP_AGE_DX 20
/*普通的file_area在FILE_AREA_TEMP_TO_COLD_AGE_DX个周期内没有被访问则被判定是冷file_area，然后释放这个file_area的page*/
#define FILE_AREA_TEMP_TO_COLD_AGE_DX  5
/*一个冷file_area，如果经过FILE_AREA_FREE_AGE_DX个周期，仍然没有被访问，则释放掉file_area结构*/
#define FILE_AREA_FREE_AGE_DX  10
/*当一个file_area在一个周期内访问超过FILE_AREA_HOT_LEVEL次数，则判定是热的file_area*/
#define FILE_AREA_HOT_LEVEL (PAGE_COUNT_IN_AREA << 2)
/*如果一个file_area在FILE_AREA_MOVE_HEAD_DX个周期内被访问了两次，然后才能移动到链表头*/
#define FILE_AREA_MOVE_HEAD_DX 3
/*在file_stat被判定为热文件后，记录当时的global_age。在未来HOT_FILE_COLD_AGE_DX时间内该文件进去冷却期：hot_file_update_file_status()函数中
 *只更新该文件file_area的age后，然后函数返回，不再做其他操作，节省性能*/
#define HOT_FILE_COLD_AGE_DX 10

#define SUPPORT_FS_UUID_LEN 50
#define SUPPORT_FS_NAME_LEN 10
#define SUPPORT_FS_COUNT 2

#define SUPPORT_FS_ALL  0
#define SUPPORT_FS_UUID 1
#define SUPPORT_FS_SINGLE     2

/**针对mmap文件新加的******************************/
#define MMAP_FILE_NAME_LEN 16
struct mmap_file_shrink_counter
{
	//扫描的file_area个数
	unsigned int scan_file_area_count;
	//扫描的file_stat个数
	unsigned int scan_file_stat_count;
	//扫描到的处于delete状态的file_stat个数
	unsigned int scan_delete_file_stat_count;
	//扫描的冷file_stat个数
	unsigned int scan_cold_file_area_count;
	//扫描到的大文件转小文件的个数
	unsigned int scan_large_to_small_count;

	//释放的page个数
	unsigned int free_pages;
	//隔离的page个数
	unsigned int isolate_lru_pages;
	//file_stat的refault链表转移到temp链表的file_area个数
	unsigned int file_area_refault_to_temp_list_count;
	//释放的file_area结构个数
	unsigned int file_area_free_count;

	//释放的file_stat个数
	unsigned int del_file_stat_count;
	//释放的file_area个数
	unsigned int del_file_area_count;
	//mmap的文件，但是没有mmap映射的文件页个数
	unsigned int in_cache_file_page_count;
};
struct hot_cold_file_shrink_counter
{
	/**get_file_area_from_file_stat_list()函数******/
	//扫描的file_area个数
	unsigned int scan_file_area_count;
	//扫描的file_stat个数
	unsigned int scan_file_stat_count;
	//扫描到的处于delete状态的file_stat个数
	unsigned int scan_delete_file_stat_count;
	//扫描的冷file_stat个数
	unsigned int scan_cold_file_area_count;
	//扫描到的大文件转小文件的个数
	unsigned int scan_large_to_small_count;
	//本次扫描到但没有冷file_area的file_stat个数
	unsigned int scan_fail_file_stat_count;

	/**free_page_from_file_area()函数******/
	//释放的page个数
	unsigned int free_pages;
	//隔离的page个数
	unsigned int isolate_lru_pages;
	//file_stat的refault链表转移到temp链表的file_area个数
	unsigned int file_area_refault_to_temp_list_count;
	//释放的file_area结构个数
	unsigned int file_area_free_count;
	//file_stat的hot链表转移到temp链表的file_area个数
	unsigned int file_area_hot_to_temp_list_count;

	/**free_page_from_file_area()函数******/
	//file_stat的hot链表转移到temp链表的file_area个数
	unsigned int file_area_hot_to_temp_list_count2;
	//释放的file_stat个数
	unsigned int del_file_stat_count;
	//释放的file_area个数
	unsigned int del_file_area_count;

	/**async_shrink_free_page()函数******/
	unsigned int lock_fail_count;
	unsigned int writeback_count;
	unsigned int dirty_count;
	unsigned int page_has_private_count;
	unsigned int mapping_count;
	unsigned int free_pages_count;
	unsigned int free_pages_fail_count;
	unsigned int page_unevictable_count; 
	unsigned int nr_unmap_fail;

	/**file_stat_has_zero_file_area_manage()函数****/
	unsigned int scan_zero_file_area_file_stat_count;

	//进程抢占lru_lock锁的次数
	unsigned int lru_lock_contended_count;
	//释放的file_area但是处于hot_file_area_cache数组的file_area个数
	unsigned int file_area_delete_in_cache_count;
	//从hot_file_area_cache命中file_area次数
	unsigned int file_area_cache_hit_count;

	//file_area内存回收期间file_area被访问的次数
	unsigned int file_area_access_count_in_free_page;
	//在内存回收期间产生的热file_area个数
	unsigned int hot_file_area_count_in_free_page;

	//一个周期内产生的热file_area个数
	unsigned int hot_file_area_count_one_period;
	//一个周期内产生的refault file_area个数
	unsigned int refault_file_area_count_one_period;
	//每个周期执行hot_file_update_file_status函数访问所有文件的所有file_area总次数
	unsigned int all_file_area_access_count;
	//每个周期直接从file_area_tree找到file_area并且不用加锁次数加1
	unsigned int find_file_area_from_tree_not_lock_count;

	//每个周期内因文件页page数太少被拒绝统计的次数
	unsigned int small_file_page_refuse_count;
	//每个周期从file_stat->file_area_last得到file_area的次数
	unsigned int find_file_area_from_last_count;

	//每个周期频繁冗余lru_lock的次数
	//unsigned int lru_lock_count;
	//释放的mmap page个数
	unsigned int mmap_free_pages_count;
	unsigned int mmap_writeback_count;
	unsigned int mmap_dirty_count;
};
//一个file_area表示了一片page范围(默认6个page)的冷热情况，比如page索引是0~5、6~11、12~17各用一个file_area来表示
struct file_area
{
	//不同取值表示file_area当前处于哪种链表
	unsigned int file_area_state;
	//该file_area最近被访问时的global_age，长时间不被访问则与global age差很多，则判定file_area是冷file_area，然后释放该file_area的page
	//如果是mmap文件页，当遍历到文件页的pte置位，才会更新对应的file_area的age为全局age，否则不更新
	unsigned int file_area_age;
	union{
		/*cache文件时，该file_area当前周期被访问的次数。mmap文件时，只有处于file_stat->temp链表上file_area才用access_count记录访问计数，
		 *处于其他file_stat->refault、hot、free等链表上file_area，不会用到access_count。但是因为跟file_area_access_age是共享枚举变量，
		 *要注意，从file_stat->refault、hot、free等链表移动file_area到file_stat->temp链表时，要对file_area_access_age清0*/
		//unsigned int access_count;
		atomic_t   access_count;
		/*处于file_stat->refault、hot、free等链表上file_area，被遍历到时记录当时的global age，不理会文件页page是否被访问了。
		 *由于和access_count是共享枚举变量，当file_area从file_stat->temp链表移动到file_stat->refault、hot、free等链表时，要对file_area_access_age清0*/
		unsigned int file_area_access_age;
	};
	//该file_area里的某个page最近一次被回收的时间点，单位秒
	unsigned int shrink_time;
	union{
		//file_area通过file_area_list添加file_stat的各种链表
		struct list_head file_area_list;
		//rcu_head和list_head都是16个字节
		struct rcu_head		i_rcu;
	};
	//该file_area代表的N个连续page的起始page索引
	pgoff_t start_index;
	struct folio __rcu *pages[PAGE_COUNT_IN_AREA];
};
struct hot_cold_file_area_tree_node
{
	//与该节点树下最多能保存多少个page指针有关
	unsigned char   shift;
	//在节点在父节点中的偏移
	unsigned char   offset;
	//指向父节点
	struct hot_cold_file_area_tree_node *parent;
	//该节点下有多少个成员
	unsigned int    count;
	//是叶子节点时保存file_area结构，是索引节点时保存子节点指针
	void    *slots[TREE_MAP_SIZE];
};
struct hot_cold_file_area_tree_root
{
	unsigned int  height;//树高度
	struct hot_cold_file_area_tree_node __rcu *root_node;
};
//热点文件统计信息，一个文件一个
struct file_stat
{
	struct address_space *mapping;
	union{
		//file_stat通过hot_cold_file_list添加到hot_cold_file_global的file_stat_hot_head链表
		struct list_head hot_cold_file_list;
		//rcu_head和list_head都是16个字节
		struct rcu_head		i_rcu;
	};
	//file_stat状态
	unsigned long file_stat_status;
	//总file_area个数
	unsigned int file_area_count;
	//热file_area个数
	unsigned int file_area_hot_count;
	//文件的file_area结构按照索引保存到这个radix tree
	struct hot_cold_file_area_tree_root hot_cold_file_area_tree_root_node;
	//file_stat锁
	spinlock_t file_stat_lock;
	//file_stat里age最大的file_area的age，调试用
	unsigned long max_file_area_age;
	/*最近一次访问的page的file_area所在的父节点，通过它直接得到file_area，然后得到page，不用每次都遍历xarray tree*/
	struct xa_node *xa_node_cache;
	/*xa_node_cache父节点保存的起始file_area的page的索引*/
	pgoff_t  xa_node_cache_base_index;
	/*在file_stat被判定为热文件后，记录当时的global_age。在未来HOT_FILE_COLD_AGE_DX时间内该文件进去冷却期：hot_file_update_file_status()函数中
	 *只更新该文件file_area的age后，然后函数返回，不再做其他操作，节省性能*/
	unsigned int file_stat_hot_base_age;


	union{
		//cache文件file_stat最近一次被异步内存回收访问时的age，调试用
		unsigned int recent_access_age;
		//mmap文件在扫描完一轮file_stat->temp链表上的file_area，进入冷却期，cooling_off_start_age记录当时的global age
		unsigned int cooling_off_start_age;
	};
	//频繁被访问的文件page对应的file_area存入这个头结点
	struct list_head file_area_hot;
	//不冷不热处于中间状态的file_area结构添加到这个链表，新分配的file_area就添加到这里
	struct list_head file_area_temp;
	//每轮扫描被释放内存page的file_area结构临时先添加到这个链表。file_area_free_temp有存在的必要
	struct list_head file_area_free_temp;
	//所有被释放内存page的file_area结构最后添加到这个链表，如果长时间还没被访问，就释放file_area结构。
	struct list_head file_area_free;
	//file_area的page被释放后，但很快又被访问，发生了refault，于是要把这种page添加到file_area_refault链表，短时间内不再考虑扫描和释放
	struct list_head file_area_refault;
#if 0
	//把最近访问的file_stat保存到hot_file_area_cache缓存数组，
	struct file_area * hot_file_area_cache[FILE_AREA_CACHE_COUNT];
	//最近一次访问的热file_area以hot_file_area_cache_index为下标保存到hot_file_area_cache数组
	unsigned char hot_file_area_cache_index;
#endif
	/*file_area_tree_node保存最近一次访问file_area的父节点，cache_file_area_tree_node_base_index是它保存的最小file_area索引。
	 *之后通过cache_file_area_tree_node->slots[]直接获取在同一个node的file_area，不用每次都遍历radix tree获取file_area*/
	unsigned int cache_file_area_tree_node_base_index;
	struct hot_cold_file_area_tree_node *cache_file_area_tree_node;

	//最新一次访问的file_area
	struct file_area *file_area_last;

	/**针对mmap文件新增的****************************/
	//根据文件mmap映射的虚拟地址，计算出文件mmap映射最大文件页索引
	unsigned int max_index;
	/*记录最近一次radix tree遍历该文件文件页索引，比如第一次遍历索引是0~11这12文件页，则last_index =11.如果last_index是0，
	 *说明该文件是第一次被遍历文件页，或者，该文件的所有文件页都被遍历过了，然后要从头开始遍历*/
	pgoff_t last_index;
	/*如果遍历完一次文件的所有page，traverse_done置1。后期每个周期加1，当traverse_done大于阀值，每个周期再尝试遍历该文件
	 *的少量page，因为这段时间文件缺页异常会分配新的文件页page。并且冷file_area的page被全部回收后，file_area会被从
	 *file_stat->mmap_file_stat_temp_head剔除并释放掉，后续就无法再从file_stat->mmap_file_stat_temp_head链表遍历到这个
	 *file_area。这种情况下，已经从radix tree遍历完一次文件的page且traverse_done是1，但是不得不每隔一段时间再遍历一次
	 *该文件的radix tree的空洞page*/
	unsigned char traverse_done;//现在使用了
	//file_area_refault链表最新一次访问的file_area
	struct file_area *file_area_refault_last;
	//file_area_free_temp链表最新一次访问的file_area
	struct file_area *file_area_free_last;
	char file_name[MMAP_FILE_NAME_LEN];
	//件file_stat->file_area_temp链表上已经扫描的file_stat个数，如果达到file_area_count_in_temp_list，说明这个文件的file_stat扫描完了，才会扫描下个文件file_stat的file_area
	unsigned int scan_file_area_count_temp_list;
	//在文件file_stat->file_area_temp链表上的file_area个数
	unsigned int file_area_count_in_temp_list;
	//file_area对应的page的pagecount大于0的，则把file_area移动到该链表
	struct list_head file_area_mapcount;
	//文件 mapcount大于1的file_area的个数
	unsigned int mapcount_file_area_count;
	//当扫描完一轮文件file_stat的temp链表上的file_area时，置1，进入冷却期，在N个age周期内不再扫描这个文件上的file_area。
	bool cooling_off_start;
};
/*hot_cold_file_node_pgdat结构体每个内存节点分配一个，内存回收前，从lruvec lru链表隔离成功page，移动到每个内存节点绑定的
 * hot_cold_file_node_pgdat结构的pgdat_page_list链表上.然后参与内存回收。内存回收后把pgdat_page_list链表上内存回收失败的
 * page在putback移动回lruvec lru链表。这样做的目的是减少内存回收失败的page在putback移动回lruvec lru链表时，可以减少
 * lruvec->lru_lock或pgdat->lru_lock加锁，详细分析见cold_file_isolate_lru_pages()函数。但实际测试时，内存回收失败的page是很少的，
 * 这个做法的意义又不太大!其实完全可以把参与内存回收的page移动到一个固定的链表也可以！*/
struct hot_cold_file_node_pgdat
{
	pg_data_t *pgdat;
	struct list_head pgdat_page_list;
	struct list_head pgdat_page_list_mmap_file;
};
//热点文件统计信息全局结构体
struct hot_cold_file_global
{
	/*被判定是热文本的file_stat添加到file_stat_hot_head链表,超过50%或者80%的file_area都是热的，则该文件就是热文件，
	 * 文件的file_stat要移动到global的file_stat_hot_head链表*/
	struct list_head file_stat_hot_head;
	//新分配的文件file_stat默认添加到file_stat_temp_head链表
	struct list_head file_stat_temp_head;
	/*如果文件file_stat上的page cache数太多，被判定为大文件，则把file_stat移动到这个链表。将来内存回收时，优先遍历这种file_stat，
	 *因为file_area足够多，能遍历到更多的冷file_area，回收到内存page*/
	struct list_head file_stat_temp_large_file_head;
	struct list_head cold_file_head;
	//inode被删除的文件的file_stat移动到这个链表
	struct list_head file_stat_delete_head;
	//0个file_area的file_stat移动到这个链表
	struct list_head file_stat_zero_file_area_head;
	//触发drop_cache后的没有file_stat的文件，分配file_stat后保存在这个链表
	struct list_head drop_cache_file_stat_head;

	//触发drop_cache后的没有file_stat的文件个数
	unsigned int drop_cache_file_count;
	//热文件file_stat个数
	unsigned int file_stat_hot_count;
	//大文件file_stat个数
	unsigned int file_stat_large_count;
	//文件file_stat个数
	unsigned int file_stat_count;
	//0个file_area的file_stat个数
	unsigned int file_stat_count_zero_file_area;

	/*当file_stat的file_area个数达到file_area_level_for_large_file时，表示该文件的page cache数太多，被判定为大文件。但一个file_area
	 *包含了多个page，一个file_area并不能填满page，因此实际file_stat的file_area个数达到file_area_level_for_large_file时，实际该文件的的page cache数会少点*/
	unsigned int file_area_level_for_large_file;
	//当一个文件的文件页page数大于nr_pages_level时，该文件的文件页page才会被本异步内存回收模块统计访问频率并回收，默认15，即64k，可通过proc接口调节大小
	unsigned int nr_pages_level;

	struct kmem_cache *file_stat_cachep;
	struct kmem_cache *file_area_cachep;
	//保存文件file_stat所有file_area的radix tree
	struct kmem_cache *hot_cold_file_area_tree_node_cachep;
	struct hot_cold_file_node_pgdat *p_hot_cold_file_node_pgdat;
	//异步内存回收线程
	struct task_struct *hot_cold_file_thead;
	int node_count;

	//有多少个进程在执行hot_file_update_file_status函数使用文件file_stat、file_area
	atomic_t   ref_count;
	//有多少个进程在执行__destroy_inode_handler_post函数，正在删除文件inode
	atomic_t   inode_del_count;
	//内存回收各个参数统计
	struct hot_cold_file_shrink_counter hot_cold_file_shrink_counter;
	//proc文件系统根节点
	struct proc_dir_entry *hot_cold_file_proc_root;

	spinlock_t global_lock;
	//全局age，每个周期加1
	unsigned long global_age;
	//异步内存回收周期，单位s
	unsigned int global_age_period;
	//热file_area经过file_area_refault_to_temp_age_dx个周期后，还没有被访问，则移动到file_area_temp链表
	unsigned int file_area_hot_to_temp_age_dx;
	//发生refault的file_area经过file_area_refault_to_temp_age_dx个周期后，还没有被访问，则移动到file_area_temp链表
	unsigned int file_area_refault_to_temp_age_dx;
	//普通的file_area在file_area_temp_to_cold_age_dx个周期内没有被访问则被判定是冷file_area，然后释放这个file_area的page
	unsigned int file_area_temp_to_cold_age_dx;
	//一个冷file_area，如果经过file_area_free_age_dx_fops个周期，仍然没有被访问，则释放掉file_area结构
	unsigned int file_area_free_age_dx;
	//当一个文件file_stat长时间不被访问，释放掉了所有的file_area，再过file_stat_delete_age_dx个周期，则释放掉file_stat结构
	unsigned int file_stat_delete_age_dx;

	//发生refault的次数,累加值
	unsigned long all_refault_count;
	//在内存回收期间产生的refault file_area个数
	unsigned int refault_file_area_count_in_free_page;

	char support_fs_type;
	char support_fs_uuid[SUPPORT_FS_UUID_LEN];
	char support_fs_name[SUPPORT_FS_COUNT][SUPPORT_FS_NAME_LEN];

	/**针对mmap文件新增的****************************/
	//新分配的文件file_stat默认添加到file_stat_temp_head链表
	struct list_head mmap_file_stat_uninit_head;
	//当一个文件的page都遍历完后，file_stat移动到这个链表
	struct list_head mmap_file_stat_temp_head;
	//文件file_stat个数超过阀值移动到这个链表
	struct list_head mmap_file_stat_temp_large_file_head;
	//热文件移动到这个链表
	struct list_head mmap_file_stat_hot_head;
	//一个文件有太多的page的mmapcount都大于1，则把该文件file_stat移动该链表
	struct list_head mmap_file_stat_mapcount_head;
	//0个file_area的file_stat移动到这个链表，暂时没用到
	struct list_head mmap_file_stat_zero_file_area_head;
	//inode被删除的文件的file_stat移动到这个链表，暂时不需要
	struct list_head mmap_file_stat_delete_head;
	//每个周期频繁冗余lru_lock的次数
	unsigned int lru_lock_count;
	unsigned int mmap_file_lru_lock_count;


	//mmap文件用的全局锁
	spinlock_t mmap_file_global_lock;

	struct file_stat *file_stat_last;
	//mmap文件个数
	unsigned int mmap_file_stat_count;
	//mapcount文件个数
	unsigned int mapcount_mmap_file_stat_count;
	//热文件个数
	unsigned int hot_mmap_file_stat_count;
	struct mmap_file_shrink_counter mmap_file_shrink_counter;
	/*当file_stat的file_area个数达到file_area_level_for_large_mmap_file时，表示该文件的page cache数太多，被判定为大文件*/
	unsigned int mmap_file_area_level_for_large_file;
};


/*******file_area状态**********************************************************/
enum file_area_status{//file_area_state是char类型，只有8个bit位可设置
	F_file_area_in_temp_list,
	F_file_area_in_hot_list,
	//F_file_area_in_free_temp_list,
	F_file_area_in_free_list,
	F_file_area_in_refault_list,
	F_file_area_in_mapcount_list,//file_area对应的page的pagecount大于0的，则把file_area移动到该链表
	F_file_area_in_cache,//file_area保存在ile_stat->hot_file_area_cache[]数组里
};
//不能使用 clear_bit_unlock、test_and_set_bit_lock、test_bit，因为要求p_file_area->file_area_state是64位数据，但实际只是u8型数据

#define MAX_FILE_AREA_LIST_BIT F_file_area_in_refault_list
#define FILE_AREA_LIST_MASK ((1 << (MAX_FILE_AREA_LIST_BIT + 1)) - 1)
//清理file_area的状态，在哪个链表
#define CLEAR_FILE_AREA_LIST_STATUS(list_name) \
	static inline void clear_file_area_in_##list_name(struct file_area *p_file_area)\
{ p_file_area->file_area_state &= ~(1 << F_file_area_in_##list_name);}
//设置file_area在哪个链表的状态
#define SET_FILE_AREA_LIST_STATUS(list_name) \
	static inline void set_file_area_in_##list_name(struct file_area *p_file_area)\
{ p_file_area->file_area_state |= (1 << F_file_area_in_##list_name);}
//测试file_area在哪个链表
#define TEST_FILE_AREA_LIST_STATUS(list_name) \
	static inline int file_area_in_##list_name(struct file_area *p_file_area)\
{return p_file_area->file_area_state & (1 << F_file_area_in_##list_name);}

#define TEST_FILE_AREA_LIST_STATUS_ERROR(list_name) \
	static inline int file_area_in_##list_name##_error(struct file_area *p_file_area)\
{return p_file_area->file_area_state & (~(1 << F_file_area_in_##list_name) & FILE_AREA_LIST_MASK);}

#define FILE_AREA_LIST_STATUS(list_name)     \
	CLEAR_FILE_AREA_LIST_STATUS(list_name) \
	SET_FILE_AREA_LIST_STATUS(list_name)  \
	TEST_FILE_AREA_LIST_STATUS(list_name) \
	TEST_FILE_AREA_LIST_STATUS_ERROR(list_name)

	FILE_AREA_LIST_STATUS(temp_list)
FILE_AREA_LIST_STATUS(hot_list)
	//FILE_AREA_LIST_STATUS(free_temp_list)
	FILE_AREA_LIST_STATUS(free_list)
	FILE_AREA_LIST_STATUS(refault_list)
FILE_AREA_LIST_STATUS(mapcount_list)

	//清理file_area的状态，在哪个链表
#define CLEAR_FILE_AREA_STATUS(status) \
		static inline void clear_file_area_in_##status(struct file_area *p_file_area)\
{ p_file_area->file_area_state &= ~(1 << F_file_area_in_##status);}
	//设置file_area在哪个链表的状态
#define SET_FILE_AREA_STATUS(status) \
		static inline void set_file_area_in_##status(struct file_area *p_file_area)\
{ p_file_area->file_area_state |= (1 << F_file_area_in_##status);}
	//测试file_area在哪个链表
#define TEST_FILE_AREA_STATUS(status) \
		static inline int file_area_in_##status(struct file_area *p_file_area)\
{return p_file_area->file_area_state & (1 << F_file_area_in_##status);}

#define FILE_AREA_STATUS(status)     \
		CLEAR_FILE_AREA_STATUS(status) \
	SET_FILE_AREA_STATUS(status)  \
	TEST_FILE_AREA_STATUS(status) 

FILE_AREA_STATUS(cache)


/*******file_stat状态**********************************************************/
enum file_stat_status{//file_area_state是long类型，只有64个bit位可设置
	F_file_stat_in_file_stat_hot_head_list,
	F_file_stat_in_file_stat_temp_head_list,
	F_file_stat_in_zero_file_area_list,
	F_file_stat_in_mapcount_file_area_list,//文件file_stat是mapcount文件
	F_file_stat_in_drop_cache,
	F_file_stat_in_free_page,//正在遍历file_stat的file_area的page，尝试释放page
	F_file_stat_in_free_page_done,//正在遍历file_stat的file_area的page，完成了page的内存回收,
	F_file_stat_in_delete,
	F_file_stat_in_cache_file,//cache文件，sysctl读写产生pagecache。有些cache文件可能还会被mmap映射，要与mmap文件互斥
	F_file_stat_in_mmap_file,//mmap文件，有些mmap文件可能也会被sysctl读写产生pagecache，要与cache文件互斥
	F_file_stat_in_large_file,
	F_file_stat_lock,
	F_file_stat_lock_not_block,//这个bit位置1，说明inode在删除的，但是获取file_stat锁失败
};
//不能使用 clear_bit_unlock、test_and_set_bit_lock、test_bit，因为要求p_file_stat->file_stat_status是64位数据，但这里只是u8型数据

#define MAX_FILE_STAT_LIST_BIT F_file_stat_in_free_page_done
#define FILE_STAT_LIST_MASK ((1 << (MAX_FILE_STAT_LIST_BIT + 1)) - 1)

//清理file_stat的状态，在哪个链表
#define CLEAR_FILE_STAT_STATUS(name)\
	static inline void clear_file_stat_in_##name##_list(struct file_stat *p_file_stat)\
{p_file_stat->file_stat_status &= ~(1 << F_file_stat_in_##name##_list);}
//设置file_stat在哪个链表的状态
#define SET_FILE_STAT_STATUS(name)\
	static inline void set_file_stat_in_##name##_list(struct file_stat *p_file_stat)\
{p_file_stat->file_stat_status |= (1 << F_file_stat_in_##name##_list);}
//测试file_stat在哪个链表
#define TEST_FILE_STAT_STATUS(name)\
	static inline int file_stat_in_##name##_list(struct file_stat *p_file_stat)\
{return (p_file_stat->file_stat_status & (1 << F_file_stat_in_##name##_list));}
#define TEST_FILE_STAT_STATUS_ERROR(name)\
	static inline int file_stat_in_##name##_list##_error(struct file_stat *p_file_stat)\
{return p_file_stat->file_stat_status & (~(1 << F_file_stat_in_##name##_list) & FILE_STAT_LIST_MASK);}

#define FILE_STAT_STATUS(name) \
	CLEAR_FILE_STAT_STATUS(name) \
	SET_FILE_STAT_STATUS(name) \
	TEST_FILE_STAT_STATUS(name) \
	TEST_FILE_STAT_STATUS_ERROR(name)

	FILE_STAT_STATUS(file_stat_hot_head)
	FILE_STAT_STATUS(file_stat_temp_head)
	FILE_STAT_STATUS(zero_file_area)
FILE_STAT_STATUS(mapcount_file_area)

	//清理文件的状态，大小文件等
#define CLEAR_FILE_STATUS(name)\
		static inline void clear_file_stat_in_##name(struct file_stat *p_file_stat)\
{p_file_stat->file_stat_status &= ~(1 << F_file_stat_in_##name);}
	//设置文件的状态，大小文件等
#define SET_FILE_STATUS(name)\
		static inline void set_file_stat_in_##name(struct file_stat *p_file_stat)\
{p_file_stat->file_stat_status |= (1 << F_file_stat_in_##name);}
	//测试文件的状态，大小文件等
#define TEST_FILE_STATUS(name)\
		static inline int file_stat_in_##name(struct file_stat *p_file_stat)\
{return (p_file_stat->file_stat_status & (1 << F_file_stat_in_##name));}
#define TEST_FILE_STATUS_ERROR(name)\
		static inline int file_stat_in_##name##_error(struct file_stat *p_file_stat)\
{return p_file_stat->file_stat_status & (~(1 << F_file_stat_in_##name) & FILE_STAT_LIST_MASK);}

#define FILE_STATUS(name) \
		CLEAR_FILE_STATUS(name) \
	SET_FILE_STATUS(name) \
	TEST_FILE_STATUS(name)\
	TEST_FILE_STATUS_ERROR(name)

FILE_STATUS(large_file)
FILE_STATUS(delete)
FILE_STATUS(drop_cache)

	//清理文件的状态，大小文件等
#define CLEAR_FILE_STATUS_ATOMIC(name)\
		static inline void clear_file_stat_in_##name(struct file_stat *p_file_stat)\
{clear_bit_unlock(F_file_stat_in_##name,&p_file_stat->file_stat_status);}
	//设置文件的状态，大小文件等
#define SET_FILE_STATUS_ATOMIC(name)\
		static inline void set_file_stat_in_##name(struct file_stat *p_file_stat)\
{if(test_and_set_bit_lock(F_file_stat_in_##name,&p_file_stat->file_stat_status)) \
	/*如果这个file_stat的bit位被多进程并发设置，不可能,应该发生了某种异常，触发crash*/  \
	panic("file_stat:0x%llx status:0x%lx alreay set %d bit\n",(u64)p_file_stat,p_file_stat->file_stat_status,F_file_stat_in_##name); \
}
	//测试文件的状态，大小文件等
#define TEST_FILE_STATUS_ATOMIC(name)\
		static inline int file_stat_in_##name(struct file_stat *p_file_stat)\
{return test_bit(F_file_stat_in_##name,&p_file_stat->file_stat_status);}
#define TEST_FILE_STATUS_ATOMIC_ERROR(name)\
		static inline int file_stat_in_##name##_error(struct file_stat *p_file_stat)\
{return p_file_stat->file_stat_status & (~(1 << F_file_stat_in_##name) & FILE_STAT_LIST_MASK);}

#define FILE_STATUS_ATOMIC(name) \
		CLEAR_FILE_STATUS_ATOMIC(name) \
	SET_FILE_STATUS_ATOMIC(name) \
	TEST_FILE_STATUS_ATOMIC(name) \
	TEST_FILE_STATUS_ATOMIC_ERROR(name) \
/* 为什么 file_stat的in_free_page、free_page_done的状态要使用test_and_set_bit_lock/clear_bit_unlock，主要是get_file_area_from_file_stat_list()函数开始内存回收，
 * 要把file_stat设置成in_free_page状态，此时hot_file_update_file_status()里就不能再把这些file_stat的file_area跨链表移动。而把file_stat设置成
 * in_free_page状态，只是加了global global_lock锁，没有加file_stat->file_stat_lock锁。没有加锁file_stat->file_stat_lock锁，就无法避免
 * hot_file_update_file_status()把把这些file_stat的file_area跨链表移动。因此，file_stat的in_free_page、free_page_done的状态设置要考虑原子操作吧，
 * 并且此时要避免此时有进程在执行hot_file_update_file_status()函数。这些在hot_file_update_file_status()和get_file_area_from_file_stat_list()函数
 * 有说明其实file_stat设置in_free_page、free_page_done 状态都有spin lock加锁，不使用test_and_set_bit_lock、clear_bit_unlock也行，
 * 目前暂定先用test_and_set_bit_lock、clear_bit_unlock吧，后续再考虑其他优化*/
FILE_STATUS_ATOMIC(free_page)
FILE_STATUS_ATOMIC(free_page_done)
/*标记file_stat delete可能在cold_file_stat_delete()和__destroy_inode_handler_post()并发执行，存在重复设置可能，用FILE_STATUS_ATOMIC会因重复设置而crash*/
//FILE_STATUS_ATOMIC(delete)
FILE_STATUS_ATOMIC(cache_file)
FILE_STATUS_ATOMIC(mmap_file)

extern struct hot_cold_file_global hot_cold_file_global_info;
extern unsigned long async_memory_reclaim_status;
extern unsigned int open_file_area_printk;
/** file_area的page bit/writeback mark bit/dirty mark bit/towrite mark bit统计**************************************************************/
#define FILE_AREA_PAGE_COUNT_SHIFT (XA_CHUNK_SHIFT + PAGE_COUNT_IN_AREA_SHIFT)//6+2
#define FILE_AREA_PAGE_COUNT_MASK ((1 << FILE_AREA_PAGE_COUNT_SHIFT) - 1)//0xFF 

/*file_area->file_area_state 的bit31~bit28 这个4个bit位标志file_area。注意，现在按照一个file_area只有4个page在
 *p_file_area->file_area_state的bit28~bit31写死了。如果file_area代表8个page，这里就得改动了!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 * */
//#define PAGE_BIT_OFFSET_IN_FILE_AREA_BASE (sizeof(&p_file_area->file_area_state)*8 - PAGE_COUNT_IN_AREA)//28  这个编译不通过
#define PAGE_BIT_OFFSET_IN_FILE_AREA_BASE (sizeof(unsigned int)*8 - PAGE_COUNT_IN_AREA)

/*writeback mark:bit27~bit24 dirty mark:bit23~bit20  towrite mark:bit19~bit16*/
#define WRITEBACK_MARK_IN_FILE_AREA_BASE (sizeof(unsigned int)*8 - PAGE_COUNT_IN_AREA*2)
#define DIRTY_MARK_IN_FILE_AREA_BASE     (sizeof(unsigned int)*8 - PAGE_COUNT_IN_AREA*3)
#define TOWRITE_MARK_IN_FILE_AREA_BASE   (sizeof(unsigned int)*8 - PAGE_COUNT_IN_AREA*4)

#define FILE_AREA_PRINT(fmt,...) \
    do{ \
        if(open_file_area_printk) \
			printk(fmt,##__VA_ARGS__); \
	}while(0);

static inline struct file_area *entry_to_file_area(void * file_area_entry)
{
	return (struct file_area *)((unsigned long)file_area_entry | 0x8000000000000000);
}
static inline void *file_area_to_entry(struct file_area *p_file_area)
{
	return (void *)((unsigned long)p_file_area & 0x7fffffffffffffff);
}
static inline int is_file_area_entry(void *file_area_entry)
{
	//最高的4个bit位依次是 0、1、1、1 则说明是file_area_entry，bit0和bit1也得是1
	return ((unsigned long)file_area_entry & 0xF000000000000003) == 0x7000000000000000;
}
static inline void clear_file_area_page_bit(struct file_area *p_file_area,unsigned char page_offset_in_file_area)
{
	unsigned int file_area_page_bit_clear = ~(1 << (PAGE_BIT_OFFSET_IN_FILE_AREA_BASE + page_offset_in_file_area));
	unsigned int file_area_page_bit_set = 1 << (PAGE_BIT_OFFSET_IN_FILE_AREA_BASE + page_offset_in_file_area);
	//如果这个page在 p_file_area->file_area_state对应的bit位没有置1，触发panic
	//if((p_file_area->file_area_state | file_area_page_bit_clear) != (sizeof(&p_file_area->file_area_state)*8 - 1))
	if((p_file_area->file_area_state & file_area_page_bit_set) == 0)
		panic("%s file_area:0x%llx file_area_state:0x%x page_offset_in_file_area:%d file_area_page_bit_set:0x%x already clear\n",__func__,(u64)p_file_area,p_file_area->file_area_state,page_offset_in_file_area,file_area_page_bit_set);

	//page在 p_file_area->file_area_state对应的bit位清0
	p_file_area->file_area_state = p_file_area->file_area_state & file_area_page_bit_clear;

}
static inline void set_file_area_page_bit(struct file_area *p_file_area,unsigned char page_offset_in_file_area)
{
	unsigned int file_area_page_bit_set = 1 << (PAGE_BIT_OFFSET_IN_FILE_AREA_BASE + page_offset_in_file_area);
	//如果这个page在 p_file_area->file_area_state对应的bit位已经置1了，触发panic
	if(p_file_area->file_area_state & file_area_page_bit_set)
		panic("%s file_area:0x%llx file_area_state:0x%x page_offset_in_file_area:%d file_area_page_bit_set:0x%x already set\n",__func__,(u64)p_file_area,p_file_area->file_area_state,page_offset_in_file_area,file_area_page_bit_set);

	//page在 p_file_area->file_area_state对应的bit位置1
	p_file_area->file_area_state = p_file_area->file_area_state | file_area_page_bit_set;
}
//测试page_offset_in_file_area这个位置的page在p_file_area->file_area_state对应的bit位是否置1了
static inline int is_file_area_page_bit_set(struct file_area *p_file_area,unsigned char page_offset_in_file_area)
{
	unsigned int file_area_page_bit_set = 1 << (PAGE_BIT_OFFSET_IN_FILE_AREA_BASE + page_offset_in_file_area);

	return (p_file_area->file_area_state & file_area_page_bit_set);
}
static inline int file_area_have_page(struct file_area *p_file_area)
{
	return  (p_file_area->file_area_state & ~((1 << PAGE_BIT_OFFSET_IN_FILE_AREA_BASE) - 1));//0XF000 0000
}
/*清理file_area所有的towrite、dirty、writeback的mark标记。这个函数是在把file_area从xarray tree剔除时执行的，之后file_area是无效的，有必要吗????????????*/
static inline void clear_file_area_towrite_dirty_writeback_mark(struct file_area *p_file_area)
{
    
}
static inline void clear_file_area_page_mark_bit(struct file_area *p_file_area,unsigned char page_offset_in_file_area,xa_mark_t type)
{
	unsigned int file_area_page_bit_clear;

	if(PAGECACHE_TAG_DIRTY == type){
		file_area_page_bit_clear = ~(1 << (DIRTY_MARK_IN_FILE_AREA_BASE + page_offset_in_file_area));
	}else if (PAGECACHE_TAG_WRITEBACK == type){
		file_area_page_bit_clear = ~(1 << (WRITEBACK_MARK_IN_FILE_AREA_BASE + page_offset_in_file_area));
	}else{
		if(PAGECACHE_TAG_TOWRITE != type)
			panic("%s file_area:0x%llx file_area_state:0x%x page_offset_in_file_area:%d type:%d\n",__func__,(u64)p_file_area,p_file_area->file_area_state,page_offset_in_file_area,type);

		file_area_page_bit_clear = ~(1 << (TOWRITE_MARK_IN_FILE_AREA_BASE + page_offset_in_file_area));
	}
	//page在 p_file_area->file_area_state对应的bit位清0
	p_file_area->file_area_state = p_file_area->file_area_state & file_area_page_bit_clear;

}
static inline void set_file_area_page_mark_bit(struct file_area *p_file_area,unsigned char page_offset_in_file_area,xa_mark_t type)
{
	unsigned int file_area_page_mark_bit_set;

	if(PAGECACHE_TAG_DIRTY == type){
		file_area_page_mark_bit_set = 1 << (DIRTY_MARK_IN_FILE_AREA_BASE + page_offset_in_file_area);
	}else if (PAGECACHE_TAG_WRITEBACK == type){
		file_area_page_mark_bit_set = 1 << (WRITEBACK_MARK_IN_FILE_AREA_BASE + page_offset_in_file_area);
	}else{
		if(PAGECACHE_TAG_TOWRITE != type)
			panic("%s file_area:0x%llx file_area_state:0x%x page_offset_in_file_area:%d type:%d\n",__func__,(u64)p_file_area,p_file_area->file_area_state,page_offset_in_file_area,type);

		file_area_page_mark_bit_set = 1 << (TOWRITE_MARK_IN_FILE_AREA_BASE + page_offset_in_file_area);
	}

	//page在 p_file_area->file_area_state对应的bit位置1
	p_file_area->file_area_state = p_file_area->file_area_state | file_area_page_mark_bit_set;
}
//测试page_offset_in_file_area这个位置的page在p_file_area->file_area_state对应的bit位是否置1了
static inline int is_file_area_page_mark_bit_set(struct file_area *p_file_area,unsigned char page_offset_in_file_area,xa_mark_t type)
{
	unsigned int file_area_page_mark_bit_set;

	if(PAGECACHE_TAG_DIRTY == type){
		file_area_page_mark_bit_set = 1 << (DIRTY_MARK_IN_FILE_AREA_BASE + page_offset_in_file_area);
	}else if (PAGECACHE_TAG_WRITEBACK == type){
		file_area_page_mark_bit_set = 1 << (WRITEBACK_MARK_IN_FILE_AREA_BASE + page_offset_in_file_area);
	}else{
		if(PAGECACHE_TAG_TOWRITE != type)
			panic("%s file_area:0x%llx file_area_state:0x%x page_offset_in_file_area:%d type:%d\n",__func__,(u64)p_file_area,p_file_area->file_area_state,page_offset_in_file_area,type);

		file_area_page_mark_bit_set = 1 << (TOWRITE_MARK_IN_FILE_AREA_BASE + page_offset_in_file_area);
	}

	return (p_file_area->file_area_state & file_area_page_mark_bit_set);
}

/*统计有多少个 mark page置位了，比如file_area有3个page是writeback，则返回3*/
static inline int file_area_page_mark_bit_count(struct file_area *p_file_area,char type)
{
	unsigned int file_area_page_mark;
	int count = 0;
	unsigned long page_mark_mask = (1 << PAGE_COUNT_IN_AREA) - 1;/*与上0xF，得到4个bit哪些置位0*/

	if(PAGECACHE_TAG_DIRTY == type){
		file_area_page_mark = (p_file_area->file_area_state >> DIRTY_MARK_IN_FILE_AREA_BASE) & page_mark_mask;
	}else if (PAGECACHE_TAG_WRITEBACK == type){
		file_area_page_mark = (p_file_area->file_area_state >> WRITEBACK_MARK_IN_FILE_AREA_BASE) & page_mark_mask;
	}else{
		if(PAGECACHE_TAG_TOWRITE != type)
			panic("%s file_area:0x%llx file_area_state:0x%x type:%d\n",__func__,(u64)p_file_area,p_file_area->file_area_state,type);

		file_area_page_mark = (p_file_area->file_area_state >> TOWRITE_MARK_IN_FILE_AREA_BASE) & page_mark_mask;
	}
	while(file_area_page_mark){
		if(file_area_page_mark & 0x1)
			count ++;

		file_area_page_mark = file_area_page_mark >> 1;
	}

	return count;
}
static inline void is_cold_file_area_reclaim_support_fs(struct address_space *mapping,struct super_block *sb)
{
	if(SUPPORT_FS_ALL == hot_cold_file_global_info.support_fs_type){
		if(sb->s_type){
			if(0 == strcmp(sb->s_type->name,"ext4") || 0 == strcmp(sb->s_type->name,"xfs") || 0 == strcmp(sb->s_type->name,"f2fs"))
				mapping->rh_reserved1 = SUPPORT_FILE_AREA_INIT_OR_DELETE;
		}
	}
	else if(SUPPORT_FS_SINGLE == hot_cold_file_global_info.support_fs_type){
		if(sb->s_type){
			int i;
			for(i = 0;i < SUPPORT_FS_COUNT;i ++){
				if(0 == strcmp(sb->s_type->name,hot_cold_file_global_info.support_fs_name[i])){
					mapping->rh_reserved1 = SUPPORT_FILE_AREA_INIT_OR_DELETE;
					break;
				}
			}
		}
	}
	else if(SUPPORT_FS_UUID == hot_cold_file_global_info.support_fs_type){

	}
}
/* 测试文件支持file_area形式读写文件和内存回收，并且已经分配了file_stat
 * mapping->rh_reserved1有3种状态
 *情况1:mapping->rh_reserved1是0：文件所属文件系统不支持file_area形式读写文件和内存回收
  情况2:mapping->rh_reserved1是1: 文件inode是初始化状态，但还没有读写文件而分配file_stat；或者文件读写后长时间未读写而文件页page全回收，
     file_stat被释放了。总之此时文件file_stat未分配，一个文件页page都没有
  情况3:mapping->rh_reserved1大于1：此时文件分配file_stat，走filemap.c里for_file_area正常读写文件流程
 */
/*#define IS_SUPPORT_FILE_AREA_READ_WRITE(mapping) \
    (mapping->rh_reserved1 > SUPPORT_FILE_AREA_INIT_OR_DELETE) 移动到 include/linux/pagemap.h 文件了*/
/*测试文件支持file_area形式读写文件和内存回收，此时情况2(mapping->rh_reserved1是1)和情况3(mapping->rh_reserved1>1)都要返回true*/
/*#define IS_SUPPORT_FILE_AREA(mapping) \
	(mapping->rh_reserved1 >=  SUPPORT_FILE_AREA_INIT_OR_DELETE)*/

/*****************************************************************************************************************************************************/
extern int shrink_page_printk_open1;
extern int shrink_page_printk_open;

static inline struct file_stat *file_stat_alloc_and_init(struct address_space *mapping)
{
	struct file_stat * p_file_stat = NULL;

	/*这里有个问题，hot_cold_file_global_info.global_lock有个全局大锁，每个进程执行到这里就会获取到。合理的是
	  应该用每个文件自己的spin lock锁!比如file_stat里的spin lock锁，但是在这里，每个文件的file_stat结构还没分配!!!!!!!!!!!!*/
	spin_lock(&hot_cold_file_global_info.global_lock);
	//如果两个进程同时访问一个文件，同时执行到这里，需要加锁。第1个进程加锁成功后，分配file_stat并赋值给
	//mapping->rh_reserved1，第2个进程获取锁后执行到这里mapping->rh_reserved1就会成立
	if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)){
		printk("%s file_stat:0x%llx already alloc\n",__func__,(u64)mapping->rh_reserved1);
		p_file_stat = (struct file_stat *)mapping->rh_reserved1;
		goto out;
	}
	//新的文件分配file_stat,一个文件一个，保存文件热点区域访问数据
	p_file_stat = kmem_cache_alloc(hot_cold_file_global_info.file_stat_cachep,GFP_ATOMIC);
	if (!p_file_stat) {
		printk("%s file_stat alloc fail\n",__func__);
		goto out;
	}
	//file_stat个数加1
	hot_cold_file_global_info.file_stat_count ++;
	memset(p_file_stat,0,sizeof(struct file_stat));
	//设置文件是cache文件状态，有些cache文件可能还会被mmap映射，要与mmap文件互斥，要么是cache文件要么是mmap文件，不能两者都是 
	set_file_stat_in_cache_file(p_file_stat);
	//初始化file_area_hot头结点
	INIT_LIST_HEAD(&p_file_stat->file_area_hot);
	INIT_LIST_HEAD(&p_file_stat->file_area_temp);
	INIT_LIST_HEAD(&p_file_stat->file_area_free_temp);
	INIT_LIST_HEAD(&p_file_stat->file_area_free);
	INIT_LIST_HEAD(&p_file_stat->file_area_refault);
	INIT_LIST_HEAD(&p_file_stat->file_area_mapcount);

	//mapping->file_stat记录该文件绑定的file_stat结构，将来判定是否对该文件分配了file_stat
	mapping->rh_reserved1 = (unsigned long)p_file_stat;
	//file_stat记录mapping结构
	p_file_stat->mapping = mapping;
	//设置file_stat in_temp_list最好放到把file_stat添加到global temp链表操作前，原因在add_mmap_file_stat_to_list()有分析
	set_file_stat_in_file_stat_temp_head_list(p_file_stat);
	smp_wmb();
	//把针对该文件分配的file_stat结构添加到hot_cold_file_global_info的file_stat_temp_head链表
	list_add(&p_file_stat->hot_cold_file_list,&hot_cold_file_global_info.file_stat_temp_head);
	//新分配的file_stat必须设置in_file_stat_temp_head_list链表
	//set_file_stat_in_file_stat_temp_head_list(p_file_stat);
	spin_lock_init(&p_file_stat->file_stat_lock);
out:	
	spin_unlock(&hot_cold_file_global_info.global_lock);

	return p_file_stat;
}
/*mmap文件跟cache文件的file_stat都保存在mapping->rh_reserved1，这样会不会有冲突?并且，主要有如下几点
 * 1：cache文件分配file_stat并保存到mapping->rh_reserved1是file_stat_alloc_and_init()函数，mmap文件分配file_stat并
 * 添添加到mapping->rh_reserved1是add_mmap_file_stat_to_list()。二者第一次执行时，都是该文件被读写，第一次分配page
 * 然后执行__filemap_add_folio_for_file_area()把page添加到xarray tree。这个过程需要防止并发，对mapping->rh_reserved1同时赋值
 * 这点，在__filemap_add_folio_for_file_area()开头有详细注释
 * 2:cache文件和mmap文件一个用的global file_global_lock，一个是global mmap_file_global_lock锁。分开使用，否则这个
 * 全局锁同时被多个进程抢占，阻塞时间会很长，把大锁分成小锁。但是分开用，就无法防止cache文件和mmap的并发!!!
 * 3：最重要的，一个文件，即有mmap映射读写、又有cache读写，怎么判断冷热和内存回收？mapping->rh_reserved1代表的file_stat
 * 是代表cache文件还是mmap文件？按照先到先得处理：
 *
 * 如果__filemap_add_folio_for_file_area()中添加该文件的第一个page到xarray tree，
 * 分配file_stat时，该文件已经建立了mmap映射，即mapping->i_mmap非NULL，则该文件就是mmap文件，然后执行add_mmap_file_stat_to_list()
 * 分配的file_stat添加global mmap_file_stat_uninit_head链表。后续，如果该文件被cache读写(read/write系统调用读写)，执行到
 * hot_file_update_file_status()函数时，只更新file_area的age，立即返回，不能再把file_area启动到file_stat->hot、refault等链表。
 * mmap文件的file_area是否移动到file_stat->hot、refault等链表，在check_file_area_cold_page_and_clear()中进行。其实，这种
 * 情况下，这些file_area在 hot_file_update_file_status()中把file_area启动到file_stat->hot、refault等链表，似乎也可以????????????
 *
 * 相反，如果__filemap_add_folio_for_file_area()中添加该文件的第一个page到xarray tree，该文件没有mmap映射，则判定为cache文件。
 * 如果后续该文件又mmap映射了，依然判定为cache文件，否则关系会错乱。但不用担心回收内存有问题，因为cache文件内存回收会跳过mmap
 * 的文件页。
 * */
static inline struct file_stat *add_mmap_file_stat_to_list(struct address_space *mapping)
{
	struct file_stat *p_file_stat = NULL;

	spin_lock(&hot_cold_file_global_info.mmap_file_global_lock);
	/*1:如果两个进程同时访问一个文件，同时执行到这里，需要加锁。第1个进程加锁成功后，分配file_stat并赋值给
	  mapping->rh_reserved1，第2个进程获取锁后执行到这里mapping->rh_reserved1就会成立
      2:异步内存回收功能禁止了*/
	if(IS_SUPPORT_FILE_AREA_READ_WRITE(mapping)){
		p_file_stat = (struct file_stat *)mapping->rh_reserved1;
		spin_unlock(&hot_cold_file_global_info.mmap_file_global_lock);
		printk("%s file_stat:0x%llx already alloc\n",__func__,(u64)mapping->rh_reserved1);
		goto out;  
	}

	p_file_stat = kmem_cache_alloc(hot_cold_file_global_info.file_stat_cachep,GFP_ATOMIC);
	if (!p_file_stat) {
		spin_unlock(&hot_cold_file_global_info.mmap_file_global_lock);
		printk("%s file_stat alloc fail\n",__func__);
		goto out;
	}
	//设置file_stat的in mmap文件状态
	hot_cold_file_global_info.mmap_file_stat_count++;
	memset(p_file_stat,0,sizeof(struct file_stat));
	//设置文件是mmap文件状态，有些mmap文件可能还会被读写，要与cache文件互斥，要么是cache文件要么是mmap文件，不能两者都是 
	set_file_stat_in_mmap_file(p_file_stat);
	INIT_LIST_HEAD(&p_file_stat->file_area_hot);
	INIT_LIST_HEAD(&p_file_stat->file_area_temp);
	INIT_LIST_HEAD(&p_file_stat->file_area_free_temp);
	INIT_LIST_HEAD(&p_file_stat->file_area_free);
	INIT_LIST_HEAD(&p_file_stat->file_area_refault);
	//file_area对应的page的pagecount大于0的，则把file_area移动到该链表
	INIT_LIST_HEAD(&p_file_stat->file_area_mapcount);

	//mapping->file_stat记录该文件绑定的file_stat结构，将来判定是否对该文件分配了file_stat
	mapping->rh_reserved1 = (unsigned long)p_file_stat;
	p_file_stat->mapping = mapping;
	/*现在把新的file_stat移动到gloabl  mmap_file_stat_uninit_head了，并且不设置状态图，目前没必要设置状态。
	 *遍历完一次page后才会移动到temp链表。*/
#if 1
	/*新分配的file_stat必须设置in_file_stat_temp_head_list链表。这个设置file_stat状态的操作必须放到 把file_stat添加到
	 *tmep链表前边，还要加内存屏障。否则会出现一种极端情况，异步内存回收线程从temp链表遍历到这个file_stat，
	 *但是file_stat还没有设置为in_temp_list状态。这样有问题会触发panic。因为mmap文件异步内存回收线程，
	 *从temp链表遍历file_stat没有mmap_file_global_lock加锁，所以与这里存在并发操作。而针对cache文件，异步内存回收线程
	 *从global temp链表遍历file_stat，全程global_lock加锁，不会跟向global temp链表添加file_stat存在方法，但最好改造一下*/
	set_file_stat_in_file_stat_temp_head_list(p_file_stat);
	smp_wmb();
#endif	

	/*把针对该文件分配的file_stat结构添加到hot_cold_file_global_info的mmap_file_stat_uninit_head链表。
	 * 现在新的方案是直接把刚分配的file_stat添加到global mmap_file_stat_temp_head链表*/
#if 0	
	list_add(&p_file_stat->hot_cold_file_list,&hot_cold_file_global_info.mmap_file_stat_uninit_head);
#else
	list_add(&p_file_stat->hot_cold_file_list,&hot_cold_file_global_info.mmap_file_stat_temp_head);
#endif	
	/*新分配的file_stat必须设置in_file_stat_temp_head_list链表。注意，现在新分配的file_stat是先添加到global mmap_file_stat_uninit_head
	 *链表，而不是添加到global temp链表，因此此时file_stat并没有设置in_file_stat_temp_head_list属性。这点很关键。但是现在
	 新的方案需要了*/

	//set_file_stat_in_file_stat_temp_head_list(p_file_stat);
	spin_lock_init(&p_file_stat->file_stat_lock);

	spin_unlock(&hot_cold_file_global_info.mmap_file_global_lock);
	//strncpy(p_file_stat->file_name,file->f_path.dentry->d_iname,MMAP_FILE_NAME_LEN-1);
	//p_file_stat->file_name[MMAP_FILE_NAME_LEN-1] = 0;
	if(shrink_page_printk_open)
		printk("%s file_stat:0x%llx\n",__func__,(u64)p_file_stat);

out:
	return p_file_stat;
}
static inline struct file_area *file_area_alloc_and_init(unsigned int area_index_for_page,struct file_stat * p_file_stat)
{
	struct file_area *p_file_area = NULL;

	spin_lock(&p_file_stat->file_stat_lock);
	/*到这里，针对当前page索引的file_area结构还没有分配,page_slot_in_tree是槽位地址，*page_slot_in_tree是槽位里的数据，就是file_area指针，
	  但是NULL，于是针对本次page索引，分配file_area结构*/
	p_file_area = kmem_cache_alloc(hot_cold_file_global_info.file_area_cachep,GFP_ATOMIC);
	if (!p_file_area) {
		//spin_unlock(&p_file_stat->file_stat_lock);
		printk("%s file_area alloc fail\n",__func__);
		goto out;
	}
	memset(p_file_area,0,sizeof(struct file_area));
	//把新分配的file_area添加到file_area_temp链表
	list_add(&p_file_area->file_area_list,&p_file_stat->file_area_temp);
	//保存该file_area对应的起始page索引，一个file_area默认包含8个索引挨着依次增大page，start_index保存其中第一个page的索引
	p_file_area->start_index = area_index_for_page << PAGE_COUNT_IN_AREA_SHIFT;//area_index_for_page * PAGE_COUNT_IN_AREA;
	p_file_stat->file_area_count ++;//文件file_stat的file_area个数加1
	set_file_area_in_temp_list(p_file_area);//新分配的file_area必须设置in_temp_list链表
			
	//在file_stat->file_area_temp链表的file_area个数加1
    p_file_stat->file_area_count_in_temp_list ++;

out:
	spin_unlock(&p_file_stat->file_stat_lock);

	return p_file_area;
}
/*令inode引用计数减1，如果inode引用计数是0则释放inode结构*/
static void inline file_inode_unlock(struct file_stat * p_file_stat)
{
    struct inode *inode = p_file_stat->mapping->host;
    //令inode引用计数减1，如果inode引用计数是0则释放inode结构
	iput(inode);
}
static void inline file_inode_unlock_mapping(struct address_space *mapping)
{
    struct inode *inode = mapping->host;
    //令inode引用计数减1，如果inode引用计数是0则释放inode结构
	iput(inode);
}

/*对文件inode加锁，如果inode已经处于释放状态则返回0，此时不能再遍历该文件的inode的address_space的radix tree获取page，释放page，
 *此时inode已经要释放了，inode、address_space、radix tree都是无效内存。否则，令inode引用计数加1，然后其他进程就无法再释放这个
 *文件的inode，此时返回1*/
static int inline file_inode_lock(struct file_stat * p_file_stat)
{
    /*不能在这里赋值，因为可能文件inode被iput后p_file_stat->mapping赋值NULL，这样会crash*/
	//struct inode *inode = p_file_stat->mapping->host;
	struct inode *inode;

	/*这里有个隐藏很深的bug!!!!!!!!!!!!!!!!如果此时其他进程并发执行iput()最后执行到__destroy_inode_handler_post()触发删除inode，
	 *然后就会立即把inode结构释放掉。此时当前进程可能执行到file_inode_lock()函数的spin_lock(&inode->i_lock)时，但inode已经被释放了，
	 则会访问已释放的inode的mapping的xarray 而crash。怎么防止这种并发？*/
    
	/*最初方案：当前函数执行lock_file_stat()对file_stat加锁。在__destroy_inode_handler_post()中也会lock_file_stat()加锁。防止
	 * __destroy_inode_handler_post()中把inode释放了，而当前函数还在遍历该文件inode的mapping的xarray tree
	 * 查询page，访问已经释放的内存而crash。这个方案太麻烦!!!!!!!!!!!!!!，现在的方案是使用rcu，这里
	 * rcu_read_lock()和__destroy_inode_handler_post()中标记inode delete形成并发。极端情况是，二者同时执行，
	 * 但这里rcu_read_lock后，进入rcu宽限期。而__destroy_inode_handler_post()执行后，触发释放inode，然后执行到destroy_inode()里的
	 * call_rcu(&inode->i_rcu, i_callback)后，无法真正释放掉inode结构。当前函数可以放心使用inode、mapping、xarray tree。
	 * 但有一点需注意，rcu_read_lock后不能休眠，否则rcu宽限期会无限延长。*/

	//lock_file_stat(p_file_stat,0);
	rcu_read_lock();
	smp_rmb();
	if(file_stat_in_delete(p_file_stat) || (NULL == p_file_stat->mapping)){
		//不要忘了异常return要先释放锁
		rcu_read_unlock();
		return 0;
	}
	inode = p_file_stat->mapping->host;

	spin_lock(&inode->i_lock);
	/*执行到这里，inode肯定没有被释放，并且inode->i_lock加锁成功，其他进程就无法再释放这个inode了。错了，又一个隐藏很深的bug。
	 *!!!!!!!!!!!!!!!!因为其他进程此时可能正在iput()->__destroy_inode_handler_post()中触发释放inode。这里rcu_read_unlock后，
	 *inode就会立即被释放掉，然后下边再使用inode就会访问无效inode结构而crash。rcu_read_unlock要放到对inode引用计数加1后*/

	//unlock_file_stat(p_file_stat);
	//rcu_read_unlock();

	//如果inode已经被标记释放了，直接return
	if( ((inode->i_state & (I_FREEING|I_WILL_FREE|I_NEW))) || atomic_read(&inode->i_count) == 0){
		spin_unlock(&inode->i_lock);
	    rcu_read_unlock();

		//如果inode已经释放了，则要goto unsed_inode分支释放掉file_stat结构
		return 0;
	}
	//令inode引用计数加1，之后就不用担心inode被其他进程释放掉
	atomic_inc(&inode->i_count);
	spin_unlock(&inode->i_lock);
	
	rcu_read_unlock();

	return 1;
}
static void inline file_area_access_count_clear(struct file_area *p_file_area)
{
	atomic_set(&p_file_area->access_count,0);
}
static void inline file_area_access_count_add(struct file_area *p_file_area,int count)
{
	atomic_add(count,&p_file_area->access_count);
}
static int inline file_area_access_count_get(struct file_area *p_file_area)
{
	return atomic_read(&p_file_area->access_count);
}


extern int hot_file_update_file_status(struct address_space *mapping,struct file_stat *p_file_stat,struct file_area *p_file_area,int access_count);
extern void printk_shrink_param(struct hot_cold_file_global *p_hot_cold_file_global,struct seq_file *m,int is_proc_print);
extern int hot_cold_file_print_all_file_stat(struct hot_cold_file_global *p_hot_cold_file_global,struct seq_file *m,int is_proc_print);//is_proc_print:1 通过proc触发的打印
extern void get_file_name(char *file_name_path,struct file_stat * p_file_stat);
extern unsigned long cold_file_isolate_lru_pages_and_shrink(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat,struct list_head *file_area_free);
extern unsigned int cold_mmap_file_isolate_lru_pages_and_shrink(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat,struct file_area *p_file_area,struct page *page_buf[],int cold_page_count);
extern unsigned long shrink_inactive_list_async(unsigned long nr_to_scan, struct lruvec *lruvec,struct hot_cold_file_global *p_hot_cold_file_global,int is_mmap_file, enum lru_list lru);
extern int walk_throuth_all_mmap_file_area(struct hot_cold_file_global *p_hot_cold_file_global);
extern int cold_mmap_file_stat_delete(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat_del);
extern unsigned int cold_file_stat_delete_all_file_area(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat_del);
extern int cold_file_stat_delete(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat_del);
extern int cold_file_area_delete(struct hot_cold_file_global *p_hot_cold_file_global,struct file_stat * p_file_stat,struct file_area *p_file_area);
#endif
