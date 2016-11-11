/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

/* This is a JNI example where we use native methods to play sounds
 * using OpenSL ES. See the corresponding Java source file located at:
 *
 *   src/com/example/nativeaudio/NativeAudio/NativeAudio.java
 */

#include <assert.h>
#include <jni.h>
#include <string.h>
#include <pthread.h>


// for __android_log_print(ANDROID_LOG_INFO, "YourApp", "formatted message");
#include <android/log.h>

// for native audio
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

// for native asset manager
#include <sys/types.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>

// pre-recorded sound clips, both are 8 kHz mono 16-bit signed little endian
static const char hello[] =
#include "hello_clip.h"
;

static const char android[] =
#include "android_clip.h"
;


#define LOG_TAG "native_audio_jni"
#define ALOG(priority, tag, fmt...) __android_log_print(ANDROID_##priority, tag, fmt)
#define ALOGD(...) ((void)ALOG(LOG_DEBUG, LOG_TAG, __VA_ARGS__))
#define ALOGE(...) ((void)ALOG(LOG_ERROR, LOG_TAG, __VA_ARGS__))

// engine interfaces
static SLObjectItf engineObject = NULL;
static SLEngineItf engineEngine;

// output mix interfaces
static SLObjectItf outputMixObject = NULL;
static SLEnvironmentalReverbItf outputMixEnvironmentalReverb = NULL;

// buffer queue player interfaces
static SLObjectItf bqPlayerObject = NULL;
static SLPlayItf bqPlayerPlay;
static SLAndroidSimpleBufferQueueItf bqPlayerBufferQueue;
static SLEffectSendItf bqPlayerEffectSend;
static SLMuteSoloItf bqPlayerMuteSolo;
static SLVolumeItf bqPlayerVolume;
static SLmilliHertz bqPlayerSampleRate = 0;
static jint   bqPlayerBufSize = 0;
static short *resampleBuf = NULL;
// a mutext to guard against re-entrance to record & playback
// as well as make recording and playing back to be mutually exclusive
// this is to avoid crash at situations like:
//    recording is in session [not finished]
//    user presses record button and another recording coming in
// The action: when recording/playing back is not finished, ignore the new request
static pthread_mutex_t  audioEngineLock = PTHREAD_MUTEX_INITIALIZER;

// aux effect on the output mix, used by the buffer queue player
static const SLEnvironmentalReverbSettings reverbSettings =
    SL_I3DL2_ENVIRONMENT_PRESET_STONECORRIDOR;

// 改变播放的采样率 单位是千分几
// 千分之一千 就是 正常 1x前进播放
// 低于-1000 就是 缓慢播放
// 高于 1000 就是 快速播放
// == 0 相当于暂停
static SLPlaybackRateItf fdlaybackRateItf = NULL;

// URI player interfaces
static SLObjectItf uriPlayerObject = NULL;
static SLPlayItf uriPlayerPlay;
static SLSeekItf uriPlayerSeek;
static SLMuteSoloItf uriPlayerMuteSolo;
static SLVolumeItf uriPlayerVolume;

// file descriptor player interfaces
static SLObjectItf fdPlayerObject = NULL;
static SLPlayItf fdPlayerPlay;
static SLSeekItf fdPlayerSeek;
static SLMuteSoloItf fdPlayerMuteSolo;
static SLVolumeItf fdPlayerVolume;


// recorder interfaces
static SLObjectItf recorderObject = NULL;
static SLRecordItf recorderRecord;
static SLAndroidSimpleBufferQueueItf recorderBufferQueue;

// synthesized sawtooth clip
#define SAWTOOTH_FRAMES 8000
static short sawtoothBuffer[SAWTOOTH_FRAMES];

// 5 seconds of recorded audio at 16 kHz mono, 16-bit signed little endian
#define RECORDER_FRAMES (16000 * 5)
static short recorderBuffer[RECORDER_FRAMES];
static unsigned recorderSize = 0;

// pointer and size of the next player buffer to enqueue, and number of remaining buffers
static short *nextBuffer;
static unsigned nextSize;
static int nextCount;


// synthesize a mono sawtooth wave and place it into a buffer (called automatically on load)
__attribute__((constructor)) static void onDlOpen(void)
{
    unsigned i;
    for (i = 0; i < SAWTOOTH_FRAMES; ++i) {
        sawtoothBuffer[i] = 32768 - ((i % 100) * 660);
    }
}

void releaseResampleBuf(void) {
    if( 0 == bqPlayerSampleRate) {
        /*
         * we are not using fast path, so we were not creating buffers, nothing to do
         */
        return;
    }

    free(resampleBuf);
    resampleBuf = NULL;
}

/*
 * Only support up-sampling
 */
short* createResampledBuf(uint32_t idx, uint32_t srcRate, unsigned *size) {
    short  *src = NULL;
    short  *workBuf;
    int    upSampleRate;
    int32_t srcSampleCount = 0;

    if(0 == bqPlayerSampleRate) {
        return NULL;
    }
    if(bqPlayerSampleRate % srcRate) {
        /*
         * simple up-sampling, must be divisible
         */
        return NULL;
    }
    upSampleRate = bqPlayerSampleRate / srcRate;

    switch (idx) {
        case 0:
            return NULL;
        case 1: // HELLO_CLIP
            srcSampleCount = sizeof(hello) >> 1;
            src = (short*)hello;
            break;
        case 2: // ANDROID_CLIP
            srcSampleCount = sizeof(android) >> 1;
            src = (short*) android;
            break;
        case 3: // SAWTOOTH_CLIP
            srcSampleCount = SAWTOOTH_FRAMES;
            src = sawtoothBuffer;
            break;
        case 4: // captured frames
            srcSampleCount = recorderSize / sizeof(short);
            src =  recorderBuffer;
            break;
        default:
            assert(0);
            return NULL;
    }

    resampleBuf = (short*) malloc((srcSampleCount * upSampleRate) << 1);
    if(resampleBuf == NULL) {
        return resampleBuf;
    }
    workBuf = resampleBuf;
    for(int sample=0; sample < srcSampleCount; sample++) {
        for(int dup = 0; dup  < upSampleRate; dup++) {
            *workBuf++ = src[sample];
        }
    }

    *size = (srcSampleCount * upSampleRate) << 1;     // sample format is 16 bit
    return resampleBuf;
}

