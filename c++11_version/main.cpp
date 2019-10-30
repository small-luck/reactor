#include <iostream>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "reactor.h"

myreactor g_reactor;

void prog_exit(int signo)
{
    std::cout << "program recv signal " << signo << " to exit." << std::endl;
    g_reactor.uninit();
}

void daemon_run()
{
    int pid;
    signal(SIGCHLD, SIG_IGN);
    //1.在父进程中，fork返回新创建子进程的进程ID
    //2.在子进程中，fork返回0
    //3.如果出现错误，fork返回一个负值
    pid = fork();
    if (pid < 0) {
        std::cout << "fork error" << std::endl;
        exit(-1);
    }
    //父进程退出，子进程独立运行
    else if (pid > 0) {
        exit(0);
    }

    //之前parent和child运行在同一个session中，parent是会话的领头进程
    //parent进程作为会话的领头进程，如果退出后，那么子进程会成为孤儿进程，并被init进程收养
    //所以需要执行setsid()，执行了setsid后，子进程会获取一个新的会话id
    //这时parent进程退出后就不会影响child进程了
    setsid();

    int fd;
    fd = open("/dev/null", O_RDWR, 0);
    if (fd != -1) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
    }

    if (fd > 2)
        close(fd);
}

int main(int argc, char* argv[])
{
    //设置信号处理
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, prog_exit);
    signal(SIGKILL, prog_exit);
    signal(SIGTERM, prog_exit);
    
    short port = 0;
    int ch;
    bool bdeamon = false;
    while ((ch = getopt(argc, argv, "p:d")) != -1) {
        switch (ch) {
            case 'd':
                bdeamon = true;
                break;
            case 'p':
                port = atol(optarg);
                break;
        }
    }

    if (bdeamon)
        daemon_run();

    if (port == 0)
        port = 12345;

    if (!g_reactor.init("0.0.0.0", port))
        return -1;

    g_reactor.main_loop(&g_reactor);

    return 0;
}

