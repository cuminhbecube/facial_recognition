#include <stdio.h>
#include <sys/poll.h>
#include <errno.h>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

#include "rk_defines.h"
#include "rk_debug.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"

#include "RgaUtils.h"
#include "im2d.hpp"

#include "dma_alloc.h"

#define TEST_VENC_MAX 2
#define TEST_WITH_FD 0
#define TEST_WITH_FD_SWITCH 0


int mode=0;

RK_BOOL bquit = RK_FALSE;
char *fbp = 0;
pthread_mutex_t cam_mutex = PTHREAD_MUTEX_INITIALIZER;

#define RKISP_DEV  "/proc/rkisp-vir0"


typedef struct _rkMpiVICtx {
    RK_S32 maxWidth;
    RK_S32 maxHeight;
    RK_S32 width;
    RK_S32 height;
    RK_S32 devId;
    RK_S32 pipeId;
    RK_S32 channelId;
    RK_S32 selectFd;
    RK_BOOL bFreeze;
    MB_POOL attachPool;

    COMPRESS_MODE_E enCompressMode;
    VI_DEV_STATUS_S stDevStatus;
    VI_DEV_ATTR_S stDevAttr;
    VI_DEV_BIND_PIPE_S stBindPipe;
    VI_CHN_ATTR_S stChnAttr;

    VI_SAVE_FILE_INFO_S stDebugFile;
    VIDEO_FRAME_INFO_S stViFrame;
    VI_CHN_STATUS_S stChnStatus;
    const char *aEntityName;
    VI_CHN_BUF_WRAP_S stChnWrap;

} TEST_VI_CTX_S;


TEST_VI_CTX_S *ctx;
TEST_VI_CTX_S *ctx1;

RK_U64 TEST_COMM_GetNowUs() {
    struct timespec time = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (RK_U64)time.tv_sec * 1000000 + (RK_U64)time.tv_nsec / 1000; /* microseconds */
}