// this callback handler is called every time a buffer finishes playing
void bqPlayerCallback(SLAndroidSimpleBufferQueueItf bq, void *context)
{
    assert(bq == bqPlayerBufferQueue);
    assert(NULL == context);
    // for streaming playback, replace this test by logic to find and fill the next buffer
    if (--nextCount > 0 && NULL != nextBuffer && 0 != nextSize) {
        SLresult result;
        // enqueue another buffer
        result = (*bqPlayerBufferQueue)->Enqueue(bqPlayerBufferQueue, nextBuffer, nextSize);
        // the most likely other result is SL_RESULT_BUFFER_INSUFFICIENT,
        // which for this code example would indicate a programming error
        if (SL_RESULT_SUCCESS != result) {
            pthread_mutex_unlock(&audioEngineLock);
        }
        (void)result;
    } else {
        releaseResampleBuf();
        pthread_mutex_unlock(&audioEngineLock);
    }
}


// this callback handler is called every time a buffer finishes recording
void bqRecorderCallback(SLAndroidSimpleBufferQueueItf bq, void *context)
{
    assert(bq == recorderBufferQueue);
    assert(NULL == context);
    // for streaming recording, here we would call Enqueue to give recorder the next buffer to fill
    // but instead, this is a one-time buffer so we stop recording
    SLresult result;
    result = (*recorderRecord)->SetRecordState(recorderRecord, SL_RECORDSTATE_STOPPED);
    if (SL_RESULT_SUCCESS == result) {
        recorderSize = RECORDER_FRAMES * sizeof(short);
    }
    pthread_mutex_unlock(&audioEngineLock);
}

