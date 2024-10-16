﻿// -----------------------------------------------------------------------------------------
// RGY by rigaya
// -----------------------------------------------------------------------------------------
//
// The MIT License
//
// Copyright (c) 2014-2016 rigaya
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

#ifndef __RGY_FILTER_LIBPLACEBO_H__
#define __RGY_FILTER_LIBPLACEBO_H__

#include "rgy_filter_cl.h"
#include "rgy_prm.h"
#include <array>

class RGYFilterParamLibplacebo : public RGYFilterParam {
public:
    VideoVUIInfo vui;
    RGYFilterParamLibplacebo() : vui() {};
    virtual ~RGYFilterParamLibplacebo() {};
};

class RGYFilterParamLibplaceboResample : public RGYFilterParamLibplacebo {
public:
    RGY_VPP_RESIZE_ALGO resize_algo;
    VppLibplaceboResample resample;
    RGYFilterParamLibplaceboResample() : RGYFilterParamLibplacebo(), resize_algo(RGY_VPP_RESIZE_AUTO), resample() {};
    virtual ~RGYFilterParamLibplaceboResample() {};
    virtual tstring print() const override;
};

#if ENABLE_LIBPLACEBO

#pragma warning (push)
#pragma warning (disable: 4244)
#pragma warning (disable: 4819)
#include <libplacebo/dispatch.h>
#include <libplacebo/renderer.h>
#include <libplacebo/shaders.h>
#include <libplacebo/utils/upload.h>
#include <libplacebo/d3d11.h>
#pragma warning (pop)

template<typename T>
struct RGYLibplaceboDeleter {
    RGYLibplaceboDeleter() : deleter(nullptr) {};
    RGYLibplaceboDeleter(std::function<void(T*)> deleter) : deleter(deleter) {};
    void operator()(T p) { deleter(&p); }
    std::function<void(T*)> deleter;
};

struct RGYLibplaceboTexDeleter {
    RGYLibplaceboTexDeleter() : gpu(nullptr) {};
    RGYLibplaceboTexDeleter(pl_gpu gpu_) : gpu(gpu_) {};
    void operator()(pl_tex p) { if (p) pl_tex_destroy(gpu, &p); }
    pl_gpu gpu;
};

std::unique_ptr<std::remove_pointer<pl_tex>::type, RGYLibplaceboTexDeleter> rgy_pl_tex_recreate(pl_gpu gpu, const pl_tex_params& tex_params);

struct RGYFrameD3D11 : public RGYFrame {
public:
    RGYFrameD3D11();
    virtual ~RGYFrameD3D11();
    virtual RGY_ERR allocate(ID3D11Device *device, const int width, const int height, const RGY_CSP csp, const int bitdepth);
    virtual void deallocate();
    const RGYFrameInfo& frameInfo() { return frame; }
    virtual bool isempty() const { return !frame.ptr[0]; }
    virtual void setTimestamp(uint64_t timestamp) override { frame.timestamp = timestamp; }
    virtual void setDuration(uint64_t duration) override { frame.duration = duration; }
    virtual void setPicstruct(RGY_PICSTRUCT picstruct) override { frame.picstruct = picstruct; }
    virtual void setInputFrameId(int id) override { frame.inputFrameId = id; }
    virtual void setFlags(RGY_FRAME_FLAGS frameflags) override { frame.flags = frameflags; }
    virtual void clearDataList() override { frame.dataList.clear(); }
    virtual const std::vector<std::shared_ptr<RGYFrameData>>& dataList() const override { return frame.dataList; }
    virtual std::vector<std::shared_ptr<RGYFrameData>>& dataList() override { return frame.dataList; }
    virtual void setDataList(const std::vector<std::shared_ptr<RGYFrameData>>& dataList) override { frame.dataList = dataList; }
    
    virtual RGYCLFrameInterop *getCLFrame(RGYOpenCLContext *clctx, RGYOpenCLQueue& queue);
    virtual void resetCLFrame() { clframe.reset(); }
protected:
    RGYFrameD3D11(const RGYFrameD3D11 &) = delete;
    void operator =(const RGYFrameD3D11 &) = delete;
    virtual RGYFrameInfo getInfo() const override {
        return frame;
    }
    RGYFrameInfo frame;
    std::unique_ptr<RGYCLFrameInterop> clframe;
};

