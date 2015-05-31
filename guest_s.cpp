#include "guest_s.h"
#include "host.h"
#include "file.h"
#include "cgi.h"

#include <openssl/err.h>

Guest_s::Guest_s(int fd, struct sockaddr_in6 *myaddr, SSL* ssl): Guest(fd, myaddr), ssl(ssl) {
    struct epoll_event event;
    event.data.ptr = this;
    event.events = EPOLLIN | EPOLLOUT;
    epoll_ctl(efd, EPOLL_CTL_MOD, fd, &event);

    handleEvent = (void (Con::*)(uint32_t))&Guest_s::shakehandHE;
}

Guest_s::Guest_s(Guest_s* copy) {
    *this = *copy;

    copy->fd = 0;
    copy->ssl = NULL;
    delete copy;

    struct epoll_event event;
    event.data.ptr = this;
    event.events = EPOLLIN | EPOLLOUT;
    epoll_ctl(efd, EPOLL_CTL_MOD, fd, &event);
}


Guest_s::~Guest_s() {
    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
}

ssize_t Guest_s::Read(void* buff, size_t size) {
    return SSL_read(ssl, buff, size);
}


ssize_t Guest_s::Write() {
    ssize_t ret = SSL_write(ssl, wbuff, writelen);

    if (ret <= 0) {
        return ret;
    }

    if ((size_t)ret != writelen) {
        memmove(wbuff, wbuff + ret, writelen - ret);
        writelen -= ret;
    } else {
        writelen = 0;
    }

    return ret;
}



void Guest_s::shakedhand() {
    epoll_event event;
    event.data.ptr = this;
    event.events = EPOLLIN;
    epoll_ctl(efd, EPOLL_CTL_MOD, fd, &event);
    handleEvent = (void (Con::*)(uint32_t))&Guest_s::defaultHE;

    const unsigned char *data;
    unsigned int len;
    SSL_get0_next_proto_negotiated(ssl, &data, &len);
    if (data) {
        if (strncasecmp((const char*)data, "spdy/3.1", len) == 0) {
            return;
        }
    }
}

int Guest_s::showerrinfo(int ret, const char* s) {
    epoll_event event;
    event.data.ptr = this;
    int error = SSL_get_error(ssl, ret);
    ERR_clear_error();
    switch (error) {
    case SSL_ERROR_WANT_READ:
        return 0;
    case SSL_ERROR_WANT_WRITE:
        event.events = EPOLLIN|EPOLLOUT;
        epoll_ctl(efd, EPOLL_CTL_MOD, fd, &event);
        return 0;
    case SSL_ERROR_ZERO_RETURN:
        break;
    case SSL_ERROR_SYSCALL:
        LOGE("([%s]:%d): %s:%s\n",
              sourceip, sourceport, s, strerror(errno));
        break;
    default:
        LOGE("([%s]:%d): %s:%s\n",
              sourceip, sourceport, s, ERR_error_string(error, NULL));
    }
    return 1;
}



void Guest_s::shakehandHE(uint32_t events) {
    if ((events & EPOLLIN)|| (events & EPOLLOUT)) {
        int ret = SSL_do_handshake(ssl);
        if (ret != 1) {
            if (showerrinfo(ret, "ssl accept error")) {
                clean(this);
            }
        } else {
            shakedhand();
        }
    }

    if (events & EPOLLERR || events & EPOLLHUP) {
        int       error = 0;
        socklen_t errlen = sizeof(error);

        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (void*)&error, &errlen) == 0) {
            LOGE("([%s]:%d): guest_s error:%s\n",
                  sourceip, sourceport, strerror(error));
        }
        clean(this);
    }
}


void Guest_s::ReqProc(HttpReqHeader& req) {
    LOG("([%s]:%d): %s %s\n", sourceip, sourceport, req.method, req.url);
    if (req.url[0] == '/') {
        if(req.parse()){
            LOG("([%s]:%d): parse url failed\n", sourceip, sourceport);
            throw 0;
        }
        if (strcmp(req.extname,".so") == 0) {
            Cgi::getcgi(req, this);
        } else {
            File::getfile(req,this);
        }
    } else {
        Host::gethost(req, this);
    }
}