/*
     *  EnvironmentalReverb和PresetReverb
     *  其中推荐
     *  在游戏场景中应用EnvironmentalReverb         android.media.audiofx.EnvironmentalReverb
     *  在音乐场景中应用PresetReverb                android.media.audiofx.PresetReverb
     *
     *  为了在通过AudioTrack、MediaPlayer进行音频播放时具有混响特效，在构建混响实例时指明音频流的会话ID即可
     *
     *  如果指定的会话ID为0，则混响作用于主要的音频输出混音器（mix）上
     *  混响将会话ID指定为0需要"android.permission.MODIFY_AUDIO_SETTINGS"权限
     *
     *
     *  混响引擎: 预置混响 和 环境混响
     *
     *  限制:
     *
     *  1. 不能在同一个OutputMix上 同时创建 两种混响
     *  2. 平台可能忽视效果 如果平台认为会增加CPU负荷
     *  3. Environmental reverb 不支持 SLEnvironmentalReverbSettings结构体的reflectionsDelay, reflectionsLevel, or reverbDelay等属性
     *  4. MIME data format只能用于audio player或者 URI data locator 不能用在 audio recorder
     *  5. 要求初始化mimeType为NULL 或者 utf-8字符串  也必须初始化containerType为一个有效值
     *      考虑其他因素，比如可移植性到其他实现或者APP不能识别内容格式  我们建议使用  mimeType = NULL 和 containerType = SL_CONTAINERTYPE_UNSPECIFIED
     *  6. 目前支持 wav@pcm wav@alaw wav@ulaw
     *              mp3 ogg
     *              AAC-LC HE-AACv1(AAC+) HE-AACv2(enhanded AAC+)
     *              AMR
     *              FLAC
     *   7. AAC格式必须存在 MP4 or ADTS 容器中
     *   8. 不支持 MIDI WMA
     *   9. 不支持直接播放 DRM 或者 encrypted content. 需要应用自己解密或者执行任何DRM的限制
     *   10. 不支持如下操作Object的方法:
     *          Resume()
     *          RegisterCallback()
     *          AbortAsyncOperation()
     *          SetPriority()
     *          GetPriority()
     *          SetLossOfControlInterfaces()
     *
     *      PCM data format:
     *          PCM是唯一一种格式 可以使用  buffer queues , 支持如下配置:
     *          1. 8-bit unsigned / 16-bit signed.
     *          2. Mono / stereo.
     *          3. 小端字节序
     *          4. 采样率 8,000   11,025  12,000  16,000  22,050  24,000 32,000 44,100  48,000
     *
     *          录音的配置 一般是 设备依赖的  通常是 16,000 Hz mono/16-bit signed
     *
     *          samplesPerSec域的单位是 mHz  为了避免错误 我们建议使用宏定义 SL_SAMPLINGRATE_44_1
     *
     *          Android5.0 API21和以上 支持 单精度 浮点数  single-precision, floating-point format.
     *          代码见 : https://developer.android.com/ndk/guides/audio/android-extensions.html#floating-point
     *
     *      Playback rate:
     *          播放速率 指示 对象表现数据的速率  千分几
     *          千分之1000 表示 正常播放
     *          支持的播放速率 和 是否支持 是依赖平台实现
     *          支持那些功能 或者 速率范围 可以通过 PlaybackRate::GetRateRange() or PlaybackRate::GetCapabilitiesOfRate()
     *
     *     Record:
     *          不这次hi  SL_RECORDEVENT_HEADATLIMIT or SL_RECORDEVENT_HEADMOVING 等事件
     *
     *      Seek:
     *          SetLoop()支持全部文件重复
     *          要启动loop 可以 设置startPos参数为startPos和endPos参数为SL_TIME_UNKNOWN
     *          (*uriPlayerSeek)->SetLoop(uriPlayerSeek, (SLboolean) isLooping, 0, SL_TIME_UNKNOWN);
     *
     *      Buffer queue data locator:
     *          如果录音或者播放 要使用buffer queue 只支持PCM格式
     *
     *      四种数据加载器
     *          1. Buffer queue data locator    内存队列   数据加载器
     *          2. I/O device data locator      I/O设备   数据加载器
     *          3. URI data locator             URI      数据加载器
     *          4. Android file descriptor data locator Android文件描述符 数据加载器
     *
     *      I/O device data locator
     *          1. 只能用于 Engine::CreateAudioRecorder的数据源
     *          2. 使用如下代码:
     *          SLDataLocator_IODevice loc_dev =  {
     *                      SL_DATALOCATOR_IODEVICE,
     *                      SL_IODEVICE_AUDIOINPUT,
     *                      SL_DEFAULTDEVICEID_AUDIOINPUT,
     *                      NULL};
     *      URI data locator
     *          1. 只能用于audio player 不能用于 audio recorder
     *          2. 而且需要带 MIME data format
     *          3. URI的schemes只支持 http: and file: 不支持 https:, ftp:, or content:  rtsp:
     *
     *      Android file descriptor data locator
     *          1. 以读方式打开的文件描述符
     *          2. 可以结合 native asset manager  因为asset manager可以返回asset资源的文件描述符
     *
     *      Data structures
     *          Android 支持 OpenSL ES 1.0.1 数据结构
     *
     *          SLInterfaceID
     *          SLEngineOption
     *          SLEnvironmentalReverbSettings
     *          SLDataFormat_MIME   SLDataFormat_PCM
     *          SLDataLocator_BufferQueue   SLDataLocator_IODevice  SLDataLocator_URI
     *          SLDataLocator_OutputMix
     *          SLDataSink      SLDataSource
     *
     *      Platform configuration
     *          支持多线程和线程安全
     *          支持每个进程 一个 引擎
     *          一个引擎 支持 32个object对象(设备内存和CPU可能会限制这个数目)
     *
     *          OpenMAX AL and OpenSL ES 可以用在一个应用中 都创建 都使用 都释放destroy
     *          这种情况中 在内部共享同一个引擎, 32个object对象限制包含了OpenMAX AL and OpenSL ES
     *          引擎维护一个引用计数 所以能够正确地destroy 和第二次destroy
     *
     *      Programming Notes
     *          辅助编程参考 : OpenSL ES Programming Notes
     *          OpenSL ES 1.0.1 specification: OpenSL_ES_Specification_1.0.1.pdf
     *
     *      Platform Issues
     *          已发现问题  known issues
     *          Dynamic interface management
     *          不支持 DynamicInterfaceManagement::AddInterface
     *          相反 在数组中定义接口ID 传递给 Create()
*/

