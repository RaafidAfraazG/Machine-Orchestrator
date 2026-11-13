#include "mo/reactor.hpp"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
namespace mo {
Reactor::Reactor(){epoll_=epoll_create1(EPOLL_CLOEXEC);wake_=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);if(valid()){epoll_event e{};e.events=EPOLLIN;e.data.u64=0;if(epoll_ctl(epoll_,EPOLL_CTL_ADD,wake_,&e)<0){::close(epoll_);epoll_=-1;}}}
Reactor::~Reactor(){stop();join();if(wake_>=0)::close(wake_);if(epoll_>=0)::close(epoll_);}
void Reactor::start(){if(valid()&&!thread_.joinable()){stopping_=false;thread_=std::thread([this]{run();});}}
void Reactor::stop(){stopping_=true;uint64_t one=1;if(wake_>=0){ssize_t r;do{r=::write(wake_,&one,sizeof(one));}while(r<0&&errno==EINTR);}}
void Reactor::join(){if(thread_.joinable())thread_.join();}
void Reactor::post(Task t){{std::lock_guard<std::mutex> l(mutex_);posts_.push(std::move(t));}uint64_t one=1;ssize_t r;do{r=::write(wake_,&one,sizeof(one));}while(r<0&&errno==EINTR);}
bool Reactor::watch(int fd,bool w,Callbacks c){if(ids_.count(fd))return false;auto id=nextWatch_++;epoll_event e{};e.events=EPOLLIN|EPOLLRDHUP|(w?static_cast<unsigned>(EPOLLOUT):0u);e.data.u64=id;if(epoll_ctl(epoll_,EPOLL_CTL_ADD,fd,&e)<0)return false;ids_[fd]=id;watches_.emplace(id,Watch{fd,std::move(c)});return true;}
bool Reactor::modify(int fd,bool w){auto i=ids_.find(fd);if(i==ids_.end())return false;epoll_event e{};e.events=EPOLLIN|EPOLLRDHUP|(w?static_cast<unsigned>(EPOLLOUT):0u);e.data.u64=i->second;return epoll_ctl(epoll_,EPOLL_CTL_MOD,fd,&e)==0;}
void Reactor::unwatch(int fd){auto i=ids_.find(fd);if(i==ids_.end())return;epoll_ctl(epoll_,EPOLL_CTL_DEL,fd,nullptr);watches_.erase(i->second);ids_.erase(i);}
uint64_t Reactor::after(std::chrono::milliseconds d,Task t){auto id=nextTimer_++;Key k{std::chrono::steady_clock::now()+d,id};timers_.emplace(k,std::move(t));timerKeys_.emplace(id,k);return id;}
void Reactor::cancel(uint64_t id){auto i=timerKeys_.find(id);if(i!=timerKeys_.end()){timers_.erase(i->second);timerKeys_.erase(i);}}
void Reactor::run(){
 while(!stopping_){
 std::queue<Task> tasks;{std::lock_guard<std::mutex> l(mutex_);tasks.swap(posts_);}while(!tasks.empty()){auto t=std::move(tasks.front());tasks.pop();t();}
 auto now=std::chrono::steady_clock::now();while(!timers_.empty()&&timers_.begin()->first.first<=now){auto i=timers_.begin();auto t=std::move(i->second);timerKeys_.erase(i->first.second);timers_.erase(i);t();now=std::chrono::steady_clock::now();}
 if(stopping_)break;
 int timeout=-1;if(!timers_.empty()){auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(timers_.begin()->first.first-now).count()+1;timeout=static_cast<int>(std::min<long long>(ms,INT_MAX));}
 epoll_event events[64];int n=epoll_wait(epoll_,events,64,timeout);if(n<0){if(errno==EINTR)continue;break;}
 for(int j=0;j<n;++j){auto id=events[j].data.u64;auto flags=events[j].events;if(id==0){uint64_t v;while(::read(wake_,&v,sizeof(v))<0&&errno==EINTR){}continue;}
 auto invoke=[&](int kind){auto i=watches_.find(id);if(i==watches_.end())return;Task t=kind==0?i->second.callbacks.onReadable:kind==1?i->second.callbacks.onWritable:i->second.callbacks.onError;if(t)t();};
 if(flags&EPOLLIN)invoke(0);
 if(flags&EPOLLOUT)invoke(1);
 if(flags&(EPOLLERR|EPOLLHUP|EPOLLRDHUP))invoke(2);
 }
 }
}
}
