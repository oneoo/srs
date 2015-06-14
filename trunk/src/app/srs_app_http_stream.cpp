/*
The MIT License (MIT)

Copyright (c) 2013-2015 SRS(simple-rtmp-server)

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <srs_app_http_stream.hpp>

#if defined(SRS_AUTO_HTTP_CORE)

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>

#include <sstream>
using namespace std;

#include <srs_protocol_buffer.hpp>
#include <srs_rtmp_utility.hpp>
#include <srs_kernel_log.hpp>
#include <srs_kernel_error.hpp>
#include <srs_app_st.hpp>
#include <srs_core_autofree.hpp>
#include <srs_app_config.hpp>
#include <srs_kernel_utility.hpp>
#include <srs_kernel_file.hpp>
#include <srs_kernel_flv.hpp>
#include <srs_rtmp_stack.hpp>
#include <srs_app_source.hpp>
#include <srs_rtmp_msg_array.hpp>
#include <srs_kernel_aac.hpp>
#include <srs_kernel_mp3.hpp>
#include <srs_kernel_ts.hpp>
#include <srs_app_pithy_print.hpp>
#include <srs_app_source.hpp>
#include <srs_app_server.hpp>

#endif

#ifdef SRS_AUTO_HTTP_SERVER

SrsStreamCache::SrsStreamCache(SrsSource* s, SrsRequest* r)
{
    req = r->copy();
    source = s;
    queue = new SrsMessageQueue(true);
    pthread = new SrsEndlessThread("http-stream", this);
}

SrsStreamCache::~SrsStreamCache()
{
    srs_freep(pthread);
    
    srs_freep(queue);
    srs_freep(req);
}

int SrsStreamCache::start()
{
    return pthread->start();
}

int SrsStreamCache::dump_cache(SrsConsumer* consumer, SrsRtmpJitterAlgorithm jitter)
{
    int ret = ERROR_SUCCESS;

    double fast_cache = _srs_config->get_vhost_http_remux_fast_cache(req->vhost);

    if (fast_cache <= 0) {
        srs_info("http: ignore dump fast cache.");
        return ret;
    }
    
    // the jitter is get from SrsSource, which means the time_jitter of vhost.
    if ((ret = queue->dump_packets(consumer, false, jitter)) != ERROR_SUCCESS) {
        return ret;
    }
    
    srs_trace("http: dump cache %d msgs, duration=%dms, cache=%.2fs", 
        queue->size(), queue->duration(), fast_cache);
    
    return ret;
}

int SrsStreamCache::cycle()
{
    int ret = ERROR_SUCCESS;
    
    SrsConsumer* consumer = NULL;
    if ((ret = source->create_consumer(consumer, false, false, true)) != ERROR_SUCCESS) {
        srs_error("http: create consumer failed. ret=%d", ret);
        return ret;
    }
    SrsAutoFree(SrsConsumer, consumer);

    SrsPithyPrint* pprint = SrsPithyPrint::create_http_stream_cache();
    SrsAutoFree(SrsPithyPrint, pprint);
    
    SrsMessageArray msgs(SRS_PERF_MW_MSGS);

    // TODO: FIXME: support reload.
    double fast_cache = _srs_config->get_vhost_http_remux_fast_cache(req->vhost);
    if (fast_cache > 0) {
        queue->set_queue_size(fast_cache);
    }
    
    while (true) {
        pprint->elapse();

        // get messages from consumer.
        // each msg in msgs.msgs must be free, for the SrsMessageArray never free them.
        int count = 0;
        if ((ret = consumer->dump_packets(&msgs, count)) != ERROR_SUCCESS) {
            srs_error("http: get messages from consumer failed. ret=%d", ret);
            return ret;
        }
        
        if (count <= 0) {
            srs_info("http: mw sleep %dms for no msg", mw_sleep);
            // directly use sleep, donot use consumer wait.
            st_usleep(SRS_CONSTS_RTMP_PULSE_TIMEOUT_US);
            
            // ignore when nothing got.
            continue;
        }

        if (pprint->can_print()) {
            srs_trace("-> "SRS_CONSTS_LOG_HTTP_STREAM_CACHE" http: got %d msgs, age=%d, min=%d, mw=%d", 
                count, pprint->age(), SRS_PERF_MW_MIN_MSGS, SRS_CONSTS_RTMP_PULSE_TIMEOUT_US / 1000);
        }
    
        // free the messages.
        for (int i = 0; i < count; i++) {
            SrsSharedPtrMessage* msg = msgs.msgs[i];
            if (fast_cache > 0) {
                queue->enqueue(msg);
            } else {
                srs_freep(msg);
            }
        }
    }
    
    return ret;
}

ISrsStreamEncoder::ISrsStreamEncoder()
{
}

ISrsStreamEncoder::~ISrsStreamEncoder()
{
}

SrsTsStreamEncoder::SrsTsStreamEncoder()
{
    enc = new SrsTsEncoder();
}

SrsTsStreamEncoder::~SrsTsStreamEncoder()
{
    srs_freep(enc);
}

int SrsTsStreamEncoder::initialize(SrsFileWriter* w, SrsStreamCache* /*c*/)
{
    int ret = ERROR_SUCCESS;
    
    if ((ret = enc->initialize(w)) != ERROR_SUCCESS) {
        return ret;
    }
    
    return ret;
}

