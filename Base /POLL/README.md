
## Poll
### 函数接口
```c
#include<poll.h>
int poll(struct pollfd* fds, nfds_t nfds, int timeout);
```
```c
struct pollfd{
    int fd;
    short events;        //注册的事件
    short revents;        //实际发生的事件
};
```
### 基本操作

    注意事项
        fds是描述符数组，nfds是监听描述符的个数，timeout是以毫秒为单位的超时值
        返回事件发生的描述符的总数。
        使用结束后，用户对刚刚传入的描述符数组进行轮询测试，看看那个pollfd上的事件已经改变了。
        POLLHUP和POLLHUP不管有没有设置都是默认监听的。
    事件类型
        POLLIN：数据可读
        POLLRDNORM：普通数据可读
        POLLRDBAND：优先数据可读（Linux不支持）
        POLLPRI：高级有限数据可读（带外数据）
        POLLOUT：数据可写
        POLLWRNORM：普通数据可写
        POLLWRBAND：优先数据可写
        POLLRDHUP：TCP连接被对方关闭，或者对方关闭了写操作
        POLLERR：错误，要使用getsocketopt进行清除
        POLLHUP：挂起，比如该描述符上的写端被关闭，该端的描述符将收到该事件
        POLLNVAL：文件描述符没有打开
    错误标志：
        EBADF：一个结构体或者多个结构体中存在文件描述符
        EFAULT：fds指向的地址超出了进程地址空间
        EINTR：在请求事件发生时收到一个信号，可以重新发起调用
        EINVAL：nfds参数超出了进程的软限制
        ENOMEM：可用内存不足，无法完成请求

### 性能分析

    时间角度：
        使用了两层for循环：其一是管理时间的循环，可以在在有事件发生或者定时器到期的条件下退出循环
        在循环之前会将进程的状态设置为TASK_INTERRUPTIBLE：进程会等待定时事件的发生，之后退出时正常
    空间角度：
        从用户的地址空间中将struct pollfd逐个拷贝进内核空间，构造成链表，然后进行轮训。有一个高级技术，使用链表混合使用堆与栈的空间。不过逐个将struct pollfd拷贝进内核地址空间，可能有点效率影响。
        内核为每一个关注的事件都会存在一个struct pollfd，与同一个fd存储在一个链表中，也就是说，链表的总长度= 文件描述符个数 × 关注的事件个数。将发生的事件从内核空间拷贝出来的时候也是逐个与拷贝。

源码分析

