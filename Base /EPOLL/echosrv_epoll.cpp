#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/epoll.h>

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include <vector>
#include <algorithm>
#include <iostream>

// struct epoll_event{
// 	uint32_t events;
// 	epoll_data_t data;
// };
typedef std::vector<struct epoll_event> EventList;

#define ERR_EXIT(m) \
        do \
        { \
                perror(m); \
                exit(EXIT_FAILURE); \
        } while(0)

int main(void)
{
	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);

	int idlefd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	int listenfd;
	//if ((listenfd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
	if ((listenfd = socket(PF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP)) < 0)
		ERR_EXIT("socket");

	struct sockaddr_in servaddr;
	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(5188);
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY);

	int on = 1;
	if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
		ERR_EXIT("setsockopt");

	if (bind(listenfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0)
		ERR_EXIT("bind");
	if (listen(listenfd, SOMAXCONN) < 0)
		ERR_EXIT("listen");


// struct epoll_event{
// 	uint32_t events;
// 	epoll_data_t data;
// };


// typedef union epoll_data{
// 	void *ptr;
// 	int fd;
// 	uint32_t u32;
// 	uint64_t u64;
// }epoll_data_t;
//联合体。利用union可以用相同的存储空间存储不同型别的数据类型，从而节省内存空间。当访问其内成员时可用"."和"->"来直接访问
//1)联合体是一个结构；
// 2)它的所有成员相对于基地址的偏移量都为0；
// 3)此结构空间要大到足够容纳最"宽"的成员；
// 4)其对齐方式要适合其中所有的成员；
// union U1
// {
// 	int n;
// 	char s[11];
// 	double d;
// };
//U1的大小为16。虽然4、11、8的最大值是11，但是11既不能被4整除
//也不能被8整除。补充到16

	std::vector<int> clients;
	int epollfd;
	epollfd = epoll_create1(EPOLL_CLOEXEC); // 初步理解，如果执行成功，返回一个非负数(实际为文件描述符)，此epoll对象的文件描述符，用于标记此epoll对象

	struct epoll_event event;
	event.data.fd = listenfd;
	event.events = EPOLLIN/* | EPOLLET*/; 
	epoll_ctl(epollfd, EPOLL_CTL_ADD, listenfd, &event); //把所关注的事件添加给他进行管理，这里先加入监听套接字，下面加入已连接套接字
	//epollfd是epoll_create1创建出来的
	//这个系统调用能够控制给定的文件描述符epollfd指向的epoll实例
	EventList events(16); //定义事件的列表，初始状态16个
	struct sockaddr_in peeraddr;
	socklen_t peerlen;
	int connfd;

	int nready;
	while (1)
	{
		nready = epoll_wait(epollfd, &*events.begin(), static_cast<int>(events.size()), -1);
		//第一个参数：所创建的epollfd，第二个参数：返回的事件都放在events里面，是一个输出参数(poll的这里是一个输入输出参数，每次需要一次拷贝)
		//不需要传递关注的事件，关注的事件已经由epoll_ctl()函数添加了
		//已经添加到epoollfd里面去管理了

		/*poll模型：每次调用poll函数的时候都需要把监听套接字与已连接套接字的事件数组 
		拷贝到内核(数据拷贝是服务器编写的四大性能杀手之一)*/
		/*epoll模型：只需要通过epoll_ctl添加一次到内核，只要没有发生改变，就不需要
		再去修改什么东西，不需要传递进去。内核中已经有epollfd了*/
		if (nready == -1)
		{
			if (errno == EINTR)
				continue;
			
			ERR_EXIT("epoll_wait");
		}
		if (nready == 0)	// nothing happended
			continue;

		if ((size_t)nready == events.size()) //？event不是会自动增长的吗？
			events.resize(events.size()*2);//需要返回的事件大于之前设置的16了。此处采用倍增
		//epoll_wait返回的事件可能是监听套接字产生的事件，也可能是已连接套接字产生的事件。统一用一个for循环处理
		//与poll的不同之处
		for (int i = 0; i < nready; ++i)
		{
			if (events[i].data.fd == listenfd)
			{
				peerlen = sizeof(peeraddr);
				connfd = ::accept4(listenfd, (struct sockaddr*)&peeraddr,
						&peerlen, SOCK_NONBLOCK | SOCK_CLOEXEC);

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
						ERR_EXIT("accept4");
				}


				std::cout<<"ip="<<inet_ntoa(peeraddr.sin_addr)<<
					" port="<<ntohs(peeraddr.sin_port)<<std::endl;

				clients.push_back(connfd);
				
				event.data.fd = connfd;
				event.events = EPOLLIN/* | EPOLLET*/;
				epoll_ctl(epollfd, EPOLL_CTL_ADD, connfd, &event);// 已连接描述符加入关注
						//epollfd是epoll_create1创建出来的,创建一次
			}
			else if (events[i].events & EPOLLIN) // 返回的events中都是活跃的事件
			{
				connfd = events[i].data.fd;
				if (connfd < 0)
					continue;

				char buf[1024] = {0};
				int ret = read(connfd, buf, 1024);
				if (ret == -1)
					ERR_EXIT("read");
				if (ret == 0) //对方关闭了 
				{
					std::cout<<"client close"<<std::endl;
					close(connfd);
					event = events[i];
					epoll_ctl(epollfd, EPOLL_CTL_DEL, connfd, &event); // EPOLL_CTL_DEL与ADD对应
					clients.erase(std::remove(clients.begin(), clients.end(), connfd), clients.end());
					continue;
				}

				std::cout<<buf;
				write(connfd, buf, strlen(buf));
			}

		}
	}

	return 0;
}

//epoll采用共享内存的方式，不像poll每次都要从用户态到内核态，内核态到用户态拷贝数据
//epoll_create创建了一块内存区域，然后可以理解为epoll_ctl就是在这块内存上进行操作
