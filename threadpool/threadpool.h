#ifndef THREADPOLL_H
#define THREADPOLL_H
#include<pthread.h>
#include<list>
#include"../lock/lock.h"
#include<cstdio>
#include"../CGImysql/sql_connection_pool.h"
template<typename T>
class threadpool{
public:
    threadpool(connection_pool*connPool,int thread_num=8,int max_workqueue=10000);
    ~threadpool();
    bool append(T*request);
public:
    static void* worker(void*arg);
    void run();
private:
    pthread_t* m_threads;
    int m_thread_num;
    std::list<T*>m_workqueue;
    int m_max_workqueue;
    bool m_stop;
    locker m_queuelock;
    sem m_queuestat;
    connection_pool*m_connPool;//数据库
};
template<typename T>
threadpool<T>::threadpool(connection_pool*connPool,int thread_num,int max_workqueue):m_thread_num(thread_num),m_max_workqueue(max_workqueue),
    m_threads(NULL),m_stop(false),m_connPool(connPool){
    if(thread_num<=0||max_workqueue<=0){
        throw std::exception();
    }
    m_threads=new pthread_t[m_thread_num];
    if(!m_threads){
        throw std::exception();
    }
    for(int i=0;i<thread_num;i++){
        if(pthread_create(m_threads+i,NULL,worker,this)!=0){
            delete [] m_threads;
            throw std::exception();
        }
        printf("create %dth thread\n",i);
        pthread_detach(m_threads[i]);
    }
}

template<typename T>
threadpool<T>::~threadpool(){
    m_stop=true;
    delete[] m_threads;
}
template<typename T>
bool threadpool<T>::append(T*request){
    m_queuelock.lock();
    if(m_workqueue.size()>m_max_workqueue){
        m_queuelock.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuestat.post();
    m_queuelock.unlock();
    return true;
}

template<typename T>
 void* threadpool<T>::worker(void*arg){
     threadpool*pthis=(threadpool*)arg;
     pthis->run();
     return pthis;
}
template<typename T>
void threadpool<T>::run(){
    while(!m_stop){
        m_queuestat.wait();
        m_queuelock.lock();
        if(m_workqueue.empty()){
            m_queuelock.unlock();
            continue;
        }
        T*request=m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelock.unlock();
        if(!request){
            continue;
        }
        connectionRAII mysqlcon(&request->mysql,m_connPool);
        request->process();
    }
}





#endif