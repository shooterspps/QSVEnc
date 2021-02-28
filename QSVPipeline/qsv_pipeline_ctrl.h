﻿// -----------------------------------------------------------------------------------------
// QSVEnc by rigaya
// -----------------------------------------------------------------------------------------
// The MIT License
//
// Copyright (c) 2011-2016 rigaya
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// ------------------------------------------------------------------------------------------

#ifndef __QSV_PIPELINE_CTRL_H__
#define __QSV_PIPELINE_CTRL_H__

#include "rgy_version.h"
#include "rgy_osdep.h"
#include "rgy_input.h"
#include "qsv_util.h"
#include "qsv_prm.h"
#include <deque>
#include <optional>
#include "mfxvideo.h"
#include "mfxvideo++.h"
#include "mfxplugin.h"
#include "mfxplugin++.h"
#include "qsv_hw_device.h"
#include "qsv_query.h"
#include "qsv_allocator.h"
#include "qsv_control.h"
#include "rgy_util.h"
#include "rgy_thread.h"
#include "rgy_timecode.h"
#include "rgy_input.h"
#include "rgy_input_sm.h"
#include "rgy_output.h"
#include "rgy_output_avcodec.h"

static void copy_crop_info(mfxFrameSurface1 *dst, const mfxFrameInfo *src) {
    if (dst != nullptr) {
        dst->Info.CropX = src->CropX;
        dst->Info.CropY = src->CropY;
        dst->Info.CropW = src->CropW;
        dst->Info.CropH = src->CropH;
    }
};

enum class PipelineTaskOutputType {
    UNKNOWN,
    SURFACE,
    BITSTREAM
};

class PipelineTaskSurface {
private:
    mfxFrameSurface1 *surf;
    std::atomic<int> *ref;
public:
    PipelineTaskSurface() : surf(nullptr), ref(nullptr) {};
    PipelineTaskSurface(mfxFrameSurface1 *surf_, std::atomic<int> *ref_) : surf(surf_), ref(ref_) { if (surf) (*ref)++; };
    PipelineTaskSurface(const PipelineTaskSurface& obj) : surf(obj.surf), ref(obj.ref) { if (surf) (*ref)++; }
    PipelineTaskSurface &operator=(const PipelineTaskSurface &obj) {
        if (this != &obj) { // 自身の代入チェック
            surf = obj.surf;
            ref = obj.ref;
            if (surf) (*ref)++;
        }
        return *this;
    }
    ~PipelineTaskSurface() { reset(); }
    void reset() { if (surf) (*ref)--; surf = nullptr; ref = nullptr; }

    mfxFrameSurface1 *operator->() {
        return get();
    }
    bool operator !() const {
        return get() == nullptr;
    }
    bool operator !=(const PipelineTaskSurface& obj) const { return get() != obj.get(); }
    bool operator ==(const PipelineTaskSurface& obj) const { return get() == obj.get(); }
    bool operator !=(nullptr_t) const { return get() != nullptr; }
    bool operator ==(nullptr_t) const { return get() == nullptr; }
    const mfxFrameSurface1 *get() const { return surf; }
    mfxFrameSurface1 *get() { return surf; }
};

// アプリ用の独自参照カウンタと組み合わせたクラス
class PipelineTaskSurfaces {
private:
    std::deque<std::pair<mfxFrameSurface1, std::atomic<int>>> m_surfaces; // フレームと参照カウンタ
public:
    PipelineTaskSurfaces() : m_surfaces() {};
    ~PipelineTaskSurfaces() { }

    void clear() {
        m_surfaces.clear();
    }
    void setSurfaces(std::vector<mfxFrameSurface1>& surfs) {
        clear();
        m_surfaces.resize(surfs.size());
        for (int i = 0; i < m_surfaces.size(); i++) {
            m_surfaces[i].first = surfs[i];
            m_surfaces[i].second = 0;
        }
    }

    PipelineTaskSurface getFreeSurf() {
        for (auto& s : m_surfaces) {
            if (isFree(&s)) {
                return PipelineTaskSurface(&s.first, &s.second);
            }
        }
        return PipelineTaskSurface();
    }
    PipelineTaskSurface get(mfxFrameSurface1 *surf) {
        auto s = findSurf(surf);
        if (s != nullptr) {
            return PipelineTaskSurface(&s->first, &s->second);
        }
        return PipelineTaskSurface();
    }
    size_t bufCount() const { return m_surfaces.size(); }

    bool isAllFree() const {
        for (const auto& s : m_surfaces) {
            if (!isFree(&s)) {
                return false;
            }
        }
        return true;
    }

    // 使用されていないフレームかを返す
    // mfxの参照カウンタと独自参照カウンタの両方をチェック
    bool isFree(const std::pair<mfxFrameSurface1, std::atomic<int>> *s) const { return s->first.Data.Locked == 0 && s->second == 0; }
protected:
    std::pair<mfxFrameSurface1, std::atomic<int>> *findSurf(mfxFrameSurface1 *surf) {
        for (auto& s : m_surfaces) {
            if (&s.first == surf) {
                return &s;
            }
        }
        return nullptr;
    }

};

class PipelineTaskOutput {
protected:
    PipelineTaskOutputType m_type;
    MFXVideoSession *m_mfxSession;
    mfxSyncPoint m_syncpoint;
public:
    PipelineTaskOutput(MFXVideoSession *mfxSession) : m_type(PipelineTaskOutputType::UNKNOWN), m_mfxSession(mfxSession), m_syncpoint(nullptr) {};
    PipelineTaskOutput(MFXVideoSession *mfxSession, PipelineTaskOutputType type, mfxSyncPoint syncpoint) : m_type(type), m_mfxSession(mfxSession), m_syncpoint(syncpoint) {};
    RGY_ERR waitsync(uint32_t wait = MSDK_WAIT_INTERVAL) {
        if (m_syncpoint == nullptr) {
            return RGY_ERR_NONE;
        }
        auto err = m_mfxSession->SyncOperation(m_syncpoint, wait);
        m_syncpoint = nullptr;
        return err_to_rgy(err);
    }
    mfxSyncPoint syncpoint() const { return m_syncpoint; }
    PipelineTaskOutputType type() const { return m_type; }
    virtual RGY_ERR write(RGYOutput *writer, QSVAllocator *allocator) {
        UNREFERENCED_PARAMETER(writer);
        UNREFERENCED_PARAMETER(allocator);
        return RGY_ERR_UNSUPPORTED;
    }
    virtual ~PipelineTaskOutput() {};
};

class PipelineTaskOutputSurf : public PipelineTaskOutput {
protected:
    PipelineTaskSurface m_surf;
public:
    PipelineTaskOutputSurf(MFXVideoSession *mfxSession, PipelineTaskSurface surf, mfxSyncPoint syncpoint) : PipelineTaskOutput(mfxSession, PipelineTaskOutputType::SURFACE, syncpoint), m_surf(surf) { };
    virtual ~PipelineTaskOutputSurf() { m_surf.reset(); };

    PipelineTaskSurface& surf() { return m_surf; }

