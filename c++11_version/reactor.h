/*
 *C++11版的reactor模式服务端实现
 * */
#ifndef __MYREACTOR_H__
#define __MYREACTOR_H__

#include <list>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>

#define WORKER_THREAD_NUM   5

class myreactor {
public:
    myreactor();
    ~myreactor();

    bool init(const char* ip, short port);
    bool uninit();

    bool close_client(int clientfd);

    static void* main_loop(void* p);

private:
    //no copyable
    myreactor(const myreactor& rhs);
    myreactor& operator = (const myreactor& rhs);

    bool create_server_listener(const char* ip, short port);

    static void accept_thread_proc(myreactor* preactor);
    static void worker_thread_proc(myreactor* preactor);

private:
    //c++11语法可以在这里初始化
    int                     m_listenfd = 0;
    int                     m_epollfd = 0;
    bool                    m_bstop = false;

    std::shared_ptr<std::thread>    m_acceptthread;
    std::shared_ptr<std::thread>    m_workerthreads[WORKER_THREAD_NUM];
    
    std::condition_variable         m_acceptcond;
    std::mutex                      m_acceptmutex;

    std::condition_variable         m_woekercond;
    std::mutex                      m_workermutex;

    std::list<int>                  m_listclients;
};


#endif //__MYREACTOR_H__