// create the engine and output mix objects
void Java_com_example_nativeaudio_NativeAudio_createEngine(JNIEnv* env, jclass clazz)
{
    SLresult result;

    // create engine
    ALOGD( "创建引擎Engine Object");
    result = slCreateEngine(&engineObject, 0, NULL, 0, NULL, NULL);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;
    /*
     *
     * SLresult SLAPIENTRY slCreateEngine(
                                SLObjectItf *pEngine,
                                SLuint32 numOptions
                                const SLEngineOption *pEngineOptions,
                                SLuint32 numInterfaces,
                                const SLInterfaceID *pInterfaceIds,
                                const SLboolean * pInterfaceRequired
                                )
     * */

    SLuint32 num = 0 ;
    result = slQueryNumSupportedEngineInterfaces(&num);
    if( result != SL_RESULT_SUCCESS){
        ALOGE("slQueryNumSupportedEngineInterfaces fail ");
    }else{
        ALOGD("OpenSL 引擎对象支持的接口 包含必须的 和 可选的 num = %d", num );
    }

    for( SLuint32 i = 0 ; i < num ; i ++){
        SLInterfaceID local_InterfaceID ;
        result = slQuerySupportedEngineInterfaces(i,&local_InterfaceID);
        ALOGD("supported Engine Interface  timestamp = %08x %04x %04x ; clock_seq = %04x ; node = %08x  " ,
              local_InterfaceID->time_low ,
              local_InterfaceID->time_mid ,
              local_InterfaceID->time_hi_and_version ,
              local_InterfaceID->clock_seq ,
              local_InterfaceID->node[0]  );
    }

    // realize the engine
    result = (*engineObject)->Realize(engineObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // get the engine interface, which is needed in order to create other objects
    ALOGD( "SLObjectItf --> SLEngineItf: 获取引擎对象Engine Object的接口  后面其他Object都要用引擎对象的接口(而非引擎对象本身)来创建");
    result = (*engineObject)->GetInterface(engineObject, SL_IID_ENGINE, &engineEngine);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;


    // create output mix, with environmental reverb specified as a non-required interface
    const SLInterfaceID ids[1] = {SL_IID_ENVIRONMENTALREVERB};
    const SLboolean req[1] = {SL_BOOLEAN_FALSE};
    result = (*engineEngine)->CreateOutputMix(engineEngine, &outputMixObject, 1, ids, req);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;
    ALOGD("通过 引擎对象的接口  创建  输出混音对象 不需要 环境混音接口 ");


    // realize the output mix
    result = (*outputMixObject)->Realize(outputMixObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;


    // get the environmental reverb interface
    // this could fail if the environmental reverb effect is not available,
    // either because the feature is not present, excessive CPU load, or
    // the required MODIFY_AUDIO_SETTINGS permission was not requested and granted
    result = (*outputMixObject)->GetInterface(outputMixObject, SL_IID_ENVIRONMENTALREVERB,
            &outputMixEnvironmentalReverb);
    if (SL_RESULT_SUCCESS == result) {
        result = (*outputMixEnvironmentalReverb)->SetEnvironmentalReverbProperties(
                outputMixEnvironmentalReverb, &reverbSettings);
        (void)result;
    }
    ALOGD("获得 输出(环境)混音对象 的 效果接口 (可能没有实现)");
    // ignore unsuccessful result codes for environmental reverb, as it is optional for this example

}


// create buffer queue audio player
void Java_com_example_nativeaudio_NativeAudio_createBufferQueueAudioPlayer(JNIEnv* env,
        jclass clazz, jint sampleRate, jint bufSize)
{
    SLresult result;
    if (sampleRate >= 0 && bufSize >= 0 ) {
        bqPlayerSampleRate = sampleRate * 1000;
        /*
         * device native buffer size is another factor to minimize audio latency, not used in this
         * sample: we only play one giant buffer here
         */
        bqPlayerBufSize = bufSize;
    }

    // configure audio source
    SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
    SLDataFormat_PCM format_pcm = {SL_DATAFORMAT_PCM, 1, SL_SAMPLINGRATE_8,
        SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN};
    /*
     * Enable Fast Audio when possible:  once we set the same rate to be the native, fast audio path
     * will be triggered
     */
    if(bqPlayerSampleRate) {
        format_pcm.samplesPerSec = bqPlayerSampleRate;       //sample rate in mili second
    }
    SLDataSource audioSrc = {&loc_bufq, &format_pcm};

    // configure audio sink
    SLDataLocator_OutputMix loc_outmix = {SL_DATALOCATOR_OUTPUTMIX, outputMixObject};
    SLDataSink audioSnk = {&loc_outmix, NULL};

    /*
     * create audio player:
     *     fast audio does not support when SL_IID_EFFECTSEND is required, skip it
     *     for fast audio case
     */
    const SLInterfaceID ids[3] = {SL_IID_BUFFERQUEUE, SL_IID_VOLUME, SL_IID_EFFECTSEND,
                                    /*SL_IID_MUTESOLO,*/};
    const SLboolean req[3] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE,
                                   /*SL_BOOLEAN_TRUE,*/ };

    result = (*engineEngine)->CreateAudioPlayer(engineEngine, &bqPlayerObject, &audioSrc, &audioSnk,
            bqPlayerSampleRate? 2 : 3, ids, req);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // realize the player
    result = (*bqPlayerObject)->Realize(bqPlayerObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // get the play interface
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_PLAY, &bqPlayerPlay);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // get the buffer queue interface
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_BUFFERQUEUE,
            &bqPlayerBufferQueue);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // 注册函数 让系统回调填充buffer
    // register callback on the buffer queue
    result = (*bqPlayerBufferQueue)->RegisterCallback(bqPlayerBufferQueue, bqPlayerCallback, NULL);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // get the effect send interface
    bqPlayerEffectSend = NULL;
    if( 0 == bqPlayerSampleRate) {
        result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_EFFECTSEND,
                                                 &bqPlayerEffectSend);
        assert(SL_RESULT_SUCCESS == result);
        (void)result;
    }

#if 0   // mute/solo is not supported for sources that are known to be mono, as this is
    // get the mute/solo interface  获得 静音/单声道 接口
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_MUTESOLO, &bqPlayerMuteSolo);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;
#endif

    // get the volume interface
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_VOLUME, &bqPlayerVolume);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // set the player's state to playing
    result = (*bqPlayerPlay)->SetPlayState(bqPlayerPlay, SL_PLAYSTATE_PLAYING);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;
}