    virtual RGY_ERR write(RGYOutput *writer, QSVAllocator *allocator) override {
        if (!writer || writer->getOutType() == OUT_TYPE_NONE) {
            return RGY_ERR_NOT_INITIALIZED;
        }
        if (writer->getOutType() != OUT_TYPE_SURFACE) {
            return RGY_ERR_INVALID_OPERATION;
        }

        auto mfxSurf = m_surf.get();
        if (mfxSurf->Data.MemId) {
            auto sts = allocator->Lock(allocator->pthis, mfxSurf->Data.MemId, &(mfxSurf->Data));
            if (sts < MFX_ERR_NONE) {
                return err_to_rgy(sts);
            }
        }
        auto err = writer->WriteNextFrame((RGYFrame *)mfxSurf);
        if (mfxSurf->Data.MemId) {
            allocator->Unlock(allocator->pthis, mfxSurf->Data.MemId, &(mfxSurf->Data));
        }
        return err;
    }
};

class PipelineTaskOutputBitstream : public PipelineTaskOutput {
protected:
    std::shared_ptr<RGYBitstream> m_bs;
public:
    PipelineTaskOutputBitstream(MFXVideoSession *mfxSession, std::shared_ptr<RGYBitstream> bs, mfxSyncPoint syncpoint) : PipelineTaskOutput(mfxSession, PipelineTaskOutputType::BITSTREAM, syncpoint), m_bs(bs) {};
    virtual ~PipelineTaskOutputBitstream() { };

    std::shared_ptr<RGYBitstream>& bitstream() { return m_bs; }

    virtual RGY_ERR write(RGYOutput *writer, QSVAllocator *allocator) override {
        if (!writer || writer->getOutType() == OUT_TYPE_NONE) {
            return RGY_ERR_NOT_INITIALIZED;
        }
        if (writer->getOutType() != OUT_TYPE_BITSTREAM) {
            return RGY_ERR_INVALID_OPERATION;
        }
        return writer->WriteNextFrame(m_bs.get());
    }
};

enum class PipelineTaskType {
    UNKNOWN,
    MFXVPP,
    MFXDEC,
    MFXENC,
    MFXENCODE,
    INPUT,
    CHECKPTS,
    TRIM,
    AUDIO,
    OPENCL,
};

static const TCHAR *getPipelineTaskTypeName(PipelineTaskType type) {
    switch (type) {
    case PipelineTaskType::MFXVPP:    return _T("MFXVPP");
    case PipelineTaskType::MFXDEC:    return _T("MFXDEC");
    case PipelineTaskType::MFXENC:    return _T("MFXENC");
    case PipelineTaskType::MFXENCODE: return _T("MFXENCODE");
    case PipelineTaskType::INPUT:     return _T("INPUT");
    case PipelineTaskType::CHECKPTS:  return _T("CHECKPTS");
    case PipelineTaskType::TRIM:      return _T("TRIM");
    case PipelineTaskType::OPENCL:    return _T("OPENCL");
    case PipelineTaskType::AUDIO:     return _T("AUDIO");
    default: return _T("UNKNOWN");
    }
}

// Alllocするときの優先度 値が高い方が優先
static const int getPipelineTaskAllocPriority(PipelineTaskType type) {
    switch (type) {
    case PipelineTaskType::MFXENCODE: return 4;
    case PipelineTaskType::MFXENC:    return 3;
    case PipelineTaskType::MFXDEC:    return 2;
    case PipelineTaskType::MFXVPP:    return 1;
    case PipelineTaskType::INPUT:
    case PipelineTaskType::CHECKPTS:
    case PipelineTaskType::TRIM:
    case PipelineTaskType::OPENCL:
    case PipelineTaskType::AUDIO:
    default: return 0;
    }
}

class PipelineTask {
protected:
    PipelineTaskType m_type;
    std::deque<std::unique_ptr<PipelineTaskOutput>> m_outQeueue;
    PipelineTaskSurfaces m_workSurfs;
    MFXVideoSession *m_mfxSession;
    QSVAllocator *m_allocator;
    mfxFrameAllocResponse m_allocResponse;
    int m_inFrames;
    int m_outFrames;
    int m_outMaxQueueSize;
    mfxVersion m_mfxVer;
    std::shared_ptr<RGYLog> m_log;
public:
    PipelineTask() : m_type(PipelineTaskType::UNKNOWN), m_outQeueue(), m_workSurfs(), m_mfxSession(nullptr), m_allocator(nullptr), m_allocResponse({ 0 }), m_inFrames(0), m_outFrames(0), m_mfxVer({ 0 }), m_outMaxQueueSize(0) {};
    PipelineTask(PipelineTaskType type, int outMaxQueueSize, MFXVideoSession *mfxSession, mfxVersion mfxVer, std::shared_ptr<RGYLog> log) :
        m_type(type), m_outQeueue(), m_workSurfs(), m_mfxSession(mfxSession), m_allocator(nullptr), m_allocResponse({ 0 }), m_inFrames(0), m_outFrames(0), m_outMaxQueueSize(outMaxQueueSize), m_mfxVer(mfxVer), m_log(log) {
    };
    virtual ~PipelineTask() {
        if (m_allocator) {
            m_allocator->Free(m_allocator->pthis, &m_allocResponse);
        }
        m_workSurfs.clear();
    }
    virtual bool isPassThrough() const { return false; }
    virtual tstring print() const { return getPipelineTaskTypeName(m_type); }
    virtual std::optional<mfxFrameAllocRequest> requiredSurfIn() = 0;
    virtual std::optional<mfxFrameAllocRequest> requiredSurfOut() = 0;
    virtual RGY_ERR sendFrame(std::unique_ptr<PipelineTaskOutput>& frame) = 0;
    std::vector<std::unique_ptr<PipelineTaskOutput>> getOutput(const bool sync) {
        std::vector<std::unique_ptr<PipelineTaskOutput>> output;
        while (m_outQeueue.size() > m_outMaxQueueSize) {
            auto out = std::move(m_outQeueue.front());
            m_outQeueue.pop_front();
            if (sync) {
                out->waitsync();
            }
            //out->depend_clear();
            m_outFrames++;
            output.push_back(std::move(out));
        }
        return output;
    }
    bool isMFXTask(const PipelineTaskType task) const {
        return task == PipelineTaskType::MFXDEC
            || task == PipelineTaskType::MFXVPP
            || task == PipelineTaskType::MFXENC
            || task == PipelineTaskType::MFXENCODE;
    }
    // mfx関連とそうでないtaskのやり取りでロックが必要
    bool requireSync(const PipelineTaskType nextTaskType) const {
        return isMFXTask(m_type) != isMFXTask(nextTaskType);
    }
    int workSurfacesAllocPriority() const {
        return getPipelineTaskAllocPriority(m_type);
    }
    size_t workSurfacesCount() const {
        return m_workSurfs.bufCount();
    }
protected:
    RGY_ERR workSurfacesClear() {
        if (m_outQeueue.size() != 0) {
            return RGY_ERR_UNSUPPORTED;
        }
        if (!m_workSurfs.isAllFree()) {
            return RGY_ERR_UNSUPPORTED;
        }
        if (m_allocator && m_workSurfs.bufCount() > 0) {
            auto err = err_to_rgy(m_allocator->Free(m_allocator->pthis, &m_allocResponse));
            if (err != RGY_ERR_NONE) {
                return err;
            }
            m_workSurfs.clear();
        }
        m_allocator = nullptr;
        return RGY_ERR_NONE;
    }
public:
    RGY_ERR workSurfacesAlloc(mfxFrameAllocRequest& allocRequest, const bool externalAlloc, QSVAllocator *allocator) {
        auto sts = workSurfacesClear();
        if (sts != RGY_ERR_NONE) {
            m_log->write(RGY_LOG_ERROR, _T("allocWorkSurfaces:   Failed to clear old surfaces: %s.\n"), get_err_mes(sts));
            return sts;
        }
        m_log->write(RGY_LOG_DEBUG, _T("allocWorkSurfaces:   cleared old surfaces: %s.\n"), get_err_mes(sts));

        m_allocator = allocator;
        sts = err_to_rgy(m_allocator->Alloc(m_allocator->pthis, &allocRequest, &m_allocResponse));
        if (sts != RGY_ERR_NONE) {
            m_log->write(RGY_LOG_ERROR, _T("allocWorkSurfaces:   Failed to allocate frames: %s.\n"), get_err_mes(sts));
            return sts;
        }
        m_log->write(RGY_LOG_DEBUG, _T("allocWorkSurfaces:   allocated %d frames.\n"), allocRequest.NumFrameSuggested);

        std::vector<mfxFrameSurface1> workSurfs(m_allocResponse.NumFrameActual);
        for (size_t i = 0; i < workSurfs.size(); i++) {
            memset(&(workSurfs[i]), 0, sizeof(workSurfs[0]));
            memcpy(&workSurfs[i].Info, &(allocRequest.Info), sizeof(mfxFrameInfo));

            if (externalAlloc) {
                workSurfs[i].Data.MemId = m_allocResponse.mids[i];
            } else {
                sts = err_to_rgy(m_allocator->Lock(m_allocator->pthis, m_allocResponse.mids[i], &(workSurfs[i].Data)));
                if (sts != RGY_ERR_NONE) {
                    m_log->write(RGY_LOG_ERROR, _T("allocWorkSurfaces:   Failed to lock frame #%d: %s.\n"), i, get_err_mes(sts));
                    return sts;
                }
            }
        }
        m_workSurfs.setSurfaces(workSurfs);
        return RGY_ERR_NONE;
    }

