/*
 *reactor.cpp
 * */
#include "reactor.h"
#include <iostream>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>      //for htonl() and htons()
#include <fcntl.h>
#include <sys/epoll.h>
#include <list>
#include <errno.h>
#include <time.h>
#include <sstream>
#include <iomanip>      //for std::setw()/setfill()
#include <unistd.h>

#define min(a, b) ((a <= b) ? (a) : (b))

myreactor::myreactor()
{

}

myreactor::~myreactor()
{

}

bool myreactor::init(const char* ip, short port)
{
    if (!create_server_listener(ip, port)) {
        std::cout << "unable to bind: " << ip << ":" << port << "." << std::endl;
        return false;
    }

    std::cout << "main thread id = " << std::this_thread::get_id() << std::endl;

    //启动接收新连接的线程
    m_acceptthread.reset(new std::thread(myreactor::accept_thread_proc, this));

    //启动工作线程
    for (auto& t : m_workerthreads) {
        t.reset(new std::thread(myreactor::worker_thread_proc, this));
    }

    return true;
}

bool myreactor::uninit()
{
    m_bstop = true;
    m_acceptcond.notify_one();
    m_woekercond.notify_all();

    m_acceptthread->join();
    for (auto& t : m_workerthreads) {
        t->join();
    }

    ::epoll_ctl(m_epollfd, EPOLL_CTL_DEL, m_listenfd, NULL);

    ::shutdown(m_listenfd, SHUT_RDWR);
    ::close(m_listenfd);
    ::close(m_epollfd);

    return true;
}

bool myreactor::close_client(int clientfd)
{
    if (::epoll_ctl(m_epollfd, EPOLL_CTL_DEL, clientfd, NULL) == -1) {
        std::cout << "close client socket failed as call epoll_ctl failed" << std::endl;
    }

    ::close(clientfd);

    return true;
}

void* myreactor::main_loop(void* p)
{
    std::cout << "main thread id = " << std::this_thread::get_id() << std::endl;
    myreactor* preactor = static_cast<myreactor*>(p);

    while (!preactor->m_bstop) {
        struct epoll_event ev[1024];
        int n = ::epoll_wait(preactor->m_epollfd, ev, 1024, 10);
        if (n == 0)
            continue;
        else if (n < 0) {
            std::cout << "epoll_wait error" << std::endl;
            continue;
        }

        int m = min(n, 1024);
        for (int i = 0; i < m; i++) {
            //通知接收连接线程接收新连接
            if (ev[i].data.fd == preactor->m_listenfd) 
                preactor->m_acceptcond.notify_one();
            //通知普通工作线程接收数据
            else {
                std::unique_lock<std::mutex> guard(preactor->m_workermutex);
                preactor->m_listclients.push_back(ev[i].data.fd);
                preactor->m_woekercond.notify_one();
            }
        }
    }

    std::cout << "main loop exit..." << std::endl;

    return NULL;
}

void myreactor::accept_thread_proc(myreactor* preactor)
{
    std::cout << "accept thread, thread id = " << std::this_thread::get_id() << std::endl;

    while (true) {
        int newfd;
        struct sockaddr_in clientaddr;
        socklen_t addrlen;
        
        std::unique_lock<std::mutex> guard(preactor->m_acceptmutex);
        preactor->m_acceptcond.wait(guard);
        if (preactor->m_bstop)
            break;

        memset(&clientaddr, 0, sizeof(clientaddr));
        newfd = ::accept(preactor->m_listenfd, (struct sockaddr*)&clientaddr, &addrlen);
        if (newfd == -1) 
            continue;

        std::cout << "new client connected: " << ::inet_ntoa(clientaddr.sin_addr) << ":" << ::ntohs(clientaddr.sin_port) << std::endl;
        
        //将新的socket设置为non-blocking
        int oldfg = ::fcntl(newfd, F_GETFL, 0);
        int newfg = oldfg | O_NONBLOCK;
        if (::fcntl(newfd, F_SETFL, newfg) == -1) {
            std::cout << "fcntl error, oldfg = " << oldfg << ", newfg = " << newfg << std::endl;
            continue;
        }

        struct epoll_event e;
        memset(&e, 0 ,sizeof(e));
        e.data.fd = newfd;
        e.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
        if (::epoll_ctl(preactor->m_epollfd, EPOLL_CTL_ADD, newfd, &e) == -1) {
            std::cout << "epoll_ctl error, fd = " << newfd << std::endl;
            close(newfd);
        }
    }

    std::cout << "accept thread exit..." << std::endl;
}

