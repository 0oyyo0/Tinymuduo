

条件触发（LT）：当条件满足时发生一个IO事件
边缘触发（ET）：当状态改变时发生一个IO事件，之后如果不处理，则状态一直维持原样
## Select
### 函数接口
```c
#include<sys/select.h>
int select( int n,fd_set* readfds,fd_set* writefds,fd_set* exceptfds,struct timeval* timeout)
FD_CLR(int fd,fd_set* set);
FD_ISSET(int fd,fd_set* set);
FD_SET(int fd,fd_set* set);
FD_ZERO(fd* fd_set);
```
### 基本操作

    注意事项：
        参数n描述的是最大的文件描述符+1
        如果struct timeval* timeout指针的为0，select立即返回；如果为NULL，则select永久的阻塞，直到某个文件描述符准备就绪；如果由定时时间，则阻塞固定的时间
        每次调用select之前都要重新设置文件描述符数组。
        返回的val描述事件发生的次数。假设同时监听一个描述符的三个事件，如果事件发生的话，会返回3。
    错误标志：
        EBADF：某个集合中存在非法的文件描述符
        EINTR：等待时捕获了一个信号，可以重新发起调用
        EINVAL：参数n是负数或者设置的超时时间非非法
        ENOMEM：没有足够的内存完成请求
    触发读事件：
        socket内核接受缓冲区的字节数大于SO_RCVLOWAT，一般为1。（可以使用非阻塞IO进行处理）
        socket通信的对方关闭连接
        监听socket上有新的连接请求
        socket上有未处理的错误，可以使用getsocketopt进行清除。
    触发写事件：
        socket内核发送缓冲区的字节数大于SO_SNDLOWAT，一般为1。（可以使用非阻塞IO进行处理）
        socket的写操作被关闭。对写操作被管理的socket执行写操作将会触发一个SIGPIPE的信号
        socket上使用非阻塞的connect连接成功或者失败
        socket上由未处理的错误，可以使用getsocketopt进行清除。
    触发异常事件：
        带外数据到达

性能分析：

    时间角度
        在核心代码中存在三个循环：一是对时间的处理，根据是否发生事件可以随时退出循环；二是对当前进程的最大描述符数组的循环处理；三是对每个描述符中关注事件的处理
        在循环之前会将进程的状态设置为TASK_INTERRUPTIBLE：进程会等待定时事件的发生，之后退出时正常
    空间角度
        在每次使用的时候都会将用户空间的fd_set拷贝到内核空间
        对于传入的参数n，也就是最大的文件描述符，感觉很鸡肋，在函数中会使用加锁获得当前文件的最大描述符，然后根据这个获得的最大描述符处理事件
        在处理事件时会在内核中会分配long*max_fds*6的数据空间（USER/KERNEL）×（in/out/exp）

### 源码分析