RK_S32 test_vi_init(TEST_VI_CTX_S *ctx) {
    RK_S32 s32Ret = RK_FAILURE;

    s32Ret = RK_MPI_VI_QueryDevStatus(ctx->devId, &ctx->stDevStatus);
    if (s32Ret == RK_SUCCESS) {
        if (!ctx->stDevStatus.bProbeOk) {
            RK_LOGE("sensor probe fail");
            return RK_FAILURE;
        }
        printf("cam%d is ready\n",ctx->devId);
    }

    // 0. get dev config status
    s32Ret = RK_MPI_VI_GetDevAttr(ctx->devId, &ctx->stDevAttr);
    if (s32Ret == RK_ERR_VI_NOT_CONFIG) {
        // 0-1.config dev
        s32Ret = RK_MPI_VI_SetDevAttr(ctx->devId, &ctx->stDevAttr);
        if (s32Ret != RK_SUCCESS) {
            RK_LOGE("RK_MPI_VI_SetDevAttr %x", s32Ret);
            goto __FAILED;
        }
    } else {
        RK_LOGE("RK_MPI_VI_SetDevAttr already");
    }
    // 1.get  dev enable status
    s32Ret = RK_MPI_VI_GetDevIsEnable(ctx->devId);
    if (s32Ret != RK_SUCCESS) {
        // 1-2.enable dev
        s32Ret = RK_MPI_VI_EnableDev(ctx->devId);
        if (s32Ret != RK_SUCCESS) {
            RK_LOGE("RK_MPI_VI_EnableDev %x", s32Ret);
            goto __FAILED;
        }
        // 1-3.bind dev/pipe
        ctx->stBindPipe.u32Num = ctx->pipeId;
        ctx->stBindPipe.PipeId[0] = ctx->pipeId;
        s32Ret = RK_MPI_VI_SetDevBindPipe(ctx->devId, &ctx->stBindPipe);
        if (s32Ret != RK_SUCCESS) {
            RK_LOGE("RK_MPI_VI_SetDevBindPipe %x", s32Ret);
            goto __FAILED;
        }
    } else {
        RK_LOGE("RK_MPI_VI_EnableDev already");
    }

    // 2.config channel
    ctx->stChnAttr.stSize.u32Width  = ctx->width;
    ctx->stChnAttr.stSize.u32Height = ctx->height;
    ctx->stChnAttr.enCompressMode   = ctx->enCompressMode;
    ctx->stChnAttr.u32Depth         = 1;
    ctx->stChnAttr.bMirror          = RK_FALSE;
    ctx->stChnAttr.bFlip            = RK_FALSE;
    ctx->stChnAttr.stIspOpt.stMaxSize.u32Width  = RK_MAX(ctx->maxWidth, ctx->width);
    ctx->stChnAttr.stIspOpt.stMaxSize.u32Height = RK_MAX(ctx->maxHeight, ctx->height);

    s32Ret = RK_MPI_VI_SetChnAttr(ctx->pipeId, ctx->channelId, &ctx->stChnAttr);
    if (s32Ret != RK_SUCCESS) {
        RK_LOGE("RK_MPI_VI_SetChnAttr %x", s32Ret);
        goto __FAILED;
    }

    // set wrap mode attr
    VI_CHN_BUF_WRAP_S stViWrap;
    memset(&stViWrap, 0, sizeof(VI_CHN_BUF_WRAP_S));
    if (ctx->stChnWrap.bEnable) {
        if (ctx->stChnWrap.u32BufLine < 128 || ctx->stChnWrap.u32BufLine > ctx->height) {
            RK_LOGE("wrap mode buffer line must between [128, H]");
            goto __FAILED;
        }
        RK_U32                maxW = RK_MAX(ctx->width, ctx->maxWidth);
        stViWrap.bEnable           = ctx->stChnWrap.bEnable;
        stViWrap.u32BufLine        = ctx->stChnWrap.u32BufLine;
        stViWrap.u32WrapBufferSize = stViWrap.u32BufLine * maxW * 3 / 2;  // nv12 (w * wrapLine *3 / 2)
        RK_LOGD("set channel wrap line: %d, wrapBuffSize = %d", ctx->stChnWrap.u32BufLine, stViWrap.u32WrapBufferSize);
        RK_MPI_VI_SetChnWrapBufAttr(ctx->pipeId, ctx->channelId, &stViWrap);
    }
    // open fd before enable chn will be better
#if TEST_WITH_FD
     ctx->selectFd = RK_MPI_VI_GetChnFd(ctx->pipeId, ctx->channelId);
     RK_LOGE("ctx->pipeId=%d, ctx->channelId=%d, ctx->selectFd:%d ", ctx->pipeId, ctx->channelId, ctx->selectFd);
#endif

    // 3.enable channel
    s32Ret = RK_MPI_VI_EnableChn(ctx->pipeId, ctx->channelId);
    if (s32Ret != RK_SUCCESS) {
        RK_LOGE("RK_MPI_VI_EnableChn %x", s32Ret);
        goto __FAILED;
    }

    // 4.save debug file
    if (ctx->stDebugFile.bCfg) {
        s32Ret = RK_MPI_VI_ChnSaveFile(ctx->pipeId, ctx->channelId, &ctx->stDebugFile);
        RK_LOGD("RK_MPI_VI_ChnSaveFile %x", s32Ret);
    }

__FAILED:
    return s32Ret;
}


