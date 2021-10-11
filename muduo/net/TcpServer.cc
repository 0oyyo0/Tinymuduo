// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include "muduo/net/TcpServer.h"

#include "muduo/base/Logging.h"
#include "muduo/net/Acceptor.h"
#include "muduo/net/EventLoop.h"
#include "muduo/net/EventLoopThreadPool.h"
#include "muduo/net/SocketsOps.h"

#include <stdio.h>  // snprintf

using namespace muduo;
using namespace muduo::net;
// 创建一个TcpServer对象，在创建过程中，首先new出来
// 自己的核心组件（Acceptor,loop,connectionMap,threadPool）之后
// TcpServer会向Acceptor注册一个新连接到来时的Connection回调函数。
// loop是由用户提供的，并且在最后向Acceptor注册一个回调对象，
// 用于处理：一个新的Client连接到来时该怎么处理。
TcpServer::TcpServer(EventLoop* loop,
                     const InetAddress& listenAddr,
                     const string& nameArg,
                     Option option)
  : loop_(CHECK_NOTNULL(loop)), // TcpServer在创建的过程中，首先new出来自己的核心组件（Acceptor,loop,connectionMap,threadPool）
    ipPort_(listenAddr.toIpPort()),
    name_(nameArg),
    acceptor_(new Acceptor(loop, listenAddr, option == kReusePort)),
    threadPool_(new EventLoopThreadPool(loop, name_)),
    connectionCallback_(defaultConnectionCallback),
    messageCallback_(defaultMessageCallback),
    nextConnId_(1)
{// 注册给acceptor的回调
  acceptor_->setNewConnectionCallback(
      std::bind(&TcpServer::newConnection, this, _1, _2));
}

TcpServer::~TcpServer()
{
  loop_->assertInLoopThread();
  LOG_TRACE << "TcpServer::~TcpServer [" << name_ << "] destructing";

  for (auto& item : connections_)
  {
    TcpConnectionPtr conn(item.second);
    item.second.reset();
    conn->getLoop()->runInLoop(
      std::bind(&TcpConnection::connectDestroyed, conn));
  }
}
//设置eventloop线程池中线程的数量。
void TcpServer::setThreadNum(int numThreads)
{
  assert(0 <= numThreads);
  threadPool_->setThreadNum(numThreads);
}
//启动服务器。
void TcpServer::start()
{
  if (started_.getAndSet(1) == 0)
  {
    // 对线程池的一个启动处理
    threadPool_->start(threadInitCallback_);
    // 打开Acceptor的监听状态
    assert(!acceptor_->listening());
    loop_->runInLoop(
        std::bind(&Acceptor::listen, get_pointer(acceptor_)));
  }
}


// 这个函数实际上是对accpet新接受到一个通信套接字以后把它放入到一个
// 新创立的TcpConnection对这个通信套接字做一个封装，并将函数指针初始化，
// 它调用了TcpConnection::connectEstablished这个函数，这个函数的作用
// 就是将接受到的socket的文件描述符注册到epoll事件列表当中。

// 换言之：

// 主线程在EventLoop::loop() 中不停查询可用的I/O. 当一个新的tcp连接
// 到来时，Channel::handleEventWithGuard 会调用Acceptor::handleRead, 
// 然后回调TcpServer::newConnection()。
void TcpServer::newConnection(int sockfd, const InetAddress& peerAddr)
{
  // 断言是否在IO线程
  loop_->assertInLoopThread();
  // 从线程池中取出一个EventLoop对象（轮流取，负载均衡），然后把该链接交给它管理，
  // 如果线程池的线程数量为0，那么将返回基础的EventLoop即loop（Acceptor的EventLoop）
  EventLoop* ioLoop = threadPool_->getNextLoop();
  char buf[64];
  snprintf(buf, sizeof buf, "-%s#%d", ipPort_.c_str(), nextConnId_);
  ++nextConnId_;
  string connName = name_ + buf;

  LOG_INFO << "TcpServer::newConnection [" << name_
           << "] - new connection [" << connName
           << "] from " << peerAddr.toIpPort();
  InetAddress localAddr(sockets::getLocalAddr(sockfd));
  // FIXME poll with zero timeout to double confirm the new connection
  // FIXME use make_shared if necessary

  // 根据ioLoop, connName, sockfd, localAddr, peerAddr构建一个新的TcpConnection
  // 当accept接受到一个新的文件描述符的时候，创建一个TcpConnection对它进行封装
  TcpConnectionPtr conn(new TcpConnection(ioLoop,
                                          connName,
                                          sockfd,
                                          localAddr,
                                          peerAddr));
  // 将新构建的conn加入ConnectionMap中
  connections_[connName] = conn;
  // 对新建立的TcpConnection进行初始化（参数来源于TcpServer，而TcpServer的参数则是由用户提供的）
  conn->setConnectionCallback(connectionCallback_);
  conn->setMessageCallback(messageCallback_);
  conn->setWriteCompleteCallback(writeCompleteCallback_);
  conn->setCloseCallback(
      std::bind(&TcpServer::removeConnection, this, _1)); // FIXME: unsafe
  ioLoop->runInLoop(std::bind(&TcpConnection::connectEstablished, conn));
}

//移除TcpConnection，注册到TcpServer::removeConnectionInLoop上
void TcpServer::removeConnection(const TcpConnectionPtr& conn)
{
  // FIXME: unsafe
  loop_->runInLoop(std::bind(&TcpServer::removeConnectionInLoop, this, conn));
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn)
{
  loop_->assertInLoopThread();
  LOG_INFO << "TcpServer::removeConnectionInLoop [" << name_
           << "] - connection " << conn->name();
  // 删除该conn（一个map对象）
  size_t n = connections_.erase(conn->name());
  (void)n;
  // conn的引用计数变为1
  assert(n == 1);
  // 获得本线程的Loop
  EventLoop* ioLoop = conn->getLoop();
  // 调用TcpConnection::connectDestroyed销毁连接
  ioLoop->queueInLoop(
      std::bind(&TcpConnection::connectDestroyed, conn));
}