基础数据结构：
```c
struct poll_list {
	struct poll_list *next;
	int len;
	struct pollfd entries[0];
};
```
通用函数：
```c
#define N_STACK_PPS ((sizeof(stack_pps) - sizeof(struct poll_list))  / sizeof(struct pollfd))
#define min_t(type,x,y)  ({ type __x = (x); type __y = (y); __x < __y ? __x: __y; })
#define max_t(type,x,y)  ({ type __x = (x); type __y = (y); __x > __y ? __x: __y; })
```
+ Order 0：asmlinkage long sys_poll(struct pollfd __user *ufds, unsigned int nfds,long timeout_msecs)：接受用户空间传入的指针，经过拷贝后在内核空间进行操作
```c
asmlinkage long sys_poll(struct pollfd __user *ufds, unsigned int nfds,long timeout_msecs)
{
	s64 timeout_jiffies;        //获得毫秒级的准确度
	int ret;

	if (timeout_msecs > 0) {        //根据传入的参数对数据进行处理
#if HZ > 1000
		/* We can only overflow if HZ > 1000 */
		if (timeout_msecs / 1000 > (s64)0x7fffffffffffffffULL / (s64)HZ)
			timeout_jiffies = -1;
		else
#endif
			timeout_jiffies = msecs_to_jiffies(timeout_msecs);
	} else {
		/* Infinite (< 0) or no (0) timeout */
		timeout_jiffies = timeout_msecs;
	}

	ret = do_sys_poll(ufds, nfds, &timeout_jiffies);        //做实际的操作
	if (ret == -EINTR) {        //如果被中断（也就是现实的告诉用户可以重复的进行调用），进行善后处理
		struct restart_block *restart_block;
		restart_block = &current_thread_info()->restart_block;
		restart_block->fn = do_restart_poll;
		restart_block->arg0 = (unsigned long)ufds;
		restart_block->arg1 = nfds;
		restart_block->arg2 = timeout_jiffies & 0xFFFFFFFF;
		restart_block->arg3 = (u64)timeout_jiffies >> 32;
		ret = -ERESTART_RESTARTBLOCK;
	}
	return ret;        //返回有事件发生的描述符的个数
}
```
+ Order 1：int do_sys_poll(struct pollfd __user *ufds, unsigned int nfds, s64 *timeout)
```c
int do_sys_poll(struct pollfd __user *ufds, unsigned int nfds, s64 *timeout)
{
	struct poll_wqueues table;            //获得poll等待队列的操作
 	int err = -EFAULT, fdcount, len, size;        //在还没有将用户空间的数据拷贝进来，将标志设置为默认出错
	long stack_pps[POLL_STACK_ALLOC/sizeof(long)];        //在栈上分配一点空间，POLL_STACK_ALLOC = 256
	struct poll_list *const head = (struct poll_list *)stack_pps;        //使用链表将描述pollfd的数据结构
 	struct poll_list *walk = head;
 	unsigned long todo = nfds;

	if (nfds > current->signal->rlim[RLIMIT_NOFILE].rlim_cur)        //如果注册的描述符大小超过当前的进程的软资源限制，直接返回错误标志
		return -EINVAL;

	len = min_t(unsigned int, nfds, N_STACK_PPS);        //获取用户传入struct pollfd的个数
	for (;;) {        //nb，挨个处理pollfd，混合栈空间和堆空间。。被秀了一脸。。
		walk->next = NULL;
		walk->len = len;
		if (!len)        //如果用户传入的len有0个，退出循环
			break;

		if (copy_from_user(walk->entries, ufds + nfds-todo,
					sizeof(struct pollfd) * walk->len))        //一次从user空间中拷贝一个struct pollfd
			goto out_fds;

		todo -= walk->len;
		if (!todo)        //如果拷贝完成了，进行处理
			break;

		len = min(todo, POLLFD_PER_PAGE);        //牛逼，竟然把栈空间和堆空间混合起来用了
		size = sizeof(struct poll_list) + sizeof(struct pollfd) * len;
		walk = walk->next = kmalloc(size, GFP_KERNEL);
		if (!walk) {
			err = -ENOMEM;
			goto out_fds;
		}
	}

	poll_initwait(&table);        //初始化等待链表
	fdcount = do_poll(nfds, head, &table, timeout);        //获得发生事件的描述符个数
	poll_freewait(&table);        //释放等待链表

	for (walk = head; walk; walk = walk->next) {        //轮训所有描述符的链表
		struct pollfd *fds = walk->entries;        //获得fds数组
		int j;

		for (j = 0; j < walk->len; j++, ufds++)        //每个描述符上观察的事件拷贝到用户空间，逐个字节拷贝
			if (__put_user(fds[j].revents, &ufds->revents))
				goto out_fds;
  	}

	err = fdcount;
out_fds:
	walk = head->next;        //对之前分配的堆空间进行处理
	while (walk) {
		struct poll_list *pos = walk;
		walk = walk->next;
		kfree(pos);
	}

	return err;
}
```
+ Order 2：static int do_poll(unsigned int nfds, struct poll_list *list,struct poll_wqueues *wait, s64 *timeout)：在链表组成的pollfd上进行操作
```c
static int do_poll(unsigned int nfds,  struct poll_list *list,struct poll_wqueues *wait, s64 *timeout)
{
	int count = 0;
	poll_table* pt = &wait->pt;

	if (!(*timeout))        //如果时间为0，也就是说只轮训一遍不管有没有事件发生都直接返回
		pt = NULL;

	for (;;) {        
		struct poll_list *walk;
		long __timeout;

		set_current_state(TASK_INTERRUPTIBLE);        //设置当前进程的状态
		for (walk = list; walk != NULL; walk = walk->next) {        //循环pollfd所在的链表
			struct pollfd * pfd, * pfd_end;

			pfd = walk->entries;
			pfd_end = pfd + walk->len;        //获得pollfd的描述
			for (; pfd != pfd_end; pfd++) {
				if (do_pollfd(pfd, pt)) {        //调用文件系统例程，对pfd进行处理
					count++;
					pt = NULL;
				}
			}
		}

		pt = NULL;
		if (!count) {        //如果没有事件发生
			count = wait->error;
			if (signal_pending(current))
				count = -EINTR;        //将设置为可以重复调用的状态
		}
		if (count || !*timeout)        //如果有事件发生或者事件用尽，打破循环，直接返回
			break;

		if (*timeout < 0) {        //和select一样对设置时间的处理
			__timeout = MAX_SCHEDULE_TIMEOUT;
		} else if (unlikely(*timeout >= (s64)MAX_SCHEDULE_TIMEOUT-1)) 
			__timeout = MAX_SCHEDULE_TIMEOUT - 1;
			*timeout -= __timeout;
		} else {
			__timeout = *timeout;
			*timeout = 0;
		}

		__timeout = schedule_timeout(__timeout);
		if (*timeout >= 0)
			*timeout += __timeout;
	}
	__set_current_state(TASK_RUNNING);        //将进程设置为正常的状态
	return count;        //返回准备就绪的文件描述符的个数
}
```
+ Order 3：static inline unsigned int do_pollfd(struct pollfd *pollfd, poll_table *pwait)：调用文件系统例程对pollfd进行处理
```
static inline unsigned int do_pollfd(struct pollfd *pollfd, poll_table *pwait)
{
	unsigned int mask;
	int fd;

	mask = 0;
	fd = pollfd->fd;
	if (fd >= 0) {
		int fput_needed;
		struct file * file;

		file = fget_light(fd, &fput_needed);
		mask = POLLNVAL;
		if (file != NULL) {
			mask = DEFAULT_POLLMASK;
			if (file->f_op && file->f_op->poll)
				mask = file->f_op->poll(file, pwait);        //文件系统例程
			mask &= pollfd->events | POLLERR | POLLHUP;
			fput_light(file, fput_needed);
		}
	}
	pollfd->revents = mask;

	return mask;
}
```
