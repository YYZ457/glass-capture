/* SPDX-License-Identifier: MIT
 * Continuous receiver + bounded processing queue. No FPGA register addresses.
 */
#define _GNU_SOURCE
#include "sg_pipeline.h"
#include "sg3000_rx_uapi.h"
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static void on_signal(int signal_number) { (void)signal_number; stopped=1; }
struct hardware { int fd; struct sg_capture_session session; struct sg_capture_stream stream; };
struct synthetic { uint32_t width, height, block, delay; };
struct output { const char *source, *processor; uint64_t last_ns, interval_ns, seen; };
static int reg_read(void *context, uint32_t offset, uint32_t *value)
{
    struct hardware *h=context; struct sg_reg r={offset,0};
    if (ioctl(h->fd, SG_IOC_READ_REG, &r)) return -errno;
    *value=r.value; return 0;
}
static int reg_write(void *context, uint32_t offset, uint32_t value)
{
    struct hardware *h=context; struct sg_reg r={offset,value};
    return ioctl(h->fd, SG_IOC_WRITE_REG, &r) ? -errno : 0;
}
static int dma_read(void *context, uint32_t source, void *buffer, uint32_t length, uint32_t timeout)
{
    struct hardware *h=context; struct sg_dma d={0};
    d.source=source; d.buffer=(uintptr_t)buffer; d.length=length; d.timeout_ms=timeout;
    if (ioctl(h->fd, SG_IOC_DMA_READ, &d)) return -errno;
    return d.completed != 1 || (d.status & (0x00f80638u|1u)) ? -EIO : 0;
}
static uint64_t now_ms(void *context) { (void)context; return sg_monotonic_ns()/1000000u; }
static void sleep_ms(void *context, uint32_t ms)
{ struct timespec t={ms/1000u,(long)(ms%1000u)*1000000L}; (void)context; nanosleep(&t,NULL); }
static int cancelled(void *context) { (void)context; return stopped != 0; }
static int hardware_next(void *context, struct sg_capture_frame *frame)
{ return sg_stream_next(&((struct hardware *)context)->stream, frame); }
static int hardware_stop(void *context)
{ return sg_stream_end(&((struct hardware *)context)->stream); }
static int synthetic_next(void *context, struct sg_capture_frame *frame)
{
    struct synthetic *s=context; size_t n=(size_t)s->width*s->height, i;
    sleep_ms(NULL,s->delay); if (stopped) return -ECANCELED;
    memset(frame,0,sizeof(*frame)); frame->data=malloc(n); if (!frame->data) return -ENOMEM;
    ++s->block;
    for(i=0;i<n;++i) frame->data[i]=(unsigned char)(i+s->block);
    frame->width=s->width; frame->height=s->height; frame->length=n;
    frame->block_id=s->block; frame->packets=1; return 0;
}
static int synthetic_stop(void *context) { (void)context; return 0; }
static int emit(const char *line, uint32_t timeout)
{
    size_t length=strlen(line), offset=0; uint64_t deadline=now_ms(NULL)+timeout;
    while(offset<length) {
        ssize_t n; struct pollfd fd={STDOUT_FILENO,POLLOUT,0}; uint64_t now=now_ms(NULL);
        if(now>=deadline) return -ETIMEDOUT;
        int ready=poll(&fd,1,(int)(deadline-now));
        if(ready<0) { if(errno==EINTR)continue; return -errno; }
        if(!ready) return -ETIMEDOUT;
        if(fd.revents & (POLLERR|POLLHUP|POLLNVAL)) return -EPIPE;
        n=write(STDOUT_FILENO,line+offset,length-offset);
        if(n<0) { if(errno==EINTR||errno==EAGAIN)continue; return -errno; }
        if(!n)return -EIO;
        offset+=(size_t)n;
    }
    return 0;
}
static int publish(void *context, const struct sg_image_view *view,
                   const struct sg_processing_result *r, uint32_t timeout)
{
    struct output *o=context; char line[1800]; int n; uint64_t now=sg_monotonic_ns();
    ++o->seen;
    /* Console updates are sampled; every block is still processed. Real decisions
     * bypass this decimation. UNKNOWN measurements do not authorize actuation. */
    if(o->last_ns && now-o->last_ns<o->interval_ns && r->decision==SG_UNKNOWN)return 0;
    o->last_ns=now;
    n=snprintf(line,sizeof(line),
        "SGJSON {\"schema\":1,\"event\":\"result\",\"source\":\"%s\",\"processor\":\"%s\","
        "\"sequence\":\"%" PRIu64 "\",\"received_line_start\":\"%" PRIu64 "\",\"received_ns\":\"%" PRIu64 "\","
        "\"block_id\":%u,\"width\":%u,\"height\":%u,\"stride\":%u,\"flags\":%u,"
        "\"processed_total\":\"%" PRIu64 "\",\"decision\":\"%s\",\"algorithm\":%u,"
        "\"pixel_count\":\"%" PRIu64 "\",\"sum\":\"%" PRIu64 "\",\"above_threshold\":\"%" PRIu64 "\","
        "\"min\":%u,\"max\":%u,\"continuity_verified\":false}\n",
        o->source,o->processor,view->sequence,view->first_line,view->timestamp_ns,
        view->card_block_id,view->width,view->height,view->stride,view->flags,
        o->seen,r->decision==SG_PASS?"pass":r->decision==SG_FAIL?"fail":"unknown",r->algorithm,
        r->pixel_count,r->sum,r->above_threshold,r->minimum,r->maximum);
    return n<0||(size_t)n>=sizeof(line)?-EOVERFLOW:emit(line,timeout);
}
static uint64_t number(const char *s, uint64_t max)
{
    char *end; unsigned long long value;
    if(!s||!*s||*s=='-'||*s=='+') { fprintf(stderr,"Invalid number\n"); exit(2); }
    errno=0; value=strtoull(s,&end,10);
    if(errno||*end||value>max) { fprintf(stderr,"Number out of range\n"); exit(2); }
    return value;
}
static void usage(void)
{
    puts("sg3000-stream 0.2 / interface ABI 1\n"
         "--source hardware|synthetic --processor stats|pl --seconds 30 (0=until stopped)\n"
         "--max-blocks 0 --queue 4 --max-frame-mib 16 --block-policy observe|strict\n"
         "--process-timeout-ms 2000 --emit-ms 1000 --threshold 128\n"
         "Synthetic only: --width 256 --height 64 --synthetic-delay-ms 50\n"
         "Output: SGJSON lines to stdout; no automatic image/result files or external actuation.\n"
         "PL requires a separate sg_imageproc kernel driver and configured FPGA design.");
}
int main(int argc,char **argv)
{
    static const struct option args[]={
        {"source",1,0,'s'},{"processor",1,0,'p'},{"seconds",1,0,'d'},
        {"max-blocks",1,0,'n'},{"queue",1,0,'q'},{"max-frame-mib",1,0,'m'},
        {"block-policy",1,0,'b'},{"process-timeout-ms",1,0,'t'},
        {"emit-ms",1,0,'e'},{"threshold",1,0,'r'},{"width",1,0,'w'},
        {"height",1,0,'h'},{"synthetic-delay-ms",1,0,'i'},{"version",0,0,'v'},
        {"help",0,0,'?'},{0,0,0,0}};
    const char *source_name="hardware",*processor_name="stats";
    struct sg_pipeline_options o={4,2000,0,30000,16u*1024u*1024u,0,0,&stopped};
    struct synthetic synthetic={256,64,0,50}; struct hardware hardware={.fd=-1};
    struct sg_processor processor={0}; struct sg_source source={0};
    struct output output={0}; struct sg_result_sink sink={&output,publish};
    struct sg_pipeline_stats stats={0}; struct sg_capture_options capture;
    struct sg_capture_io io={&hardware,reg_read,reg_write,dma_read,now_ms,sleep_ms,cancelled};
    struct sg_info info={0}; struct sigaction action={0};
    uint32_t threshold=128; uint64_t emit_ms=1000; int c,ret=0,lockfd=-1,old_flags=-1,begin_attempted=0;
    char line[1800];
    while((c=getopt_long(argc,argv,"",args,NULL))!=-1) {
        switch(c) {
        case 's':source_name=optarg;break; case 'p':processor_name=optarg;break;
        case 'd':o.duration_ms=number(optarg,86400)*1000;break;
        case 'n':o.max_blocks=number(optarg,UINT64_MAX);break;
        case 'q':o.queue_depth=(unsigned)number(optarg,16);break;
        case 'm':o.max_frame_bytes=number(optarg,256)*1024u*1024u;break;
        case 'b':if(strcmp(optarg,"observe")&&strcmp(optarg,"strict"))return 2;o.strict_block_ids=!strcmp(optarg,"strict");break;
        case 't':o.processing_timeout_ms=(unsigned)number(optarg,60000);break;
        case 'e':emit_ms=number(optarg,60000);break;
        case 'r':threshold=(uint32_t)number(optarg,255);break;
        case 'w':synthetic.width=(uint32_t)number(optarg,32768);break;
        case 'h':synthetic.height=(uint32_t)number(optarg,32768);break;
        case 'i':synthetic.delay=(uint32_t)number(optarg,10000);break;
        case 'v':puts("sg3000-stream 0.2 ABI 1");return 0;
        default:usage();return c=='?'&&optind>1&&!strcmp(argv[optind-1],"--help")?0:2;
        }
    }
    if(optind!=argc || (strcmp(source_name,"hardware")&&strcmp(source_name,"synthetic")) ||
       (strcmp(processor_name,"stats")&&strcmp(processor_name,"pl")) || !o.queue_depth ||
       !o.max_frame_bytes || !o.processing_timeout_ms || !emit_ms ||
       o.max_frame_bytes*(o.queue_depth+2u)>UINT64_C(512)*1024*1024 ||
       !synthetic.width||!synthetic.height||(uint64_t)synthetic.width*synthetic.height>o.max_frame_bytes)return 2;
    o.synthetic=!strcmp(source_name,"synthetic");
    action.sa_handler=on_signal; sigemptyset(&action.sa_mask);
    if(sigaction(SIGINT,&action,NULL)||sigaction(SIGTERM,&action,NULL))return 1;
    signal(SIGPIPE,SIG_IGN);
    output.source=source_name;output.processor=processor_name;output.interval_ns=emit_ms*1000000;
    old_flags=fcntl(STDOUT_FILENO,F_GETFL); if(old_flags<0||fcntl(STDOUT_FILENO,F_SETFL,old_flags|O_NONBLOCK))return 1;
    ret=!strcmp(processor_name,"stats")?sg_stats_open(&processor,threshold):sg_pl_open(&processor,"/dev/sg_imageproc");
    if(ret)goto done;
    if(o.synthetic) source=(struct sg_source){&synthetic,synthetic_next,synthetic_stop};
    else {
        lockfd=open("/run/sg3000-control.lock",O_CREAT|O_RDWR|O_CLOEXEC,0600);
        if(lockfd<0||flock(lockfd,LOCK_EX|LOCK_NB)) {ret=-errno;goto done;}
        hardware.fd=open("/dev/sg3000_rx_0000:01:00.0",O_RDWR|O_CLOEXEC);
        if(hardware.fd<0){ret=-errno;goto done;}
        if(ioctl(hardware.fd,SG_IOC_INFO,&info)){ret=-errno;goto done;}
        if(info.abi!=SG_ABI_VERSION||info.vendor!=0x4c54||info.device!=0x3000||info.poisoned||
           info.max_transfer<SG_CAPTURE_DMA_CHUNK||(info.engine_id>>16)!=0x1fc1u||(info.engine_id&0x8f00u)){ret=-ENODEV;goto done;}
        sg_capture_default_options(&capture);capture.max_frame_bytes=(size_t)o.max_frame_bytes;
        ret=sg_capture_session_init(&hardware.session,&io);if(ret)goto done;
        begin_attempted=1;ret=sg_stream_begin(&hardware.stream,&hardware.session,&capture);
        snprintf(line,sizeof(line),"SGJSON {\"schema\":1,\"event\":\"preflight\",\"valid_mask\":%u,\"ddr\":%u,\"clocks\":%u,\"error\":%d}\n",
            hardware.stream.preflight.valid_mask,hardware.stream.preflight.ddr_status,hardware.stream.preflight.pixel_clocks,ret);
        {int e=emit(line,2000);if(!ret)ret=e;} if(ret)goto done;
        source=(struct sg_source){&hardware,hardware_next,hardware_stop};
    }
    snprintf(line,sizeof(line),"SGJSON {\"schema\":1,\"event\":\"started\",\"source\":\"%s\",\"processor\":\"%s\",\"queue_depth\":%u,\"max_frame_bytes\":\"%" PRIu64 "\",\"continuity_verified\":false}\n",source_name,processor_name,o.queue_depth,o.max_frame_bytes);
    ret=emit(line,2000);if(ret)goto done;
    ret=sg_pipeline_run(&source,&processor,&sink,&o,&stats);
done:
    if(begin_attempted) {int e=sg_stream_end(&hardware.stream); if(e)stats.stop_error=e;if(!ret)ret=e;}
    snprintf(line,sizeof(line),"SGJSON {\"schema\":1,\"event\":\"ended\",\"source\":\"%s\",\"processor\":\"%s\",\"error\":%d,\"source_error\":%d,\"processing_error\":%d,\"output_error\":%d,\"stop_error\":%d,\"cancelled\":%s,\"received\":\"%" PRIu64 "\",\"processed\":\"%" PRIu64 "\",\"bytes\":\"%" PRIu64 "\",\"id_discontinuities\":\"%" PRIu64 "\",\"queue_full_events\":\"%" PRIu64 "\",\"queue_peak\":%u,\"elapsed_ms\":\"%" PRIu64 "\"}\n",
        source_name,processor_name,ret,stats.source_error,stats.processing_error,stats.output_error,stats.stop_error,
        stats.cancelled?"true":"false",stats.received,stats.processed,stats.bytes,stats.id_discontinuities,
        stats.queue_full_events,stats.queue_peak,(stats.finished_ns-stats.started_ns)/1000000u);
    {int e=emit(line,2000);if(!ret)ret=e;}
    if(processor.close)processor.close(processor.context);
    if(hardware.fd>=0)close(hardware.fd);
    if(lockfd>=0)close(lockfd);
    if(old_flags>=0)fcntl(STDOUT_FILENO,F_SETFL,old_flags);
    if(ret)fprintf(stderr,"Stream failed: %s (%d)\n",strerror(-ret),ret);
    return ret?1:0;
}