    // surfの対応するPipelineTaskSurfaceを見つけ、これから使用するために参照を増やす
    // 破棄時にアプリ側の参照カウンタを減算するようにshared_ptrで設定してある
    PipelineTaskSurface useTaskSurf(mfxFrameSurface1 *surf) {
        return m_workSurfs.get(surf);
    }
    // 使用中でないフレームを探してきて、参照カウンタを加算したものを返す
    // 破棄時にアプリ側の参照カウンタを減算するようにshared_ptrで設定してある
    PipelineTaskSurface getWorkSurf() {
        if (m_workSurfs.bufCount() == 0) {
            return PipelineTaskSurface();
        }
        for (uint32_t i = 0; i < MSDK_WAIT_INTERVAL; i++) {
            PipelineTaskSurface s = m_workSurfs.getFreeSurf();
            if (s != nullptr) {
                return s;
            }
            sleep_hybrid(i);
        }
        return PipelineTaskSurface();
    }

    void setOutputMaxQueueSize(int size) { m_outMaxQueueSize = size; }

    PipelineTaskType taskType() const { return m_type; }
    int inputFrames() const { return m_inFrames; }
    int outputFrames() const { return m_outFrames; }
    int outputMaxQueueSize() const { return m_outMaxQueueSize; }
};

class PipelineTaskInput : public PipelineTask {
    RGYInput *m_input;
    QSVAllocator *m_allocator;
public:
    PipelineTaskInput(MFXVideoSession *mfxSession, QSVAllocator *allocator, int outMaxQueueSize, RGYInput *input, mfxVersion mfxVer, std::shared_ptr<RGYLog> log)
        : PipelineTask(PipelineTaskType::INPUT, outMaxQueueSize, mfxSession, mfxVer, log), m_input(input), m_allocator(allocator) {

    };
    virtual ~PipelineTaskInput() {};
    virtual std::optional<mfxFrameAllocRequest> requiredSurfIn() override { return std::nullopt; };
    virtual std::optional<mfxFrameAllocRequest> requiredSurfOut() override { return std::nullopt; };
    virtual RGY_ERR sendFrame(std::unique_ptr<PipelineTaskOutput>& frame) override {
        auto surfWork = getWorkSurf();
        if (surfWork == nullptr) {
            m_log->write(RGY_LOG_ERROR, _T("failed to get work surface for input.\n"));
            return RGY_ERR_NOT_ENOUGH_BUFFER;
        }
        auto mfxSurf = surfWork.get();
        if (mfxSurf->Data.MemId) {
            auto sts = m_allocator->Lock(m_allocator->pthis, mfxSurf->Data.MemId, &(mfxSurf->Data));
            if (sts < MFX_ERR_NONE) {
                return err_to_rgy(sts);
            }
        }
        auto err = m_input->LoadNextFrame((RGYFrame *)mfxSurf);
        if (err != RGY_ERR_NONE) {
            //Unlockする必要があるので、ここに入ってもすぐにreturnしてはいけない
            if (err == RGY_ERR_MORE_DATA) { // EOF
                err = RGY_ERR_MORE_BITSTREAM; // EOF を PipelineTaskMFXDecode のreturnコードに合わせる
            } else {
                m_log->write(RGY_LOG_ERROR, _T("Error in reader: %s.\n"), get_err_mes(err));
            }
        }
        if (mfxSurf->Data.MemId) {
            m_allocator->Unlock(m_allocator->pthis, mfxSurf->Data.MemId, &(mfxSurf->Data));
        }
        if (err == RGY_ERR_NONE) {
            m_outQeueue.push_back(std::make_unique<PipelineTaskOutputSurf>(m_mfxSession, surfWork, nullptr));
        }
        return err;
    }
};

class PipelineTaskMFXDecode : public PipelineTask {
protected:
    MFXVideoDECODE *m_dec;
    mfxVideoParam& m_mfxDecParams;
    RGYInput *m_input;
    bool m_getNextBitstream;
    RGYBitstream m_decInputBitstream;
public:
    PipelineTaskMFXDecode(MFXVideoSession *mfxSession, int outMaxQueueSize, MFXVideoDECODE *mfxdec, mfxVideoParam& decParams, RGYInput *input, mfxVersion mfxVer, std::shared_ptr<RGYLog> log)
        : PipelineTask(PipelineTaskType::MFXDEC, outMaxQueueSize, mfxSession, mfxVer, log), m_dec(mfxdec), m_mfxDecParams(decParams), m_input(input), m_getNextBitstream(true), m_decInputBitstream() {
        m_decInputBitstream.init(16*1024*1024);
        //TimeStampはQSVに自動的に計算させる
        m_decInputBitstream.setPts(MFX_TIMESTAMP_UNKNOWN);
    };
    virtual ~PipelineTaskMFXDecode() {};
    void setDec(MFXVideoDECODE *mfxdec) { m_dec = mfxdec; };

