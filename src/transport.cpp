#include "mo/transport.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>
namespace mo {
FdTransport::~FdTransport(){close();}
void FdTransport::close(){if(fd_>=0)::close(fd_);fd_=-1;}
Result<size_t> FdTransport::send(const uint8_t* p,size_t n){ssize_t r;do{r=socket_?::send(fd_,p,n,MSG_NOSIGNAL) : ::write(fd_,p,n);}while(r<0&&errno==EINTR);if(r>=0)return static_cast<size_t>(r);error_=errno;return errno==EAGAIN||errno==EWOULDBLOCK?Error::Incomplete:Error::Io;}
Result<size_t> FdTransport::recv(uint8_t* p,size_t n){ssize_t r;do{r=::read(fd_,p,n);}while(r<0&&errno==EINTR);if(r>0)return static_cast<size_t>(r);if(r==0)return Error::Closed;error_=errno;return errno==EAGAIN||errno==EWOULDBLOCK?Error::Incomplete:Error::Io;}
Result<bool> TcpTransport::open(){close();error_=0;fd_=::socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(fd_<0){error_=errno;return Error::Io;}int yes=1;if(setsockopt(fd_,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof(yes))<0){error_=errno;close();return Error::Io;}sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port_);if(inet_pton(AF_INET,host_.c_str(),&a.sin_addr)!=1){close();return Error::InvalidArgument;}if(::connect(fd_,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0)return true;if(errno==EINPROGRESS)return false;error_=errno;close();return Error::Io;}
Result<bool> TcpTransport::finishConnect(){int e=0;socklen_t n=sizeof(e);if(getsockopt(fd_,SOL_SOCKET,SO_ERROR,&e,&n)<0)e=errno;if(e){error_=e;return Error::Io;}return true;}
Result<bool> SerialTransport::open(){close();speed_t speed;switch(baud_){case 9600:speed=B9600;break;case 19200:speed=B19200;break;case 38400:speed=B38400;break;case 57600:speed=B57600;break;case 115200:speed=B115200;break;default:return Error::InvalidArgument;}fd_=::open(path_.c_str(),O_RDWR|O_NOCTTY|O_NONBLOCK|O_CLOEXEC);if(fd_<0){error_=errno;return Error::Io;}termios t{};if(tcgetattr(fd_,&t)<0){error_=errno;close();return Error::Io;}cfmakeraw(&t);t.c_cflag|=CLOCAL|CREAD;t.c_cflag&=~CSTOPB;t.c_cc[VMIN]=1;t.c_cc[VTIME]=0;cfsetispeed(&t,speed);cfsetospeed(&t,speed);if(tcsetattr(fd_,TCSANOW,&t)<0){error_=errno;close();return Error::Io;}error_=0;return true;}
}
