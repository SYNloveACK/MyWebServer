#include "webserver.h"
#include<iostream>
using namespace std;

WebServer::WebServer(char *server_Path)
{
    users = new http_conn[MAX_FD];
    
    char root[6]="/root";
    m_root=(char*)malloc(strlen(server_Path)+strlen(root)+1);
    strcpy(m_root, server_Path);
    strcat(m_root, root);
    users_timer = new client_data[MAX_FD];
}

WebServer::~WebServer()
{
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[0]);
    close(m_pipefd[1]);
    free(m_root);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}

void WebServer::init(int port, string user, string passwd, 
    string databaseName, int log_write, int opt_linger, 
    int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
    m_port = port;
    m_user = user;
    m_passWord = passwd;
    m_databaseName = databaseName;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_close_log = close_log;
    m_actormodel = actor_model;

}

void WebServer::thread_pool()
{
    m_pool = new threadpool<http_conn>(m_actormodel,m_connPool,m_thread_num);
}

void WebServer::sql_pool()
{
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("192.168.137.1",m_user,m_passWord,
                        m_databaseName,3306,m_sql_num,m_close_log);
    users->initmysql_result(m_connPool);
}

void WebServer::log_write()
{
    if(0 == m_close_log)
    {
        if(1 == m_log_write)
        {
            Log::get_instance()->init("./ServerLog",m_close_log,2000,800000,800);
        }
        else
        {
            Log::get_instance()->init("./ServerLog",m_close_log,2000,800000,0);
        }
    }
}

void WebServer::trig_mod()
{
    //0:LT
    //1:ET
    if(0 == m_TRIGMode)
    {
        m_listen_trigmode=0;
        m_conn_trigmode=0;
    }
    else if(1 == m_TRIGMode)
    {
        m_listen_trigmode=0;
        m_conn_trigmode=1;
    }
    else if(2 == m_TRIGMode)
    {
        m_listen_trigmode=1;
        m_conn_trigmode=0;
    }
    else
    {
        m_listen_trigmode=1;
        m_conn_trigmode=1;
    }
}

int websetnonblocking(int fd)
{
    int old_option = fcntl(fd,F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd,F_SETFL,new_option);
    return old_option;
}

void webaddfd(int epollfd,int fd,bool one_shot,int TRIGMod)
{
    epoll_event event;
    event.data.fd=fd;
    if(1 == TRIGMod)
    {
        event.events=EPOLLIN|EPOLLET|EPOLLRDHUP; //ET模式
    }
    else
    {
        event.events=EPOLLIN|EPOLLRDHUP; //LT模式
    }
    if(one_shot)
    {
        event.events |= EPOLLONESHOT; //设置为单次触发
    }
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
    websetnonblocking(fd); //设置为非阻塞
}

void WebServer::eventListen()//to add ok
{
    m_listenfd = socket(PF_INET,SOCK_STREAM,0);
    assert(m_listenfd >= 0);

    if(0 == m_OPT_LINGER)
    {
        struct linger tmp={0,1};
        setsockopt(m_listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp));
    }
    else if(1 == m_OPT_LINGER)
    {
        struct linger tmp={1,1};
        setsockopt(m_listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp));
    }

    int ret =0;
    struct sockaddr_in address;
    bzero(&address,sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr= htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    int flag = 1;
    setsockopt(m_listenfd,SOL_SOCKET,SO_REUSEADDR,&flag,sizeof(flag));
    ret = bind(m_listenfd,(struct sockaddr*)&address,sizeof(address));
    assert(ret >= 0);
    ret = listen(m_listenfd,5);
    assert(ret >= 0);

    utils.init(TIMESLOT);
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);
    //webaddfd(m_epollfd,m_listenfd,false,m_listen_trigmode);
    utils.addfd(m_epollfd, m_listenfd, false, m_listen_trigmode);
    http_conn::m_epollfd = m_epollfd;

    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);
    utils.setnonblocking(m_pipefd[1]);
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);

    utils.addsig(SIGPIPE, SIG_IGN);
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);

    alarm(TIMESLOT);

    //工具类,信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;

}