int SrsTsStreamEncoder::write_audio(int64_t timestamp, char* data, int size)
{
    return enc->write_audio(timestamp, data, size);
}

int SrsTsStreamEncoder::write_video(int64_t timestamp, char* data, int size)
{
    return enc->write_video(timestamp, data, size);
}

int SrsTsStreamEncoder::write_metadata(int64_t /*timestamp*/, char* /*data*/, int /*size*/)
{
    return ERROR_SUCCESS;
}

bool SrsTsStreamEncoder::has_cache()
{
    // for ts stream, use gop cache of SrsSource is ok.
    return false;
}

int SrsTsStreamEncoder::dump_cache(SrsConsumer* /*consumer*/, SrsRtmpJitterAlgorithm /*jitter*/)
{
    // for ts stream, ignore cache.
    return ERROR_SUCCESS;
}

SrsFlvStreamEncoder::SrsFlvStreamEncoder()
{
    enc = new SrsFlvEncoder();
}

SrsFlvStreamEncoder::~SrsFlvStreamEncoder()
{
    srs_freep(enc);
}

int SrsFlvStreamEncoder::initialize(SrsFileWriter* w, SrsStreamCache* /*c*/)
{
    int ret = ERROR_SUCCESS;
    
    if ((ret = enc->initialize(w)) != ERROR_SUCCESS) {
        return ret;
    }
    
    // write flv header.
    if ((ret = enc->write_header())  != ERROR_SUCCESS) {
        return ret;
    }
    
    return ret;
}

int SrsFlvStreamEncoder::write_audio(int64_t timestamp, char* data, int size)
{
    return enc->write_audio(timestamp, data, size);
}

int SrsFlvStreamEncoder::write_video(int64_t timestamp, char* data, int size)
{
    return enc->write_video(timestamp, data, size);
}

int SrsFlvStreamEncoder::write_metadata(int64_t timestamp, char* data, int size)
{
    return enc->write_metadata(SrsCodecFlvTagScript, data, size);
}

bool SrsFlvStreamEncoder::has_cache()
{
    // for flv stream, use gop cache of SrsSource is ok.
    return false;
}

int SrsFlvStreamEncoder::dump_cache(SrsConsumer* /*consumer*/, SrsRtmpJitterAlgorithm /*jitter*/)
{
    // for flv stream, ignore cache.
    return ERROR_SUCCESS;
}

#ifdef SRS_PERF_FAST_FLV_ENCODER
SrsFastFlvStreamEncoder::SrsFastFlvStreamEncoder()
{
}

SrsFastFlvStreamEncoder::~SrsFastFlvStreamEncoder()
{
}

int SrsFastFlvStreamEncoder::write_tags(SrsSharedPtrMessage** msgs, int count)
{
    return enc->write_tags(msgs, count);
}
#endif

SrsAacStreamEncoder::SrsAacStreamEncoder()
{
    enc = new SrsAacEncoder();
    cache = NULL;
}

SrsAacStreamEncoder::~SrsAacStreamEncoder()
{
    srs_freep(enc);
}

int SrsAacStreamEncoder::initialize(SrsFileWriter* w, SrsStreamCache* c)
{
    int ret = ERROR_SUCCESS;
    
    cache = c;
    
    if ((ret = enc->initialize(w)) != ERROR_SUCCESS) {
        return ret;
    }
    
    return ret;
}

int SrsAacStreamEncoder::write_audio(int64_t timestamp, char* data, int size)
{
    return enc->write_audio(timestamp, data, size);
}

int SrsAacStreamEncoder::write_video(int64_t /*timestamp*/, char* /*data*/, int /*size*/)
{
    // aac ignore any flv video.
    return ERROR_SUCCESS;
}