// create URI audio player
jboolean Java_com_example_nativeaudio_NativeAudio_createUriAudioPlayer(JNIEnv* env, jclass clazz,
        jstring uri)
{
    SLresult result;

    // convert Java string to UTF-8
    const char *utf8 = (*env)->GetStringUTFChars(env, uri, NULL);
    assert(NULL != utf8);

    ALOGD("create Uri AudioPlayer %s" , utf8 );

    // configure audio source
    // (requires the INTERNET permission depending on the uri parameter)
    SLDataLocator_URI loc_uri = {SL_DATALOCATOR_URI, (SLchar *) utf8}; // 路径 data locator
    /*
     * typedef struct SLDataFormat_MIME_ {
            SLuint32 		formatType;     format type可以是 SL_DATAFORMAT_MIME 或者 SL_DATAFORMAT_PCM
            SLchar * 		mimeType;
            SLuint32		containerType;  container Type 容器类型可以是 raw ogg mp3 mp4 3gpp wav等
        } SLDataFormat_MIME;
     */
    SLDataFormat_MIME format_mime = {SL_DATAFORMAT_MIME, NULL, SL_CONTAINERTYPE_UNSPECIFIED}; // container type unspecified 容器类型不确定
    SLDataSource audioSrc = {&loc_uri, &format_mime};

    // configure audio sink
    SLDataLocator_OutputMix loc_outmix = {SL_DATALOCATOR_OUTPUTMIX, outputMixObject};
    SLDataSink audioSnk = {&loc_outmix, NULL};

    // create audio player
    const SLInterfaceID ids[3] = {SL_IID_SEEK, SL_IID_MUTESOLO, SL_IID_VOLUME};
    const SLboolean req[3] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
    result = (*engineEngine)->CreateAudioPlayer(engineEngine, &uriPlayerObject, &audioSrc,
            &audioSnk, 3, ids, req);
    ALOGD("通过 引擎对象的接口 创建 URL播放器对象(SLObjectItf)  需要支持接口 1.SEEK 2.MUTE 3.VOLUME ");
    if( result != SL_RESULT_SUCCESS ){
        ALOGE("CreateAudioPlayer fail result = %d " , result);
        return JNI_FALSE ;
    }
    // note that an invalid URI is not detected here, but during prepare/prefetch on Android,
    // or possibly during Realize on other platforms
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // release the Java string and UTF-8
    (*env)->ReleaseStringUTFChars(env, uri, utf8);

    // realize the player
    ALOGD("实例化  URL播放器对象(SLObjectItf) ");
    result = (*uriPlayerObject)->Realize(uriPlayerObject, SL_BOOLEAN_FALSE);
    // this will always succeed on Android, but we check result for portability to other platforms
    if (SL_RESULT_SUCCESS != result) {
        ALOGE("AudioPlayer Realize fail result = %d " , result);
        (*uriPlayerObject)->Destroy(uriPlayerObject);
        uriPlayerObject = NULL;
        return JNI_FALSE;
    }

    ALOGD("获得 URL播放器对象(SLObjectItf) 四个方法: play（SLPlayItf） seek（SLSeekItf） mute(SLMuteSoloItf) volume(SLVolumeItf)");
    // get the play interface
    result = (*uriPlayerObject)->GetInterface(uriPlayerObject, SL_IID_PLAY, &uriPlayerPlay);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;
    if (SL_RESULT_SUCCESS != result) {
        ALOGE("GetInterface SL_IID_PLAY fail result = %d " , result);
        return JNI_FALSE;
    }


    // get the seek interface
    result = (*uriPlayerObject)->GetInterface(uriPlayerObject, SL_IID_SEEK, &uriPlayerSeek);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // get the mute/solo interface
    result = (*uriPlayerObject)->GetInterface(uriPlayerObject, SL_IID_MUTESOLO, &uriPlayerMuteSolo);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // get the volume interface
    result = (*uriPlayerObject)->GetInterface(uriPlayerObject, SL_IID_VOLUME, &uriPlayerVolume);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;



    return JNI_TRUE;
}


// set the playing state for the URI audio player
// to PLAYING (true) or PAUSED (false)
void Java_com_example_nativeaudio_NativeAudio_setPlayingUriAudioPlayer(JNIEnv* env,
        jclass clazz, jboolean isPlaying)
{
    SLresult result;

    // make sure the URI audio player was created
    if (NULL != uriPlayerPlay) {

        ALOGD("调用 URL播放器对象(SLObjectItf) 的方法 SLPlayItf ");
        // set the player's state
        result = (*uriPlayerPlay)->SetPlayState(uriPlayerPlay, isPlaying ?
            SL_PLAYSTATE_PLAYING : SL_PLAYSTATE_PAUSED); // 暂停 和 播放
        assert(SL_RESULT_SUCCESS == result); // SL_RESULT_SUCCESS 是 0
        (void)result;
        ALOGD("SetPlayState %s done with %d ",
              (isPlaying ? "SL_PLAYSTATE_PLAYING" : "SL_PLAYSTATE_PAUSED"),
              result );
    }

}


// set the whole file looping state for the URI audio player
void Java_com_example_nativeaudio_NativeAudio_setLoopingUriAudioPlayer(JNIEnv* env,
        jclass clazz, jboolean isLooping)
{
    SLresult result;

    // make sure the URI audio player was created
    if (NULL != uriPlayerSeek) {
        // set the looping state
        result = (*uriPlayerSeek)->SetLoop(uriPlayerSeek, (SLboolean) isLooping, 0,
                SL_TIME_UNKNOWN);
        assert(SL_RESULT_SUCCESS == result);
        (void)result;
    }

}


// expose the mute/solo APIs to Java for one of the 3 players

static SLMuteSoloItf getMuteSolo()
{
    if (uriPlayerMuteSolo != NULL)
        return uriPlayerMuteSolo;
    else if (fdPlayerMuteSolo != NULL)
        return fdPlayerMuteSolo;
    else
        return bqPlayerMuteSolo;
}

void Java_com_example_nativeaudio_NativeAudio_setChannelMuteUriAudioPlayer(JNIEnv* env,
        jclass clazz, jint chan, jboolean mute)
{
    SLresult result;
    SLMuteSoloItf muteSoloItf = getMuteSolo();
    if (NULL != muteSoloItf) {
        ALOGD("Mute SetChannelSolo设置单独播放某个声道 SetChannelMuted单独静音某个声道 接口都是muteSoloItf ");
        result = (*muteSoloItf)->SetChannelMute(muteSoloItf, chan, mute);
        assert(SL_RESULT_SUCCESS == result);
        (void)result;
    }
}