void WebServer::eventLoop()// to add ok
{
    bool timeout = false;
    bool stop_server = false;

    while (!stop_server)
    {
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            //处理新到的客户连接
            if (sockfd == m_listenfd)
            {
                bool flag = dealClientConn();
                if (false == flag)
                    continue;
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            //处理信号
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                bool flag = dealSignal(timeout, stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }
            //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                dealRead(sockfd);
            }
            else if (events[i].events & EPOLLOUT)
            {
                dealWrite(sockfd);
            }
        }
        if (timeout)
        {
            utils.timer_handler();

            LOG_INFO("%s", "timer tick");

            timeout = false;
        }
    }
}

void WebServer::timer(int connfd, sockaddr_in client_address)
{
    users[connfd].init(connfd, client_address, m_root, m_conn_trigmode, m_close_log, m_user, m_passWord, m_databaseName);

    //初始化client_data数据
    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    util_timer *timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    users_timer[connfd].timer = timer;
    utils.m_timer_lst.add_timer(timer);
}

void WebServer::adjust_timer(util_timer *timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);
    LOG_INFO("%s", "adjust timer once");
}

void WebServer::deal_timer(util_timer *timer, int sockfd)
{
    timer->cb_func(&users_timer[sockfd]);
    if (timer)
    {
        utils.m_timer_lst.del_timer(timer);
    }
    cout<<"close fd "<<users_timer[sockfd].sockfd<<endl;
    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

bool WebServer::dealClientConn()
{
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    if(0 == m_listen_trigmode)//LT
    {
        int connfd=accept(m_listenfd,(struct sockaddr*)&client_address,&client_addrlength);
        if(connfd <0)
        {
            //cout<<"webserver connect error"<<endl;
            LOG_ERROR("%s:errno is:%d", "accept error", errno);

            return false;
        }
        if(http_conn::m_user_count >=MAX_FD)
        {
            //cout<<"webserver reach max fd"<<endl;
            LOG_ERROR("%s", "Internal server busy");

            return false;
        }
        //users[connfd].init(connfd,client_address,m_root,m_conn_trigmode,m_close_log,m_user,m_passWord,m_databaseName);
        timer(connfd, client_address);
    }
    else 
    {
        while(true)
        {
            int connfd=accept(m_listenfd,(struct sockaddr*)&client_address,&client_addrlength);
            if(connfd < 0)
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if(http_conn::m_user_count >= MAX_FD)
            {
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            //users[connfd].init(connfd,client_address,m_root,m_conn_trigmode,m_close_log,m_user,m_passWord,m_databaseName);
            timer(connfd, client_address);
        }
        return false;//?
    }
    return true;

}

bool WebServer::dealSignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1)
    {
        return false;
    }
    else if (ret == 0)
    {
        return false;
    }
    else
    {
        for (int i = 0; i < ret; ++i)
        {
            switch (signals[i])
            {
            case SIGALRM:
            {
                timeout = true;
                break;
            }
            case SIGTERM:
            {
                stop_server = true;
                break;
            }
            }
        }
    }
    return true;
}

void WebServer::dealRead(int sockfd)//to add ok
{
    util_timer *timer = users_timer[sockfd].timer;

    //reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        //若监测到读事件，将该事件放入请求队列
        m_pool->append(users + sockfd, 0);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        //proactor
        if (users[sockfd].read_once())
        {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            //若监测到读事件，将该事件放入请求队列
            m_pool->append_p(users + sockfd);

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}


void WebServer::dealWrite(int sockfd)// to add ok
{
    util_timer *timer = users_timer[sockfd].timer;
    //reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        m_pool->append(users + sockfd, 1);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        //proactor
        if (users[sockfd].write())
        {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}