int SrsAacStreamEncoder::write_metadata(int64_t /*timestamp*/, char* /*data*/, int /*size*/)
{
    // aac ignore any flv metadata.
    return ERROR_SUCCESS;
}

bool SrsAacStreamEncoder::has_cache()
{
    return true;
}

int SrsAacStreamEncoder::dump_cache(SrsConsumer* consumer, SrsRtmpJitterAlgorithm jitter)
{
    srs_assert(cache);
    return cache->dump_cache(consumer, jitter);
}

SrsMp3StreamEncoder::SrsMp3StreamEncoder()
{
    enc = new SrsMp3Encoder();
    cache = NULL;
}

SrsMp3StreamEncoder::~SrsMp3StreamEncoder()
{
    srs_freep(enc);
}

int SrsMp3StreamEncoder::initialize(SrsFileWriter* w, SrsStreamCache* c)
{
    int ret = ERROR_SUCCESS;
    
    cache = c;
    
    if ((ret = enc->initialize(w)) != ERROR_SUCCESS) {
        return ret;
    }
    
    if ((ret = enc->write_header()) != ERROR_SUCCESS) {
        return ret;
    }
    
    return ret;
}

int SrsMp3StreamEncoder::write_audio(int64_t timestamp, char* data, int size)
{
    return enc->write_audio(timestamp, data, size);
}

int SrsMp3StreamEncoder::write_video(int64_t /*timestamp*/, char* /*data*/, int /*size*/)
{
    // mp3 ignore any flv video.
    return ERROR_SUCCESS;
}

int SrsMp3StreamEncoder::write_metadata(int64_t /*timestamp*/, char* /*data*/, int /*size*/)
{
    // mp3 ignore any flv metadata.
    return ERROR_SUCCESS;
}

bool SrsMp3StreamEncoder::has_cache()
{
    return true;
}

int SrsMp3StreamEncoder::dump_cache(SrsConsumer* consumer, SrsRtmpJitterAlgorithm jitter)
{
    srs_assert(cache);
    return cache->dump_cache(consumer, jitter);
}

SrsStreamWriter::SrsStreamWriter(ISrsHttpResponseWriter* w)
{
    writer = w;
}

SrsStreamWriter::~SrsStreamWriter()
{
}

int SrsStreamWriter::open(std::string /*file*/)
{
    return ERROR_SUCCESS;
}

void SrsStreamWriter::close()
{
}

bool SrsStreamWriter::is_open()
{
    return true;
}

int64_t SrsStreamWriter::tellg()
{
    return 0;
}

int SrsStreamWriter::write(void* buf, size_t count, ssize_t* pnwrite)
{
    if (pnwrite) {
        *pnwrite = count;
    }
    return writer->write((char*)buf, (int)count);
}

int SrsStreamWriter::writev(iovec* iov, int iovcnt, ssize_t* pnwrite)
{
    return writer->writev(iov, iovcnt, pnwrite);
}

SrsLiveStream::SrsLiveStream(SrsSource* s, SrsRequest* r, SrsStreamCache* c)
{
    source = s;
    cache = c;
    req = r->copy();
}

SrsLiveStream::~SrsLiveStream()
{
    srs_freep(req);
}