void myreactor::worker_thread_proc(myreactor* preactor)
{
    std::cout << "new worker thread, thread id = " << std::this_thread::get_id() << std::endl;

    while (true) {
        int clienfd;
        {
            std::unique_lock<std::mutex> guard(preactor->m_workermutex);
            while (preactor->m_listclients.empty()) {
                if (preactor->m_bstop) {
                    std::cout << "work thread exit..." << std::endl;
                    return;
                }
                preactor->m_woekercond.wait(guard);
            }
            clienfd = preactor->m_listclients.front();
            preactor->m_listclients.pop_front();
        }

        //gdb调试时不能实时刷新标准输出，用这个函数刷新标准输出，使信息在屏幕上实时显示
        std::cout << std::endl;

        std::string strclientmsg;
        char buff[256];
        bool berror = false;
        while (true) {
            memset(buff, 0, sizeof(buff));
            int nrecv = ::recv(clienfd, buff, 256, 0);
            if (nrecv == -1) {
                if (errno == EWOULDBLOCK)
                    break;
                else {
                    std::cout << "recv error, client disconnected, fd = " << clienfd << std::endl;
                    preactor->close_client(clienfd);
                    berror = true;
                    break;
                }
            }
            
            //对端关闭了socket，本端也关闭
            if (nrecv == 0) {
                std::cout << "peer closed, client disconnected, fd = " << clienfd << std::endl;
                preactor->close_client(clienfd);
                berror = true;
                break;
            }

            strclientmsg += buff;
        }

        if (berror)
            continue;

        std::cout << "client msg: " << strclientmsg;

        //将消息加上时间标签后返回
        time_t now = time(NULL);
        struct tm* nowstr = localtime(&now);
        std::ostringstream ostimestr;
        ostimestr << "[" << nowstr->tm_year + 1900 << "-"
                  << std::setw(2) << std::setfill('0') << nowstr->tm_mon + 1 << "-"
                  << std::setw(2) << std::setfill('0') << nowstr->tm_mday << " "
                  << std::setw(2) << std::setfill('0') << nowstr->tm_hour << ":"
                  << std::setw(2) << std::setfill('0') << nowstr->tm_min << ":"
                  << std::setw(2) << std::setfill('0') << nowstr->tm_sec << "]server reply: ";
        
        strclientmsg.insert(0, ostimestr.str());

        while (true) {
            int nsent = ::send(clienfd, strclientmsg.c_str(), strclientmsg.length(), 0);
            if (nsent == -1) {
                if (errno == EWOULDBLOCK) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                } else {
                    std::cout << "send error, fd = " << clienfd << std::endl;
                    preactor->close_client(clienfd);
                    break;
                }
            }

            std::cout << "send: " << strclientmsg;
            strclientmsg.erase(0, nsent);
            if (strclientmsg.empty())
                break;
        }
    }
}

bool myreactor::create_server_listener(const char* ip, short port)
{
    m_listenfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (m_listenfd == -1)
        return false;

    int on = 1;
    ::setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, (char*)&on, sizeof(on));
    ::setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEPORT, (char*)&on, sizeof(on));

    struct sockaddr_in seraddr;
    memset(&seraddr, 0 , sizeof(seraddr));

    seraddr.sin_family = AF_INET;
    seraddr.sin_port = htons(port);
    seraddr.sin_addr.s_addr = inet_addr(ip);

    if (::bind(m_listenfd, (struct sockaddr*)&seraddr, sizeof(seraddr)) == -1)
        return false;

    if (::listen(m_listenfd, 50) == -1)
        return false;

    m_epollfd = ::epoll_create(1);
    if (m_epollfd == -1)
        return false;

    struct epoll_event e;
    memset(&e, 0, sizeof(e));
    e.events = EPOLLIN | EPOLLRDHUP;
    e.data.fd = m_listenfd;
    if (::epoll_ctl(m_epollfd, EPOLL_CTL_ADD, m_listenfd, &e) == -1)
        return false;

    return true;
}