    virtual std::optional<mfxFrameAllocRequest> requiredSurfIn() override { return std::nullopt; };
    virtual std::optional<mfxFrameAllocRequest> requiredSurfOut() override {
        mfxFrameAllocRequest allocRequest = { 0 };
        auto err = err_to_rgy(m_dec->QueryIOSurf(&m_mfxDecParams, &allocRequest));
        if (err != RGY_ERR_NONE) {
            m_log->write(RGY_LOG_ERROR, _T("  Failed to get required buffer size for %s: %s\n"), getPipelineTaskTypeName(m_type), get_err_mes(err));
            return std::nullopt;
        }
        m_log->write(RGY_LOG_DEBUG, _T("  %s required buffer: %d [%s]\n"), getPipelineTaskTypeName(m_type), allocRequest.NumFrameSuggested, qsv_memtype_str(allocRequest.Type).c_str());
        return std::optional<mfxFrameAllocRequest>(allocRequest);
    }
    virtual RGY_ERR sendFrame(std::unique_ptr<PipelineTaskOutput>& frame) override {
        if (m_getNextBitstream
            //m_DecInputBitstream.size() > 0のときにbitstreamを連結してしまうと
            //環境によっては正常にフレームが取り出せなくなることがある
            //これを避けるため、m_DecInputBitstream.size() == 0のときのみbitstreamを取得する
            //これにより GetNextFrame / SetNextFrame の回数が異常となり、
            //GetNextFrameのロックが抜けれらなくなる場合がある。
            //HWデコード時、本来GetNextFrameのロックは必要ないので、
            //これを無視する実装も併せて行った。
            && (m_decInputBitstream.size() <= 1)) {
            auto ret = m_input->LoadNextFrame(nullptr);
            if (ret != RGY_ERR_NONE && ret != MFX_ERR_MORE_DATA && ret != RGY_ERR_MORE_BITSTREAM) {
                m_log->write(RGY_LOG_ERROR, _T("Error in reader: %s.\n"), get_err_mes(ret));
                return ret;
            }
            //この関数がMFX_ERR_NONE以外を返せば、入力ビットストリームは終了
            ret = m_input->GetNextBitstream(&m_decInputBitstream);
            if (ret == RGY_ERR_MORE_BITSTREAM) {
                m_getNextBitstream = false;
                return ret; //入力ビットストリームは終了
            }
            if (ret != RGY_ERR_NONE) {
                m_log->write(RGY_LOG_ERROR, _T("Error on getting video bitstream: %s.\n"), get_err_mes(ret));
                return ret;
            }
        }

        m_getNextBitstream |= m_decInputBitstream.size() > 0;

        //デコードも行う場合は、デコード用のフレームをpSurfVppInかpSurfEncInから受け取る
        auto surfDecWork = getWorkSurf();
        if (surfDecWork == nullptr) {
            m_log->write(RGY_LOG_ERROR, _T("failed to get work surface for decoder.\n"));
            return RGY_ERR_NOT_ENOUGH_BUFFER;
        }
        mfxBitstream *inputBitstream = (m_getNextBitstream) ? &m_decInputBitstream.bitstream() : nullptr;

        if (!m_mfxDecParams.mfx.FrameInfo.FourCC) {
            //デコード前には、デコード用のパラメータでFrameInfoを更新
            copy_crop_info(surfDecWork.get(), &m_mfxDecParams.mfx.FrameInfo);
        }
        if (check_lib_version(m_mfxVer, MFX_LIB_VERSION_1_9)
            && (m_mfxDecParams.mfx.CodecId == MFX_CODEC_VP8 || m_mfxDecParams.mfx.CodecId == MFX_CODEC_VP9)) { // VP8/VP9ではこの処理が必要
            if (surfDecWork->Info.BitDepthLuma == 0 || surfDecWork->Info.BitDepthChroma == 0) {
                surfDecWork->Info.BitDepthLuma = m_mfxDecParams.mfx.FrameInfo.BitDepthLuma;
                surfDecWork->Info.BitDepthChroma = m_mfxDecParams.mfx.FrameInfo.BitDepthChroma;
            }
        }
        if (inputBitstream != nullptr) {
            if (inputBitstream->TimeStamp == (mfxU64)AV_NOPTS_VALUE) {
                inputBitstream->TimeStamp = (mfxU64)MFX_TIMESTAMP_UNKNOWN;
            }
            inputBitstream->DecodeTimeStamp = MFX_TIMESTAMP_UNKNOWN;
        }
        surfDecWork->Data.TimeStamp = (mfxU64)MFX_TIMESTAMP_UNKNOWN;
        surfDecWork->Data.DataFlag |= MFX_FRAMEDATA_ORIGINAL_TIMESTAMP;
        m_inFrames++;

        mfxStatus dec_sts = MFX_ERR_NONE;
        mfxSyncPoint lastSyncP = nullptr;
        mfxFrameSurface1 *surfDecOut = nullptr;
        for (int i = 0; ; i++) {
            const auto inputDataLen = (inputBitstream) ? inputBitstream->DataLength : 0;
            mfxSyncPoint decSyncPoint = nullptr;
            dec_sts = m_dec->DecodeFrameAsync(inputBitstream, surfDecWork.get(), &surfDecOut, &decSyncPoint);
            lastSyncP = decSyncPoint;

            if (MFX_ERR_NONE < dec_sts && !decSyncPoint) {
                if (MFX_WRN_DEVICE_BUSY == dec_sts)
                    sleep_hybrid(i);
                if (i > 1024 * 1024 * 30) {
                    m_log->write(RGY_LOG_ERROR, _T("device kept on busy for 30s, unknown error occurred.\n"));
                    return RGY_ERR_GPU_HANG;
                }
            } else if (MFX_ERR_NONE < dec_sts && decSyncPoint) {
                dec_sts = MFX_ERR_NONE; //出力があれば、警告は無視する
                break;
            } else if (dec_sts < MFX_ERR_NONE && (dec_sts != MFX_ERR_MORE_DATA && dec_sts != MFX_ERR_MORE_SURFACE)) {
                m_log->write(RGY_LOG_ERROR, _T("DecodeFrameAsync error: %s.\n"), get_err_mes(dec_sts));
                break;
            } else {
                //pInputBitstreamの長さがDecodeFrameAsyncを経ても全く変わっていない場合は、そのデータは捨てる
                //これを行わないとデコードが止まってしまう
                if (dec_sts == MFX_ERR_MORE_DATA && inputBitstream && inputBitstream->DataLength == inputDataLen) {
                    m_log->write((inputDataLen >= 10) ? RGY_LOG_WARN : RGY_LOG_DEBUG,
                        _T("DecodeFrameAsync: removing %d bytes from input bitstream not read by decoder.\n"), inputDataLen);
                    inputBitstream->DataLength = 0;
                    inputBitstream->DataOffset = 0;
                }
                break;
            }
        }
        if (surfDecOut != nullptr && lastSyncP != nullptr) {
            m_outQeueue.push_back(std::make_unique<PipelineTaskOutputSurf>(m_mfxSession, useTaskSurf(surfDecOut), lastSyncP));
        }
        return err_to_rgy(dec_sts);
    }
};


class PipelineTaskCheckPTS : public PipelineTask {
protected:
    rgy_rational<int> m_srcTimebase;
    rgy_rational<int> m_outputTimebase;
    RGYTimestamp& m_timestamp;
    RGYAVSync m_avsync;
    int64_t m_outFrameDuration; //(m_outputTimebase基準)
    int64_t m_tsOutFirst;     //(m_outputTimebase基準)
    int64_t m_tsOutEstimated; //(m_outputTimebase基準)
    int64_t m_tsPrev;         //(m_outputTimebase基準)
public:
    PipelineTaskCheckPTS(rgy_rational<int> srcTimebase, rgy_rational<int> outputTimebase, RGYTimestamp& timestamp, int64_t outFrameDuration, RGYAVSync avsync, int outMaxQueueSize, mfxVersion mfxVer, std::shared_ptr<RGYLog> log) :
        PipelineTask(PipelineTaskType::CHECKPTS, outMaxQueueSize, nullptr, mfxVer, log),
        m_srcTimebase(srcTimebase), m_outputTimebase(outputTimebase), m_timestamp(timestamp), m_avsync(avsync), m_outFrameDuration(outFrameDuration), m_tsOutFirst(-1), m_tsOutEstimated(0), m_tsPrev(-1) {
    };
    virtual ~PipelineTaskCheckPTS() {};