static int color_cvt(int ch,void *src_data){
    int ret = 0;
    int src_width, src_height, src_format;
    int dst_width, dst_height, dst_format;
    char *src_buf,*dst_buf;
    int src_buf_size, dst_buf_size;
    int src_dma_fd, dst_dma_fd;

    rga_buffer_t src_img, dst_img;
    rga_buffer_handle_t src_handle, dst_handle;

    memset(&src_img, 0, sizeof(src_img));
    memset(&dst_img, 0, sizeof(dst_img));

    if(mode==1){
        src_width = 2880;
        src_height = 1620;
        src_format = RK_FORMAT_YCrCb_420_SP;

        dst_width = 720;
        dst_height = 405;
        dst_format = RK_FORMAT_RGBA_8888;
    }else{
        src_width = 1632;
        src_height = 1224;
        src_format = RK_FORMAT_YCrCb_420_SP;

        dst_width = 816;
        dst_height = 612;
        dst_format = RK_FORMAT_RGBA_8888;
    }


    src_buf_size = src_width * src_height * get_bpp_from_format(src_format);
    dst_buf_size = dst_width * dst_height * get_bpp_from_format(dst_format);

    printf("%f,%f\n",get_bpp_from_format(src_format),get_bpp_from_format(dst_format));


    ret = dma_buf_alloc(RV1106_CMA_HEAP_PATH, src_buf_size, &src_dma_fd, (void **)&src_buf);
    if (ret < 0) {
        printf("alloc src CMA buffer failed!\n");
        return -1;
    }

    ret = dma_buf_alloc(RV1106_CMA_HEAP_PATH, dst_buf_size, &dst_dma_fd, (void **)&dst_buf);
    if (ret < 0) {
        printf("alloc dst CMA buffer failed!\n");
        dma_buf_free(src_buf_size, &src_dma_fd, src_buf);
        return -1;
    }

    memcpy(src_buf, src_data, src_buf_size);

    src_handle = importbuffer_fd(src_dma_fd, src_buf_size);
    dst_handle = importbuffer_fd(dst_dma_fd, dst_buf_size);
    if (src_handle == 0 || dst_handle == 0) {
        printf("import dma_fd error!\n");
        ret = -1;
        goto free_buf;
    }

    src_img = wrapbuffer_handle(src_handle, src_width, src_height, src_format);
    dst_img = wrapbuffer_handle(dst_handle, dst_width, dst_height, dst_format);

    ret = imcheck(src_img, dst_img, {}, {});
    if (IM_STATUS_NOERROR != ret) {
        printf("%d, check error! %s", __LINE__, imStrError((IM_STATUS)ret));
        return -1;
    }

    ret = imcvtcolor(src_img, dst_img, src_format, dst_format);
    if (ret == IM_STATUS_SUCCESS) {
        printf("*");
    } else {
        printf("%s running failed, %s\n", "12#", imStrError((IM_STATUS)ret));
        goto release_buffer;
    }

    pthread_mutex_lock(&cam_mutex);

    if(mode==1){
        for(int i=0;i<405;i++){
            if(ch)
                memcpy(fbp+800*i*4+400*4,dst_buf+i*408*4,400*4);
            else
                memcpy(fbp+800*i*4,dst_buf+i*720*4,720*4);
        }
    }else{
        for(int i=0;i<480;i++){
            if(ch)
                memcpy(fbp+800*i*4+400*4,dst_buf+i*408*4,400*4);
            else
                memcpy(fbp+800*i*4,dst_buf+i*816*4,800*4);
        }

    }
        
    pthread_mutex_unlock(&cam_mutex);
release_buffer:
    if (src_handle)
        releasebuffer_handle(src_handle);
    if (dst_handle)
        releasebuffer_handle(dst_handle);
free_buf:
    dma_buf_free(src_buf_size, &src_dma_fd, src_buf);
    dma_buf_free(dst_buf_size, &dst_dma_fd, dst_buf);

    return ret;
}