int SrsLiveStream::serve_http(ISrsHttpResponseWriter* w, ISrsHttpMessage* r)
{
    int ret = ERROR_SUCCESS;
    
    ISrsStreamEncoder* enc = NULL;
    
    srs_assert(entry);
    if (srs_string_ends_with(entry->pattern, ".flv")) {
        w->header()->set_content_type("video/x-flv");
#ifdef SRS_PERF_FAST_FLV_ENCODER
        enc = new SrsFastFlvStreamEncoder();
#else
        enc = new SrsFlvStreamEncoder();
#endif
    } else if (srs_string_ends_with(entry->pattern, ".aac")) {
        w->header()->set_content_type("audio/x-aac");
        enc = new SrsAacStreamEncoder();
    } else if (srs_string_ends_with(entry->pattern, ".mp3")) {
        w->header()->set_content_type("audio/mpeg");
        enc = new SrsMp3StreamEncoder();
    } else if (srs_string_ends_with(entry->pattern, ".ts")) {
        w->header()->set_content_type("video/MP2T");
        enc = new SrsTsStreamEncoder();
    } else {
        ret = ERROR_HTTP_LIVE_STREAM_EXT;
        srs_error("http: unsupported pattern %s", entry->pattern.c_str());
        return ret;
    }
    SrsAutoFree(ISrsStreamEncoder, enc);
    
    // create consumer of souce, ignore gop cache, use the audio gop cache.
    SrsConsumer* consumer = NULL;
    if ((ret = source->create_consumer(consumer, true, true, !enc->has_cache())) != ERROR_SUCCESS) {
        srs_error("http: create consumer failed. ret=%d", ret);
        return ret;
    }
    SrsAutoFree(SrsConsumer, consumer);
    srs_verbose("http: consumer created success.");

    SrsPithyPrint* pprint = SrsPithyPrint::create_http_stream();
    SrsAutoFree(SrsPithyPrint, pprint);
    
    SrsMessageArray msgs(SRS_PERF_MW_MSGS);
    
    // the memory writer.
    SrsStreamWriter writer(w);
    if ((ret = enc->initialize(&writer, cache)) != ERROR_SUCCESS) {
        srs_error("http: initialize stream encoder failed. ret=%d", ret);
        return ret;
    }
    
    // if gop cache enabled for encoder, dump to consumer.
    if (enc->has_cache()) {
        if ((ret = enc->dump_cache(consumer, source->jitter())) != ERROR_SUCCESS) {
            srs_error("http: dump cache to consumer failed. ret=%d", ret);
            return ret;
        }
    }
    
#ifdef SRS_PERF_FAST_FLV_ENCODER
    SrsFastFlvStreamEncoder* ffe = dynamic_cast<SrsFastFlvStreamEncoder*>(enc);
#endif
    
    while (true) {
        pprint->elapse();

        // get messages from consumer.
        // each msg in msgs.msgs must be free, for the SrsMessageArray never free them.
        int count = 0;
        if ((ret = consumer->dump_packets(&msgs, count)) != ERROR_SUCCESS) {
            srs_error("http: get messages from consumer failed. ret=%d", ret);
            return ret;
        }
        
        if (count <= 0) {
            srs_info("http: mw sleep %dms for no msg", mw_sleep);
            // directly use sleep, donot use consumer wait.
            st_usleep(SRS_CONSTS_RTMP_PULSE_TIMEOUT_US);
            
            // ignore when nothing got.
            continue;
        }

        if (pprint->can_print()) {
            srs_info("-> "SRS_CONSTS_LOG_HTTP_STREAM" http: got %d msgs, age=%d, min=%d, mw=%d", 
                count, pprint->age(), SRS_PERF_MW_MIN_MSGS, SRS_CONSTS_RTMP_PULSE_TIMEOUT_US / 1000);
        }
        
        // sendout all messages.
#ifdef SRS_PERF_FAST_FLV_ENCODER
        if (ffe) {
            ret = ffe->write_tags(msgs.msgs, count);
        } else {
            ret = streaming_send_messages(enc, msgs.msgs, count);
        }
#else
        ret = streaming_send_messages(enc, msgs.msgs, count);
#endif
    
        // free the messages.
        for (int i = 0; i < count; i++) {
            SrsSharedPtrMessage* msg = msgs.msgs[i];
            srs_freep(msg);
        }
        
        // check send error code.
        if (ret != ERROR_SUCCESS) {
            if (!srs_is_client_gracefully_close(ret)) {
                srs_error("http: send messages to client failed. ret=%d", ret);
            }
            return ret;
        }
    }
    
    return ret;
}

int SrsLiveStream::streaming_send_messages(ISrsStreamEncoder* enc, SrsSharedPtrMessage** msgs, int nb_msgs)
{
    int ret = ERROR_SUCCESS;
    
    for (int i = 0; i < nb_msgs; i++) {
        SrsSharedPtrMessage* msg = msgs[i];
        
        if (msg->is_audio()) {
            ret = enc->write_audio(msg->timestamp, msg->payload, msg->size);
        } else if (msg->is_video()) {
            ret = enc->write_video(msg->timestamp, msg->payload, msg->size);
        } else {
            ret = enc->write_metadata(msg->timestamp, msg->payload, msg->size);
        }
        
        if (ret != ERROR_SUCCESS) {
            return ret;
        }
    }
    
    return ret;
}

SrsLiveEntry::SrsLiveEntry(std::string m, bool h)
{
    mount = m;
    hstrs = h;
    
    stream = NULL;
    cache = NULL;
    
    std::string ext;
    size_t pos = string::npos;
    if ((pos = m.rfind(".")) != string::npos) {
        ext = m.substr(pos);
    }
    _is_flv = (ext == ".flv");
    _is_ts = (ext == ".ts");
    _is_mp3 = (ext == ".mp3");
    _is_aac = (ext == ".aac");
}

bool SrsLiveEntry::is_flv()
{
    return _is_flv;
}

bool SrsLiveEntry::is_ts()
{
    return _is_ts;
}