    virtual bool isPassThrough() const override { return true; }
    static const int MAX_FORCECFR_INSERT_FRAMES = 16;

    virtual std::optional<mfxFrameAllocRequest> requiredSurfIn() override { return std::nullopt; };
    virtual std::optional<mfxFrameAllocRequest> requiredSurfOut() override { return std::nullopt; };
    virtual RGY_ERR sendFrame(std::unique_ptr<PipelineTaskOutput>& frame) override {
        if (!frame) {
            return RGY_ERR_MORE_DATA;
        }
        const bool vpp_rff = false;
        const bool vpp_afs_rff_aware = false;
        const rgy_rational<int> hw_timebase = rgy_rational<int>(1, HW_TIMEBASE);
        int64_t outPtsSource = m_tsOutEstimated; //(m_outputTimebase基準)
        int64_t outDuration = m_outFrameDuration; //入力fpsに従ったduration

        PipelineTaskOutputSurf *taskSurf = dynamic_cast<PipelineTaskOutputSurf *>(frame.get());
        if (taskSurf == nullptr) {
            m_log->write(RGY_LOG_ERROR, _T("Invalid frame type: failed to cast to PipelineTaskOutputSurf.\n"));
            return RGY_ERR_UNSUPPORTED;
        }

        if ((m_srcTimebase.n() > 0 && m_srcTimebase.is_valid())
            && ((m_avsync & (RGY_AVSYNC_VFR | RGY_AVSYNC_FORCE_CFR)) || vpp_rff || vpp_afs_rff_aware)) {
            //CFR仮定ではなく、オリジナルの時間を見る
            const auto srcTimestamp = taskSurf->surf()->Data.TimeStamp;
            outPtsSource = rational_rescale(srcTimestamp, m_srcTimebase, m_outputTimebase);
        }
        m_log->write(RGY_LOG_TRACE, _T("check_pts(%d): nOutEstimatedPts %lld, outPtsSource %lld, outDuration %d\n"), m_inFrames, m_tsOutEstimated, outPtsSource, outDuration);
        if (m_tsOutFirst < 0) {
            m_tsOutFirst = outPtsSource; //最初のpts
            m_log->write(RGY_LOG_TRACE, _T("check_pts: m_tsOutFirst %lld\n"), outPtsSource);
        }
        //最初のptsを0に修正
        outPtsSource -= m_tsOutFirst;

#if 0
        if ((m_avsync & RGY_AVSYNC_VFR) || vpp_rff || vpp_afs_rff_aware) {
            if (vpp_rff || vpp_afs_rff_aware) {
                if (std::abs(outPtsSource - nOutEstimatedPts) >= 32 * m_outFrameDuration) {
                    m_log->write(RGY_LOG_TRACE, _T("check_pts: detected gap %lld, changing offset.\n"), outPtsSource, std::abs(outPtsSource - nOutEstimatedPts));
                    //timestampに一定以上の差があればそれを無視する
                    m_tsOutFirst += (outPtsSource - nOutEstimatedPts); //今後の位置合わせのための補正
                    outPtsSource = nOutEstimatedPts;
                    m_log->write(RGY_LOG_TRACE, _T("check_pts:   changed to m_tsOutFirst %lld, outPtsSource %lld.\n"), m_tsOutFirst, outPtsSource);
                }
                auto ptsDiff = outPtsSource - nOutEstimatedPts;
                if (ptsDiff <= std::min<int64_t>(-1, -1 * m_outFrameDuration * 7 / 8)) {
                    //間引きが必要
                    m_log->write(RGY_LOG_TRACE, _T("check_pts(%d):   skipping frame (vfr)\n"), pInputFrame->getFrameInfo().inputFrameId);
                    return RGY_ERR_MORE_SURFACE;
                }
            }
            if (streamIn) {
                //cuvidデコード時は、timebaseの分子はかならず1なので、streamIn->time_baseとズレているかもしれないのでオリジナルを計算
                const auto orig_pts = rational_rescale(pInputFrame->getTimeStamp(), m_srcTimebase, to_rgy(streamIn->time_base));
                //ptsからフレーム情報を取得する
                const auto framePos = pReader->GetFramePosList()->findpts(orig_pts, &nInputFramePosIdx);
                m_log->write(RGY_LOG_TRACE, _T("check_pts(%d):   estimetaed orig_pts %lld, framePos %d\n"), pInputFrame->getFrameInfo().inputFrameId, orig_pts, framePos.poc);
                if (framePos.poc != FRAMEPOS_POC_INVALID && framePos.duration > 0) {
                    //有効な値ならオリジナルのdurationを使用する
                    outDuration = rational_rescale(framePos.duration, to_rgy(streamIn->time_base), m_outputTimebase);
                    m_log->write(RGY_LOG_TRACE, _T("check_pts(%d):   changing duration to original: %d\n"), pInputFrame->getFrameInfo().inputFrameId, outDuration);
                }
            }
        }
        if (m_avsync & RGY_AVSYNC_FORCE_CFR) {
            if (std::abs(outPtsSource - nOutEstimatedPts) >= CHECK_PTS_MAX_INSERT_FRAMES * m_outFrameDuration) {
                //timestampに一定以上の差があればそれを無視する
                m_tsOutFirst += (outPtsSource - nOutEstimatedPts); //今後の位置合わせのための補正
                outPtsSource = nOutEstimatedPts;
                m_log->write(RGY_LOG_WARN, _T("Big Gap was found between 2 frames, avsync might be corrupted.\n"));
                m_log->write(RGY_LOG_TRACE, _T("check_pts:   changed to m_tsOutFirst %lld, outPtsSource %lld.\n"), m_tsOutFirst, outPtsSource);
            }
            auto ptsDiff = outPtsSource - nOutEstimatedPts;
            if (ptsDiff <= std::min<int64_t>(-1, -1 * m_outFrameDuration * 7 / 8)) {
                //間引きが必要
                m_log->write(RGY_LOG_TRACE, _T("check_pts(%d):   skipping frame (assume_cfr)\n"), pInputFrame->getFrameInfo().inputFrameId);
                return RGY_ERR_MORE_SURFACE;
            }
            while (ptsDiff >= std::max<int64_t>(1, m_outFrameDuration * 7 / 8)) {
                //水増しが必要
                createCopy
                nOutEstimatedPts += m_outFrameDuration;
                ptsDiff = outPtsSource - nOutEstimatedPts;
            }
            outPtsSource = nOutEstimatedPts;
        }
        if (m_tsPrev >= outPtsSource) {
            if (m_tsPrev - outPtsSource >= MAX_FORCECFR_INSERT_FRAMES * m_outFrameDuration) {
                m_log->write(RGY_LOG_DEBUG, _T("check_pts: previous pts %lld, current pts %lld, estimated pts %lld, m_tsOutFirst %lld, changing offset.\n"), m_tsPrev, outPtsSource, nOutEstimatedPts, m_tsOutFirst);
                m_tsOutFirst += (outPtsSource - nOutEstimatedPts); //今後の位置合わせのための補正
                outPtsSource = nOutEstimatedPts;
                m_log->write(RGY_LOG_DEBUG, _T("check_pts:   changed to m_tsOutFirst %lld, outPtsSource %lld.\n"), m_tsOutFirst, outPtsSource);
            } else {
                if (m_avsync & RGY_AVSYNC_FORCE_CFR) {
                    //間引きが必要
                    m_log->write(RGY_LOG_WARN, _T("check_pts(%d): timestamp of video frame is smaller than previous frame, skipping frame: previous pts %lld, current pts %lld.\n"), pInputFrame->getFrameInfo().inputFrameId, m_tsPrev, outPtsSource);
                    return RGY_ERR_MORE_SURFACE;
                } else {
                    const auto origPts = outPtsSource;
                    outPtsSource = m_tsPrev + std::max<int64_t>(1, m_outFrameDuration / 8);
                    m_log->write(RGY_LOG_WARN, _T("check_pts(%d): timestamp of video frame is smaller than previous frame, changing pts: %lld -> %lld (previous pts %lld).\n"),
                        pInputFrame->getFrameInfo().inputFrameId, origPts, outPtsSource, m_tsPrev);
                }
            }
        }

#endif
        //次のフレームのptsの予想
        m_inFrames++;
        m_tsOutEstimated += outDuration;
        m_tsPrev = outPtsSource;
        taskSurf->surf()->Data.TimeStamp = rational_rescale(outPtsSource, m_outputTimebase, hw_timebase);
        taskSurf->surf()->Data.DataFlag |= MFX_FRAMEDATA_ORIGINAL_TIMESTAMP;
        m_timestamp.add(taskSurf->surf()->Data.TimeStamp, rational_rescale(outDuration, m_outputTimebase, hw_timebase));
        m_outQeueue.push_back(std::make_unique<PipelineTaskOutputSurf>(m_mfxSession, taskSurf->surf(), taskSurf->syncpoint()));
        return RGY_ERR_NONE;
    }
};

