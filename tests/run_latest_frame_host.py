"""执行生产 provider 代码，验证慢消费者收到最新 JPEG 及释放/错误语义。"""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
source = (repo / "main/rtsp_push.c").read_text(encoding="utf-8")
start = source.find("static esp_err_t camera_source_read_latest(")
if start < 0:
    start = source.index("static esp_err_t camera_source_get_provider(")
end = source.index("static esp_err_t camera_source_on_start", start)
production = source[start:end]
stub = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NOT_FOUND 2
#define ESP_ERR_INVALID_SIZE 3
#define ESP_FAIL 4
#define ESP_MEDIA_TRACK_TYPE_VIDEO 1
#define ESP_MEDIA_DEFAULT_STREAM 0
#define CAMERA_TRACK_ID 0
typedef int esp_media_stream_id_t;
typedef struct { int type, track_id; void *data; size_t size; int64_t pts, dts; int flags; } esp_media_frame_t;
typedef struct { int q_num, q_size; } esp_media_track_mngr_queue_info_t;
typedef struct manager { int count, next, held, released, read_timeout, fail_query, fail_acquire, fail_release, aborted, refill; } esp_media_track_mngr_t;
typedef struct { esp_err_t (*read_frame)(void *, esp_media_frame_t *, uint32_t); int (*other)(void *); } esp_media_provider_ops_t;
typedef struct { const esp_media_provider_ops_t *ops; void *ctx; } esp_media_provider_t;
typedef struct { esp_media_track_mngr_t *mngr; esp_media_provider_ops_t provider_ops; } camera_source_t;
typedef camera_source_t esp_service_t;
static int other(void *ctx) { return ((esp_media_track_mngr_t *)ctx)->aborted; }
static esp_err_t raw_read(void *ctx, esp_media_frame_t *out, uint32_t timeout) {
    esp_media_track_mngr_t *m=ctx;
    if(!out || !out->data) return ESP_ERR_INVALID_ARG;
    m->read_timeout=(int)timeout;
    if(m->aborted) return ESP_FAIL;
    if(!m->count) return ESP_ERR_NOT_FOUND;
    int value=++m->next; m->count--; m->released++;
    if(out->size<sizeof(value)) return ESP_ERR_INVALID_SIZE;
    memcpy(out->data,&value,sizeof(value)); out->size=sizeof(value); out->pts=value*200; out->dts=out->pts;
    return ESP_OK;
}
static esp_err_t esp_media_track_mngr_get_provider(esp_media_track_mngr_t *m, esp_media_provider_t *p) {
    static const esp_media_provider_ops_t ops={raw_read,other};
    if(!m || !p) return ESP_ERR_INVALID_ARG;
    *p=(esp_media_provider_t){.ops=&ops,.ctx=m}; return ESP_OK;
}
static esp_err_t esp_media_provider_read_frame(const esp_media_provider_t *p,esp_media_frame_t *f,uint32_t timeout){return p->ops->read_frame(p->ctx,f,timeout);}
static esp_err_t esp_media_track_mngr_query(esp_media_track_mngr_t *m,int type,esp_media_track_mngr_queue_info_t *q){
    assert(type==ESP_MEDIA_TRACK_TYPE_VIDEO);
    if(m->fail_query) return ESP_FAIL;
    q->q_num=m->count; q->q_size=m->count*4; return ESP_OK;
}
static esp_err_t esp_media_provider_acquire_frame(const esp_media_provider_t *p,esp_media_frame_t *f,uint32_t timeout){
    esp_media_track_mngr_t *m=p->ctx;
    assert(!timeout && !m->held && f->type==ESP_MEDIA_TRACK_TYPE_VIDEO);
    if(m->aborted || m->fail_acquire) return ESP_FAIL;
    if(!m->count) return ESP_ERR_NOT_FOUND;
    m->count--; m->next++; m->held=1; return ESP_OK;
}
static esp_err_t esp_media_provider_release_frame(const esp_media_provider_t *p,esp_media_frame_t *f){
    (void)f; esp_media_track_mngr_t *m=p->ctx;
    assert(m->held); m->held=0; m->released++; if(m->refill) m->count++;
    return m->fail_release?ESP_FAIL:ESP_OK;
}
'''
main = r'''
static esp_media_provider_t provider(esp_media_track_mngr_t *m,camera_source_t *src){
    src->mngr=m; esp_media_provider_t p;
    assert(camera_source_get_provider((esp_service_t*)src,0,&p)==ESP_OK);
    assert(p.ctx==m && p.ops->other(m)==m->aborted);
    return p;
}
int main(void){
    const int depths[]={1,2,50};
    for(unsigned test=0;test<sizeof(depths)/sizeof(depths[0]);test++){
        int count=depths[test];
        esp_media_track_mngr_t m={.count=count}; camera_source_t src={0};
        esp_media_provider_t p=provider(&m,&src);
        int output=-1; esp_media_frame_t f={.type=1,.data=&output,.size=sizeof(output)};
        assert(p.ops->read_frame(p.ctx,&f,23)==ESP_OK);
        assert(output==count && f.pts==count*200 && f.dts==f.pts);
        assert(m.released==count && !m.held && !m.count && m.read_timeout==23);
    }
    esp_media_track_mngr_t m={0}; camera_source_t src={0}; esp_media_provider_t p=provider(&m,&src);
    int output=-1; esp_media_frame_t f={.type=1,.data=&output,.size=sizeof(output)};
    assert(p.ops->read_frame(p.ctx,&f,0)==ESP_ERR_NOT_FOUND);
    m.count=3;
    assert(p.ops->read_frame(p.ctx,NULL,0)==ESP_ERR_INVALID_ARG && m.count==3);
    f.data=NULL; assert(p.ops->read_frame(p.ctx,&f,0)==ESP_ERR_INVALID_ARG && m.count==3); f.data=&output;
    m.fail_query=1; assert(p.ops->read_frame(p.ctx,&f,0)==ESP_FAIL && m.count==3); m.fail_query=0;
    m.fail_acquire=1; assert(p.ops->read_frame(p.ctx,&f,0)==ESP_FAIL && !m.held); m.fail_acquire=0;
    m.fail_release=1; assert(p.ops->read_frame(p.ctx,&f,0)==ESP_FAIL && !m.held); m.fail_release=0;
    m.count=3; f.size=1;
    assert(p.ops->read_frame(p.ctx,&f,0)==ESP_ERR_INVALID_SIZE && !m.held && !m.count && output==-1);
    m.count=2; m.aborted=1; f.size=sizeof(output);
    assert(p.ops->read_frame(p.ctx,&f,0)==ESP_FAIL && !m.held);
    m.aborted=0; m.count=1; m.next=100;
    assert(p.ops->read_frame(p.ctx,&f,0)==ESP_OK && output==101);
    assert(camera_source_get_provider((esp_service_t*)&src,1,&p)==ESP_ERR_INVALID_ARG);
    m=(esp_media_track_mngr_t){.count=3,.refill=1}; p=provider(&m,&src);
    assert(p.ops->read_frame(p.ctx,&f,0)==ESP_OK && output==3 && m.released==3 && m.count==2);
    puts("latest-frame backlog, lifetime, capacity, abort and provider compatibility passed");
}
'''
with tempfile.TemporaryDirectory(prefix="zaowu-latest-frame-") as temp:
    folder = Path(temp)
    c = folder / "latest_frame.c"
    exe = folder / "latest_frame.exe"
    c.write_text(stub + production + main, encoding="utf-8")
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Wno-unused-function", "-Werror", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