bool SrsLiveEntry::is_aac()
{
    return _is_aac;
}

bool SrsLiveEntry::is_mp3()
{
    return _is_mp3;
}

SrsHlsM3u8Stream::SrsHlsM3u8Stream()
{
}

SrsHlsM3u8Stream::~SrsHlsM3u8Stream()
{
}

void SrsHlsM3u8Stream::set_m3u8(std::string v)
{
    m3u8 = v;
}

int SrsHlsM3u8Stream::serve_http(ISrsHttpResponseWriter* w, ISrsHttpMessage* r)
{
    int ret = ERROR_SUCCESS;
    
    std::string data = m3u8;
    
    w->header()->set_content_length((int)data.length());
    w->header()->set_content_type("application/x-mpegURL;charset=utf-8");

    if ((ret = w->write((char*)data.data(), (int)data.length())) != ERROR_SUCCESS) {
        if (!srs_is_client_gracefully_close(ret)) {
            srs_error("send m3u8 failed. ret=%d", ret);
        }
        return ret;
    }

    return ret;
}

SrsHlsTsStream::SrsHlsTsStream()
{
}

SrsHlsTsStream::~SrsHlsTsStream()
{
}

void SrsHlsTsStream::set_ts(std::string v)
{
    ts = v;
}

int SrsHlsTsStream::serve_http(ISrsHttpResponseWriter* w, ISrsHttpMessage* r)
{
    int ret = ERROR_SUCCESS;
    
    std::string data = ts;
    
    w->header()->set_content_length((int)data.length());
    w->header()->set_content_type("video/MP2T");

    if ((ret = w->write((char*)data.data(), (int)data.length())) != ERROR_SUCCESS) {
        if (!srs_is_client_gracefully_close(ret)) {
            srs_error("send ts failed. ret=%d", ret);
        }
        return ret;
    }

    return ret;
}

SrsHlsEntry::SrsHlsEntry()
{
    tmpl = NULL;
}

SrsHttpStreamServer::SrsHttpStreamServer(SrsServer* svr)
{
    server = svr;
    
    mux.hijack(this);
}

SrsHttpStreamServer::~SrsHttpStreamServer()
{
    mux.unhijack(this);
    
    if (true) {
        std::map<std::string, SrsLiveEntry*>::iterator it;
        for (it = tflvs.begin(); it != tflvs.end(); ++it) {
            SrsLiveEntry* entry = it->second;
            srs_freep(entry);
        }
        tflvs.clear();
    }
    if (true) {
        std::map<std::string, SrsLiveEntry*>::iterator it;
        for (it = sflvs.begin(); it != sflvs.end(); ++it) {
            SrsLiveEntry* entry = it->second;
            srs_freep(entry);
        }
        sflvs.clear();
    }
    if (true) {
        std::map<std::string, SrsHlsEntry*>::iterator it;
        for (it = thls.begin(); it != thls.end(); ++it) {
            SrsHlsEntry* entry = it->second;
            srs_freep(entry);
        }
        thls.clear();
    }
    if (true) {
        std::map<std::string, SrsHlsEntry*>::iterator it;
        for (it = shls.begin(); it != shls.end(); ++it) {
            SrsHlsEntry* entry = it->second;
            srs_freep(entry);
        }
        shls.clear();
    }
}

int SrsHttpStreamServer::initialize()
{
    int ret = ERROR_SUCCESS;
    
    // remux rtmp to flv live streaming
    if ((ret = initialize_flv_streaming()) != ERROR_SUCCESS) {
        return ret;
    }
    
    // remux rtmp to hls live streaming
    if ((ret = initialize_hls_streaming()) != ERROR_SUCCESS) {
        return ret;
    }
    
    return ret;
}