class PipelineTaskAudio : public PipelineTask {
protected:
    RGYInput *m_input;
    std::map<int, std::shared_ptr<RGYOutputAvcodec>> m_pWriterForAudioStreams;
    std::vector<std::shared_ptr<RGYInput>> m_audioReaders;
    std::map<int, std::shared_ptr<QSVEncPlugin>> m_filterForStreams;
public:
    PipelineTaskAudio(RGYInput *input, std::vector<std::shared_ptr<RGYInput>>& audioReaders, std::vector<std::shared_ptr<RGYOutput>>& fileWriterListAudio, int outMaxQueueSize, mfxVersion mfxVer, std::shared_ptr<RGYLog> log) :
        PipelineTask(PipelineTaskType::AUDIO, outMaxQueueSize, nullptr, mfxVer, log),
        m_input(input), m_audioReaders(audioReaders) {
        //streamのindexから必要なwriteへのポインタを返すテーブルを作成
        for (auto writer : fileWriterListAudio) {
            auto pAVCodecWriter = std::dynamic_pointer_cast<RGYOutputAvcodec>(writer);
            if (pAVCodecWriter) {
                auto trackIdList = pAVCodecWriter->GetStreamTrackIdList();
                for (auto trackID : trackIdList) {
                    m_pWriterForAudioStreams[trackID] = pAVCodecWriter;
                }
            }
        }
#if 0
        //streamのtrackIdからパケットを送信するvppフィルタへのポインタを返すテーブルを作成
        for (const auto& pPlugins : m_VppPrePlugins) {
            const int trackId = pPlugins->getTargetTrack();
            if (trackId != 0) {
                m_filterForStreams[trackId] = pPlugins->getPluginHandle();
            }
        }
        for (const auto& pPlugins : m_VppPostPlugins) {
            const int trackId = pPlugins->getTargetTrack();
            if (trackId != 0) {
                m_filterForStreams[trackId] = pPlugins->getPluginHandle();
            }
        }
#endif
    };
    virtual ~PipelineTaskAudio() {};

    virtual bool isPassThrough() const override { return true; }

    virtual std::optional<mfxFrameAllocRequest> requiredSurfIn() override { return std::nullopt; };
    virtual std::optional<mfxFrameAllocRequest> requiredSurfOut() override { return std::nullopt; };

    RGY_ERR extractAudio(int inputFrames) {
        RGY_ERR ret = RGY_ERR_NONE;
#if ENABLE_AVSW_READER
        if (m_pWriterForAudioStreams.size() + m_filterForStreams.size() > 0) {
#if ENABLE_SM_READER
            RGYInputSM *pReaderSM = dynamic_cast<RGYInputSM *>(m_input);
            const int droppedInAviutl = (pReaderSM != nullptr) ? pReaderSM->droppedFrames() : 0;
#else
            const int droppedInAviutl = 0;
#endif

            auto packetList = m_input->GetStreamDataPackets(inputFrames + droppedInAviutl);

            //音声ファイルリーダーからのトラックを結合する
            for (const auto& reader : m_audioReaders) {
                vector_cat(packetList, reader->GetStreamDataPackets(inputFrames + droppedInAviutl));
            }
            //パケットを各Writerに分配する
            for (uint32_t i = 0; i < packetList.size(); i++) {
                const int nTrackId = (int)((uint32_t)packetList[i].flags >> 16);
                if (m_pWriterForAudioStreams.count(nTrackId)) {
                    auto pWriter = m_pWriterForAudioStreams[nTrackId];
                    if (pWriter == nullptr) {
                        m_log->write(RGY_LOG_ERROR, _T("Invalid writer found for track %d\n"), nTrackId);
                        return RGY_ERR_NULL_PTR;
                    }
                    if (RGY_ERR_NONE != (ret = pWriter->WriteNextPacket(&packetList[i]))) {
                        return ret;
                    }
                } else if (m_filterForStreams.count(nTrackId)) {
                    auto pFilter = m_filterForStreams[nTrackId];
                    if (pFilter == nullptr) {
                        m_log->write(RGY_LOG_ERROR, _T("Invalid filter found for track %d\n"), nTrackId);
                        return RGY_ERR_NULL_PTR;
                    }
                    auto sts = pFilter->SendData(PLUGIN_SEND_DATA_AVPACKET, &packetList[i]);
                    if (sts != MFX_ERR_NONE) {
                        return err_to_rgy(sts);
                    }
                } else {
                    m_log->write(RGY_LOG_ERROR, _T("Failed to find writer for track %d\n"), nTrackId);
                    return RGY_ERR_NOT_FOUND;
                }
            }
        }
#endif //ENABLE_AVSW_READER
        return ret;
    };

    virtual RGY_ERR sendFrame(std::unique_ptr<PipelineTaskOutput>& frame) override {
        if (!frame) {
            return RGY_ERR_MORE_DATA;
        }
        m_inFrames++;
        auto err = extractAudio(m_inFrames);
        if (err != RGY_ERR_NONE) {
            return err;
        }
        PipelineTaskOutputSurf *taskSurf = dynamic_cast<PipelineTaskOutputSurf *>(frame.get());
        m_outQeueue.push_back(std::make_unique<PipelineTaskOutputSurf>(m_mfxSession, taskSurf->surf(), taskSurf->syncpoint()));
        return RGY_ERR_NONE;
    }
};

class PipelineTaskTrim : public PipelineTask {
protected:
    const sTrimParam &m_trimParam;
public:
    PipelineTaskTrim(const sTrimParam &trimParam, int outMaxQueueSize, mfxVersion mfxVer, std::shared_ptr<RGYLog> log) :
        PipelineTask(PipelineTaskType::TRIM, outMaxQueueSize, nullptr, mfxVer, log),
        m_trimParam(trimParam) {
    };
    virtual ~PipelineTaskTrim() {};