static RK_S32 test_vi_get_release_frame_loop(TEST_VI_CTX_S *ctx) {
    RK_S32 s32Ret;
    RK_S32 loopCount = 0;
    RK_S32 waitTime = 100;

    RK_BOOL bChangeRgn = RK_TRUE;

    /* test use getframe&release_frame */
    s32Ret = test_vi_init(ctx);
    if (s32Ret != RK_SUCCESS) {
        RK_LOGE("vi %d:%d init failed:%x", ctx->devId, ctx->channelId, s32Ret);
        goto __FAILED;
    }

    // freeze test
    RK_MPI_VI_SetChnFreeze(ctx->pipeId, ctx->channelId, ctx->bFreeze);

    while (1) {
        // 5.get the frame
        s32Ret = RK_MPI_VI_GetChnFrame(ctx->pipeId, ctx->channelId, &ctx->stViFrame, waitTime);
        if (s32Ret == RK_SUCCESS) {
            RK_U64 nowUs = TEST_COMM_GetNowUs();
            void *data = RK_MPI_MB_Handle2VirAddr(ctx->stViFrame.stVFrame.pMbBlk);
            RK_U64 phyAddr = RK_MPI_MB_Handle2PhysAddr(ctx->stViFrame.stVFrame.pMbBlk);
            RK_LOGD("RK_MPI_VI_GetChnFrame ok:data %p, phyaddr:0x%08x, loop:%d seq:%d pts:%lld ms len=%d",
                     data,
                     (RK_U32)phyAddr,
                     loopCount,
                     ctx->stViFrame.stVFrame.u32TimeRef,
                     ctx->stViFrame.stVFrame.u64PTS / 1000,
                     ctx->stViFrame.stVFrame.u64PrivateData);
            // 6.get the channel status
            s32Ret = RK_MPI_VI_QueryChnStatus(ctx->pipeId, ctx->channelId, &ctx->stChnStatus);
            RK_LOGD("RK_MPI_VI_QueryChnStatus ret %x, w:%d,h:%d,enable:%d," \
                    "current frame id:%d,input lost:%d,output lost:%d," \
                    "framerate:%d,vbfail:%d delay=%lldus",
                     s32Ret,
                     ctx->stChnStatus.stSize.u32Width,
                     ctx->stChnStatus.stSize.u32Height,
                     ctx->stChnStatus.bEnable,
                     ctx->stChnStatus.u32CurFrameID,
                     ctx->stChnStatus.u32InputLostFrame,
                     ctx->stChnStatus.u32OutputLostFrame,
                     ctx->stChnStatus.u32FrameRate,
                     ctx->stChnStatus.u32VbFail,
                     nowUs - ctx->stViFrame.stVFrame.u64PTS);


            color_cvt(ctx->pipeId,data);
            // 7.release the frame
            s32Ret = RK_MPI_VI_ReleaseChnFrame(ctx->pipeId, ctx->channelId, &ctx->stViFrame);
            if (s32Ret != RK_SUCCESS) {
                RK_LOGE("RK_MPI_VI_ReleaseChnFrame fail %x", s32Ret);
            }
            loopCount++;
        } else {
            RK_LOGE("RK_MPI_VI_GetChnFrame timeout %x", s32Ret);
        }

        usleep(10*1000);
    }

__FAILED:


    // 9. disable one chn
    s32Ret = RK_MPI_VI_DisableChn(ctx->pipeId, ctx->channelId);
    RK_LOGE("RK_MPI_VI_DisableChn %x", s32Ret);

    // 10.disable dev(will diabled all chn)
    s32Ret = RK_MPI_VI_DisableDev(ctx->devId);
    RK_LOGE("RK_MPI_VI_DisableDev %x", s32Ret);
    return NULL;
}


void *start_stream(void* arg){
    test_vi_get_release_frame_loop(ctx);
    return NULL;
}