void Java_com_example_nativeaudio_NativeAudio_setChannelSoloUriAudioPlayer(JNIEnv* env,
        jclass clazz, jint chan, jboolean solo)
{
    SLresult result;
    SLMuteSoloItf muteSoloItf = getMuteSolo();
    if (NULL != muteSoloItf) {
        ALOGD("Solo SetChannelSolo设置单独播放某个声道 SetChannelMuted单独静音某个声道 接口都是muteSoloItf ");
        result = (*muteSoloItf)->SetChannelSolo(muteSoloItf, chan, solo);
        assert(SL_RESULT_SUCCESS == result);
        (void)result;
    }
}

int Java_com_example_nativeaudio_NativeAudio_getNumChannelsUriAudioPlayer(JNIEnv* env, jclass clazz)
{
    SLuint8 numChannels;
    SLresult result;
    SLMuteSoloItf muteSoloItf = getMuteSolo();
    if (NULL != muteSoloItf) {
        result = (*muteSoloItf)->GetNumChannels(muteSoloItf, &numChannels);
        if (SL_RESULT_PRECONDITIONS_VIOLATED == result) {
            // channel count is not yet known
            numChannels = 0;
        } else {
            assert(SL_RESULT_SUCCESS == result);
        }
    } else {
        numChannels = 0;
    }
    return numChannels;
}

// expose the volume APIs to Java for one of the 3 players

static SLVolumeItf getVolume()
{
    if (uriPlayerVolume != NULL)
        return uriPlayerVolume;
    else if (fdPlayerVolume != NULL)
        return fdPlayerVolume;
    else
        return bqPlayerVolume;
}

void Java_com_example_nativeaudio_NativeAudio_setVolumeUriAudioPlayer(JNIEnv* env, jclass clazz,
        jint millibel)
{
    SLresult result;
    SLVolumeItf volumeItf = getVolume();
    if (NULL != volumeItf) {
        result = (*volumeItf)->SetVolumeLevel(volumeItf, millibel);// 设置音量  没有区分左右声道
        assert(SL_RESULT_SUCCESS == result);
        (void)result;
    }
}

void Java_com_example_nativeaudio_NativeAudio_setPlaybackRateUriAudioPlayer(JNIEnv* env, jclass clazz,
                                                                      jint permille)
{
    SLresult result;

    if (NULL != fdlaybackRateItf) {
        result = (*fdlaybackRateItf)->SetRate(fdlaybackRateItf, permille);
        if(SL_RESULT_SUCCESS == result){
            ALOGD("set playback rate to %d done " , permille );
        }else{
            ALOGE("set playback rate to %d ERROR " , permille );
        }
        (void)result;
    }
}




void Java_com_example_nativeaudio_NativeAudio_setMuteUriAudioPlayer(JNIEnv* env, jclass clazz,
        jboolean mute)
{
    SLresult result;
    SLVolumeItf volumeItf = getVolume();
    if (NULL != volumeItf) {
        result = (*volumeItf)->SetMute(volumeItf, mute);
        assert(SL_RESULT_SUCCESS == result);
        (void)result;
    }
}

// 左右声道  相对大小
void Java_com_example_nativeaudio_NativeAudio_enableStereoPositionUriAudioPlayer(JNIEnv* env,
        jclass clazz, jboolean enable)
{
    SLresult result;
    SLVolumeItf volumeItf = getVolume();
    if (NULL != volumeItf) {
        result = (*volumeItf)->EnableStereoPosition(volumeItf, enable);
        assert(SL_RESULT_SUCCESS == result);
        (void)result;
    }
}

void Java_com_example_nativeaudio_NativeAudio_setStereoPositionUriAudioPlayer(JNIEnv* env,
        jclass clazz, jint permille)
{
    SLresult result;
    SLVolumeItf volumeItf = getVolume();
    if (NULL != volumeItf) {
        result = (*volumeItf)->SetStereoPosition(volumeItf, permille);
        assert(SL_RESULT_SUCCESS == result);
        (void)result;
    }
}

// enable reverb on the buffer queue player
jboolean Java_com_example_nativeaudio_NativeAudio_enableReverb(JNIEnv* env, jclass clazz,
        jboolean enabled)
{
    SLresult result;

    // we might not have been able to add environmental reverb to the output mix
    if (NULL == outputMixEnvironmentalReverb) {
        return JNI_FALSE;
    }

    if(bqPlayerSampleRate) {
        /*
         * we are in fast audio, reverb is not supported.
         */
        return JNI_FALSE;
    }
    result = (*bqPlayerEffectSend)->EnableEffectSend(bqPlayerEffectSend,
            outputMixEnvironmentalReverb, (SLboolean) enabled, (SLmillibel) 0);
    // and even if environmental reverb was present, it might no longer be available
    if (SL_RESULT_SUCCESS != result) {
        return JNI_FALSE;
    }

    return JNI_TRUE;
}