// TODO: FIXME: rename for HTTP FLV mount.
int SrsHttpStreamServer::http_mount(SrsSource* s, SrsRequest* r)
{
    int ret = ERROR_SUCCESS;
    
    // the id to identify stream.
    std::string sid = r->get_stream_url();
    SrsLiveEntry* entry = NULL;
    
    // create stream from template when not found.
    if (sflvs.find(sid) == sflvs.end()) {
        if (tflvs.find(r->vhost) == tflvs.end()) {
            srs_info("ignore mount flv stream for disabled");
            return ret;
        }
    
        SrsLiveEntry* tmpl = tflvs[r->vhost];

        std::string mount = tmpl->mount;
    
        // replace the vhost variable
        mount = srs_string_replace(mount, "[vhost]", r->vhost);
        mount = srs_string_replace(mount, "[app]", r->app);
        mount = srs_string_replace(mount, "[stream]", r->stream);
    
        // remove the default vhost mount
        mount = srs_string_replace(mount, SRS_CONSTS_RTMP_DEFAULT_VHOST"/", "/");
        
        entry = new SrsLiveEntry(mount, tmpl->hstrs);
    
        entry->cache = new SrsStreamCache(s, r);
        entry->stream = new SrsLiveStream(s, r, entry->cache);
        
        sflvs[sid] = entry;
        
        // mount the http flv stream.
        // we must register the handler, then start the thread,
        // for the thread will cause thread switch context.
        // @see https://github.com/simple-rtmp-server/srs/issues/404
        if ((ret = mux.handle(mount, entry->stream)) != ERROR_SUCCESS) {
            srs_error("http: mount flv stream for vhost=%s failed. ret=%d", sid.c_str(), ret);
            return ret;
        }
        
        // start http stream cache thread
        if ((ret = entry->cache->start()) != ERROR_SUCCESS) {
            srs_error("http: start stream cache failed. ret=%d", ret);
            return ret;
        }
        srs_trace("http: mount flv stream for vhost=%s, mount=%s", sid.c_str(), mount.c_str());
    } else {
        entry = sflvs[sid];
    }
    
    // TODO: FIXME: supports reload.
    if (entry->stream) {
        entry->stream->entry->enabled = true;
        return ret;
    }
    
    return ret;
}

void SrsHttpStreamServer::http_unmount(SrsSource* s, SrsRequest* r)
{
    std::string sid = r->get_stream_url();
    
    if (sflvs.find(sid) == sflvs.end()) {
        srs_info("ignore unmount flv stream for disabled");
        return;
    }

    SrsLiveEntry* entry = sflvs[sid];
    entry->stream->entry->enabled = false;
}

int SrsHttpStreamServer::mount_hls(SrsRequest* r)
{
    int ret = ERROR_SUCCESS;
    
    std::string sid = r->get_stream_url();
    
    if (shls.find(sid) == shls.end()) {
        srs_info("ignore mount hls stream for disabled");
        return ret;
    }
    
    SrsHlsEntry* entry = shls[sid];
    
    // TODO: FIXME: supports reload.
    std::map<std::string, ISrsHttpHandler*>::iterator it;
    for (it = entry->streams.begin(); it != entry->streams.end(); ++it) {
        ISrsHttpHandler* stream = it->second;
        stream->entry->enabled = true;
    }

    return ret;
}

int SrsHttpStreamServer::hls_update_m3u8(SrsRequest* r, string m3u8)
{
    int ret = ERROR_SUCCESS;

    std::string mount;
    
    std::string sid = r->get_stream_url();
    SrsHlsEntry* entry = NULL;
    
    // create stream from template when not found.
    if (shls.find(sid) == shls.end()) {
        if (thls.find(r->vhost) == thls.end()) {
            srs_info("ignore mount hls stream for disabled");
            return ret;
        }
    
        SrsHlsEntry* tmpl = thls[r->vhost];
        srs_assert(tmpl);
        
        entry = new SrsHlsEntry();
        mount = tmpl->mount;
        
        // replace the vhost variable
        mount = srs_string_replace(mount, "[vhost]", r->vhost);
        mount = srs_string_replace(mount, "[app]", r->app);
        mount = srs_string_replace(mount, "[stream]", r->stream);
    
        // remove the default vhost mount
        mount = srs_string_replace(mount, SRS_CONSTS_RTMP_DEFAULT_VHOST"/", "/");
        
        entry->tmpl = tmpl;
        entry->mount = mount;
        shls[sid] = entry;
    
        if (entry->streams.find(mount) == entry->streams.end()) {
            ISrsHttpHandler* he = new SrsHlsM3u8Stream();
            entry->streams[mount] = he;
    
            if ((ret = mux.handle(mount, he)) != ERROR_SUCCESS) {
                srs_error("handle mount=%s failed. ret=%d", mount.c_str(), ret);
                return ret;
            }
        }
    } else {
        entry = shls[sid];
    }

    mount = entry->mount;

    // update the m3u8 stream.
    SrsHlsM3u8Stream* hms = dynamic_cast<SrsHlsM3u8Stream*>(entry->streams[mount]);
    if (hms) {
        hms->set_m3u8(m3u8);
    }
    srs_trace("hls update m3u8 ok, mount=%s", mount.c_str());

    return ret;
}