    virtual bool isPassThrough() const override { return true; }
    virtual std::optional<mfxFrameAllocRequest> requiredSurfIn() override { return std::nullopt; };
    virtual std::optional<mfxFrameAllocRequest> requiredSurfOut() override { return std::nullopt; };

    virtual RGY_ERR sendFrame(std::unique_ptr<PipelineTaskOutput>& frame) override {
        if (!frame) {
            return RGY_ERR_MORE_DATA;
        }
        if (!frame_inside_range(m_inFrames++, m_trimParam.list).first) {
            return RGY_ERR_NONE;
        }
        PipelineTaskOutputSurf *taskSurf = dynamic_cast<PipelineTaskOutputSurf *>(frame.get());
        m_outQeueue.push_back(std::make_unique<PipelineTaskOutputSurf>(m_mfxSession, taskSurf->surf(), taskSurf->syncpoint()));
        return RGY_ERR_NONE;
    }
};

class PipelineTaskMFXVpp : public PipelineTask {
protected:
    MFXVideoVPP *m_vpp;
    mfxVideoParam& m_mfxVppParams;
public:
    PipelineTaskMFXVpp(MFXVideoSession *mfxSession, int outMaxQueueSize, MFXVideoVPP *mfxvpp, mfxVideoParam& vppParams, mfxVersion mfxVer, std::shared_ptr<RGYLog> log)
        : PipelineTask(PipelineTaskType::MFXVPP, outMaxQueueSize, mfxSession, mfxVer, log), m_vpp(mfxvpp), m_mfxVppParams(vppParams) {};
    virtual ~PipelineTaskMFXVpp() {};
    void setVpp(MFXVideoVPP *mfxvpp) { m_vpp = mfxvpp; };
protected:
    RGY_ERR requiredSurfInOut(mfxFrameAllocRequest allocRequest[2]) {
        memset(allocRequest, 0, sizeof(allocRequest));
        // allocRequest[0]はvppへの入力, allocRequest[1]はvppからの出力
        auto err = err_to_rgy(m_vpp->QueryIOSurf(&m_mfxVppParams, allocRequest));
        if (err != RGY_ERR_NONE) {
            m_log->write(RGY_LOG_ERROR, _T("  Failed to get required buffer size for %s: %s\n"), getPipelineTaskTypeName(m_type), get_err_mes(err));
            return err;
        }
        m_log->write(RGY_LOG_DEBUG, _T("  %s required buffer in: %d [%s], out %d [%s]\n"), getPipelineTaskTypeName(m_type),
            allocRequest[0].NumFrameSuggested, qsv_memtype_str(allocRequest[0].Type).c_str(),
            allocRequest[1].NumFrameSuggested, qsv_memtype_str(allocRequest[1].Type).c_str());
        return err;
    }
public:
    virtual std::optional<mfxFrameAllocRequest> requiredSurfIn() override {
        mfxFrameAllocRequest allocRequest[2];
        if (requiredSurfInOut(allocRequest) != RGY_ERR_NONE) {
            return std::nullopt;
        }
        return std::optional<mfxFrameAllocRequest>(allocRequest[0]);
    };
    virtual std::optional<mfxFrameAllocRequest> requiredSurfOut() override {
        mfxFrameAllocRequest allocRequest[2];
        if (requiredSurfInOut(allocRequest) != RGY_ERR_NONE) {
            return std::nullopt;
        }
        return std::optional<mfxFrameAllocRequest>(allocRequest[1]);
    };
    virtual RGY_ERR sendFrame(std::unique_ptr<PipelineTaskOutput>& frame) override {
        if (frame && frame->type() != PipelineTaskOutputType::SURFACE) {
            m_log->write(RGY_LOG_ERROR, _T("Invalid frame type.\n"));
            return RGY_ERR_UNSUPPORTED;
        }

        mfxStatus vpp_sts = MFX_ERR_NONE;

        if (frame) m_inFrames++;

        mfxFrameSurface1 *surfVppIn = (frame) ? dynamic_cast<PipelineTaskOutputSurf *>(frame.get())->surf().get() : nullptr;
        //vpp前に、vpp用のパラメータでFrameInfoを更新
        copy_crop_info(surfVppIn, &m_mfxVppParams.mfx.FrameInfo);

        bool vppMoreOutput = false;
        do {
            vppMoreOutput = false;
            auto surfVppOut = getWorkSurf();
            mfxSyncPoint lastSyncPoint = nullptr;
            for (int i = 0; ; i++) {
                //bob化の際、pSurfVppInに連続で同じフレーム(同じtimestamp)を投入すると、
                //最初のフレームには設定したtimestamp、次のフレームにはMFX_TIMESTAMP_UNKNOWNが設定されて出てくる
                //特別pSurfVppOut側のTimestampを設定する必要はなさそう
                mfxSyncPoint VppSyncPoint = nullptr;
                vpp_sts = m_vpp->RunFrameVPPAsync(surfVppIn, surfVppOut.get(), nullptr, &VppSyncPoint);
                lastSyncPoint = VppSyncPoint;

                if (MFX_ERR_NONE < vpp_sts && !VppSyncPoint) {
                    if (MFX_WRN_DEVICE_BUSY == vpp_sts)
                        sleep_hybrid(i);
                    if (i > 1024 * 1024 * 30) {
                        m_log->write(RGY_LOG_ERROR, _T("device kept on busy for 30s, unknown error occurred.\n"));
                        return RGY_ERR_GPU_HANG;
                    }
                } else if (MFX_ERR_NONE < vpp_sts && VppSyncPoint) {
                    vpp_sts = MFX_ERR_NONE;
                    break;
                } else {
                    break;
                }
            }

            if (surfVppIn && vpp_sts == MFX_ERR_MORE_DATA) {
                vpp_sts = MFX_ERR_NONE;
            } else if (vpp_sts == MFX_ERR_MORE_SURFACE) {
                vppMoreOutput = true;
                vpp_sts = MFX_ERR_NONE;
            } else if (vpp_sts != MFX_ERR_NONE) {
                return err_to_rgy(vpp_sts);
            }

            if (lastSyncPoint != nullptr) {
                m_outQeueue.push_back(std::make_unique<PipelineTaskOutputSurf>(m_mfxSession, surfVppOut, lastSyncPoint));
            }
        } while (vppMoreOutput);
        return err_to_rgy(vpp_sts);
    }
};

class PipelineTaskMFXENC : public PipelineTask {
protected:
    MFXVideoENC *m_enc;
public:
    PipelineTaskMFXENC(MFXVideoSession *mfxSession, int outMaxQueueSize, MFXVideoENC *mfxenc, mfxVersion mfxVer, std::shared_ptr<RGYLog> log)
        : m_enc(mfxenc), PipelineTask(PipelineTaskType::MFXENC, outMaxQueueSize, mfxSession, mfxVer, log) {};
    virtual ~PipelineTaskMFXENC() {};
    void setEnc(MFXVideoENC *mfxenc) { m_enc = mfxenc; };

    virtual RGY_ERR sendFrame(std::unique_ptr<PipelineTaskOutput>& frame) override {
        if (frame && frame->type() != PipelineTaskOutputType::SURFACE) {
            m_log->write(RGY_LOG_ERROR, _T("Invalid frame type.\n"));
            return RGY_ERR_UNSUPPORTED;
        }
    }

};

