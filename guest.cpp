#include "guest.h"
#include "net.h"
#include "responser.h"

#include <set>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>


void guesttick(Guest* guest){
    guest->request_next();
}

Guest::Guest(int fd,  struct sockaddr_in6 *myaddr): Requester(fd, myaddr) {
    add_tick_func((void (*)(void *))guesttick, this);
}


void Guest::ResetResponser(Responser *r, uint32_t id){
    assert(id == 1);
    assert(r);
    responser_ptr = r;
}

void Guest::request_next() {
    while(status == Status::none && reqs.size()){
        HttpReq req = std::move(reqs.front());
        reqs.pop();
        Responser* responser = distribute(req.header, responser_ptr, responser_id);
        if(responser){
            if(req.header.ismethod("CONNECT")){
                status = Status::presistent;
            }else{
                status = Status::requesting;
            }
            responser_ptr = responser;
            responser_id = responser_ptr->request(std::move(req.header));
            while(1){
                auto block = req.body.pop();
                if(block.second == 0)
                    break;
                responser_ptr->Write(block.first, block.second, responser_id);
            }
        }
    }
}


void Guest::defaultHE(uint32_t events) {
    if (events & EPOLLERR || events & EPOLLHUP) {
        int       error = 0;
        socklen_t errlen = sizeof(error);

        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (void*)&error, &errlen) == 0) {
            LOGE("(%s): guest error:%s\n", getsrc(), strerror(error));
        }
        clean(INTERNAL_ERR, 0);
    }

    if (events & EPOLLIN ) {
        (this->*Http_Proc)();
    }

    if (events & EPOLLOUT) {
        int ret = Peer::Write_buff();
        if (ret <= 0) {
            if (showerrinfo(ret, "guest write error")) {
                clean(WRITE_ERR, 0);
            }
            return;
        }
        if (ret != WRITE_NOTHING && responser_ptr){
            responser_ptr->writedcb(responser_id);
        }

    }
}

ssize_t Guest::Read(void* buff, size_t len){
    return Peer::Read(buff, len);
}

void Guest::ErrProc(int errcode) {
    if (showerrinfo(errcode, "Guest-Http error")) {
        clean(errcode, 0);
    }
}

void Guest::ReqProc(HttpReqHeader&& req) {
    req.http_id = 1;
    if(status == Status::none && reqs.empty()){
        Responser* responser = distribute(req, responser_ptr, responser_id);
        if(responser){
            if(req.ismethod("CONNECT")){
                status = Status::presistent;
            }else{
                status = Status::requesting;
            }
            responser_ptr = responser;
            responser_id = responser_ptr->request(std::move(req));
        }
    }else{
        reqs.push(HttpReq(req));
    }
}

void Guest::response(HttpResHeader&& res) {
    assert(res.http_id == 1);
    if(status == Status::presistent){
        if(memcmp(res.status, "200", 3) == 0){
            strcpy(res.status, "200 Connection established");
            res.del("Transfer-Encoding");
        }
    }else if(res.get("Transfer-Encoding")){
        status = Status::chunked;
    }else if(res.no_body()){
        status = Status::none;
    }
    size_t len;
    char *buff=res.getstring(len);
    Requester::Write(buff, len, 0);
}

ssize_t Guest::Write(void *buff, size_t size, uint32_t id) {
    assert(id == 1);
    size_t ret;
    size_t len = Min(bufleft(responser_id), size);
    if(status == Status::chunked){
        char chunkbuf[100];
        int chunklen = snprintf(chunkbuf, sizeof(chunkbuf), "%x" CRLF, (uint32_t)size);
        buff = p_move(buff, -chunklen);
        memcpy(buff, chunkbuf, chunklen);
        ret = Requester::Write(buff, chunklen + len, 0);
        if(ret > 0){
            assert(ret >= (size_t)chunklen);
            if(Requester::Write(p_memdup(CRLF, strlen(CRLF)), strlen(CRLF), id) != strlen(CRLF))
                assert(0);
            ret -= chunklen;
        }
    }else{
        ret =  Requester::Write(buff, size, 0);
    }
    if(size == 0){
        status = Status::none;
    }
    return ret;
}

ssize_t Guest::DataProc(const void *buff, size_t size) {
    if (responser_ptr == nullptr) {
        LOGE("(%s): connecting to host lost\n", getsrc());
        clean(PEER_LOST_ERR, 0);
        return -1;
    }
    int len = responser_ptr->bufleft(responser_id);
    if (len <= 0) {
        LOGE("(%s): The host's buff is full\n", getsrc());
        responser_ptr->wait(responser_id);
        updateEpoll(0);
        return -1;
    }
    if(reqs.size()){
        return reqs.back().body.push(buff, size);
    }else{
        return responser_ptr->Write(buff, Min(size, len), responser_id);
    }
}

void Guest::clean(uint32_t errcode, uint32_t id) {
    assert(id == 0 || id == 1);
    if(id == 0 && responser_ptr){
        responser_ptr->clean(errcode, responser_id);
    }
    responser_ptr = nullptr;
    del_tick_func((void (*)(void *))guesttick, this);
    Peer::clean(errcode, 0);
}

void Guest::discard() {
    responser_ptr = nullptr;
    Requester::discard();
}
