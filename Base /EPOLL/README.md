
## Epoll
### 函数接口
```c
#include<sys/epoll.h>
int epoll_create1(int flags);
int epoll_create(int size);
int epoll_ctl(int epollfd,int op,int fd,struct epoll_event* event);
int epoll_wait(int epollfd,struct epoll_event* events,int maxevents,int timeout);

struct epoll_event{
    uint32_t events;
    union{
        void* ptr;
        int fd;
        uint32_t u32;
        uint64_t u64;
    } data;
}
```
### 基本操作

    注意事项：
        调用epoll_create时，返回一个内核维护的描述符，往后的所有操作依赖于这个描述符。size参数没什么用处，保证大于0就行。
        调用epoll_ctl可以动态的改变内核维护的表格。在epollfd中加入和修改一个fd关注的事件时，需要用户提供event实例；在epollfd中删除一个fd时，传入保存数据的struct epoll_event结构体或者NULL
        调用epoll_wait时，提供从epoll_create中获得描述符，和事先准备好的数组，最大的事件个数以及超时时间。在完成后，内核会将发生事件的struct epoll_event*拷贝到用户空间
    事件类型：
        EPOLLERR：文件出错，默认监听
        EPOLLET：在监听的文件上开启ET模式
        EPOLLHUP：文件被挂起，默认监听
        EPOLLIN：文件未阻塞，可读
        EPOLLOUT：文件未阻塞，可写
        EPOLLONESHOT：事件生成并处理的过程中，被epoll自动移除，防止多线程同时操作一个文件描述符。如果还要继续监听的话，需要手动加入。
        EPOLLPRI：带外数据可读。
    epoll_create错误标志：
        EINVAL：参数flags非法（基本弃用）
        EMFILE：用户打开文件到达上限
        ENFILE：系统打开文件到达上限
        ENOMEN：内存不足，不能完成此次操作
    epoll_ctl错误标志：
        EBADF：epollfd非法或者fd非法
        EEXIST：op值为EPOLL_CTL_ADD，但是fd已经和epollfd关联
        EINVAL：epollfd不是epoll实例，epollfd和fd相同或者op无效
        ENOENT：op值设置为EPOLL_CTL_MOD或者EPOLL_CTL_DEL，但是epollfd和fd没有关联
        ENOMEN：没有足够的内存来处理请求
        EPERM：fd不支持epoll
    epoll_wait错误标志：
        EBADF：epollfd是无效的文件描述符
        EFAULT：进程对events所指向的内存没有写权限
        EINTR：系统调用在完成前发生信号中断或者超时
        EINVAL：epollfd不是有效的epoll实例，或者maxevents小于0
    epoll_ctl操作类型：
        EPOLL_CTL_ADD：将fd描述的事件添加到epollfd关注的事件中
        EPOLL_CTL_DEL：删除epollfd中fd描述的事件
        EPOLL_CTL_MOD：修改epollfd中fd描述的事件
    工作方式：
        LT（Level Triggered）
            Epoll缺省的工作方式，相当于一个速度比较快的Poll。
            收到多次数据会触发多次事件，内核仅仅告诉你一个事件发生了，如果你不处理，内核会继续通知你。在异步模型中，这样的特性会影响事件的处理方式。
            同时支持阻塞与非阻塞两种IO模型
        ET（Edge Triggered）：
            需要使用EPOLLET来设置
            使用epoll_wait获得事件后，如果这次没有将内核缓冲区中的数据处理干净，如果没有新的数据到达（再一次触发事件），将没法从epoll_wait获取事件
            仅支持非阻塞的IO模型