int SrsHttpStreamServer::hls_update_ts(SrsRequest* r, string uri, string ts)
{
    int ret = ERROR_SUCCESS;
    
    std::string sid = r->get_stream_url();
    
    // when no hls mounted, init with empty m3u8.
    if (shls.find(sid) == shls.end()) {
        if ((ret = hls_update_m3u8(r, "")) != ERROR_SUCCESS) {
            return ret;
        }
    }
    
    // find again, ignore if not exits.
    if (shls.find(sid) == shls.end()) {
        return ret;
    }

    SrsHlsEntry* entry = shls[sid];
    srs_assert(entry);
    srs_assert(entry->tmpl);

    std::string mount = hls_mount_generate(r, uri, entry->tmpl->mount);

    if (entry->streams.find(mount) == entry->streams.end()) {
        ISrsHttpHandler* he = new SrsHlsTsStream();
        entry->streams[mount] = he;

        if ((ret = mux.handle(mount, he)) != ERROR_SUCCESS) {
            srs_error("handle mount=%s failed. ret=%d", mount.c_str(), ret);
            return ret;
        }
    }

    // update the ts stream.
    SrsHlsTsStream* hts = dynamic_cast<SrsHlsTsStream*>(entry->streams[mount]);
    if (hts) {
        hts->set_ts(ts);
    }
    srs_trace("hls update ts ok, mount=%s", mount.c_str());

    return ret;
}


int SrsHttpStreamServer::hls_remove_ts(SrsRequest* r, string uri)
{
    int ret = ERROR_SUCCESS;
    
    std::string sid = r->get_stream_url();
    
    // when no hls mounted, ignore.
    if (shls.find(sid) == shls.end()) {
        return ret;
    }
    
    SrsHlsEntry* entry = shls[sid];
    srs_assert(entry);
    srs_assert(entry->tmpl);
    
    std::string mount = hls_mount_generate(r, uri, entry->tmpl->mount);
    
    // ignore when no ts mounted.
    if (entry->streams.find(mount) == entry->streams.end()) {
        return ret;
    }
    
    // update the ts stream.
    SrsHlsTsStream* hts = dynamic_cast<SrsHlsTsStream*>(entry->streams[mount]);
    if (hts) {
        hts->set_ts("");
        // TODO: FIXME: unmount and remove the http handler.
    }
    srs_trace("hls remove ts ok, mount=%s", mount.c_str());
    
    return ret;
}

void SrsHttpStreamServer::unmount_hls(SrsRequest* r)
{
    std::string sid = r->get_stream_url();
    
    if (shls.find(sid) == shls.end()) {
        srs_info("ignore unmount hls stream for disabled");
        return;
    }

    SrsHlsEntry* entry = shls[sid];

    std::map<std::string, ISrsHttpHandler*>::iterator it;
    for (it = entry->streams.begin(); it != entry->streams.end(); ++it) {
        ISrsHttpHandler* stream = it->second;
        stream->entry->enabled = false;
    }
}

int SrsHttpStreamServer::on_reload_vhost_http_remux_updated()
{
    int ret = ERROR_SUCCESS;
    // TODO: FIXME: implements it.
    return ret;
}

int SrsHttpStreamServer::on_reload_vhost_hls(string vhost)
{
    int ret = ERROR_SUCCESS;
    // TODO: FIXME: implements it.
    return ret;
}

