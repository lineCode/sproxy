#include "proxy.h"
#include "proxy2.h"
#include "req/requester.h"
#include "misc/dtls.h"
#include "misc/job.h"

#include <openssl/err.h>

Proxy::Proxy(const char* hostname, uint16_t port, Protocol protocol): 
        Host(hostname, port, protocol){

}

Responser* Proxy::getproxy(HttpReqHeader* req, Responser* responser_ptr) {
    if (proxy2) {
        return proxy2;
    }
    Proxy *proxy = dynamic_cast<Proxy *>(responser_ptr);
    if(req->ismethod("CONNECT") || req->ismethod("SEND")){
        return new Proxy(SHOST, SPORT, SPROT);
    }
    if(proxy){
        return proxy;
    }
    return new Proxy(SHOST, SPORT, SPROT);
}

ssize_t Proxy::Read(void* buff, size_t size) {
    return ssl->read(buff, size);
}


ssize_t Proxy::Write(const void *buff, size_t size) {
    return ssl->write(buff, size);
}

int verify_host_callback(int ok, X509_STORE_CTX *ctx){
    char    buf[256];
    X509   *err_cert;
    int     err, depth;

    err_cert = X509_STORE_CTX_get_current_cert(ctx);
    err = X509_STORE_CTX_get_error(ctx);
    depth = X509_STORE_CTX_get_error_depth(ctx);

    X509_NAME_oneline(X509_get_subject_name(err_cert), buf, 256);

    /*
     * Catch a too long certificate chain. The depth limit set using
     * SSL_CTX_set_verify_depth() is by purpose set to "limit+1" so
     * that whenever the "depth>verify_depth" condition is met, we
     * have violated the limit and want to log this error condition.
     * We must do it here, because the CHAIN_TOO_LONG error would not
     * be found explicitly; only errors introduced by cutting off the
     * additional certificates would be logged.
     */
    if (!ok) {
        LOGE("verify cert error:num=%d:%s:depth=%d:%s\n", err,
                 X509_verify_cert_error_string(err), depth, buf);
    } else {
//        LOG("cert depth=%d:%s\n", depth, buf);
    }

    /*
     * At this point, err contains the last verification error. We can use
     * it for something special
     */
    if (!ok && (err == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT))
    {
        X509_NAME_oneline(X509_get_issuer_name(err_cert), buf, 256);
        LOGE("unable to get issuer= %s\n", buf);
    }

    if (ignore_cert_error)
        return 1;
    else
        return ok; 
}

static const unsigned char alpn_protos_string[] =
    "\x8http/1.1" \
    "\x2h2";


void Proxy::waitconnectHE(uint32_t events) {
    if (events & EPOLLERR || events & EPOLLHUP) {
        int       error = 0;
        socklen_t errlen = sizeof(error);

        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (void*)&error, &errlen) == 0) {
            LOGE("(%s): connect to proxy error: %s\n", hostname, strerror(error));
        }
        goto reconnect;
    }

    if (events & EPOLLOUT) {
        int error;
        socklen_t len = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len)) {
            LOGE("(%s): proxy getsokopt error: %s\n", hostname, strerror(errno));
            goto reconnect;
        }
            
        if (error != 0) {
            LOGE("(%s): connect to proxy:%s\n", hostname, strerror(error));
            goto reconnect;
        }
        if(protocol == Protocol::TCP){
            ctx = SSL_CTX_new(SSLv23_client_method());
            if (ctx == NULL) {
                ERR_print_errors_fp(stderr);
                goto reconnect;
            }
            SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2|SSL_OP_NO_SSLv3);  // 去除支持SSLv2 SSLv3
            SSL_CTX_set_read_ahead(ctx, 1);
            
            SSL *ssl = SSL_new(ctx);
            SSL_set_fd(ssl, fd);
            this->ssl = new Ssl(ssl);
        }else{
            ctx = SSL_CTX_new(DTLS_client_method());
            if (ctx == NULL) {
                ERR_print_errors_fp(stderr);
                goto reconnect;
            }
            SSL *ssl = SSL_new(ctx);
            BIO* bio = BIO_new_dgram(fd, BIO_NOCLOSE);
            BIO_ctrl(bio, BIO_CTRL_DGRAM_SET_CONNECTED, 0, &addrs[testedaddr-1]);
            SSL_set_bio(ssl, bio, bio);
            this->ssl = new Dtls(ssl);
        }
#ifdef __ANDROID__
        if (SSL_CTX_load_verify_locations(ctx, cafile, "/etc/security/cacerts/") != 1)
#else
        if (SSL_CTX_load_verify_locations(ctx, cafile, "/etc/ssl/certs/") != 1)
#endif
            ERR_print_errors_fp(stderr);

        if (SSL_CTX_set_default_verify_paths(ctx) != 1)
            ERR_print_errors_fp(stderr);
        ssl->set_hostname(SHOST, verify_host_callback);
        
        if(use_http2){
            ssl->set_alpn(alpn_protos_string, sizeof(alpn_protos_string)-1);
        }
        
        updateEpoll(EPOLLIN | EPOLLOUT);
        handleEvent = (void (Con::*)(uint32_t))&Proxy::shakehandHE;
    }
    return;
reconnect:
    connect();
}

void Proxy::shakehandHE(uint32_t events) {
    if (events & EPOLLERR || events & EPOLLHUP) {
        int       error = 0;
        socklen_t errlen = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (void*)&error, &errlen) == 0) {
            LOGE("(%s): proxy unkown error: %s\n", hostname, strerror(error));
        }
        deleteLater(INTERNAL_ERR);
        return;
    }

    if ((events & EPOLLIN) || (events & EPOLLOUT)) {
        int ret = ssl->connect();
        if (ret != 1) {
            if (errno != EAGAIN) {
                LOGE("(%s): ssl connect error:%s\n", hostname, strerror(errno));
                deleteLater(SSL_SHAKEHAND_ERR);
            }
            return;
        }
        
        const unsigned char *data;
        unsigned int len;
        ssl->get_alpn(&data, &len);
        if ((data && strncasecmp((const char*)data, "h2", len) == 0))
        {
            Proxy2 *new_proxy = new Proxy2(fd, ctx,ssl);
            new_proxy->init();
            if(!proxy2){
                proxy2 = new_proxy;
            }
            while(!reqs.empty()){
                Requester* req_ptr = reqs.front().header->src;
                void*      req_index = reqs.front().header->index;
                req_ptr->transfer(req_index, new_proxy,
                                  new_proxy->request(std::move(reqs.front())));
                reqs.pop_front();
            }
            this->discard();
            deleteLater(PEER_LOST_ERR);
        }else{
            if(protocol == Protocol::UDP){
                LOGE("Warning: Use http1.1 on dtls!\n");
            }
            updateEpoll(EPOLLIN | EPOLLOUT);
            handleEvent = (void (Con::*)(uint32_t))&Proxy::defaultHE;
        }
        del_delayjob((job_func)con_timeout, this);
        return;
    }
}


void Proxy::discard() {
    ssl = nullptr;
    ctx = nullptr;
    Host::discard();
}



Proxy::~Proxy() {
    if (ssl) {
        delete ssl;
    }
    if (ctx){
        SSL_CTX_free(ctx);
    }
}