基本数据结构描述：
```c
#undef __NFDBITS
#define __NFDBITS	(8 * sizeof(unsigned long))        //结果为32/64

#undef __FD_SETSIZE
#define __FD_SETSIZE	1024

#undef __FDSET_LONGS
#define __FDSET_LONGS	(__FD_SETSIZE/__NFDBITS)        //结果为32/16

#undef __FDELT
#define	__FDELT(d)	((d) / __NFDBITS)

#undef __FDMASK
#define	__FDMASK(d)	(1UL << ((d) % __NFDBITS))

typedef struct {
	unsigned long fds_bits [__FDSET_LONGS];        //描述的位数为：1024
} __kernel_fd_set;

typedef __kernel_fd_set		fd_set;        //一个fd_set中有1024位

typedef struct {        
	unsigned long *in, *out, *ex;
	unsigned long *res_in, *res_out, *res_ex;
} fd_set_bits;        //用于在一个总的空间中描述用户输入的三个fd_set的开始位置，与内核返回的三个fd_set的起始位置

struct poll_wqueues {
	poll_table pt;
	struct poll_table_page * table;
	int error;
	int inline_index;
	struct poll_table_entry inline_entries[N_INLINE_POLL_ENTRIES];
};

typedef struct poll_table_struct {
	poll_queue_proc qproc;
} poll_table;

struct poll_table_entry {
	struct file * filp;
	wait_queue_t wait;
	wait_queue_head_t * wait_address;
};
```
辅助函数集合
```c
static inline int get_fd_set(unsigned long nr, void __user *ufdset, unsigned long *fdset)
{
	nr = FDS_BYTES(nr);
	if (ufdset)
		return copy_from_user(fdset, ufdset, nr) ? -EFAULT : 0;

	memset(fdset, 0, nr);
	return 0;
}

static inline unsigned long __must_check set_fd_set(unsigned long nr, void __user *ufdset, unsigned long *fdset)
{
	if (ufdset)
		return __copy_to_user(ufdset, fdset, FDS_BYTES(nr));
	return 0;
}

static inline void zero_fd_set(unsigned long nr, unsigned long *fdset)
{
	memset(fdset, 0, FDS_BYTES(nr));
}

#define FDS_BITPERLONG	(8*sizeof(long))
#define FDS_LONGS(nr)	(((nr)+FDS_BITPERLONG-1)/FDS_BITPERLONG)
#define FDS_BYTES(nr)	(FDS_LONGS(nr)*sizeof(long))        //特定于体系结构的处理，返回nr * long的个数
```
+ Order 0：asmlinkage long sys_select(int n, fd_set __user *inp, fd_set __user *outp,fd_set __user *exp, struct timeval __user *tvp)：接受从用户空间传入的参数，然后在内核中进行操作
```c
asmlinkage long sys_select(int n, fd_set __user *inp, fd_set __user *outp,fd_set __user *exp, struct timeval __user *tvp)
{
	s64 timeout = -1;        //默认时阻塞类型的
	struct timeval tv;
	int ret;

	if (tvp) {        //如果时间指针存在数据的话，将时间拷贝到内核空间并进行处理
		if (copy_from_user(&tv, tvp, sizeof(tv)))        //将用户空间的数据拷贝到内核空间
			return -EFAULT;

		if (tv.tv_sec < 0 || tv.tv_usec < 0)        //如果用户提供的时间有错误，直接返回
			return -EINVAL;

		if ((u64)tv.tv_sec >= (u64)MAX_INT64_SECONDS)        //对时间进行处理
			timeout = -1;	
		else {
			timeout = DIV_ROUND_UP(tv.tv_usec, USEC_PER_SEC/HZ);
			timeout += tv.tv_sec * HZ;
		}
	}
yon
	ret = core_sys_select(n, inp, outp, exp, &timeout);        //处理核心

	if (tvp) {        //如果用户设置了定时的话，需要根据这次select使用的时间，将没有用完的定时时间做返回处理
		struct timeval rtv;

		if (current->personality & STICKY_TIMEOUTS)
			goto sticky;
		rtv.tv_usec = jiffies_to_usecs(do_div((*(u64*)&timeout), HZ));
		rtv.tv_sec = timeout;
		if (timeval_compare(&rtv, &tv) >= 0)
			rtv = tv;
		if (copy_to_user(tvp, &rtv, sizeof(rtv))) {        //将没有用完的时间拷贝给用户
sticky:
			if (ret == -ERESTARTNOHAND)
				ret = -EINTR;
		}
	}

	return ret;        //返回事件发生的次数
}
```
+ Order 1：static int core_sys_select(int n, fd_set __user *inp, fd_set __user *outp,fd_set __user *exp, s64 *timeout)：对用户空间传入的fd_set进行处理（拷贝到内核）
```c
static int core_sys_select(int n, fd_set __user *inp, fd_set __user *outp,fd_set __user *exp, s64 *timeout)
{
	fd_set_bits fds;
	void *bits;
	int ret, max_fds;
	unsigned int size;
	struct fdtable *fdt;
	long stack_fds[SELECT_STACK_ALLOC/sizeof(long)];        //分配一些栈空间，方便快速的进行处理

	ret = -EINVAL;
	if (n < 0)        //如果输入的最大描述符小于0，直接做出错处理
		goto out_nofds;        //这块是直接返回ret

	rcu_read_lock();        //因为进程中的max_fds一致处于增加的状态，所以使用锁来获得当前的状态，避免竞争
	fdt = files_fdtable(current->files);
	max_fds = fdt->max_fds;        //获取当前进程的最大描述符
	rcu_read_unlock();
	if (n > max_fds)        //我去，这个n传入与不传入没啥区别啊....
		n = max_fds;

       //针对用户传入的fd_set与返回的fd_set，我们需要使用6个位图来描述in/out/ex，在必要的时候我们需要使用kmalloc获取一些内存 	 
	size = FDS_BYTES(n);        //每个返回n×long的字节数，即在总体的数组中，每个fd_set使用long类型来描述
	bits = stack_fds;
	if (size > sizeof(stack_fds) / 6) {        //没有足够的栈空间用来描述位图，需要从内核中分配一些。此处因为需要6个描述符数组，所以使用1/6来判断总的描述符数组是否足够
		ret = -ENOMEM;
		bits = kmalloc(6 * size, GFP_KERNEL);        //此时，bits指向了所有的描述符数组的内存区域
		if (!bits)
			goto out_nofds;
	}
	fds.in      = bits;        //使用fds的位置描述方式来描述在一个长的数组中的各个fd_set的信息。此时，为每一个select关注的fd分配了一个long类型的数据来描述状态。fd_set数据类型还没用过
	fds.out     = bits +   size;
	fds.ex      = bits + 2*size;
	fds.res_in  = bits + 3*size;
	fds.res_out = bits + 4*size;
	fds.res_ex  = bits + 5*size;

	if ((ret = get_fd_set(n, inp, fds.in)) ||
	    (ret = get_fd_set(n, outp, fds.out)) ||
	    (ret = get_fd_set(n, exp, fds.ex)))        //逐个将用户空间传入的数组通过拷贝到总的描述数组中固定的位置去。如果用户没有提供某一个数组，做清零处理
		goto out;
	zero_fd_set(n, fds.res_in);        //然后将总的描述符数组中返回的位置清零
	zero_fd_set(n, fds.res_out);
	zero_fd_set(n, fds.res_ex);

	ret = do_select(n, &fds, timeout);        //得到事件发生的总次数

	if (ret < 0)        //如果出现了错误
		goto out;
	if (!ret) {        //如果没有事件发生
		ret = -ERESTARTNOHAND;
		if (signal_pending(current))
			goto out;
		ret = 0;
	}

	if (set_fd_set(n, inp, fds.res_in) ||
	    set_fd_set(n, outp, fds.res_out) ||
	    set_fd_set(n, exp, fds.res_ex))        //将内核空间中的数据拷贝到用户空间
		ret = -EFAULT;

out:
	if (bits != stack_fds)        //释放分配的总数组空间，然后返回
		kfree(bits);
out_nofds:        //返回错误
	return ret;
}
```
+ Order 2：int do_select(int n, fd_set_bits *fds, s64 *timeout)
```c
int do_select(int n, fd_set_bits *fds, s64 *timeout)
{
	struct poll_wqueues table;        //一个轮询的等待队列
	poll_table *wait;
	int retval, i;

	rcu_read_lock();
	retval = max_select_fd(n, fds);        //获得关注的最大描述符的个数，并将fd_set_bits中关注的bits置位
	rcu_read_unlock();

	if (retval < 0)        
		return retval;
	n = retval;        //获取还珠的描述符个数

	poll_initwait(&table);        //初始化轮询对流而
	wait = &table.pt;        //将队列中的wait赋值给局部的变量进行操作
	if (!*timeout)        //如果timeout不存在，将wait初始化为NULL
		wait = NULL;
	retval = 0;
	for (;;) {
		unsigned long *rinp, *routp, *rexp, *inp, *outp, *exp;
		long __timeout;

		set_current_state(TASK_INTERRUPTIBLE);        //状态意义：针对等待某时间发生才会继续执行

		inp = fds->in; outp = fds->out; exp = fds->ex;        //为局部变量们赋值，在总体的数组中描述
		rinp = fds->res_in; routp = fds->res_out; rexp = fds->res_ex;

		for (i = 0; i < n; ++rinp, ++routp, ++rexp) {        //对所有的进程描述符进行轮询哇...
			unsigned long in, out, ex, all_bits, bit = 1, mask, j;
			unsigned long res_in = 0, res_out = 0, res_ex = 0;
			const struct file_operations *f_op = NULL;        //调用VFS中的例程
			struct file *file = NULL;

			in = *inp++; out = *outp++; ex = *exp++;        //获得实际描述某一个fd的long类型的数据
			all_bits = in | out | ex;        //表示这三个表示的状态不同意，没有监听状态
			if (all_bits == 0) {
				i += __NFDBITS;
				continue;
			}        

			for (j = 0; j < __NFDBITS; ++j, ++i, bit <<= 1) {        //有监听的标志
				int fput_needed;
				if (i >= n)
					break;
				if (!(bit & all_bits))
					continue;
				file = fget_light(i, &fput_needed);
				if (file) {
					f_op = file->f_op;
					mask = DEFAULT_POLLMASK;
					if (f_op && f_op->poll)
						mask = (*f_op->poll)(file, retval ? NULL : wait);        //调用虚拟文件系统中的例程，根据体系相关的代码对数据进行设置
					fput_light(file, fput_needed);
					if ((mask & POLLIN_SET) && (in & bit)) {
						res_in |= bit;
						retval++;
					}
					if ((mask & POLLOUT_SET) && (out & bit)) {
						res_out |= bit;
						retval++;        //对状态改变的描述符进行累加。如果，一个描述符同时关注了三个事件，且三个事件都发生了，那么会累加三次。
					}
					if ((mask & POLLEX_SET) && (ex & bit)) {
						res_ex |= bit;
						retval++;
					}
				}
				cond_resched();
			}        //第一个for循环结束，在一个文件描述符上的事件测试完毕
			if (res_in)        //对返回的结果进行设置
				*rinp = res_in;
			if (res_out)
				*routp = res_out;
			if (res_ex)
				*rexp = res_ex;
		}        //第二个for循环结束，完成对所有的监视的文件描述符的设置
		wait = NULL;
		if (retval || !*timeout || signal_pending(current))        //如果没有设置超时时间的话，就可以直接返回了
			break;
		if(table.error) {
			retval = table.error;
			break;
		}

    		if (*timeout < 0) {        //如果timeout=-1的话，就是说需要阻塞到有状态改变才行。前面已经进行过判断了，如果有状态改变的话，直接就可以进行break
			__timeout = MAX_SCHEDULE_TIMEOUT;        
		} else if (unlikely(*timeout >= (s64)MAX_SCHEDULE_TIMEOUT - 1)) {        //对超时时间进行处理，减去已经运行的事件。粒度有点粗哈
			__timeout = MAX_SCHEDULE_TIMEOUT - 1;
			*timeout -= __timeout;
		} else {
			__timeout = *timeout;        //这块也存在事件 发生，但是定时没有用完的可能性，直接对剩余的时间进行处理返回给用户
			*timeout = 0;
		}
		__timeout = schedule_timeout(__timeout);        //在这块进行等待，然后在进行轮训
		if (*timeout >= 0)
			*timeout += __timeout;
	}        //这个循环是针对超时时间的设置，根据timeout的不同的设置决定什么时候返回
	__set_current_state(TASK_RUNNING);        //将进程的状态设置我正常

	poll_freewait(&table);    //释放等待列表

	return retval;
}
```
+ Order 3：static int max_select_fd(unsigned long n, fd_set_bits *fds)：传入的参数一个是当前进程使用的最大的描述符，和fd_set的总的数组。获取最大的描述符的个数，并将关注的fd_set_bits中的bit置位
```c
static int max_select_fd(unsigned long n, fd_set_bits *fds)
{
	unsigned long *open_fds;
	unsigned long set;
	int max;
	struct fdtable *fdt;

	/* handle last in-complete long-word first */
	set = ~(~0UL << (n & (__NFDBITS-1)));
	n /= __NFDBITS;
	fdt = files_fdtable(current->files);
	open_fds = fdt->open_fds->fds_bits+n;        //获取当前进程打开的文件描述符的个数。
	max = 0;
	if (set) {
		set &= BITS(fds, n);
		if (set) {
			if (!(set & ~*open_fds))
				goto get_max;
			return -EBADF;
		}
	}
	while (n) {
		open_fds--;
		n--;
		set = BITS(fds, n);
		if (!set)
			continue;
		if (set & ~*open_fds)
			return -EBADF;
		if (max)
			continue;
get_max:
		do {
			max++;
			set >>= 1;
		} while (set);
		max += n * __NFDBITS;
	}

	return max;
}
```
