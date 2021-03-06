#define LOG_TAG "SpeechVMRecorder"

#include "SpeechVMRecorder.h"

#include <sys/prctl.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <cutils/properties.h>

#include <utils/threads.h>

#include <ftw.h>

#include <hardware_legacy/power.h>

#include "SpeechType.h"
#include "AudioMTKStreamManager.h"

#include "SpeechDriverFactory.h"
#include "SpeechDriverInterface.h"

#include "AudioResourceManager.h"
#include "SpeechDriverFactory.h"

//#define FORCE_ENABLE_VM
//#define VM_FILENAME_ONLY_USE_VM0_TO_VM7

namespace android {

/*==============================================================================
 *                     Property keys
 *============================================================================*/
const char PROPERTY_KEY_VM_INDEX[PROPERTY_KEY_MAX]      = "persist.af.vm_index";
const char PROPERTY_KEY_VM_RECYCLE_ON[PROPERTY_KEY_MAX] = "persist.af.vm_recycle_on";


/*==============================================================================
 *                     Constant
 *============================================================================*/
static const char VM_RECORD_WAKELOCK_NAME[] = "VM_RECORD_WAKELOCK";

static const uint32_t kCondWaitTimeoutMsec = 100; // 100 ms (modem local buf: 10k, and EPL has 2304 byte for each frame (20 ms))

static const uint32_t kReadBufferSize = 0x4000;   // 16 k


/*==============================================================================
 *                     VM File Recycle
 *============================================================================*/
static const uint32_t kMaxNumOfVMFiles = 4096;

static const uint32_t kMinKeepNumOfVMFiles = 16;        // keep at least 16 files which will not be removed
static const uint32_t kMaxSizeOfAllVMFiles = 209715200; // Total > 200 M

static const char     kFolderOfVMFile[]     = "/sdcard/mtklog/audio_dump/";
static const char     kPrefixOfVMFileName[] = "/sdcard/mtklog/audio_dump/VMLog";
static const uint32_t kSizeOfPrefixOfVMFileName = sizeof(kPrefixOfVMFileName) - 1;
static const uint32_t kMaxSizeOfVMFileName = 128;

typedef struct {
    char     path[kMaxSizeOfVMFileName];
    uint32_t size;
} vm_file_info_t;

static vm_file_info_t gVMFileList[kMaxNumOfVMFiles];
static uint32_t       gNumOfVMFiles = 0;
static uint32_t       gTotalSizeOfVMFiles; // Total size of VM files in SD card

static int GetVMFileList(const char *path, const struct stat *sb, int typeflag __unused) {
    if (strncmp(path, kPrefixOfVMFileName, kSizeOfPrefixOfVMFileName) != 0) {
        return 0;
    }

    if (gNumOfVMFiles >= kMaxNumOfVMFiles) {
        return 0;
    }

    // path
    audio_strncpy(gVMFileList[gNumOfVMFiles].path, path, kMaxSizeOfVMFileName);

    // size
    gVMFileList[gNumOfVMFiles].size = sb->st_size;
    gTotalSizeOfVMFiles += sb->st_size;

    // increase index
    gNumOfVMFiles++;

    return 0; // To tell ftw() to continue
}

static int CompareVMFileName(const void *a, const void *b) {
    return strcmp(((vm_file_info_t *)a)->path,
                  ((vm_file_info_t *)b)->path);
}


/*==============================================================================
 *                     Implementation
 *============================================================================*/

SpeechVMRecorder *SpeechVMRecorder::mSpeechVMRecorder = NULL;
SpeechVMRecorder *SpeechVMRecorder::GetInstance() {
    static Mutex mGetInstanceLock;
    Mutex::Autolock _l(mGetInstanceLock);

    if (mSpeechVMRecorder == NULL) {
        mSpeechVMRecorder = new SpeechVMRecorder();
    }
    ASSERT(mSpeechVMRecorder != NULL);
    return mSpeechVMRecorder;
}

SpeechVMRecorder::SpeechVMRecorder() {
    mStarting = false;
    mEnable = false;

    mDumpFile = NULL;
    memset((void *)&mRingBuf, 0, sizeof(RingBuf));

    AUDIO_CUSTOM_PARAM_STRUCT eSphParamNB;
    AudioCustParamClient::GetInstance()->GetNBSpeechParamFromNVRam(&eSphParamNB);
    mAutoVM = eSphParamNB.uAutoVM;

    m_CtmDebug_Started = false;
    pCtmDumpFileUlIn = NULL;
    pCtmDumpFileDlIn = NULL;
    pCtmDumpFileUlOut = NULL;
    pCtmDumpFileDlOut = NULL;

    mOpenIndex = 0;
}

SpeechVMRecorder::~SpeechVMRecorder() {
    Close();
}

status_t SpeechVMRecorder::Open() {
    AL_LOCK(mMutex);

    ALOGD("+%s()", __FUNCTION__);

    ASSERT(mEnable == false);

    int ret = acquire_wake_lock(PARTIAL_WAKE_LOCK, VM_RECORD_WAKELOCK_NAME);
    ALOGD("%s(), acquire_wake_lock: %s, return %d.", __FUNCTION__, VM_RECORD_WAKELOCK_NAME, ret);

    // create another thread to avoid fwrite() block CCCI read thread
    pthread_create(&mRecordThread, NULL, DumpVMRecordDataThread, (void *)this);

    AL_UNLOCK(mMutex);
    mEnable = true;

    ALOGD("-%s(), mEnable=%d ", __FUNCTION__, mEnable);
    return NO_ERROR;
}

status_t SpeechVMRecorder::OpenFile() {
    char vm_file_path[kMaxSizeOfVMFileName];
    memset((void *)vm_file_path, 0, kMaxSizeOfVMFileName);

#ifdef VM_FILENAME_ONLY_USE_VM0_TO_VM7
    char property_value[PROPERTY_VALUE_MAX];
    property_get(PROPERTY_KEY_VM_INDEX, property_value, "0");

    uint8_t vm_file_number = atoi(property_value);
    sprintf(vm_file_path, "%s_%u.vm", kPrefixOfVMFileName, vm_file_number++);

    sprintf(property_value, "%u", vm_file_number & 0x7);
    property_set(PROPERTY_KEY_VM_INDEX, property_value);
#else
    time_t rawtime;
    time(&rawtime);
    struct tm *timeinfo = localtime(&rawtime);
    audio_strncpy(vm_file_path, kPrefixOfVMFileName, kMaxSizeOfVMFileName);
    strftime(vm_file_path + kSizeOfPrefixOfVMFileName, kMaxSizeOfVMFileName - kSizeOfPrefixOfVMFileName - 1, "_%Y_%m_%d_%H%M%S.vm", timeinfo);
#endif

    ALOGD("%s(), vm_file_path: \"%s\"", __FUNCTION__, vm_file_path);

    // check vm_file_path is valid
    int ret = AudiocheckAndCreateDirectory(vm_file_path);
    if (ret < 0) {
        ALOGE("%s(), AudiocheckAndCreateDirectory(%s) fail!!", __FUNCTION__, vm_file_path);
        return UNKNOWN_ERROR;
    }

    // open VM file
    mDumpFile = fopen(vm_file_path, "wb");
    if (mDumpFile == NULL) {
        ALOGE("%s(), fopen(%s) fail!!", __FUNCTION__, vm_file_path);
        return UNKNOWN_ERROR;
    }

    return NO_ERROR;
}

uint16_t SpeechVMRecorder::CopyBufferToVM(RingBuf ul_ring_buf) {
    AL_LOCK(mMutex);

    if (mStarting == false) {
        ALOGD("%s(), mStarting == false, return.", __FUNCTION__);
        AL_SIGNAL(mMutex); // wake up thread to exit
        AL_UNLOCK(mMutex);
        return 0;
    }

    // get free space of internal input buffer
    uint16_t free_space = RingBuf_getFreeSpace(&mRingBuf);
    SLOGV("%s(), mRingBuf remain data count: %u, free_space: %u", __FUNCTION__, RingBuf_getDataCount(&mRingBuf), free_space);

    // get data count in share buffer
    uint16_t ul_data_count = RingBuf_getDataCount(&ul_ring_buf);
    SLOGV("%s(), ul_ring_buf data count: %u", __FUNCTION__, ul_data_count);

    // check free space for internal input buffer
    uint16_t copy_data_count = 0;
    if (ul_data_count <= free_space) {
        copy_data_count = ul_data_count;
    } else {
        ALOGE("%s(), ul_data_count(%u) > free_space(%u)", __FUNCTION__, ul_data_count, free_space);
        copy_data_count = free_space;
    }

    // copy data from modem share buffer to internal input buffer
    if (copy_data_count > 0) {
        SLOGV("%s(), copy_data_count: %u", __FUNCTION__, copy_data_count);
        RingBuf_copyFromRingBuf(&mRingBuf, &ul_ring_buf, copy_data_count);
    }

    // signal
    AL_SIGNAL(mMutex); // wake up thread to fwrite data.
    AL_UNLOCK(mMutex);

    return copy_data_count;
}

void *SpeechVMRecorder::DumpVMRecordDataThread(void *arg) {
    // Adjust thread priority
    prctl(PR_SET_NAME, (unsigned long)__FUNCTION__, 0, 0, 0);
    int rtnPrio = setpriority(PRIO_PROCESS, 0, ANDROID_PRIORITY_AUDIO);
    if (0 != rtnPrio) {
        ALOGE("[%s] failed, errno: %d, return=%d", __FUNCTION__, errno, rtnPrio);
    } else {
        ALOGD("%s setpriority ok, priority: %d", __FUNCTION__, ANDROID_PRIORITY_AUDIO);
    }

    ALOGD("%s(), pid: %d, tid: %d", __FUNCTION__, getpid(), gettid());

    SpeechVMRecorder *pSpeechVMRecorder = (SpeechVMRecorder *)arg;
    RingBuf &ring_buf = pSpeechVMRecorder->mRingBuf;

    // open file
    if (pSpeechVMRecorder->OpenFile() != NO_ERROR) {
        pSpeechVMRecorder->mEnable = false;
        pthread_exit(NULL);
        return 0;
    }

    // open modem record function
    SpeechDriverInterface *pSpeechDriver = SpeechDriverFactory::GetInstance()->GetSpeechDriver();
    status_t retval = pSpeechDriver->VoiceMemoRecordOn();
    if (retval != NO_ERROR) {
        ALOGE("%s(), VoiceMemoRecordOn() fail!! Return.", __FUNCTION__);
        pSpeechDriver->VoiceMemoRecordOff();
        pSpeechVMRecorder->mEnable = false;
        pthread_exit(NULL);
        return 0;
    }

    // Internal Input Buffer Initialization
    pSpeechVMRecorder->mRingBuf.pBufBase = new char[kReadBufferSize];
    pSpeechVMRecorder->mRingBuf.bufLen   = kReadBufferSize;
    pSpeechVMRecorder->mRingBuf.pRead    = pSpeechVMRecorder->mRingBuf.pBufBase;
    pSpeechVMRecorder->mRingBuf.pWrite   = pSpeechVMRecorder->mRingBuf.pBufBase;

    ASSERT(pSpeechVMRecorder->mRingBuf.pBufBase != NULL);
    memset(pSpeechVMRecorder->mRingBuf.pBufBase, 0, pSpeechVMRecorder->mRingBuf.bufLen);

    pSpeechVMRecorder->mStarting = true;

    while (1) {
        // lock & wait data
        AL_LOCK(pSpeechVMRecorder->mMutex);
        if (AL_WAIT_MS(pSpeechVMRecorder->mMutex, kCondWaitTimeoutMsec) != 0) {
            ALOGW("%s(), waitRelative fail", __FUNCTION__);
        }

        // make sure VM is still recording after conditional wait
        if (pSpeechVMRecorder->mEnable == false) {

            // close file
            if (pSpeechVMRecorder->mDumpFile != NULL) {
                fflush(pSpeechVMRecorder->mDumpFile);
                fclose(pSpeechVMRecorder->mDumpFile);
                pSpeechVMRecorder->mDumpFile = NULL;
            }

            // release local ring buffer
            if (pSpeechVMRecorder->mRingBuf.pBufBase != NULL) {
                delete []pSpeechVMRecorder->mRingBuf.pBufBase;
                pSpeechVMRecorder->mRingBuf.pBufBase = NULL;

                pSpeechVMRecorder->mRingBuf.pRead    = NULL;
                pSpeechVMRecorder->mRingBuf.pWrite   = NULL;
                pSpeechVMRecorder->mRingBuf.bufLen   = 0;
            }

            ALOGD("%s(), pid: %d, tid: %d, mEnable == false, break.", __FUNCTION__, getpid(), gettid());
            AL_UNLOCK(pSpeechVMRecorder->mMutex);
            break;
        }

        // write data to sd card
        const uint16_t data_count = RingBuf_getDataCount(&ring_buf);
        uint16_t write_bytes = 0;

        if (data_count > 0) {
            const char *end = ring_buf.pBufBase + ring_buf.bufLen;
            if (ring_buf.pRead <= ring_buf.pWrite) {
                write_bytes += fwrite((void *)ring_buf.pRead, sizeof(char), data_count, pSpeechVMRecorder->mDumpFile);
            } else {
                int r2e = end - ring_buf.pRead;
                write_bytes += fwrite((void *)ring_buf.pRead, sizeof(char), r2e, pSpeechVMRecorder->mDumpFile);
                write_bytes += fwrite((void *)ring_buf.pBufBase, sizeof(char), data_count - r2e, pSpeechVMRecorder->mDumpFile);
            }

            ring_buf.pRead += write_bytes;
            if (ring_buf.pRead >= end) { ring_buf.pRead -= ring_buf.bufLen; }

            SLOGV("data_count: %u, write_bytes: %u", data_count, write_bytes);
        }

        if (write_bytes != data_count) {
            ALOGE("%s(), write_bytes(%d) != data_count(%d), SD Card might be full!!", __FUNCTION__, write_bytes, data_count);
        }

        // unlock
        AL_UNLOCK(pSpeechVMRecorder->mMutex);
    }

    pthread_exit(NULL);
    return 0;
}


status_t SpeechVMRecorder::Close() {
    AL_LOCK(mMutex);

    ALOGD("+%s()", __FUNCTION__);

    if (mEnable == false) {
        ALOGW("-%s(), mEnable == false, return!!", __FUNCTION__);
        AL_UNLOCK(mMutex);
        return INVALID_OPERATION;
    }

    mStarting = false;

    int ret = 0;


    // VM log recycle mechanism
    char property_value[PROPERTY_VALUE_MAX];
    property_get(PROPERTY_KEY_VM_RECYCLE_ON, property_value, "1"); //"1": default on
    const bool vm_recycle_on = (property_value[0] == '0') ? false : true;

    if (vm_recycle_on == true) {
        // Get gVMFileList, gNumOfVMFiles, gTotalSizeOfVMFiles
        memset(gVMFileList, 0, sizeof(gVMFileList));
        gNumOfVMFiles = 0;
        gTotalSizeOfVMFiles = 0;

        ret = ftw(kFolderOfVMFile, GetVMFileList, FTW_D);
        ASSERT(ret == 0);

        // Sort file name
        qsort(gVMFileList, gNumOfVMFiles, sizeof(vm_file_info_t), CompareVMFileName);
        //for(int i = 0; i < gNumOfVMFiles; i++)
        //    ALOGD("%s(), %s, %u", __FUNCTION__, gVMFileList[i].path, gVMFileList[i].size);

        // Remove VM files
        uint32_t index_vm_file_list = 0;
        while (gNumOfVMFiles > kMinKeepNumOfVMFiles && gTotalSizeOfVMFiles > kMaxSizeOfAllVMFiles) {
            ALOGD("%s(), gNumOfVMFiles = %lu, gTotalSizeOfVMFiles = %lu", __FUNCTION__, gNumOfVMFiles, gTotalSizeOfVMFiles);

            ALOGD("%s(), remove(%s), size = %lu", __FUNCTION__, gVMFileList[index_vm_file_list].path, gVMFileList[index_vm_file_list].size);
            ret = remove(gVMFileList[index_vm_file_list].path);
            ASSERT(ret == 0);

            gNumOfVMFiles--;
            gTotalSizeOfVMFiles -= gVMFileList[index_vm_file_list].size;

            index_vm_file_list++;
        }
    }


    // release wake lock
    ret = release_wake_lock(VM_RECORD_WAKELOCK_NAME);
    ALOGD("%s(), release_wake_lock:%s return %d.", __FUNCTION__, VM_RECORD_WAKELOCK_NAME, ret);

    // turn off record function
    SpeechDriverFactory::GetInstance()->GetSpeechDriver()->VoiceMemoRecordOff();

    mEnable = false;
    AL_UNLOCK(mMutex);
    AL_SIGNAL(mMutex); // wake up thread to exit

    ALOGD("-%s()", __FUNCTION__);
    return NO_ERROR;
}

void SpeechVMRecorder::SetVMRecordCapability(const AUDIO_CUSTOM_PARAM_STRUCT *pSphParamNB) {
    ALOGD("%s(), uAutoVM = 0x%x, debug_info[0] = %u, speech_common_para[0] = %u", __FUNCTION__,
          pSphParamNB->uAutoVM, pSphParamNB->debug_info[0], pSphParamNB->speech_common_para[0]);

    mAutoVM = pSphParamNB->uAutoVM;

    SpeechDriverInterface *pSpeechDriver = SpeechDriverFactory::GetInstance()->GetSpeechDriver();
    const bool speech_on = pSpeechDriver->GetApSideModemStatus(SPEECH_STATUS_MASK);
    const bool rec_on    = pSpeechDriver->GetApSideModemStatus(RECORD_STATUS_MASK);

    AudioResourceManager::getInstance()->EnableAudioLock(AudioResourceManagerInterface::AUDIO_STREAMINMANAGER_LOCK, AUDIO_LOCK_TIMEOUT_VALUE_MS);

    if (GetVMRecordCapability() == true && GetVMRecordStatus() == false && speech_on == true) {
        // turn off normal phone record
        if (rec_on == true) {
            ALOGW("%s(), Turn off normal phone recording!!", __FUNCTION__);
            ALOGW("%s(), The following record file will be silence until VM/EPL is closed.", __FUNCTION__);
            AudioMTKStreamManager::getInstance()->ForceAllStandby(); // TODO: only need to standby input stream
        }

        ALOGD("%s(), Open VM/EPL record", __FUNCTION__);
        Open();
    } else if (GetVMRecordCapability() == false && GetVMRecordStatus() == true) {
        ALOGD("%s(), Close VM/EPL record", __FUNCTION__);
        ALOGD("%s(), Able to continue to do phone record.", __FUNCTION__);
        Close();
    }

    AudioResourceManager::getInstance()->DisableAudioLock(AudioResourceManagerInterface::AUDIO_STREAMINMANAGER_LOCK);
}

bool SpeechVMRecorder::GetVMRecordCapability() const {
#if defined(FORCE_ENABLE_VM)
    return true;
#else
    return ((mAutoVM & VM_RECORD_VM_MASK) > 0);
#endif
}

bool SpeechVMRecorder::GetVMRecordCapabilityForCTM4Way() const {
    bool retval = false;

    if ((mAutoVM & VM_RECORD_VM_MASK) > 0) { // cannot support VM and CTM4way record at the same time
        retval =  false;
    } else if ((mAutoVM & VM_RECORD_CTM4WAY_MASK) > 0) {
        retval = true;
    }

    return retval;
}

int SpeechVMRecorder::StartCtmDebug() {
    ALOGD("%s()", __FUNCTION__);

    if (m_CtmDebug_Started) { return false; }

    const uint8_t kMaxPathLength = 80;
    char ctm_file_path_UlIn[kMaxPathLength];
    char ctm_file_path_DlIn[kMaxPathLength];
    char ctm_file_path_UlOut[kMaxPathLength];
    char ctm_file_path_DlOut[kMaxPathLength];
    memset((void *)ctm_file_path_UlIn, 0, kMaxPathLength);
    memset((void *)ctm_file_path_DlIn, 0, kMaxPathLength);
    memset((void *)ctm_file_path_UlOut, 0, kMaxPathLength);
    memset((void *)ctm_file_path_DlOut, 0, kMaxPathLength);
    time_t rawtime;
    time(&rawtime);
    struct tm *timeinfo = localtime(&rawtime);
    strftime(ctm_file_path_UlIn, kMaxPathLength, "/sdcard/mtklog/audio_dump/%Y_%m_%d_%H%M%S_CtmUlIn.pcm", timeinfo);
    strftime(ctm_file_path_DlIn, kMaxPathLength, "/sdcard/mtklog/audio_dump/%Y_%m_%d_%H%M%S_CtmDlIn.pcm", timeinfo);
    strftime(ctm_file_path_UlOut, kMaxPathLength, "/sdcard/mtklog/audio_dump/%Y_%m_%d_%H%M%S_CtmUlOut.pcm", timeinfo);
    strftime(ctm_file_path_DlOut, kMaxPathLength, "/sdcard/mtklog/audio_dump/%Y_%m_%d_%H%M%S_CtmDlOut.pcm", timeinfo);
    int ret;
    ret = AudiocheckAndCreateDirectory(ctm_file_path_UlIn);
    if (ret < 0) {
        ALOGE("%s(), AudiocheckAndCreateDirectory(%s) fail!!", __FUNCTION__, ctm_file_path_UlIn);
        return UNKNOWN_ERROR;
    }
    ret = AudiocheckAndCreateDirectory(ctm_file_path_DlIn);
    if (ret < 0) {
        ALOGE("%s(), AudiocheckAndCreateDirectory(%s) fail!!", __FUNCTION__, ctm_file_path_DlIn);
        return UNKNOWN_ERROR;
    }
    ret = AudiocheckAndCreateDirectory(ctm_file_path_UlOut);
    if (ret < 0) {
        ALOGE("%s(), AudiocheckAndCreateDirectory(%s) fail!!", __FUNCTION__, ctm_file_path_UlOut);
        return UNKNOWN_ERROR;
    }
    ret = AudiocheckAndCreateDirectory(ctm_file_path_DlOut);
    if (ret < 0) {
        ALOGE("%s(), AudiocheckAndCreateDirectory(%s) fail!!", __FUNCTION__, ctm_file_path_DlOut);
        return UNKNOWN_ERROR;
    }
    pCtmDumpFileUlIn = fopen(ctm_file_path_UlIn, "wb");
    pCtmDumpFileDlIn = fopen(ctm_file_path_DlIn, "wb");
    pCtmDumpFileUlOut = fopen(ctm_file_path_UlOut, "wb");
    pCtmDumpFileDlOut = fopen(ctm_file_path_DlOut, "wb");

    if (pCtmDumpFileUlIn == NULL) { ALOGW("Fail to Open pCtmDumpFileUlIn"); }
    if (pCtmDumpFileDlIn == NULL) { ALOGW("Fail to Open pCtmDumpFileDlIn"); }
    if (pCtmDumpFileUlOut == NULL) { ALOGW("Fail to Open pCtmDumpFileUlOut"); }
    if (pCtmDumpFileDlOut == NULL) { ALOGW("Fail to Open pCtmDumpFileDlOut"); }

    m_CtmDebug_Started = true;

    return true;
}

int SpeechVMRecorder::StopCtmDebug() {
    ALOGD("%s()", __FUNCTION__);

    if (!m_CtmDebug_Started) { return false; }

    m_CtmDebug_Started = false;

    fclose(pCtmDumpFileUlIn);
    fclose(pCtmDumpFileDlIn);
    fclose(pCtmDumpFileUlOut);
    fclose(pCtmDumpFileDlOut);
    return true;
}
void SpeechVMRecorder::GetCtmDebugDataFromModem(RingBuf ul_ring_buf, FILE *pFile) {
    int InpBuf_freeSpace = 0;
    size_t ShareBuf_dataCnt = 0;

    if (m_CtmDebug_Started == false) {
        ALOGD("GetCtmDebugDataFromModem, m_CtmDebug_Started=false");
        return;
    }

    // get data count in share buffer
    ShareBuf_dataCnt = (size_t) RingBuf_getDataCount(&ul_ring_buf);

    char linear_buffer[ShareBuf_dataCnt];
    char *pM2AShareBufEnd = ul_ring_buf.pBufBase + ul_ring_buf.bufLen;
    if (ul_ring_buf.pRead + ShareBuf_dataCnt <= pM2AShareBufEnd) {
        memcpy(linear_buffer, ul_ring_buf.pRead, ShareBuf_dataCnt);
    } else {
        uint32_t r2e = pM2AShareBufEnd - ul_ring_buf.pRead;
        memcpy(linear_buffer, ul_ring_buf.pRead, r2e);
        memcpy((void *)(linear_buffer + r2e), ul_ring_buf.pBufBase, ShareBuf_dataCnt - r2e);
    }

    fwrite(linear_buffer, sizeof(char), ShareBuf_dataCnt, pFile);

}

};
