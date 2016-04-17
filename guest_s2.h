#ifndef GUEST_S2_H__
#define GUEST_S2_H__

#include "guest_s.h"
#include "http2.h"
#include "binmap.h"

class Guest_s2: public Guest_s, public Http2Res{
    binmap<Peer *, int> idmap;
    std::set<Peer *> waitlist;
    virtual void defaultHE(uint32_t events);
    virtual ssize_t Read(void *buff, size_t size)override;
    virtual ssize_t Write(const void *buff, size_t size)override;
    virtual ssize_t Write(const void *buff, size_t size, Peer *who, uint32_t id=0)override;
    virtual void SendFrame(Http2_header *header)override;
    virtual void GoawayProc(Http2_header *header)override;
    virtual void Response(HttpResHeader& res, Peer *who)override;
    virtual void DataProc(Http2_header *header)override;
    virtual void ReqProc(HttpReqHeader &req)override;
    
    virtual void RstProc(uint32_t id, uint32_t errcode)override;
    virtual void WindowUpdateProc(uint32_t id, uint32_t size)override;
    virtual void ErrProc(int errcode)override;
    virtual void AdjustInitalFrameWindowSize(ssize_t diff)override;
    virtual void clean(uint32_t errcode, Peer *who, uint32_t id = 0)override;
public:
    Guest_s2(Guest_s *const copy);
    virtual int32_t bufleft(Peer*)override;
    virtual void wait(Peer *who)override;
    virtual void writedcb(Peer *who)override;
//    virtual int showstatus(char *buff, Peer *who)override;
};

#endif
