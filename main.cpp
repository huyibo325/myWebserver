#include<stdio.h>
#include<arpa/inet.h>
#include<signal.h>
#include<sys/epoll.h>
#include"./threadpool/threadpool.h"
#include<stdlib.h>
#include<assert.h>
#include<unistd.h>
#include<time.h>
#include<sys/time.h>
#include"./http/http_conn.h"
#include"./lock/lock.h"
#include"./timer/lst_timer.h"
#include"./log/log.h"
#include"./CGImysql/sql_connection_pool.h"
#define MAX_FD 65535
#define MAX_EPOLL_NUMBER 10000
#define TIMESLOT 5


//设置定时器相关参数
static int pipefd[2];
static sort_timer_lst timer_lst;
static int epollfd=0;


//将文件描述符添加到epoll中
extern void addfd(int epollfd,int fd,bool one_shot);
//从epoll中删除文件描述符
extern void removefd(int epollfd,int fd);
//修改文件描述符
extern void modfd(int epollfd,int fd,int ev);
extern int setnonblocking(int fd);

//信号处理函数
void sig_handler(int sig){
    int save_errno=errno;
    int msg=sig;
    send(pipefd[1],(char*)&msg,1,0);
    errno=save_errno;
}
//添加信号
void addsig(int sig,void(handler)(int)){
    struct sigaction act;
    act.sa_flags=0;
    sigfillset(&act.sa_mask);
    act.sa_handler=handler;
    sigaction(sig,&act,NULL);
}
//定时处理任务，重新定时以不断触发SIGALARM信号
void timer_handler(){
    printf("关闭超时连接\n");
    timer_lst.tick();
    alarm(TIMESLOT);
}
//定时器回调函数，删除非活动连接在socket上的注册时间，并关闭
void cb_func(client_data*user_data){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
    LOG_INFO("close fd %d", user_data->sockfd);
    Log::get_instance()->flush();
}

void show_error(int connfd,const char*info){
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}
int main(int argc,char*argv[]){

    Log::get_instance()->init("ServerLog",2000,800000,8);// 异步日志模型
    if(argc<=1){
        printf("请按照如下格式运行：%s port\n",argv[0]);
        exit(-1);
    }
    //获取端口号
    int port=atoi(argv[1]);
    //对SIGPIPE信号进行处理
    addsig(SIGPIPE,SIG_IGN);

    //数据库连接池
    connection_pool*connPool=connection_pool::GetInstance();
    connPool->init("localhost","root","123456","yourdb",0,8);
    //创建线程池
    threadpool<http_conn>*pool=NULL;
    try{
        pool=new threadpool<http_conn>(connPool);
    }catch(...){
        exit(-1);
    }
    //创建一个数组，用于保存客户端信息
    http_conn*users=new http_conn[MAX_FD];
    users->initmysql_result(connPool);

    //创建套接字
    int listenfd=socket(AF_INET,SOCK_STREAM,0);
    //设置端口复用
    int reuse=1;
    setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));

    //绑定
    int ret=0;
    struct sockaddr_in server_address;
    server_address.sin_family=AF_INET;
    server_address.sin_port=htons(port);
    server_address.sin_addr.s_addr=INADDR_ANY;
    bind(listenfd,(struct sockaddr*)&server_address,sizeof(server_address));
    //监听
    listen(listenfd,5);
    //创建epoll
    epoll_event events[MAX_EPOLL_NUMBER];
    epollfd=epoll_create(5);
    //将监听的文件描述符加入到epollfd
    addfd(epollfd,listenfd,false);
    http_conn::m_epollfd=epollfd;

    //创建管道
    ret=socketpair(PF_UNIX,SOCK_STREAM,0,pipefd);
    assert(ret!=-1);
    setnonblocking(pipefd[1]);
    addfd(epollfd,pipefd[0],false);

    addsig(SIGALRM,sig_handler);
    addsig(SIGTERM,sig_handler);
    bool stop_server=false;

    client_data *user_timer=new client_data[MAX_FD];
    bool timeout=false;
    alarm(TIMESLOT);

    while(!stop_server){
        int num=epoll_wait(epollfd,events,MAX_EPOLL_NUMBER,-1);
        if((num<0)&&(errno!=EINTR)){
            LOG_ERROR("%s","epoll failure");
            break;
        }
        //循环遍历事件数组
        for(int i=0;i<num;i++){
            int sockfd=events[i].data.fd;
            if(sockfd==listenfd){
                struct sockaddr_in client_address;
                socklen_t len=sizeof(client_address);
                int connfd=accept(listenfd,(struct sockaddr*)&client_address,&len);

                if(connfd<0){
                    LOG_ERROR("%s:errno is:%d","accept error",errno);
                    continue;
                }

                if(http_conn::m_user_count>=MAX_FD){
                    //目前连接满了
                    //给客户端发信息：服务器正忙
                    show_error(connfd,"Internal server busy");
                    LOG_ERROR("%s","Internal server busty");
                    continue;
                }
                //将新的客户的数据初始化，放到数组中
                users[connfd].init(connfd,client_address);

                //初始化client_data数据
                user_timer[connfd].address=client_address;
                user_timer[connfd].sockfd=connfd;
                util_timer*timer=new util_timer;
                timer->user_data=&user_timer[connfd];
                timer->cb_func=cb_func;
                time_t cur =time(NULL);
                timer->expire=cur+3*TIMESLOT;
                user_timer[connfd].timer=timer;
                timer_lst.add_timer(timer);
            }else if(events[i].events&(EPOLLRDHUP|EPOLLHUP|EPOLLERR)){
                util_timer*timer=user_timer[sockfd].timer;
                timer->cb_func(&user_timer[sockfd]);
                if(!timer){
                    timer_lst.del_timer(timer);
                }
            }else if((sockfd==pipefd[0])&&(events[i].events&EPOLLIN)){
                int sig;
                char signals[1024];
                ret=recv(pipefd[0],signals,sizeof(signals),0);
                if(ret==01){
                    continue;
                }
                else if(ret==0){
                    continue;
                }
                else{
                    for(int i=0;i<ret;i++){
                        switch(signals[i]){
                            case SIGALRM:
                            {
                                timeout=true;
                                break;
                            }
                            case SIGTERM:
                            {
                                stop_server=true;
                            }
                        }
                    }
                }
            }
            else if(events[i].events&EPOLLIN){
                util_timer*timer =user_timer[sockfd].timer;
                if(users[sockfd].read()){
                    LOG_INFO("deal with the client(%s)",inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
                    pool->append(users+sockfd);
                    if(timer){
                        time_t cur=time(NULL);
                        timer->expire=cur+3*TIMESLOT;

                        LOG_INFO("%s","adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else{
                    timer->cb_func(&user_timer[sockfd]);
                    if(timer){
                        timer_lst.del_timer(timer);
                    }
                }
            }else if(events[i].events&EPOLLOUT){
                util_timer*timer=user_timer[sockfd].timer;
                if(users[sockfd].write()){
                    //printf("xie shujv\n");
                    LOG_INFO("send data to the client(%s)",inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
                    if(timer){
                        time_t cur=time(NULL);
                        timer->expire=cur+3*TIMESLOT;
                        LOG_INFO("%s","adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }else{
                    timer->cb_func(&user_timer[sockfd]);
                    if(timer){
                        timer_lst.del_timer(timer);
                    }
                }
            }
        }
        if(timeout){
            timer_handler();
            timeout=false;
        }
    }
    close(listenfd);
    close(epollfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete []users;
    delete []user_timer;
    delete pool;
    return 0;
 }