性能分析

    时间角度
        使用了回调函数对发生事件的描述符进行处理，如果有事件状态发生改变会调用回调函数进行处理，没有使用轮询，效率会高很多
    空间角度
        使用VFS提供的操作将底层组织起来，用户只需要使用表面的epollfd即可。除了注册事件，用户不会向内核拷贝大量的数据。但是在每一次有事件发生的时候，内核会向用户拷贝大量的数据。
        内核为每一个关注的fd在进程中提供了一个file在VFS层进行描述，实际上使用struct epitem描述每一个fd，并使用红黑树将这些数据组织起来，对于事件就绪的返回中使用单链表进行描述。
        在LT模式下，每次处理完事件之后都会直接将struct epitem加入到事件就绪链表中，不管有没有下次有没有事件发生，都会返回。对于ET模式，除非事件的状态发生改变（事件再次发生），调用file->f_op->poll()显示的调用内核提供的回调函数，否则不会将struct item加入到就绪链表中。

### 源码分析

基础数据结构：
```c
//每创建一个epollfd，就会在内核中分配一个eventpoll与之关联
struct eventpoll {
    spinlock_t lock;        //自旋锁保护数据
    struct mutex mtx;        //添加，修改或者删除监听fd的时候，以及epoll_wait返回，向用户空间传递数据的时候都会持有这个互斥锁，所以在用户空间可以放心的在多个线程执行与epollfd相关的操作
    wait_queue_head_t wq;        //在sys_epoll_wait中使用，如果使用定时设置，会在这个队列上休眠
    wait_queue_head_t poll_wait;        //这个用于底层文件系统例程file->poll
    struct list_head rdllist;        //所有已经准备好的epitem都在这个链表上
    struct rb_root rbr;        //所有要监听的epitem（fd）都使用红黑树组织起来
    struct epitem *ovflist;        //这是一个单链表，用于将事件在用户空间和内核进行传递
};
```
```c
//epitem表示一个被监听的fd
struct epitem {
    struct rb_node rbn;        //使用红黑树将每一个需要监听的epitem（fd）组织起来
    struct list_head rdllink;        //所有已经就绪的spitem（fd）都会用这个链表组织起来

    struct epitem *next;
    struct epoll_filefd ffd;        //epitem对应的fd和file
    int nwait;
    struct list_head pwqlist;
    struct eventpoll *ep;        //反向指针，指向关联的容器
    struct list_head fllink;
    struct epoll_event event;        //当前关注的事件，由用户传递进来
};
```
```c
struct epoll_filefd
{
    struct file* file;
    int fd;
}
```
```c
//与poll之间关联的钩子
struct eppoll_entry {
    struct list_head llink; 
    void *base;
    wait_queue_t wait;
    wait_queue_head_t *whead;

struct ep_pqueue{
    int maxevents;
    strut epoll_event __user *events;
}

struct ep_send_events_data{
    int maxevents;
    struct epoll_event __user *events;
}
```
通用函数：
+ Order 0：asmlinkage long sys_epoll_create(int size)：创建epollfd
```c
asmlinkage long sys_epoll_create(int size)
{
    int error, fd = -1;        
    struct eventpoll *ep;        
    struct inode *inode;        //在虚拟文件系统中分配一个inode，与epollfd相关联
    struct file *file;

    error = -EINVAL;
    if (size <= 0 || (error = ep_alloc(&ep)) != 0)        //分配一个eventpoll实例，可见：size只要>0就行
        goto error_return;
    
//这里在在进程中创建一个struct file描述ep，并在虚拟文件系统中创建一个inode用以调用VFS提供的一些通用的例程。
//在一般的file中，会默认使用由VFS提供的例程，但是这里传递了eventpoll_ops的例程用以填充file_ops
//epoll只实现了poll（没有使用VFS提供的poll）和release（与close）相同作用
//ep存储在file->private(void*)指针中，用以可以直接从file对象中获得ep
//总之，为了可以通过fd->file->eventpoll
    error = anon_inode_getfd(&fd, &inode, &file, "[eventpoll]",&eventpoll_fops, ep);        
    if (error)
        goto error_free;

    return fd;        //一般在块，epoll_create就结束了
error_free:
    ep_free(ep);
error_return:
    return error;
}
```
+ Order 1：static int ep_alloc(struct eventpoll **pep) ：在内核分配一个eventpoll，用以描述epollfd
```c
static int ep_alloc(struct eventpoll **pep)
{
     struct eventpoll *ep = kzalloc(sizeof(*ep), GFP_KERNEL);         //从slab中分配一个eventpoll实例
     if (!ep)
          return -ENOMEM;        //如果分配失败，返回没有足够内存的信号
     
     spin_lock_init(&ep->lock);        //初始化ep中的各种数据结构
     mutex_init(&ep->mtx);
     init_waitqueue_head(&ep->wq);
     init_waitqueue_head(&ep->poll_wait);
     INIT_LIST_HEAD(&ep->rdllist);
     ep->rbr = RB_ROOT;
     ep->ovflist = EP_UNACTIVE_PTR;
     *pep = ep;
     return 0;
}
```
+ Order 0：asmlinkage long sys_epoll_ctl(int epfd, int op, int fd, struct epoll_event __user *event) ：对上一步创建的epollfd进行操作
```c
asmlinkage long sys_epoll_ctl(int epfd, int op, int fd,  struct epoll_event __user *event) 
{
    int error;
    struct file *file, *tfile;
    struct eventpoll *ep;
    struct epitem *epi;
    struct epoll_event epds;
    error = -EFAULT;
    if (ep_op_has_event(op) &&  copy_from_user(&epds, event, sizeof(struct epoll_event)))         //将epoll_event从用户空间copy到内核中
        goto error_return;
 
    error = -EBADF;
    file = fget(epfd);        //从进程ftable中获得epollfd关联的file
    if (!file)
        goto error_return;

    tfile = fget(fd);        //获得传入的fd关联的file
    if (!tfile)
        goto error_fput;

     error = -EPERM;
    if (!tfile->f_op || !tfile->f_op->poll)        //判断fd是否支持poll
        goto error_tgt_fput; 
 
    error = -EINVAL;        //epoll不能监听自己
    if (file == tfile || !is_file_epoll(file))
        goto error_tgt_fput;

    ep = file->private_data;        //从file中eventpoll实例
    mutex_lock(&ep->mtx);        //在每次对epollfd进行操作时，都会加锁

    epi = ep_find(ep, tfile, fd);        //epoll不允许重复添加，从红黑树种查找epitem（用以描述的fd）
    error = -EINVAL;
    switch (op) {
    case EPOLL_CTL_ADD: 
        if (!epi) {         //如果epitem不存在
            epds.events |= POLLERR | POLLHUP;        //默认关注这两个事件 
            error = ep_insert(ep, &epds, tfile, fd);        //将这个事件添加到epollfd中去
        } else 
        error = -EEXIST; 
        break; 
    case EPOLL_CTL_DEL: 
        if (epi)         //如果epitem存在，从eventpoll中移除
            error = ep_remove(ep, epi); 
        else 
            error = -ENOENT;
        break;
    case EPOLL_CTL_MOD: 
        if (epi) { 
        epds.events |= POLLERR | POLLHUP;        //重新设置epitem的事件
        error = ep_modify(ep, epi, &epds);
        } else 
            error = -ENOENT; 
        break;
    } 
    mutex_unlock(&ep->mtx); 
error_tgt_fput: 
    fput(tfile);
error_fput: 
    fput(file);
error_return:
    return error; 
}
```
+ Order 1：static int ep_modify(struct eventpoll *ep, struct epitem *epi, struct epoll_event *event) ：在eventpoll的红黑树中调整epitem中关注的事件
```c
static int ep_modify(struct eventpoll *ep, struct epitem *epi, struct epoll_event *event)
{
    int pwake = 0;
    unsigned int revents;
    unsigned long flags;

    epi->event.events = event->events;        //调整epitem关注的事件

    revents = epi->ffd.file->f_op->poll(epi->ffd.file, NULL);        //顺带调用底层的poll例程进行处理
    spin_lock_irqsave(&ep->lock, flags);

    epi->event.data = event->data;        //将user的data数据拷贝到epitem中

    if (revents & event->events) {        //如果确实有事件发生，且这个item没有在eventpoll的事件发生的链表中，将这个发生的事件加入单链表中
        if (!ep_is_linked(&epi->rdllink)) { 
            list_add_tail(&epi->rdllink, &ep->rdllist);
  
            if (waitqueue_active(&ep->wq))         //唤醒等待的事件的任务队列
                __wake_up_locked(&ep->wq, TASK_UNINTERRUPTIBLE | TASK_INTERRUPTIBLE); 
           if (waitqueue_active(&ep->poll_wait))
               pwake++;
        } 
    } 
    spin_unlock_irqrestore(&ep->lock, flags);
 
    if (pwake)         
        ep_poll_safewake(&psw, &ep->poll_wait); 
 return 0;
}

    Order 1：static struct epitem *ep_find(struct eventpoll *ep, struct file *file, int fd) ：标准的红黑树的查找过程

static struct epitem *ep_find(struct eventpoll *ep, struct file *file, int fd)
{
	int kcmp;
	struct rb_node *rbp;
	struct epitem *epi, *epir = NULL;
	struct epoll_filefd ffd;

	ep_set_ffd(&ffd, file, fd);
	for (rbp = ep->rbr.rb_node; rbp; ) {
		epi = rb_entry(rbp, struct epitem, rbn);
		kcmp = ep_cmp_ffd(&ffd, &epi->ffd);
		if (kcmp > 0)
			rbp = rbp->rb_right;
		else if (kcmp < 0)
			rbp = rbp->rb_left;
		else {
			epir = epi;
			break;
		}
	}

	return epir;
}
```
+ Order 1：static int ep_remove(struct eventpoll *ep, struct epitem *epi)：从eventpoll维护的红黑树中删除epitem
```c
static int ep_remove(struct eventpoll *ep, struct epitem *epi)
{
	unsigned long flags;
	struct file *file = epi->ffd.file;

	ep_unregister_pollwait(ep, epi);        //从poll的等待队列中删除

	spin_lock(&file->f_ep_lock);
	if (ep_is_linked(&epi->fllink))        //不知道是哪个链表
		list_del_init(&epi->fllink);
	spin_unlock(&file->f_ep_lock);

	if (ep_rb_linked(&epi->rbn))
		ep_rb_erase(&epi->rbn, &ep->rbr);        //从红黑树中删除这个节点

	spin_lock_irqsave(&ep->lock, flags);
	if (ep_is_linked(&epi->rdllink))        //如果存在于已经准备就绪的链表中，从链表中删除这个epitem
		list_del_init(&epi->rdllink);
	spin_unlock_irqrestore(&ep->lock, flags);

	kmem_cache_free(epi_cache, epi);        /释放从slab中分配的空间

	return 0;
}
```
+ Order 1：static int ep_insert(struct eventpoll *ep, struct epoll_event *event, struct file *tfile, int fd)：向eventpoll（epollfd）中添加事件
```c
static int ep_insert(struct eventpoll *ep, struct epoll_event *event, struct file *tfile, int fd)
{
	int error, revents, pwake = 0;
	unsigned long flags;
	struct epitem *epi;
	struct ep_pqueue epq;

	error = -ENOMEM;
	if (!(epi = kmem_cache_alloc(epi_cache, GFP_KERNEL)))        //先从slab中分配一个epitem对象，用以描述fd
		goto error_return;

	ep_rb_initnode(&epi->rbn);        /将epitem添加到eventpoll中去
	INIT_LIST_HEAD(&epi->rdllink);
	INIT_LIST_HEAD(&epi->fllink);
	INIT_LIST_HEAD(&epi->pwqlist);
	epi->ep = ep;
	ep_set_ffd(&epi->ffd, tfile, fd);        //对epitem进行设置
	epi->event = *event;
	epi->nwait = 0;
	epi->next = EP_UNACTIVE_PTR;

	epq.epi = epi;
	init_poll_funcptr(&epq.pt, ep_ptable_queue_proc);     //初始化一个poll_table，并设置指定的回调函数，当有状态发生变化时，会通过ep_ptable_queue_proc进行通知。   

	revents = tfile->f_op->poll(tfile, &epq.pt);        //进行一次查询，返回目前的状态
	if (epi->nwait < 0)
		goto error_unregister;

	spin_lock(&tfile->f_ep_lock);
	list_add_tail(&epi->fllink, &tfile->f_ep_links);        //每个file会将自己监听的epitem连接起来
	spin_unlock(&tfile->f_ep_lock);

	ep_rbtree_insert(ep, epi);        //将这个epitem插入eventpoll管理的红黑树中

	spin_lock_irqsave(&ep->lock, flags);

	if ((revents & event->events) && !ep_is_linked(&epi->rdllink)) {        //如果当前有事发生，将epitem添加到发生事件的链表
		list_add_tail(&epi->rdllink, &ep->rdllist);        

		if (waitqueue_active(&ep->wq))        //当前谁在epoll_wait，就唤醒谁
			__wake_up_locked(&ep->wq, TASK_UNINTERRUPTIBLE | TASK_INTERRUPTIBLE);
		if (waitqueue_active(&ep->poll_wait))        //谁在epoll当前的epollfd也唤醒谁
			pwake++;
	}

	spin_unlock_irqrestore(&ep->lock, flags);

	if (pwake)
		ep_poll_safewake(&psw, &ep->poll_wait);

	DNPRINTK(3, (KERN_INFO "[%p] eventpoll: ep_insert(%p, %p, %d)\n",
		     current, ep, tfile, fd));

	return 0;

error_unregister:
	ep_unregister_pollwait(ep, epi);        //一系列的回滚操作

	spin_lock_irqsave(&ep->lock, flags);
	if (ep_is_linked(&epi->rdllink))
		list_del_init(&epi->rdllink);
	spin_unlock_irqrestore(&ep->lock, flags);

	kmem_cache_free(epi_cache, epi);
error_return:
	return error;
}
```
+ Order 2：static void ep_ptable_queue_proc(struct file *file, wait_queue_head_t *whead, poll_table *pt)：向每一个fd注册的回调函数
```c
static void ep_ptable_queue_proc(struct file *file, wait_queue_head_t *whead, poll_table *pt)
{
	struct epitem *epi = ep_item_from_epqueue(pt);        //获取当前发生事件的epitem
	struct eppoll_entry *pwq;

	if (epi->nwait >= 0 && (pwq = kmem_cache_alloc(pwq_cache, GFP_KERNEL))) {
		init_waitqueue_func_entry(&pwq->wait, ep_poll_callback);        //初始化等待队列，指定ep_poll_callback为回调函数，当fd状态改变时（队列头被唤醒），回调函数被调用
		pwq->whead = whead;        //有必要用一个队列？？？
		pwq->base = epi;
		add_wait_queue(whead, &pwq->wait);        //
		list_add_tail(&pwq->llink, &epi->pwqlist);
		epi->nwait++;        //在这个等待队列中等待的个数，最大是1
	} else {
		epi->nwait = -1;
	}
}
```
= Order 3：static int ep_poll_callback(wait_queue_t *wait, unsigned mode, int sync, void *key)：全局最关键的回调函数，key携带的是event
```c
static int ep_poll_callback(wait_queue_t *wait, unsigned mode, int sync, void *key)
{
	int pwake = 0;
	unsigned long flags;
	struct epitem *epi = ep_item_from_wait(wait);        //从等待队列中获得epitem
	struct eventpoll *ep = epi->ep;        //获得管理这个epitem的eventpoll

	spin_lock_irqsave(&ep->lock, flags);

	if (!(epi->event.events & ~EP_PRIVATE_BITS))        //如果没有事件发生，直接返回处理
		goto out_unlock;

//如果callback被调用的同时，epoll_wait已经返回了，此时已经在应用层处理events了，这种情况下使用一个链表将当前发生事件的epitem链接起来，在下一次调用epoll_wait时返回给用户
	if (unlikely(ep->ovflist != EP_UNACTIVE_PTR)) {        
		if (epi->next == EP_UNACTIVE_PTR) {
			epi->next = ep->ovflist;
			ep->ovflist = epi;
		}
		goto out_unlock;
	}

	if (ep_is_linked(&epi->rdllink))
		goto is_linked;

	list_add_tail(&epi->rdllink, &ep->rdllist);

//用来处理有事件发生，并且描述事件的epitem已经放入链表中
is_linked:
	if (waitqueue_active(&ep->wq))
		__wake_up_locked(&ep->wq, TASK_UNINTERRUPTIBLE |
				 TASK_INTERRUPTIBLE);
	if (waitqueue_active(&ep->poll_wait))
		pwake++;

//用来处理没有我们关心的状况，就是解锁，然后再次睡眠
out_unlock:
	spin_unlock_irqrestore(&ep->lock, flags);

	/* We have to call this outside the lock */
	if (pwake)
		ep_poll_safewake(&psw, &ep->poll_wait);

	return 1;
}
```
+ Order 0：asmlinkage long sys_epoll_wait(int epfd, struct epoll_event __user *events, int maxevents, int timeout)：使用epoll_wait对关注的epollfd进行处理
```c
asmlinkage long sys_epoll_wait(int epfd, struct epoll_event __user *events, int maxevents, int timeout)
{
	int error;
	struct file *file;
	struct eventpoll *ep;

	if (maxevents <= 0 || maxevents > EP_MAX_EVENTS)        //判断关注的事件数量是否超过限制
		return -EINVAL;

	if (!access_ok(VERIFY_WRITE, events, maxevents * sizeof(struct epoll_event))) {        //验证用户提供的数据是否正常
		error = -EFAULT;
		goto error_return;
	}

	error = -EBADF;
	file = fget(epfd);        //得到描述epollfd的file
	if (!file)
		goto error_return;

	error = -EINVAL;
	if (!is_file_epoll(file))        //判断这个file是否用来描述epollfd
		goto error_fput;

	ep = file->private_data;        //获得描述epollfd的eventpoll

	error = ep_poll(ep, events, maxevents, timeout);        //实际上进行的操作

error_fput:
	fput(file);        
error_return:

	return error;
}
```
+ Order 1：static int ep_poll(struct eventpoll *ep, struct epoll_event __user *events, int maxevents, long timeout)：实际上进行轮训的操作
```c
static int ep_poll(struct eventpoll *ep, struct epoll_event __user *events, int maxevents, long timeout)
{
	int res, eavail;
	unsigned long flags;
	long jtimeout;
	wait_queue_t wait;

	jtimeout = (timeout < 0 || timeout >= EP_MAX_MSTIMEO) ?
		MAX_SCHEDULE_TIMEOUT : (timeout * HZ + 999) / 1000;        //计算睡眠的时间

retry:
	spin_lock_irqsave(&ep->lock, flags);

	res = 0;
	if (list_empty(&ep->rdllist)) {        //如果当前没有任何事件发生，不能睡觉，要干活
		init_waitqueue_entry(&wait, current);        //初始化一个等待队列
		wait.flags |= WQ_FLAG_EXCLUSIVE;
		__add_wait_queue(&ep->wq, &wait);        //挂载在eventpoll的等待队列，需要的时候进行睡眠

		for (;;) {
			set_current_state(TASK_INTERRUPTIBLE);        //将这个进程设置为可以睡眠的状态，之后可以睡眠，但是还没有睡眠
			if (!list_empty(&ep->rdllist) || !jtimeout)        //如果返回给用户的队列不为空，可以直接打破循环
				break;
			if (signal_pending(current)) {        //如果有信号产生，也要返回
				res = -EINTR;
				break;
			}

			spin_unlock_irqrestore(&ep->lock, flags);
			jtimeout = schedule_timeout(jtimeout);        //没有事情发生，睡觉
			spin_lock_irqsave(&ep->lock, flags);
		}
		__remove_wait_queue(&ep->wq, &wait);        //从休眠队列中删除eventpoll

		set_current_state(TASK_RUNNING);        //将当前进程的状态设置为正常
	}

	eavail = !list_empty(&ep->rdllist);        

	spin_unlock_irqrestore(&ep->lock, flags);

	if (!res && eavail &&
	    !(res = ep_send_events(ep, events, maxevents)) && jtimeout)        //有事情发生，准备将发生的事件copy给用户
		goto retry;

	return res;
}
```
+ Order 2：static int ep_send_events(struct eventpoll *ep, struct epoll_event __user *events, int maxevents)：将发生的事件返回给用户
```c
static int ep_send_events(struct eventpoll *ep, struct epoll_event __user *events, int maxevents)
{
	int eventcnt, error = -EFAULT, pwake = 0;
	unsigned int revents;
	unsigned long flags;
	struct epitem *epi, *nepi;
	struct list_head txlist;

	INIT_LIST_HEAD(&txlist);

	mutex_lock(&ep->mtx);        //将eventpoll锁起来进行操作

	spin_lock_irqsave(&ep->lock, flags);
	list_splice(&ep->rdllist, &txlist);        //将readlist中已经发生的事件转移到txlist中暂存，此时没有rdlist为空
	INIT_LIST_HEAD(&ep->rdllist);
	ep->ovflist = NULL;        //清空链表（前面解释过），此时我们不希望有新的events加入到监听队列中来
	spin_unlock_irqrestore(&ep->lock, flags);

	for (eventcnt = 0; !list_empty(&txlist) && eventcnt < maxevents;) {        //挨个处理现在发生的事件
		epi = list_first_entry(&txlist, struct epitem, rdllink);

		list_del_init(&epi->rdllink);        //初始化要返回事件发生的链表

		revents = epi->ffd.file->f_op->poll(epi->ffd.file, NULL);        //获取发生的事件
		revents &= epi->event.events;        //判断发生的事件与我们期望的事件是否相同

		if (revents) {        //如果有相同的事件发生，交给用户空间，一次处理一个事件
			if (__put_user(revents,
				       &events[eventcnt].events) ||
			    __put_user(epi->event.data,
				       &events[eventcnt].data))
				goto errxit;
			if (epi->event.events & EPOLLONESHOT)        //如果设置了ONESHOT事件，会从eventpoll中删除epi关注的事件
				epi->event.events &= EP_PRIVATE_BITS;
			eventcnt++;        //发生事件的数量增加
		}
		
//ET和非ET就这一步之差
//如果是ET：epitem不会再 进入就绪链表，除非fd上的事件发生改变或者epoll_wait_callback再次被调用
//如果时LT：不管有没有事件发生都会直接加入rdlist，在下一次epoll_wait时立即返回。如果确实没有事情发生，epoll_wait会返回0，空转一次
		if (!(epi->event.events & EPOLLET) &&
		    (revents & epi->event.events))
			list_add_tail(&epi->rdllink, &ep->rdllist);        
	}
	error = 0;

errxit:

	spin_lock_irqsave(&ep->lock, flags);
	
	for (nepi = ep->ovflist; (epi = nepi) != NULL;
	     nepi = epi->next, epi->next = EP_UNACTIVE_PTR) {
		if (!ep_is_linked(&epi->rdllink) &&
		    (epi->event.events & ~EP_PRIVATE_BITS))
			list_add_tail(&epi->rdllink, &ep->rdllist);        //将在ovslist中缓存的已经有事件发生的epitem加入就绪链表，下一次返回
	}
	
	ep->ovflist = EP_UNACTIVE_PTR;
	list_splice(&txlist, &ep->rdllist);        //将设置了LT模式的epitem交给txlist

	if (!list_empty(&ep->rdllist)) {        //如果rdlist为空的话，唤醒线程起来干活了
		if (waitqueue_active(&ep->wq))
			__wake_up_locked(&ep->wq, TASK_UNINTERRUPTIBLE |
					 TASK_INTERRUPTIBLE);
		if (waitqueue_active(&ep->poll_wait))
			pwake++;
	}
	spin_unlock_irqrestore(&ep->lock, flags);

	mutex_unlock(&ep->mtx);

	if (pwake)
		ep_poll_safewake(&psw, &ep->poll_wait);

	return eventcnt == 0 ? error: eventcnt;        //返回发生事件的描述符的个数
}
```