// select the desired clip and play count, and enqueue the first buffer if idle
jboolean Java_com_example_nativeaudio_NativeAudio_selectClip(JNIEnv* env, jclass clazz, jint which,
        jint count)
{
    if (pthread_mutex_trylock(&audioEngineLock)) {
        // If we could not acquire audio engine lock, reject this request and client should re-try
        return JNI_FALSE;
    }
    switch (which) {
    case 0:     // CLIP_NONE
        nextBuffer = (short *) NULL;
        nextSize = 0;
        break;
    case 1:     // CLIP_HELLO
        nextBuffer = createResampledBuf(1, SL_SAMPLINGRATE_8, &nextSize);
        if(!nextBuffer) {
            nextBuffer = (short*)hello;
            nextSize  = sizeof(hello);
        }
        break;
    case 2:     // CLIP_ANDROID
        nextBuffer = createResampledBuf(2, SL_SAMPLINGRATE_8, &nextSize);
        if(!nextBuffer) {
            nextBuffer = (short*)android;
            nextSize  = sizeof(android);
        }
        break;
    case 3:     // CLIP_SAWTOOTH
        nextBuffer = createResampledBuf(3, SL_SAMPLINGRATE_8, &nextSize);
        if(!nextBuffer) {
            nextBuffer = (short*)sawtoothBuffer;
            nextSize  = sizeof(sawtoothBuffer);
        }
        break;
    case 4:     // CLIP_PLAYBACK
        nextBuffer = createResampledBuf(4, SL_SAMPLINGRATE_16, &nextSize);
        // we recorded at 16 kHz, but are playing buffers at 8 Khz, so do a primitive down-sample
        if(!nextBuffer) {
            unsigned i;
            for (i = 0; i < recorderSize; i += 2 * sizeof(short)) {
                recorderBuffer[i >> 2] = recorderBuffer[i >> 1];
            }
            recorderSize >>= 1;
            nextBuffer = recorderBuffer;
            nextSize = recorderSize;
        }
        break;
    default:
        nextBuffer = NULL;
        nextSize = 0;
        break;
    }
    nextCount = count;
    if (nextSize > 0) {
        // here we only enqueue one buffer because it is a long clip,
        // but for streaming playback we would typically enqueue at least 2 buffers to start
        SLresult result;
        result = (*bqPlayerBufferQueue)->Enqueue(bqPlayerBufferQueue, nextBuffer, nextSize);
        if (SL_RESULT_SUCCESS != result) {
            pthread_mutex_unlock(&audioEngineLock);
            return JNI_FALSE;
        }
    }

    return JNI_TRUE;
}