class RGYFilterLibplacebo : public RGYFilter {
public:
    RGYFilterLibplacebo(shared_ptr<RGYOpenCLContext> context);
    virtual ~RGYFilterLibplacebo();
    virtual RGY_ERR init(shared_ptr<RGYFilterParam> pParam, shared_ptr<RGYLog> pPrintMes) override;
protected:
    virtual RGY_ERR run_filter(const RGYFrameInfo *pInputFrame, RGYFrameInfo **ppOutputFrames, int *pOutputFrameNum, RGYOpenCLQueue &queue, const std::vector<RGYOpenCLEvent> &wait_events, RGYOpenCLEvent *event) override;
    virtual void close() override;

    virtual RGY_ERR initCommon(shared_ptr<RGYFilterParam> pParam);
    virtual RGY_ERR checkParam(const RGYFilterParam *param) = 0;
    virtual RGY_ERR setLibplaceboParam(const RGYFilterParam *param) = 0;
    virtual RGY_ERR procPlane(pl_tex texOut, const RGYFrameInfo *pDstPlane, pl_tex texIn, const RGYFrameInfo *pSrcPlane) = 0;
    int getTextureBytePerPix(const DXGI_FORMAT format) const;
    virtual RGY_ERR initLibplacebo(const RGYFilterParam *param);
    RGY_CSP getTextureCsp(const RGY_CSP csp);
    DXGI_FORMAT getTextureDXGIFormat(const RGY_CSP csp);

    RGY_CSP m_textCspIn;
    RGY_CSP m_textCspOut;
    DXGI_FORMAT m_dxgiformatIn;
    DXGI_FORMAT m_dxgiformatOut;

    std::unique_ptr<std::remove_pointer<pl_log>::type, RGYLibplaceboDeleter<pl_log>> m_log;
    std::unique_ptr<std::remove_pointer<pl_d3d11>::type, RGYLibplaceboDeleter<pl_d3d11>> m_d3d11;
    std::unique_ptr<std::remove_pointer<pl_dispatch>::type, RGYLibplaceboDeleter<pl_dispatch>> m_dispatch;
    std::unique_ptr<std::remove_pointer<pl_renderer>::type, RGYLibplaceboDeleter<pl_renderer>> m_renderer;
    std::unique_ptr<std::remove_pointer<pl_shader_obj>::type, RGYLibplaceboDeleter<pl_shader_obj>> m_dither_state;

    std::unique_ptr<RGYCLFrame> m_textFrameBufOut;
    std::unique_ptr<RGYFrameD3D11> m_textIn;
    std::unique_ptr<RGYFrameD3D11> m_textOut;
    std::unique_ptr<RGYFilter> m_srcCrop;
    std::unique_ptr<RGYFilter> m_dstCrop;
};

class RGYFilterLibplaceboResample : public RGYFilterLibplacebo {
public:
    RGYFilterLibplaceboResample(shared_ptr<RGYOpenCLContext> context);
    virtual ~RGYFilterLibplaceboResample();
protected:
    virtual RGY_ERR checkParam(const RGYFilterParam *param) override;
    virtual RGY_ERR setLibplaceboParam(const RGYFilterParam *param) override;
    virtual RGY_ERR procPlane(pl_tex texOut, const RGYFrameInfo *pDstPlane, pl_tex texIn, const RGYFrameInfo *pSrcPlane) override;

    std::unique_ptr<pl_sample_filter_params> m_filter_params;
};

#else

class RGYFilterLibplaceboResample : public RGYFilterDisabled {
public:
    RGYFilterLibplaceboResample(shared_ptr<RGYOpenCLContext> context);
    virtual ~RGYFilterLibplaceboResample();
};

#endif // ENABLE_LIBPLACEBO

#endif // __RGY_FILTER_LIBPLACEBO_H__
