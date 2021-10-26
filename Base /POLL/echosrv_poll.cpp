#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/wait.h>
#include <poll.h>

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include <vector>
#include <iostream>

#define ERR_EXIT(m) \
        do \
        { \
                perror(m); \
                exit(EXIT_FAILURE); \
        } while(0)

/*
struct pollfd{
	int fd;
	short events; //requested events
	short revents; //returned events
}
*/ // 每个结构体的 events 域是由用户来设置，告诉内核我们关注的是什么，而 revents 域是返回时内核设置的，
//以说明该描述符发生了什么事件
typedef std::vector<struct pollfd> PollFdList;

int main(void)
{
	//在reader中止之后写Pipe的时候发送
	signal(SIGPIPE, SIG_IGN);  //首先忽略一个pipe信号，
	//如果客户端关闭套接字close 而服务端又调用了一次write，服务器会接收到一个RST segment(TCP传输层)， 
	//如果服务端再次调用write，这个时候就会产生SIGPIPE信号
	//如果没有忽略这个信号，默认处理方式就是退出进程
	
	
	
	signal(SIGCHLD, SIG_IGN); //进程Terminate或Stop的时候，SIGCHLD会发送给它的父进程。缺省情况下该Signal会被忽略

	int idlefd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	int listenfd;

	//if ((listenfd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
	if ((listenfd = socket(PF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP)) < 0)//非阻塞、
		ERR_EXIT("socket");//使用 socket() 函数创建套接字以后，返回值就是一个 int 类型的文件描述符

	struct sockaddr_in servaddr;
	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(5188);
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY);

	int on = 1;
	if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)//设置地址的重复利用
		ERR_EXIT("setsockopt");  

	if (bind(listenfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0)//绑定和监听
		ERR_EXIT("bind");
	if (listen(listenfd, SOMAXCONN) < 0)
		ERR_EXIT("listen");  

	struct pollfd pfd;
	pfd.fd = listenfd;
	pfd.events = POLLIN;  //关注POLLIN事件   POLLIN There is data to read. POLLIN给了events

	PollFdList pollfds; // typedef std::vector<struct pollfd> PollFdList;
	pollfds.push_back(pfd);

	int nready;

	struct sockaddr_in peeraddr;
	socklen_t peerlen;
	int connfd;

	while (1)
	{
		nready = poll(&*pollfds.begin(), pollfds.size(), -1); // &*pollfds.begin() pollfds数组的首地址，等价于pollfds.data()，-1表示不设定超时
		//从用户的地址空间中将struct pollfd逐个拷贝进内核空间，构造成链表，然后进行轮训。有一个高级技术，使用链表混合使用堆与栈的空间。不过逐个将struct pollfd拷贝进内核地址空间，可能有点效率影响。将发生的事件从内核空间拷贝出来的时候也是逐个与拷贝。
		// 返回结果就是就绪的文件描述符的个数
		//每次调用poll都需要把把监听套接字与已连接套接字的事件数组拷贝到内核
		if (nready == -1)
		{
			if (errno == EINTR)
				continue;
			
			ERR_EXIT("poll");
		}
		if (nready == 0)	// nothing happended
			continue;
		
		if (pollfds[0].revents & POLLIN) //第一个就是监听套接字
		{
			peerlen = sizeof(peeraddr);
			connfd = accept4(listenfd, (struct sockaddr*)&peeraddr,
						&peerlen, SOCK_NONBLOCK | SOCK_CLOEXEC);  //如果有POLLIN，调用accept返回一个连接套接字

			// if (connfd == -1)
			// 	ERR_EXIT("accept4");


			if (connfd == -1)
			{
				if (errno == EMFILE)
				{
					close(idlefd);
					idlefd = accept(listenfd, NULL, NULL);
					close(idlefd);
					idlefd = open("/dev/null", O_RDONLY | O_CLOEXEC);
					continue;
				}
				else
					ERR_EXIT("accept4"); //如果是其他错误
			}  
			
			//EMFILE 进程打开的文件描述符超出了上限
			//处理方法：1、调高进程文件描述符的数目，治标不治本
			//2、死等
			//3、退出程序
			//4、关闭监听套接字。那什么时候重新打开呢？
	
			//5、如果是epoll模型，可以改用edge trigger。问题是如果漏掉一次accept(2)，程序再也不会收到新连接
			//处理套接字被用完的情况。先占用一个套接字，然后有客户端到来的时候，把这个套接字给他，然后立马关闭它(优雅的断开连接)，并又占用
			//这个套接字，这样新客户端的连接总会不成功。存在的意义是什么？

			pfd.fd = connfd;
			pfd.events = POLLIN;
			pfd.revents = 0; //先加入监听，没有任何事件返回，置为0
			pollfds.push_back(pfd); //pollfds数组，第一个元素是监听套接字，后面的是连接套接字
			--nready; //?

			std::cout<<"ip="<<inet_ntoa(peeraddr.sin_addr)<<
				" port="<<ntohs(peeraddr.sin_port)<<std::endl;
			if (nready == 0)
				continue; //不再执行下面的for循环
		}//上面处理pollfds[0]的可读事件，并把新连接套接字加入pollfds
		//以备后面遍历已连接套接字的可读事件

		//std::cout<<pollfds.size()<<std::endl;
		//std::cout<<nready<<std::endl;
		for (PollFdList::iterator it=pollfds.begin()+1; //第一个套接字总是监听套接字，从begin()+1开始是已连接套接字
			it != pollfds.end() && nready >0; ++it) //遍历已连接套接字
		{
				if (it->revents & POLLIN) //当连接套接字revent不为0并且有数据到来的时候，处理读和写
				{
					--nready;
					connfd = it->fd;//把已连接套接字取出然后读取
					char buf[1024] = {0};
					int ret = read(connfd, buf, 1024);
					if (ret == -1)
						ERR_EXIT("read");
					if (ret == 0)  //说明对方关闭了套接字
					{
						std::cout<<"client close"<<std::endl;
						it = pollfds.erase(it); //那就把它移除
						--it; //防止有一个没被遍历到，erase的原因 使用vector删除的时候会自动移动，保证整个数组是连续的，如果使用普通数组的话，删除了元素之后，想要保证连续，还需要自己操作

						close(connfd);
						continue;
					}

					std::cout<<buf;
					write(connfd, buf, strlen(buf));//读到的内容ret>0的话就回射回去
					
				}
		}
	}

	return 0;
}



//另外的，对于cmake，暂且理解为cmake之后生成makefile，之后再make
//此时生成的makefile很标准