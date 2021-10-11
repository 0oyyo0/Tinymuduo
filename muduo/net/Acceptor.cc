// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include "muduo/net/Acceptor.h"

#include "muduo/base/Logging.h"
#include "muduo/net/EventLoop.h"
#include "muduo/net/InetAddress.h"
#include "muduo/net/SocketsOps.h"

#include <errno.h>
#include <fcntl.h>
//#include <sys/types.h>
//#include <sys/stat.h>
#include <unistd.h>

using namespace muduo;
using namespace muduo::net;


// Accpetor（连接处理器）的作用如下：
// 1、创建监听（接收者）套接字
// 2、设置套接字选项
// 3、创建监听套接字的事件处理器，主要用于处理监听套接字的读事件（新连接）
// 4、绑定地址
// 5、开始监听
// 6、等待事件到来

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reuseport)
  : loop_(loop),
    acceptSocket_(sockets::createNonblockingOrDie(listenAddr.family())),
    acceptChannel_(loop, acceptSocket_.fd()),
    listening_(false),
    idleFd_(::open("/dev/null", O_RDONLY | O_CLOEXEC))
{
  assert(idleFd_ >= 0);
  // 地址复用
  acceptSocket_.setReuseAddr(true);
  // 端口复用
  acceptSocket_.setReusePort(reuseport);
  // 绑定地址
  acceptSocket_.bindAddress(listenAddr);
  // 设置读事件的回调函数
  acceptChannel_.setReadCallback(
      std::bind(&Acceptor::handleRead, this));
}

Acceptor::~Acceptor()
{
  acceptChannel_.disableAll();
  acceptChannel_.remove();
  ::close(idleFd_);
}

void Acceptor::listen()
{
  loop_->assertInLoopThread();
  listening_ = true;
  // 底层调用了listen()函数
  acceptSocket_.listen();
  // 让监听字的channel关注可读事件
  acceptChannel_.enableReading();
}


// 写服务器应用程序必须要考虑到服务器资源不足的情况，
// 其中常见的一个是打开的文件数量（文件描述符数量）不能超过系统限制，
// 当接受的连接太多时就会到达系统的限制，即表示打开的套接字文件描述符
// 太多，从而导致accpet失败，返回EMFILE错误，但此时连接已经在系统内核
// 建立好了，所以占用了系统的资源，我们不能让接受不了的连接继续占用系统
// 资源，如果不处理这种错误就会有越来越多的内核连接建立，系统资源被占用
// 也会越来越多，直到系统崩溃。一个常见的处理方式就是，先打开一个文件，
// 预留一个文件描述符，出现EMFILE错误的时候，把打开的文件关闭，此时就会
// 空出一个可用的文件描述符，再次调用accept就会成功，接受到客户连接之后，
// 我们马上把它关闭，这样这个连接在系统中占用的资源就会被释放。关闭之后
// 又会有一个文件描述符空闲，我们再次打开一个文件，占用文件描述符，
// 等待下一次的EMFILE错误。


// 即 用一个占位

void Acceptor::handleRead() // 处理读事件
{
  loop_->assertInLoopThread();
  InetAddress peerAddr;
  //FIXME loop until no more
  // 接受一个连接
  int connfd = acceptSocket_.accept(&peerAddr);
  if (connfd >= 0)
  {
    // string hostport = peerAddr.toIpPort();
    // LOG_TRACE << "Accepts of " << hostport;
    // 调用新连接到来的回调函数
    if (newConnectionCallback_)
    {
      // TcpServer注册的，创建新的conn,并且加入TcpServer的ConnectionMap中。
      newConnectionCallback_(connfd, peerAddr);
    }
    else
    {
      sockets::close(connfd);
    }
  }
  else
  {
    LOG_SYSERR << "in Acceptor::handleRead";
    // Read the section named "The special problem of
    // accept()ing when you can't" in libev's doc.
    // By Marc Lehmann, author of libev.
    // 超过了最大连接数，文件描述符用完了
    if (errno == EMFILE)
    {
      // 关闭预留的文件描述符
      ::close(idleFd_);
      // 然后立即打开，即可得到一个可用的文件描述符，马上接受新链接
      idleFd_ = ::accept(acceptSocket_.fd(), NULL, NULL);
      // 然后立即关闭这个连接，表示服务器不再提供服务，因为系统资源已经不足
      // 服务器使用这个方法来拒绝客户的连接
      ::close(idleFd_);
      // 再次获得一个空洞文件描述符保存到idleFd_中
      idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    }
  }
}