// create asset audio player
jboolean Java_com_example_nativeaudio_NativeAudio_createAssetAudioPlayer(JNIEnv* env, jclass clazz,
        jobject assetManager, jstring filename)
{
    SLresult result;

    // convert Java string to UTF-8
    const char *utf8 = (*env)->GetStringUTFChars(env, filename, NULL);
    assert(NULL != utf8);

    // use asset manager to open asset by filename
    AAssetManager* mgr = AAssetManager_fromJava(env, assetManager);
    assert(NULL != mgr);
    AAsset* asset = AAssetManager_open(mgr, utf8, AASSET_MODE_UNKNOWN);

    // release the Java string and UTF-8
    (*env)->ReleaseStringUTFChars(env, filename, utf8);

    // the asset might not be found
    if (NULL == asset) {
        return JNI_FALSE;
    }

    // open asset as file descriptor
    off_t start, length;
    int fd = AAsset_openFileDescriptor(asset, &start, &length);
    assert(0 <= fd);
    AAsset_close(asset);

    // configure audio source
    SLDataLocator_AndroidFD loc_fd = {SL_DATALOCATOR_ANDROIDFD, fd, start, length};
    SLDataFormat_MIME format_mime = {SL_DATAFORMAT_MIME, NULL, SL_CONTAINERTYPE_UNSPECIFIED};
    SLDataSource audioSrc = {&loc_fd, &format_mime};

    // configure audio sink
    SLDataLocator_OutputMix loc_outmix = {SL_DATALOCATOR_OUTPUTMIX, outputMixObject};
    SLDataSink audioSnk = {&loc_outmix, NULL};

    // create audio player
    const SLInterfaceID ids[3] = {SL_IID_SEEK, SL_IID_MUTESOLO, SL_IID_VOLUME};
    const SLboolean req[3] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
    result = (*engineEngine)->CreateAudioPlayer(engineEngine, &fdPlayerObject, &audioSrc, &audioSnk,
            3, ids, req);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // realize the player
    result = (*fdPlayerObject)->Realize(fdPlayerObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // get the play interface
    result = (*fdPlayerObject)->GetInterface(fdPlayerObject, SL_IID_PLAY, &fdPlayerPlay);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // get the seek interface
    result = (*fdPlayerObject)->GetInterface(fdPlayerObject, SL_IID_SEEK, &fdPlayerSeek);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // get the mute/solo interface
    result = (*fdPlayerObject)->GetInterface(fdPlayerObject, SL_IID_MUTESOLO, &fdPlayerMuteSolo);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // get the volume interface
    result = (*fdPlayerObject)->GetInterface(fdPlayerObject, SL_IID_VOLUME, &fdPlayerVolume);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;


    result = (*fdPlayerObject)->GetInterface(fdPlayerObject, SL_IID_PLAYBACKRATE, &fdlaybackRateItf);
    // assert(SL_RESULT_SUCCESS == result); // 不支持 SL_IID_PLAYBACKRATE 这里会产生断言
    if( result != SL_RESULT_SUCCESS ){
        ALOGE("PLAYBACKRATE result = %d " , result);
        fdPlayerObject = NULL ;
    }

    // enable whole file looping
    result = (*fdPlayerSeek)->SetLoop(fdPlayerSeek, SL_BOOLEAN_TRUE, 0, SL_TIME_UNKNOWN);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    return JNI_TRUE;
}

// set the playing state for the asset audio player
void Java_com_example_nativeaudio_NativeAudio_setPlayingAssetAudioPlayer(JNIEnv* env,
        jclass clazz, jboolean isPlaying)
{
    SLresult result;

    // make sure the asset audio player was created
    if (NULL != fdPlayerPlay) {

        // set the player's state
        result = (*fdPlayerPlay)->SetPlayState(fdPlayerPlay, isPlaying ?
            SL_PLAYSTATE_PLAYING : SL_PLAYSTATE_PAUSED);
        assert(SL_RESULT_SUCCESS == result);
        (void)result;
    }

}

// create audio recorder: recorder is not in fast path
//    like to avoid excessive re-sampling while playing back from Hello & Android clip
jboolean Java_com_example_nativeaudio_NativeAudio_createAudioRecorder(JNIEnv* env, jclass clazz)
{
    SLresult result;

    // configure audio source
    SLDataLocator_IODevice loc_dev = {SL_DATALOCATOR_IODEVICE, SL_IODEVICE_AUDIOINPUT,
            SL_DEFAULTDEVICEID_AUDIOINPUT, NULL};
    SLDataSource audioSrc = {&loc_dev, NULL};

    // configure audio sink
    SLDataLocator_AndroidSimpleBufferQueue loc_bq = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
    SLDataFormat_PCM format_pcm = {SL_DATAFORMAT_PCM, 1, SL_SAMPLINGRATE_16,
        SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN};
    SLDataSink audioSnk = {&loc_bq, &format_pcm};

    // create audio recorder
    // (requires the RECORD_AUDIO permission)
    const SLInterfaceID id[1] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    const SLboolean req[1] = {SL_BOOLEAN_TRUE};
    result = (*engineEngine)->CreateAudioRecorder(engineEngine, &recorderObject, &audioSrc,
            &audioSnk, 1, id, req);
    if (SL_RESULT_SUCCESS != result) {
        return JNI_FALSE;
    }

    // realize the audio recorder
    result = (*recorderObject)->Realize(recorderObject, SL_BOOLEAN_FALSE);
    if (SL_RESULT_SUCCESS != result) {
        return JNI_FALSE;
    }

    // get the record interface
    result = (*recorderObject)->GetInterface(recorderObject, SL_IID_RECORD, &recorderRecord);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // get the buffer queue interface
    result = (*recorderObject)->GetInterface(recorderObject, SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
            &recorderBufferQueue);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // register callback on the buffer queue
    result = (*recorderBufferQueue)->RegisterCallback(recorderBufferQueue, bqRecorderCallback,
            NULL);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    return JNI_TRUE;
}


// set the recording state for the audio recorder
void Java_com_example_nativeaudio_NativeAudio_startRecording(JNIEnv* env, jclass clazz)
{
    SLresult result;

    if (pthread_mutex_trylock(&audioEngineLock)) {
        return;
    }
    // in case already recording, stop recording and clear buffer queue
    result = (*recorderRecord)->SetRecordState(recorderRecord, SL_RECORDSTATE_STOPPED);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;
    result = (*recorderBufferQueue)->Clear(recorderBufferQueue);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // the buffer is not valid for playback yet
    recorderSize = 0;

    // enqueue an empty buffer to be filled by the recorder
    // (for streaming recording, we would enqueue at least 2 empty buffers to start things off)
    result = (*recorderBufferQueue)->Enqueue(recorderBufferQueue, recorderBuffer,
            RECORDER_FRAMES * sizeof(short));
    // the most likely other result is SL_RESULT_BUFFER_INSUFFICIENT,
    // which for this code example would indicate a programming error
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // start recording
    result = (*recorderRecord)->SetRecordState(recorderRecord, SL_RECORDSTATE_RECORDING);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;
}


// shut down the native audio system
void Java_com_example_nativeaudio_NativeAudio_shutdown(JNIEnv* env, jclass clazz)
{

    // destroy buffer queue audio player object, and invalidate all associated interfaces
    if (bqPlayerObject != NULL) {
        (*bqPlayerObject)->Destroy(bqPlayerObject);
        bqPlayerObject = NULL;
        bqPlayerPlay = NULL;
        bqPlayerBufferQueue = NULL;
        bqPlayerEffectSend = NULL;
        bqPlayerMuteSolo = NULL;
        bqPlayerVolume = NULL;
    }

    // destroy file descriptor audio player object, and invalidate all associated interfaces
    if (fdPlayerObject != NULL) {
        (*fdPlayerObject)->Destroy(fdPlayerObject);
        fdPlayerObject = NULL;
        fdPlayerPlay = NULL;
        fdPlayerSeek = NULL;
        fdPlayerMuteSolo = NULL;
        fdPlayerVolume = NULL;
    }

    // destroy URI audio player object, and invalidate all associated interfaces
    if (uriPlayerObject != NULL) {
        (*uriPlayerObject)->Destroy(uriPlayerObject);
        uriPlayerObject = NULL;
        uriPlayerPlay = NULL;
        uriPlayerSeek = NULL;
        uriPlayerMuteSolo = NULL;
        uriPlayerVolume = NULL;
    }

    // destroy audio recorder object, and invalidate all associated interfaces
    if (recorderObject != NULL) {
        (*recorderObject)->Destroy(recorderObject);
        recorderObject = NULL;
        recorderRecord = NULL;
        recorderBufferQueue = NULL;
    }

    // destroy output mix object, and invalidate all associated interfaces
    if (outputMixObject != NULL) {
        (*outputMixObject)->Destroy(outputMixObject);
        outputMixObject = NULL;
        outputMixEnvironmentalReverb = NULL;
    }

    // destroy engine object, and invalidate all associated interfaces
    if (engineObject != NULL) {
        (*engineObject)->Destroy(engineObject);
        engineObject = NULL;
        engineEngine = NULL;
    }

    pthread_mutex_destroy(&audioEngineLock);
}