class PipelineTaskMFXEncode : public PipelineTask {
protected:
    MFXVideoENCODE *m_encode;
    RGYTimecode *m_timecode;
    RGYTimestamp& m_timestamp;
    mfxVideoParam& m_mfxEncParams;
    rgy_rational<int> m_outputTimebase;
    RGYListRef<RGYBitstream> m_bitStreamOut;
public:
    PipelineTaskMFXEncode(
        MFXVideoSession *mfxSession, int outMaxQueueSize, MFXVideoENCODE *mfxencode, mfxVersion mfxVer, mfxVideoParam& encParams,
        RGYTimecode *timecode, rgy_rational<int> outputTimebase, RGYTimestamp& timestamp, std::shared_ptr<RGYLog> log)
        : PipelineTask(PipelineTaskType::MFXENCODE, outMaxQueueSize, mfxSession, mfxVer, log),
        m_encode(mfxencode), m_mfxEncParams(encParams), m_timecode(timecode), m_outputTimebase(outputTimebase), m_timestamp(timestamp) {};
    virtual ~PipelineTaskMFXEncode() {
        m_outQeueue.clear(); // m_bitStreamOutが解放されるよう前にこちらを解放する
    };
    void setEnc(MFXVideoENCODE *mfxencode) { m_encode = mfxencode; };

    virtual std::optional<mfxFrameAllocRequest> requiredSurfIn() override {
        mfxFrameAllocRequest allocRequest = { 0 };
        auto err = err_to_rgy(m_encode->QueryIOSurf(&m_mfxEncParams, &allocRequest));
        if (err != RGY_ERR_NONE) {
            m_log->write(RGY_LOG_ERROR, _T("  Failed to get required buffer size for %s: %s\n"), getPipelineTaskTypeName(m_type), get_err_mes(err));
            return std::nullopt;
        }
        m_log->write(RGY_LOG_DEBUG, _T("  %s required buffer: %d [%s]\n"), getPipelineTaskTypeName(m_type), allocRequest.NumFrameSuggested, qsv_memtype_str(allocRequest.Type).c_str());
        return std::optional<mfxFrameAllocRequest>(allocRequest);
    };
    virtual std::optional<mfxFrameAllocRequest> requiredSurfOut() override { return std::nullopt; };
    virtual RGY_ERR sendFrame(std::unique_ptr<PipelineTaskOutput>& frame) override {
        if (frame && frame->type() != PipelineTaskOutputType::SURFACE) {
            m_log->write(RGY_LOG_ERROR, _T("Invalid frame type.\n"));
            return RGY_ERR_UNSUPPORTED;
        }

        auto bsOut = m_bitStreamOut.get([enc = m_encode, log = m_log](RGYBitstream *bs) {
            mfxVideoParam par = { 0 };
            mfxStatus sts = enc->GetVideoParam(&par);
            if (sts != MFX_ERR_NONE) {
                log->write(RGY_LOG_ERROR, _T("Failed to get required output buffer size from encoder: %s\n"), get_err_mes(sts));
                mfxBitstreamClear(bs->bsptr());
                return 1;
            }

            sts = mfxBitstreamInit(bs->bsptr(), par.mfx.BufferSizeInKB * 1000 * (std::max)(1, (int)par.mfx.BRCParamMultiplier));
            if (sts != MFX_ERR_NONE) {
                log->write(RGY_LOG_ERROR, _T("Failed to allocate memory for output bufffer: %s\n"), get_err_mes(sts));
                mfxBitstreamClear(bs->bsptr());
                return 1;
            }
            return 0;
        });
        if (!bsOut) {
            return RGY_ERR_NULL_PTR;
        }

        //以下の処理は
        mfxFrameSurface1 *surfEncodeIn = (frame) ? dynamic_cast<PipelineTaskOutputSurf*>(frame.get())->surf().get() : nullptr;
        if (surfEncodeIn) {
            m_inFrames++;
            //TimeStampをMFX_TIMESTAMP_UNKNOWNにしておくと、きちんと設定される
            bsOut->setPts((uint64_t)MFX_TIMESTAMP_UNKNOWN);
            bsOut->setDts((uint64_t)MFX_TIMESTAMP_UNKNOWN);
            //bob化の際に増えたフレームのTimeStampには、MFX_TIMESTAMP_UNKNOWNが設定されているのでこれを補間して修正する
            surfEncodeIn->Data.TimeStamp = (uint64_t)m_timestamp.check(surfEncodeIn->Data.TimeStamp);
            if (m_timecode) {
                m_timecode->write(surfEncodeIn->Data.TimeStamp, m_outputTimebase);
            }
        }

        auto enc_sts = MFX_ERR_NONE;
        mfxSyncPoint lastSyncP = nullptr;
        bool bDeviceBusy = false;
        for (int i = 0; ; i++) {
            enc_sts = m_encode->EncodeFrameAsync(nullptr, surfEncodeIn, bsOut->bsptr(), &lastSyncP);
            bDeviceBusy = false;

            if (MFX_ERR_NONE < enc_sts && lastSyncP == nullptr) {
                bDeviceBusy = true;
                if (enc_sts == MFX_WRN_DEVICE_BUSY) {
                    sleep_hybrid(i);
                }
                if (i > 65536 * 1024 * 30) {
                    m_log->write(RGY_LOG_ERROR, _T("device kept on busy for 30s, unknown error occurred.\n"));
                    return RGY_ERR_GPU_HANG;
                }
            } else if (MFX_ERR_NONE < enc_sts && lastSyncP != nullptr) {
                enc_sts = MFX_ERR_NONE;
                break;
            } else if (enc_sts == MFX_ERR_NOT_ENOUGH_BUFFER) {
                enc_sts = mfxBitstreamExtend(bsOut->bsptr(), (uint32_t)bsOut->bufsize() * 3 / 2);
                if (enc_sts < MFX_ERR_NONE) return err_to_rgy(enc_sts);
            } else if (enc_sts < MFX_ERR_NONE && (enc_sts != MFX_ERR_MORE_DATA && enc_sts != MFX_ERR_MORE_SURFACE)) {
                m_log->write(RGY_LOG_ERROR, _T("EncodeFrameAsync error: %s.\n"), get_err_mes(enc_sts));
                break;
            } else {
                QSV_IGNORE_STS(enc_sts, MFX_ERR_MORE_BITSTREAM);
                break;
            }
        }
        if (lastSyncP != nullptr) {
            m_outQeueue.push_back(std::make_unique<PipelineTaskOutputBitstream>(m_mfxSession, bsOut, lastSyncP));
        }
        return err_to_rgy(enc_sts);
    }

};
class PipelineTaskOpenCL : public PipelineTask {
public:
    PipelineTaskOpenCL(MFXVideoSession *mfxSession, int outMaxQueueSize, std::shared_ptr<RGYLog> log) :
        PipelineTask(PipelineTaskType::OPENCL, outMaxQueueSize, mfxSession, MFX_LIB_VERSION_0_0, log) {};
    virtual ~PipelineTaskOpenCL() {};

    virtual std::optional<mfxFrameAllocRequest> requiredSurfIn() override { return std::nullopt; };
    virtual std::optional<mfxFrameAllocRequest> requiredSurfOut() override { return std::nullopt; };
    virtual RGY_ERR sendFrame(std::unique_ptr<PipelineTaskOutput>& frame) override {
        return RGY_ERR_NONE;
    }

};

class PipelineTaskCtrl {
    std::vector<PipelineTask> tasks;
};


#endif // __QSV_PIPELINE_CTRL_H__