int main(int argc, const char **argv) {
    RK_S32 i;
    RK_S32 s32Ret = RK_FAILURE;

    if(argc < 2) {
        printf("Usage: %s <camera_id>\n", argv[0]);
        return 0;
    }

    mode=atoi(argv[1]);
    
    ctx = reinterpret_cast<TEST_VI_CTX_S *>(malloc(sizeof(TEST_VI_CTX_S)));
    memset(ctx, 0, sizeof(TEST_VI_CTX_S));


    if(mode==1){
        ctx->width = 2880;
        ctx->height = 1620;
        ctx->maxWidth = 2880;
        ctx->maxHeight = 1620;
    }else{
        ctx->width = 1632;
        ctx->height = 1224;
        ctx->maxWidth = 1632;
        ctx->maxHeight = 1224;
    }

    ctx->stChnAttr.stIspOpt.u32BufCount = 3;
    ctx->stChnAttr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    ctx->stChnAttr.stIspOpt.bNoUseLibV4L2 = RK_TRUE;
    ctx->stChnAttr.u32Depth = 0;
    ctx->aEntityName = RK_NULL;
    ctx->stChnAttr.enPixelFormat = RK_FMT_YUV420SP;
    ctx->stChnAttr.stFrameRate.s32SrcFrameRate = -1;
    ctx->stChnAttr.stFrameRate.s32DstFrameRate = -1;

    ctx1 = reinterpret_cast<TEST_VI_CTX_S *>(malloc(sizeof(TEST_VI_CTX_S)));
    memset(ctx1, 0, sizeof(TEST_VI_CTX_S));


    if(mode==1){
        ctx1->width = 2880;
        ctx1->height = 1620;
        ctx1->maxWidth = 2880;
        ctx1->maxHeight = 1620;
    }else{
        ctx1->width = 1632;
        ctx1->height = 1224;
        ctx1->maxWidth = 1632;
        ctx1->maxHeight = 1224;
    }

    ctx1->stChnAttr.stIspOpt.u32BufCount = 3;
    ctx1->stChnAttr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    ctx1->stChnAttr.stIspOpt.bNoUseLibV4L2 = RK_TRUE;
    ctx1->stChnAttr.u32Depth = 0;
    ctx1->aEntityName = RK_NULL;
    ctx1->stChnAttr.enPixelFormat = RK_FMT_YUV420SP;
    ctx1->stChnAttr.stFrameRate.s32SrcFrameRate = -1;
    ctx1->stChnAttr.stFrameRate.s32DstFrameRate = -1;


    int fbfd;
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    
    int width, height, bpp;

    //打开framebuffer设备文件
    fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd == -1) {
        perror("Error: cannot open framebuffer device");
        return 1;
    }

    //获取屏幕信息
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo) == -1) {
        perror("Error: cannot get variable screen information");
        return 1;
    }
    if (ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo) == -1) {
        perror("Error: cannot get fixed screen information");
        return 1;
    }

    //获取屏幕宽度、高度、位深度
    width = vinfo.xres;
    height = vinfo.yres;
    bpp = vinfo.bits_per_pixel;

    //映射framebuffer到内存
    fbp = (char *)mmap(0, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fbp == MAP_FAILED) {
        perror("Error: failed to map framebuffer device to memory");
        return 1;
    }

    //清空framebuffer
    memset(fbp, 0, finfo.smem_len);


    if (RK_MPI_SYS_Init() != RK_SUCCESS) {
        RK_LOGE("rk mpi sys init fail!");
        goto __FAILED;
    }

    ctx->devId = 0;
    ctx->pipeId = ctx->devId;
    ctx->channelId = 0;

    pthread_t tid_ch0;
    if (pthread_create(&tid_ch0, NULL, start_stream, NULL) != 0) {
        RK_LOGE("create start_stream thread fail!");
        goto __FAILED2;
    }

    // sleep(3);

    // ctx1->devId = 1;
    // ctx1->pipeId = ctx1->devId;
    // ctx1->channelId = 0;
    // test_vi_get_release_frame_loop(ctx1);

    pthread_join(tid_ch0, NULL);    

__FAILED:
    RK_LOGE("test running exit:%d", s32Ret);
    RK_MPI_SYS_Exit();
__FAILED2:
    if (ctx) {
        free(ctx);
        ctx = RK_NULL;
    }

    close(fbfd);
    return 0;
}