int SrsHttpStreamServer::hijack(ISrsHttpMessage* request, ISrsHttpHandler** ph)
{
    int ret = ERROR_SUCCESS;
    
    // when handler not the root, we think the handler is ok.
    ISrsHttpHandler* h = *ph? *ph : NULL;
    if (h && h->entry && h->entry->pattern != "/") {
        return ret;
    }
    
    // only hijack for http streaming, http-flv/ts/mp3/aac.
    std::string ext = request->ext();
    if (ext.empty()) {
        return ret;
    }
    
    // find the actually request vhost.
    SrsConfDirective* vhost = _srs_config->get_vhost(request->host());
    if (!vhost || !_srs_config->get_vhost_enabled(vhost)) {
        return ret;
    }
    
    // find the entry template for the stream.
    SrsLiveEntry* entry = NULL;
    if (true) {
        // no http streaming on vhost, ignore.
        std::map<std::string, SrsLiveEntry*>::iterator it = tflvs.find(vhost->arg0());
        if (it == tflvs.end()) {
            return ret;
        }
        
        // hstrs not enabled, ignore.
        entry = it->second;
        if (!entry->hstrs) {
            return ret;
        }

        // check entry and request extension.
        if (entry->is_flv()) {
            if (ext != ".flv") {
                return ret;
            }
        } else if (entry->is_ts()) {
            if (ext != ".ts") {
                return ret;
            }
        } else if (entry->is_mp3()) {
            if (ext != ".mp3") {
                return ret;
            }
        } else if (entry->is_aac()) {
            if (ext != ".aac") {
                return ret;
            }
        } else {
            return ret;
        }
    }
    
    // convert to concreate class.
    SrsHttpMessage* hreq = dynamic_cast<SrsHttpMessage*>(request);
    srs_assert(hreq);
    
    // hijack for entry.
    SrsRequest* r = hreq->to_request(vhost->arg0());
    SrsAutoFree(SrsRequest, r);
    SrsSource* s = SrsSource::fetch(r);
    if (!s) {
        if ((ret = SrsSource::create(r, server, server, &s)) != ERROR_SUCCESS) {
            return ret;
        }
    }
    srs_assert(s != NULL);
    
    // create http streaming handler.
    if ((ret = http_mount(s, r)) != ERROR_SUCCESS) {
        return ret;
    }
    
    // use the handler if exists.
    if (ph) {
        std::string sid = r->get_stream_url();
        if (sflvs.find(sid) != sflvs.end()) {
            entry = sflvs[sid];
            *ph = entry->stream;
        }
    }
    
    // trigger edge to fetch from origin.
    bool vhost_is_edge = _srs_config->get_vhost_is_edge(r->vhost);
    srs_trace("hstrs: source url=%s, is_edge=%d, source_id=%d[%d]",
        r->get_stream_url().c_str(), vhost_is_edge, s->source_id(), s->source_id());
    
    // TODO: FIXME: disconnect when all connection closed.
    if (vhost_is_edge) {
        // notice edge to start for the first client.
        if ((ret = s->on_edge_start_play()) != ERROR_SUCCESS) {
            srs_error("notice edge start play stream failed. ret=%d", ret);
            return ret;
        }
    }
    
    return ret;
}

int SrsHttpStreamServer::initialize_flv_streaming()
{
    int ret = ERROR_SUCCESS;
    
    // http flv live stream mount for each vhost.
    SrsConfDirective* root = _srs_config->get_root();
    for (int i = 0; i < (int)root->directives.size(); i++) {
        SrsConfDirective* conf = root->at(i);
        
        if (!conf->is_vhost()) {
            continue;
        }
        
        std::string vhost = conf->arg0();
        if (!_srs_config->get_vhost_http_remux_enabled(vhost)) {
            continue;
        }
        
        SrsLiveEntry* entry = new SrsLiveEntry(
            _srs_config->get_vhost_http_remux_mount(vhost),
            _srs_config->get_vhost_http_remux_hstrs(vhost)
        );
        tflvs[vhost] = entry;
        srs_trace("http flv live stream, vhost=%s, mount=%s", 
            vhost.c_str(), entry->mount.c_str());
    }
    
    return ret;
}

int SrsHttpStreamServer::initialize_hls_streaming()
{
    int ret = ERROR_SUCCESS;
    
    // http hls live stream mount for each vhost.
    SrsConfDirective* root = _srs_config->get_root();
    for (int i = 0; i < (int)root->directives.size(); i++) {
        SrsConfDirective* conf = root->at(i);
        
        if (!conf->is_vhost()) {
            continue;
        }
        
        std::string vhost = conf->arg0();
        if (!_srs_config->get_hls_enabled(vhost)) {
            continue;
        }

        std::string storage = _srs_config->get_hls_storage(vhost);
        if (storage != "ram" && storage != "both") {
            continue;
        }
        
        SrsHlsEntry* entry = new SrsHlsEntry();
        entry->mount = _srs_config->get_hls_mount(vhost);
        thls[vhost] = entry;
        srs_trace("http hls live stream, vhost=%s, mount=%s", 
            vhost.c_str(), entry->mount.c_str());
    }
    
    return ret;
}

string SrsHttpStreamServer::hls_mount_generate(SrsRequest* r, string uri, string tmpl)
{
    std::string mount = tmpl;
    
    // the ts is relative from the m3u8, the same start dir.
    size_t pos = string::npos;
    if ((pos = mount.rfind("/")) != string::npos) {
        mount = mount.substr(0, pos);
    }
    
    // replace the vhost variable
    mount = srs_string_replace(mount, "[vhost]", r->vhost);
    mount = srs_string_replace(mount, "[app]", r->app);
    
    // remove the default vhost mount
    mount = srs_string_replace(mount, SRS_CONSTS_RTMP_DEFAULT_VHOST"/", "/");
    
    // mount with ts.
    mount += "/";
    mount += uri;
    
    return mount;
}

#endif
