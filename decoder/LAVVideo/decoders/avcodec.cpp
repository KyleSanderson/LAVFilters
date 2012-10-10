/*
 *      Copyright (C) 2010-2012 Hendrik Leppkes
 *      http://www.1f0.de
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "stdafx.h"
#include "avcodec.h"

#include "moreuuids.h"
#include "parsers/MPEG2HeaderParser.h"
#include "parsers/H264SequenceParser.h"
#include "parsers/VC1HeaderParser.h"

#include "Media.h"

#ifdef DEBUG
#include "lavf_log.h"
#endif

extern "C" {
#include "libavutil/pixdesc.h"
};

////////////////////////////////////////////////////////////////////////////////
// Constructor
////////////////////////////////////////////////////////////////////////////////

ILAVDecoder *CreateDecoderAVCodec() {
  return new CDecAvcodec();
}

////////////////////////////////////////////////////////////////////////////////
// Multi-threaded decoding configuration
////////////////////////////////////////////////////////////////////////////////
static struct {
  AVCodecID codecId;
  int     threadFlags;
} ff_thread_codecs[] = {
  { AV_CODEC_ID_H264,       FF_THREAD_FRAME|FF_THREAD_SLICE },
  { AV_CODEC_ID_MPEG1VIDEO,                 FF_THREAD_SLICE },
  { AV_CODEC_ID_MPEG2VIDEO,                 FF_THREAD_SLICE },
  { AV_CODEC_ID_DVVIDEO,                    FF_THREAD_SLICE },
  { AV_CODEC_ID_VP8,        FF_THREAD_FRAME                 },
  { AV_CODEC_ID_VP3,        FF_THREAD_FRAME                 },
  { AV_CODEC_ID_THEORA,     FF_THREAD_FRAME                 },
  { AV_CODEC_ID_HUFFYUV,    FF_THREAD_FRAME                 },
  { AV_CODEC_ID_FFVHUFF,    FF_THREAD_FRAME                 },
  //{ AV_CODEC_ID_MPEG4,      FF_THREAD_FRAME                 },
  { AV_CODEC_ID_PRORES,                     FF_THREAD_SLICE },
  { AV_CODEC_ID_UTVIDEO,    FF_THREAD_FRAME                 },
  { AV_CODEC_ID_RV30,       FF_THREAD_FRAME                 },
  { AV_CODEC_ID_RV40,       FF_THREAD_FRAME                 },
  { AV_CODEC_ID_DNXHD,      FF_THREAD_FRAME                 },
  { AV_CODEC_ID_FFV1,                       FF_THREAD_SLICE },
  { AV_CODEC_ID_LAGARITH,   FF_THREAD_FRAME                 },
  { AV_CODEC_ID_FRAPS,      FF_THREAD_FRAME                 },
};

int getThreadFlags(AVCodecID codecId)
{
  for(int i = 0; i < countof(ff_thread_codecs); ++i) {
    if (ff_thread_codecs[i].codecId == codecId) {
      return ff_thread_codecs[i].threadFlags;
    }
  }
  return 0;
}

////////////////////////////////////////////////////////////////////////////////
// Create DXVA2 Extended Flags from a AVFrame and AVCodecContext
////////////////////////////////////////////////////////////////////////////////

static DXVA2_ExtendedFormat GetDXVA2ExtendedFlags(AVCodecContext *ctx, AVFrame *frame)
{
  DXVA2_ExtendedFormat fmt;
  ZeroMemory(&fmt, sizeof(fmt));

  fillDXVAExtFormat(fmt, -1, ctx->color_primaries, ctx->colorspace, ctx->color_trc);

  // Chroma location
  switch(ctx->chroma_sample_location) {
  case AVCHROMA_LOC_LEFT:
    fmt.VideoChromaSubsampling = DXVA2_VideoChromaSubsampling_MPEG2;
    break;
  case AVCHROMA_LOC_CENTER:
    fmt.VideoChromaSubsampling = DXVA2_VideoChromaSubsampling_MPEG1;
    break;
  case AVCHROMA_LOC_TOPLEFT:
    fmt.VideoChromaSubsampling = DXVA2_VideoChromaSubsampling_DV_PAL;
    break;
  }

  // Color Range, 0-255 or 16-235
  BOOL ffFullRange = (ctx->color_range == AVCOL_RANGE_JPEG) || ctx->pix_fmt == PIX_FMT_YUVJ420P || ctx->pix_fmt == PIX_FMT_YUVJ422P || ctx->pix_fmt == PIX_FMT_YUVJ444P || ctx->pix_fmt == PIX_FMT_YUVJ440P;
  fmt.NominalRange = ffFullRange ? DXVA2_NominalRange_0_255 : (ctx->color_range == AVCOL_RANGE_MPEG) ? DXVA2_NominalRange_16_235 : DXVA2_NominalRange_Unknown;

  return fmt;
}

////////////////////////////////////////////////////////////////////////////////
// avcodec -> LAV codec mappings
////////////////////////////////////////////////////////////////////////////////

// This mapping table should contain all pixel formats, except hardware formats (VDPAU, XVMC, DXVA, etc)
// A format that is not listed will be converted to YUV420
static struct PixelFormatMapping {
  PixelFormat    ffpixfmt;
  LAVPixelFormat lavpixfmt;
  BOOL           conversion;
  int            bpp;
} ff_pix_map[] = {
  { PIX_FMT_YUV420P,   LAVPixFmt_YUV420, FALSE },
  { PIX_FMT_YUYV422,   LAVPixFmt_YUY2,   FALSE },
  { PIX_FMT_RGB24,     LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_BGR24,     LAVPixFmt_RGB24,  FALSE },
  { PIX_FMT_YUV422P,   LAVPixFmt_YUV422, FALSE },
  { PIX_FMT_YUV444P,   LAVPixFmt_YUV444, FALSE },
  { PIX_FMT_YUV410P,   LAVPixFmt_YUV420, TRUE  },
  { PIX_FMT_YUV411P,   LAVPixFmt_YUV422, TRUE  },
  { PIX_FMT_GRAY8,     LAVPixFmt_YUV420, TRUE  },
  { PIX_FMT_MONOWHITE, LAVPixFmt_YUV420, TRUE  },
  { PIX_FMT_MONOBLACK, LAVPixFmt_YUV420, TRUE  },
  { PIX_FMT_PAL8,      LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_YUVJ420P,  LAVPixFmt_YUV420, FALSE },
  { PIX_FMT_YUVJ422P,  LAVPixFmt_YUV422, FALSE },
  { PIX_FMT_YUVJ444P,  LAVPixFmt_YUV444, FALSE },
  { PIX_FMT_UYVY422,   LAVPixFmt_YUV422, TRUE  },
  { PIX_FMT_UYYVYY411, LAVPixFmt_YUV422, TRUE  },
  { PIX_FMT_BGR8,      LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_BGR4,      LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_BGR4_BYTE, LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_RGB8,      LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_RGB4,      LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_RGB4_BYTE, LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_NV12,      LAVPixFmt_NV12,   FALSE },
  { PIX_FMT_NV21,      LAVPixFmt_NV12,   TRUE  },

  { PIX_FMT_ARGB,      LAVPixFmt_ARGB32, TRUE  },
  { PIX_FMT_RGBA,      LAVPixFmt_ARGB32, TRUE  },
  { PIX_FMT_ABGR,      LAVPixFmt_ARGB32, TRUE  },
  { PIX_FMT_BGRA,      LAVPixFmt_ARGB32, FALSE },

  { PIX_FMT_GRAY16BE,  LAVPixFmt_YUV420, TRUE  },
  { PIX_FMT_GRAY16LE,  LAVPixFmt_YUV420, TRUE  },
  { PIX_FMT_YUV440P,   LAVPixFmt_YUV444, TRUE  },
  { PIX_FMT_YUVJ440P,  LAVPixFmt_YUV444, TRUE  },
  { PIX_FMT_YUVA420P,  LAVPixFmt_YUV420, TRUE  },
  { PIX_FMT_RGB48BE,   LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_RGB48LE,   LAVPixFmt_RGB32,  TRUE  },

  { PIX_FMT_RGB565BE,  LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_RGB565LE,  LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_RGB555BE,  LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_RGB555LE,  LAVPixFmt_RGB32,  TRUE  },

  { PIX_FMT_BGR565BE,  LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_BGR565LE,  LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_BGR555BE,  LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_BGR555LE,  LAVPixFmt_RGB32,  TRUE  },

  { PIX_FMT_YUV420P16LE, LAVPixFmt_YUV420bX, FALSE, 16 },
  { PIX_FMT_YUV420P16BE, LAVPixFmt_YUV420bX, TRUE,  16 },
  { PIX_FMT_YUV422P16LE, LAVPixFmt_YUV422bX, FALSE, 16 },
  { PIX_FMT_YUV422P16BE, LAVPixFmt_YUV422bX, TRUE,  16 },
  { PIX_FMT_YUV444P16LE, LAVPixFmt_YUV444bX, FALSE, 16 },
  { PIX_FMT_YUV444P16BE, LAVPixFmt_YUV444bX, TRUE,  16 },

  { PIX_FMT_RGB444LE,  LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_RGB444BE,  LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_BGR444LE,  LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_BGR444BE,  LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_GRAY8A,    LAVPixFmt_YUV420, TRUE  },
  { PIX_FMT_BGR48BE,   LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_BGR48LE,   LAVPixFmt_RGB32,  TRUE  },

  { PIX_FMT_YUV420P9BE,  LAVPixFmt_YUV420bX, TRUE,  9 },
  { PIX_FMT_YUV420P9LE,  LAVPixFmt_YUV420bX, FALSE, 9 },
  { PIX_FMT_YUV420P10BE, LAVPixFmt_YUV420bX, TRUE,  10 },
  { PIX_FMT_YUV420P10LE, LAVPixFmt_YUV420bX, FALSE, 10 },
  { PIX_FMT_YUV422P10BE, LAVPixFmt_YUV422bX, TRUE,  10 },
  { PIX_FMT_YUV422P10LE, LAVPixFmt_YUV422bX, FALSE, 10 },
  { PIX_FMT_YUV444P9BE,  LAVPixFmt_YUV444bX, TRUE,  9 },
  { PIX_FMT_YUV444P9LE,  LAVPixFmt_YUV444bX, FALSE, 9 },
  { PIX_FMT_YUV444P10BE, LAVPixFmt_YUV444bX, TRUE,  10 },
  { PIX_FMT_YUV444P10LE, LAVPixFmt_YUV444bX, FALSE, 10 },
  { PIX_FMT_YUV422P9BE,  LAVPixFmt_YUV422bX, TRUE,  9 },
  { PIX_FMT_YUV422P9LE,  LAVPixFmt_YUV422bX, FALSE, 9 },

  { PIX_FMT_GBRP,      LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_GBRP9BE,   LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_GBRP9LE,   LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_GBRP10BE,  LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_GBRP10LE,  LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_GBRP16BE,  LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_GBRP16LE,  LAVPixFmt_RGB32,  TRUE  },

  { PIX_FMT_RGBA64BE,  LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_RGBA64LE,  LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_BGRA64BE,  LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_BGRA64LE,  LAVPixFmt_RGB32,  TRUE  },

  { PIX_FMT_0RGB,      LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_RGB0,      LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_0BGR,      LAVPixFmt_RGB32,  TRUE  },
  { PIX_FMT_BGR0,      LAVPixFmt_RGB32,  FALSE },
  { PIX_FMT_YUVA444P,  LAVPixFmt_YUV444, TRUE  },
  { PIX_FMT_YUVA422P,  LAVPixFmt_YUV422, TRUE  },

  { PIX_FMT_YUV420P12BE, LAVPixFmt_YUV420bX, TRUE,  12 },
  { PIX_FMT_YUV420P12LE, LAVPixFmt_YUV420bX, FALSE, 12 },
  { PIX_FMT_YUV420P14BE, LAVPixFmt_YUV420bX, TRUE,  14 },
  { PIX_FMT_YUV420P14LE, LAVPixFmt_YUV420bX, FALSE, 14 },
  { PIX_FMT_YUV422P12BE, LAVPixFmt_YUV422bX, TRUE,  12 },
  { PIX_FMT_YUV422P12LE, LAVPixFmt_YUV422bX, FALSE, 12 },
  { PIX_FMT_YUV422P14BE, LAVPixFmt_YUV422bX, TRUE,  14 },
  { PIX_FMT_YUV422P14LE, LAVPixFmt_YUV422bX, FALSE, 14 },
  { PIX_FMT_YUV444P12BE, LAVPixFmt_YUV444bX, TRUE,  12 },
  { PIX_FMT_YUV444P12LE, LAVPixFmt_YUV444bX, FALSE, 12 },
  { PIX_FMT_YUV444P14BE, LAVPixFmt_YUV444bX, TRUE,  14 },
  { PIX_FMT_YUV444P14LE, LAVPixFmt_YUV444bX, FALSE, 14 },
  { PIX_FMT_GBRP12BE, LAVPixFmt_RGB32, TRUE },
  { PIX_FMT_GBRP12LE, LAVPixFmt_RGB32, TRUE },
  { PIX_FMT_GBRP14BE, LAVPixFmt_RGB32, TRUE },
  { PIX_FMT_GBRP14LE, LAVPixFmt_RGB32, TRUE },

  { PIX_FMT_DXVA2_VLD, LAVPixFmt_DXVA2, FALSE },
};

static AVCodecID ff_interlace_capable[] = {
  AV_CODEC_ID_DNXHD,
  AV_CODEC_ID_DVVIDEO,
  AV_CODEC_ID_FRWU,
  AV_CODEC_ID_MJPEG,
  AV_CODEC_ID_MPEG4,
  AV_CODEC_ID_MPEG2VIDEO,
  AV_CODEC_ID_H264,
  AV_CODEC_ID_VC1,
  AV_CODEC_ID_PNG,
  AV_CODEC_ID_PRORES,
  AV_CODEC_ID_RAWVIDEO,
  AV_CODEC_ID_UTVIDEO
};

static struct PixelFormatMapping getPixFmtMapping(PixelFormat pixfmt) {
  const PixelFormatMapping def = { pixfmt, LAVPixFmt_YUV420, TRUE, 8 };
  PixelFormatMapping result = def;
  for (int i = 0; i < countof(ff_pix_map); i++) {
    if (ff_pix_map[i].ffpixfmt == pixfmt) {
      result = ff_pix_map[i];
    }
  }
  if (result.lavpixfmt != LAVPixFmt_YUV420bX && result.lavpixfmt != LAVPixFmt_YUV422bX && result.lavpixfmt != LAVPixFmt_YUV444bX)
    result.bpp = 8;

  return result;
}

////////////////////////////////////////////////////////////////////////////////
// AVCodec decoder implementation
////////////////////////////////////////////////////////////////////////////////

CDecAvcodec::CDecAvcodec(void)
  : CDecBase()
  , m_pAVCodec(NULL)
  , m_pAVCtx(NULL)
  , m_pParser(NULL)
  , m_pFrame(NULL)
  , m_pFFBuffer(NULL), m_nFFBufferSize(0)
  , m_pFFBuffer2(NULL), m_nFFBufferSize2(0)
  , m_pSwsContext(NULL)
  , m_nCodecId(AV_CODEC_ID_NONE)
  , m_rtStartCache(AV_NOPTS_VALUE)
  , m_bResumeAtKeyFrame(FALSE)
  , m_bWaitingForKeyFrame(FALSE)
  , m_bBFrameDelay(TRUE)
  , m_nBFramePos(0)
  , m_iInterlaced(-1)
  , m_CurrentThread(0)
  , m_bDXVA(FALSE)
  , m_bInputPadded(FALSE)
  , m_bDR1(FALSE)
  , m_bEndOfSequence(FALSE)
{
}

CDecAvcodec::~CDecAvcodec(void)
{
  DestroyDecoder();
}

// ILAVDecoder
STDMETHODIMP CDecAvcodec::Init()
{
#ifdef DEBUG
  DbgSetModuleLevel (LOG_CUSTOM1, DWORD_MAX); // FFMPEG messages use custom1
  av_log_set_callback(lavf_log_callback);
#else
  av_log_set_callback(NULL);
#endif

  avcodec_register_all();
  return S_OK;
}

STDMETHODIMP CDecAvcodec::InitDecoder(AVCodecID codec, const CMediaType *pmt)
{
  DestroyDecoder();
  DbgLog((LOG_TRACE, 10, L"Initializing ffmpeg for codec %S", avcodec_get_name(codec)));

  BITMAPINFOHEADER *pBMI = NULL;
  videoFormatTypeHandler((const BYTE *)pmt->Format(), pmt->FormatType(), &pBMI);

  m_pAVCodec = avcodec_find_decoder(codec);
  CheckPointer(m_pAVCodec, VFW_E_UNSUPPORTED_VIDEO);

  m_pAVCtx = avcodec_alloc_context3(m_pAVCodec);
  CheckPointer(m_pAVCtx, E_POINTER);

  if(codec == AV_CODEC_ID_MPEG1VIDEO || codec == AV_CODEC_ID_MPEG2VIDEO || pmt->subtype == FOURCCMap(MKTAG('H','2','6','4')) || pmt->subtype == FOURCCMap(MKTAG('h','2','6','4'))) {
    m_pParser = av_parser_init(codec);
  }

  LONG biRealWidth = pBMI->biWidth, biRealHeight = pBMI->biHeight;
  if (pmt->formattype == FORMAT_VideoInfo || pmt->formattype == FORMAT_MPEGVideo) {
    VIDEOINFOHEADER *vih = (VIDEOINFOHEADER *)pmt->Format();
    if (vih->rcTarget.right != 0 && vih->rcTarget.bottom != 0) {
      biRealWidth  = vih->rcTarget.right;
      biRealHeight = vih->rcTarget.bottom;
    }
  } else if (pmt->formattype == FORMAT_VideoInfo2 || pmt->formattype == FORMAT_MPEG2Video) {
    VIDEOINFOHEADER2 *vih2 = (VIDEOINFOHEADER2 *)pmt->Format();
    if (vih2->rcTarget.right != 0 && vih2->rcTarget.bottom != 0) {
      biRealWidth  = vih2->rcTarget.right;
      biRealHeight = vih2->rcTarget.bottom;
    }
  }

  m_pAVCtx->codec_id              = codec;
  m_pAVCtx->codec_tag             = pBMI->biCompression;
  m_pAVCtx->coded_width           = pBMI->biWidth;
  m_pAVCtx->coded_height          = abs(pBMI->biHeight);
  m_pAVCtx->bits_per_coded_sample = pBMI->biBitCount;
  m_pAVCtx->error_concealment     = FF_EC_GUESS_MVS | FF_EC_DEBLOCK;
  m_pAVCtx->err_recognition       = AV_EF_CAREFUL;
  m_pAVCtx->workaround_bugs       = FF_BUG_AUTODETECT;

  if (codec == AV_CODEC_ID_H264)
    m_pAVCtx->flags2             |= CODEC_FLAG2_SHOW_ALL;

  // Setup threading
  int thread_type = getThreadFlags(codec);
  if (thread_type) {
    // Thread Count. 0 = auto detect
    int thread_count = m_pSettings->GetNumThreads();
    if (thread_count == 0) {
      SYSTEM_INFO systemInfo;
      GetSystemInfo(&systemInfo);
      thread_count = systemInfo.dwNumberOfProcessors * 3 / 2;
    }

    m_pAVCtx->thread_count = max(1, min(thread_count, AVCODEC_MAX_THREADS));
    m_pAVCtx->thread_type = thread_type;
  } else {
    m_pAVCtx->thread_count = 1;
  }

  if ((codec == AV_CODEC_ID_THEORA || codec == AV_CODEC_ID_VP3)
    && (m_pCallback->FilterInGraph(PINDIR_INPUT, CLSID_MPCHCOggSplitter) || m_pCallback->FilterInGraph(PINDIR_INPUT, CLSID_MPCHCOggSource))) {
    m_pAVCtx->thread_count = 1;
  }

  m_pFrame = avcodec_alloc_frame();
  CheckPointer(m_pFrame, E_POINTER);

  m_h264RandomAccess.SetAVCNALSize(0);

  // Process Extradata
  BYTE *extra = NULL;
  size_t extralen = 0;
  getExtraData((const BYTE *)pmt->Format(), pmt->FormatType(), pmt->FormatLength(), NULL, &extralen);

  BOOL bH264avc = FALSE;
  if (extralen > 0) {
    DbgLog((LOG_TRACE, 10, L"-> Processing extradata of %d bytes", extralen));
    // Reconstruct AVC1 extradata format
    if (pmt->formattype == FORMAT_MPEG2Video && (m_pAVCtx->codec_tag == MAKEFOURCC('a','v','c','1') || m_pAVCtx->codec_tag == MAKEFOURCC('A','V','C','1') || m_pAVCtx->codec_tag == MAKEFOURCC('C','C','V','1'))) {
      MPEG2VIDEOINFO *mp2vi = (MPEG2VIDEOINFO *)pmt->Format();
      extralen += 7;
      extra = (uint8_t *)av_mallocz(extralen + FF_INPUT_BUFFER_PADDING_SIZE);
      extra[0] = 1;
      extra[1] = (BYTE)mp2vi->dwProfile;
      extra[2] = 0;
      extra[3] = (BYTE)mp2vi->dwLevel;
      extra[4] = (BYTE)(mp2vi->dwFlags ? mp2vi->dwFlags : 2) - 1;

      // Actually copy the metadata into our new buffer
      size_t actual_len;
      getExtraData((const BYTE *)pmt->Format(), pmt->FormatType(), pmt->FormatLength(), extra+6, &actual_len);

      // Count the number of SPS/PPS in them and set the length
      // We'll put them all into one block and add a second block with 0 elements afterwards
      // The parsing logic does not care what type they are, it just expects 2 blocks.
      BYTE *p = extra+6, *end = extra+6+actual_len;
      BOOL bSPS = FALSE, bPPS = FALSE;
      int count = 0;
      while (p+1 < end) {
        unsigned len = (((unsigned)p[0] << 8) | p[1]) + 2;
        if (p + len > end) {
          break;
        }
        if ((p[2] & 0x1F) == 7)
          bSPS = TRUE;
        if ((p[2] & 0x1F) == 8)
          bPPS = TRUE;
        count++;
        p += len;
      }
      extra[5] = count;
      extra[extralen-1] = 0;

      bH264avc = TRUE;
      m_h264RandomAccess.SetAVCNALSize(mp2vi->dwFlags);
      if (!bSPS) {
        DbgLog((LOG_TRACE, 10, L"-> AVC1 extradata doesn't contain a SPS, setting thread_count = 1"));
        m_pAVCtx->thread_count = 1;
      }
    } else if (pmt->subtype == MEDIASUBTYPE_LAV_RAWVIDEO) {
      if (extralen != sizeof(m_pAVCtx->pix_fmt)) {
        DbgLog((LOG_TRACE, 10, L"-> LAV RAW Video extradata is wrong size.."));
      } else {
        getExtraData((const BYTE *)pmt->Format(), pmt->FormatType(), pmt->FormatLength(), (BYTE *)&m_pAVCtx->pix_fmt, NULL);
      }
    } else {
      // Just copy extradata for other formats
      extra = (uint8_t *)av_mallocz(extralen + FF_INPUT_BUFFER_PADDING_SIZE);
      getExtraData((const BYTE *)pmt->Format(), pmt->FormatType(), pmt->FormatLength(), extra, NULL);
    }
    // Hack to discard invalid MP4 metadata with AnnexB style video
    if (codec == AV_CODEC_ID_H264 && !bH264avc && extra[0] == 1) {
      av_freep(&extra);
      extralen = 0;
    }
    m_pAVCtx->extradata = extra;
    m_pAVCtx->extradata_size = (int)extralen;
  } else {
    if (codec == AV_CODEC_ID_VP6 || codec == AV_CODEC_ID_VP6A || codec == AV_CODEC_ID_VP6F) {
      int cropH = pBMI->biWidth - biRealWidth;
      int cropV = pBMI->biHeight - biRealHeight;
      if (cropH >= 0 && cropH <= 0x0f && cropV >= 0 && cropV <= 0x0f) {
        m_pAVCtx->extradata = (uint8_t *)av_mallocz(1 + FF_INPUT_BUFFER_PADDING_SIZE);
        m_pAVCtx->extradata_size = 1;
        m_pAVCtx->extradata[0] = (cropH << 4) | cropV;
      }
    }
  }

  m_h264RandomAccess.flush(m_pAVCtx->thread_count);
  m_CurrentThread = 0;
  m_rtStartCache = AV_NOPTS_VALUE;

  LAVPinInfo lavPinInfo = {0};
  BOOL bLAVInfoValid = SUCCEEDED(m_pCallback->GetLAVPinInfo(lavPinInfo));

  m_bInputPadded = m_pCallback->IsLAVSplitter();

  // Setup codec-specific timing logic
  BOOL bVC1IsPTS = (codec == AV_CODEC_ID_VC1 && !m_pCallback->VC1IsDTS());

  // Use ffmpegs logic to reorder timestamps
  // This is required for H264 content (except AVI), and generally all codecs that use frame threading
  // VC-1 is also a special case. Its required for splitters that deliver PTS timestamps (see bVC1IsPTS above)
  m_bFFReordering        =  ( codec == AV_CODEC_ID_H264 && !m_pCallback->H264IsAVI())
                           || codec == AV_CODEC_ID_VP8
                           || codec == AV_CODEC_ID_VP3
                           || codec == AV_CODEC_ID_THEORA
                           || codec == AV_CODEC_ID_HUFFYUV
                           || codec == AV_CODEC_ID_FFVHUFF
                           || codec == AV_CODEC_ID_MPEG2VIDEO
                           || codec == AV_CODEC_ID_MPEG1VIDEO
                           || codec == AV_CODEC_ID_DIRAC
                           || codec == AV_CODEC_ID_UTVIDEO
                           || codec == AV_CODEC_ID_DNXHD
                           || (codec == AV_CODEC_ID_MPEG4 && pmt->formattype == FORMAT_MPEG2Video)
                           || bVC1IsPTS;

  // Stop time is unreliable, drop it and calculate it
  m_bCalculateStopTime   = (codec == AV_CODEC_ID_H264 || codec == AV_CODEC_ID_DIRAC || (codec == AV_CODEC_ID_MPEG4 && pmt->formattype == FORMAT_MPEG2Video) || bVC1IsPTS);

  // Real Video content has some odd timestamps
  // LAV Splitter does them allright with RV30/RV40, everything else screws them up
  m_bRVDropBFrameTimings = (codec == AV_CODEC_ID_RV10 || codec == AV_CODEC_ID_RV20 || ((codec == AV_CODEC_ID_RV30 || codec == AV_CODEC_ID_RV40) && (!m_pCallback->IsLAVSplitter() || (bLAVInfoValid && (lavPinInfo.flags & LAV_STREAM_FLAG_RV34_MKV)))));

  // Enable B-Frame delay handling
  m_bBFrameDelay = !m_bFFReordering && !m_bRVDropBFrameTimings;

  m_bWaitingForKeyFrame = TRUE;
  m_bResumeAtKeyFrame =    codec == AV_CODEC_ID_MPEG2VIDEO
                        || codec == AV_CODEC_ID_VC1
                        || codec == AV_CODEC_ID_RV30
                        || codec == AV_CODEC_ID_RV40;

  m_bNoBufferConsumption =    codec == AV_CODEC_ID_MJPEGB
                           || codec == AV_CODEC_ID_LOCO;

  m_bHasPalette = m_pAVCtx->bits_per_coded_sample <= 8 && m_pAVCtx->extradata_size && !m_pCallback->IsLAVSplitter()
                  &&  (codec == AV_CODEC_ID_MSVIDEO1
                    || codec == AV_CODEC_ID_MSRLE
                    || codec == AV_CODEC_ID_CINEPAK
                    || codec == AV_CODEC_ID_8BPS
                    || codec == AV_CODEC_ID_QPEG
                    || codec == AV_CODEC_ID_QTRLE
                    || codec == AV_CODEC_ID_TSCC);

#if AVCODEC_USE_DR1
  if (!m_bDXVA && m_pAVCodec->capabilities & CODEC_CAP_DR1) {
    m_bDR1                   = TRUE;
    m_pAVCtx->opaque         = this;
    m_pAVCtx->get_buffer     = lav_get_buffer;
    m_pAVCtx->reget_buffer   = lav_reget_buffer;
    m_pAVCtx->release_buffer = lav_release_buffer;
    m_pAVCtx->thread_safe_callbacks = 1;
  } else {
    m_bDR1                   = FALSE;
  }
#endif

  if (FAILED(AdditionaDecoderInit())) {
    return E_FAIL;
  }

  if (bLAVInfoValid) {
    // Setting has_b_frames to a proper value will ensure smoother decoding of H264
    if (lavPinInfo.has_b_frames >= 0) {
      DbgLog((LOG_TRACE, 10, L"-> Setting has_b_frames to %d", lavPinInfo.has_b_frames));
      m_pAVCtx->has_b_frames = lavPinInfo.has_b_frames;
    }
  }

  // Open the decoder
  int ret = avcodec_open2(m_pAVCtx, m_pAVCodec, NULL);
  if (ret >= 0) {
    DbgLog((LOG_TRACE, 10, L"-> ffmpeg codec opened successfully (ret: %d)", ret));
    m_nCodecId = codec;
  } else {
    DbgLog((LOG_TRACE, 10, L"-> ffmpeg codec failed to open (ret: %d)", ret));
    DestroyDecoder();
    return VFW_E_UNSUPPORTED_VIDEO;
  }

  m_iInterlaced = 0;
  for (int i = 0; i < countof(ff_interlace_capable); i++) {
    if (codec == ff_interlace_capable[i]) {
      m_iInterlaced = -1;
      break;
    }
  }

  // Detect chroma and interlaced
  if (m_pAVCtx->extradata && m_pAVCtx->extradata_size) {
    if (codec == AV_CODEC_ID_MPEG2VIDEO) {
      CMPEG2HeaderParser mpeg2Parser(extra, extralen);
      if (mpeg2Parser.hdr.valid) {
        if (mpeg2Parser.hdr.chroma < 2) {
          m_pAVCtx->pix_fmt = PIX_FMT_YUV420P;
        } else if (mpeg2Parser.hdr.chroma == 2) {
          m_pAVCtx->pix_fmt = PIX_FMT_YUV422P;
        }
        m_iInterlaced = mpeg2Parser.hdr.interlaced;
      }
    } else if (codec == AV_CODEC_ID_H264) {
      CH264SequenceParser h264parser;
      if (bH264avc)
        h264parser.ParseNALs(extra+6, extralen-6, 2);
      else
        h264parser.ParseNALs(extra, extralen, 0);
      if (h264parser.sps.valid)
        m_iInterlaced = h264parser.sps.interlaced;
    } else if (codec == AV_CODEC_ID_VC1) {
      CVC1HeaderParser vc1parser(extra, extralen);
      if (vc1parser.hdr.valid)
        m_iInterlaced = (vc1parser.hdr.interlaced ? -1 : 0);
    }
  }

  if (codec == AV_CODEC_ID_DNXHD)
    m_pAVCtx->pix_fmt = PIX_FMT_YUV422P10;
  else if (codec == AV_CODEC_ID_FRAPS)
    m_pAVCtx->pix_fmt = PIX_FMT_BGR24;

  if (bLAVInfoValid && codec != AV_CODEC_ID_FRAPS)
    m_pAVCtx->pix_fmt = lavPinInfo.pix_fmt;

  DbgLog((LOG_TRACE, 10, L"AVCodec init successfull. interlaced: %d", m_iInterlaced));

  return S_OK;
}

STDMETHODIMP CDecAvcodec::DestroyDecoder()
{
  DbgLog((LOG_TRACE, 10, L"Shutting down ffmpeg..."));
  m_pAVCodec	= NULL;

  if (m_pParser) {
    av_parser_close(m_pParser);
    m_pParser = NULL;
  }

  if (m_pAVCtx) {
    avcodec_close(m_pAVCtx);
    av_freep(&m_pAVCtx->extradata);
    av_freep(&m_pAVCtx);
  }
  av_freep(&m_pFrame);

  av_freep(&m_pFFBuffer);
  m_nFFBufferSize = 0;

  av_freep(&m_pFFBuffer2);
  m_nFFBufferSize2 = 0;

  if (m_pSwsContext) {
    sws_freeContext(m_pSwsContext);
    m_pSwsContext = NULL;
  }

  m_nCodecId = AV_CODEC_ID_NONE;

  return S_OK;
}

#if AVCODEC_USE_DR1

int CDecAvcodec::lav_get_buffer(struct AVCodecContext *c, AVFrame *pic)
{
  CDecAvcodec *pDec = (CDecAvcodec *)c->opaque;
  CAutoLock lock(&pDec->m_BufferCritSec);
  int ret = avcodec_default_get_buffer(c, pic);
#if AVCODEC_DR1_DEBUG
  int state = pDec->m_Buffers[pic->data[0]];
  DbgLog((LOG_TRACE, 10, L"lav_get_buffer of: %p (state: %d)", pic->data[0], state));
  ASSERT(state == 0);
#endif
  return ret;
}

static void ff_init_buffer_info(AVCodecContext *s, AVFrame *pic)
{
  if (s->pkt) {
    pic->pkt_pts = s->pkt->pts;
    pic->pkt_pos = s->pkt->pos;
    pic->pkt_duration = s->pkt->duration;
  } else {
    pic->pkt_pts = AV_NOPTS_VALUE;
    pic->pkt_pos = -1;
    pic->pkt_duration = 0;
  }
  pic->reordered_opaque= s->reordered_opaque;
  pic->sample_aspect_ratio = s->sample_aspect_ratio;
  pic->width               = s->width;
  pic->height              = s->height;
  pic->format              = s->pix_fmt;
}

/* Modified copy of avcodec_default_reget_buffer to accomodate for buffers which are used for rendering */
int CDecAvcodec::lav_reget_buffer(struct AVCodecContext *s, AVFrame *pic)
{
  AVFrame temp_pic;
  int i;

  ASSERT(s->codec_type == AVMEDIA_TYPE_VIDEO);

  if (pic->data[0] && (pic->width != s->width || pic->height != s->height || pic->format != s->pix_fmt)) {
    av_log(s, AV_LOG_WARNING, "Picture changed from size:%dx%d fmt:%s to size:%dx%d fmt:%s in reget buffer()\n",
      pic->width, pic->height, av_get_pix_fmt_name((PixelFormat)pic->format), s->width, s->height, av_get_pix_fmt_name(s->pix_fmt));
    s->release_buffer(s, pic);
  }

  ff_init_buffer_info(s, pic);

  /* If no picture return a new buffer */
  if(pic->data[0] == NULL) {
    /* We will copy from buffer, so must be readable */
    pic->buffer_hints |= FF_BUFFER_HINTS_READABLE;
    return s->get_buffer(s, pic);
  }

  ASSERT(s->pix_fmt == pic->format);

  /* If internal buffer type return the same buffer,
     but only if its not referenced for rendering. */
  if(pic->type == FF_BUFFER_TYPE_INTERNAL) {
    CDecAvcodec *pDec = (CDecAvcodec *)s->opaque;
    CAutoLock lock(&pDec->m_BufferCritSec);
    if (pDec->m_Buffers[pic->data[0]] == 0)
      return 0;
  }

  /*
  * Not internal type and reget_buffer not overridden, emulate cr buffer
  */
  temp_pic = *pic;
  for(i = 0; i < AV_NUM_DATA_POINTERS; i++)
    pic->data[i] = pic->base[i] = NULL;
  pic->opaque = NULL;
  /* Allocate new frame */
  if (s->get_buffer(s, pic))
    return -1;
  /* Copy image data from old buffer to new buffer */
  av_picture_copy((AVPicture*)pic, (AVPicture*)&temp_pic, s->pix_fmt, s->width,
    s->height);
  s->release_buffer(s, &temp_pic); // Release old frame
  return 0;
}

void CDecAvcodec::lav_release_buffer(struct AVCodecContext *c, AVFrame *pic)
{
  CDecAvcodec *pDec = (CDecAvcodec *)c->opaque;
  CAutoLock lock(&pDec->m_BufferCritSec);
#if AVCODEC_DR1_DEBUG
  DbgLog((LOG_TRACE, 10, L"lav_release_buffer of: %p", pic->data[0]));
#endif
  int state = pDec->m_Buffers[pic->data[0]];
  if (state == 1) {
#if AVCODEC_DR1_DEBUG
    DbgLog((LOG_TRACE, 10, L"-> buffer in use"));
#endif
    pDec->m_Buffers[pic->data[0]] = 2;
    for (int i = 0; i < AV_NUM_DATA_POINTERS; i++) {
      pic->data[i] = NULL;
    }
  } else if (!state) {
#if AVCODEC_DR1_DEBUG
    DbgLog((LOG_TRACE, 10, L"-> releasing buffer"));
#endif
    avcodec_default_release_buffer(c, pic);
  }
}

void CDecAvcodec::lav_frame_destruct(struct LAVFrame *f)
{
  CDecAvcodec *pDec = (CDecAvcodec *)f->priv_data;
  CAutoLock lock(&pDec->m_BufferCritSec);
#if AVCODEC_DR1_DEBUG
  DbgLog((LOG_TRACE, 10, L"lav_frame_destruct of: %p", f->data[0]));
#endif
  int state = pDec->m_Buffers[f->data[0]];
  pDec->m_Buffers[f->data[0]] = 0;
  if (state == 2) {
#if AVCODEC_DR1_DEBUG
    DbgLog((LOG_TRACE, 10, L"-> releasing buffer"));
#endif
    AVFrame pic;
    pic.type = FF_BUFFER_TYPE_INTERNAL;
    pic.data[0] = f->data[0];
    avcodec_default_release_buffer(pDec->m_pAVCtx, &pic);
  }
#if AVCODEC_DR1_DEBUG
  else {
    DbgLog((LOG_TRACE, 10, L"-> buffer in use"));
  }
#endif
}

#endif

extern "C" const uint8_t *avpriv_mpv_find_start_code(const uint8_t *p, const uint8_t *end, uint32_t *state);

STDMETHODIMP CDecAvcodec::Decode(const BYTE *buffer, int buflen, REFERENCE_TIME rtStartIn, REFERENCE_TIME rtStopIn, BOOL bSyncPoint, BOOL bDiscontinuity)
{
  int     got_picture = 0;
  int     used_bytes  = 0;
  BOOL    bParserFrame = FALSE;
  BOOL    bFlush = (buffer == NULL);
  BOOL    bEndOfSequenceFlush = FALSE;

  AVPacket avpkt;
  av_init_packet(&avpkt);

  if (m_pAVCtx->active_thread_type & FF_THREAD_FRAME) {
    if (!m_bFFReordering) {
      m_tcThreadBuffer[m_CurrentThread].rtStart = rtStartIn;
      m_tcThreadBuffer[m_CurrentThread].rtStop  = rtStopIn;
    }

    m_CurrentThread = (m_CurrentThread + 1) % m_pAVCtx->thread_count;
  } else if (m_bBFrameDelay) {
    m_tcBFrameDelay[m_nBFramePos].rtStart = rtStartIn;
    m_tcBFrameDelay[m_nBFramePos].rtStop = rtStopIn;
    m_nBFramePos = !m_nBFramePos;
  }

  uint8_t *pDataBuffer = NULL;
  if (!bFlush && buflen > 0) {
    if (!m_bInputPadded && (!(m_pAVCtx->active_thread_type & FF_THREAD_FRAME) || m_pParser)) {
      // Copy bitstream into temporary buffer to ensure overread protection
      // Verify buffer size
      if (buflen > m_nFFBufferSize) {
        m_nFFBufferSize	= buflen;
        m_pFFBuffer = (BYTE *)av_realloc_f(m_pFFBuffer, m_nFFBufferSize + FF_INPUT_BUFFER_PADDING_SIZE, 1);
        if (!m_pFFBuffer) {
          m_nFFBufferSize = 0;
          return E_OUTOFMEMORY;
        }
      }
      
      memcpy(m_pFFBuffer, buffer, buflen);
      memset(m_pFFBuffer+buflen, 0, FF_INPUT_BUFFER_PADDING_SIZE);
      pDataBuffer = m_pFFBuffer;
    } else {
      pDataBuffer = (uint8_t *)buffer;
    }

    if (m_nCodecId == AV_CODEC_ID_H264) {
      BOOL bRecovered = m_h264RandomAccess.searchRecoveryPoint(pDataBuffer, buflen);
      if (!bRecovered) {
        return S_OK;
      }
    } else if (m_nCodecId == AV_CODEC_ID_VP8 && m_bWaitingForKeyFrame) {
      if (!(pDataBuffer[0] & 1)) {
        DbgLog((LOG_TRACE, 10, L"::Decode(): Found VP8 key-frame, resuming decoding"));
        m_bWaitingForKeyFrame = FALSE;
      } else {
        return S_OK;
      }
    }
  }

  while (buflen > 0 || bFlush) {
    REFERENCE_TIME rtStart = rtStartIn, rtStop = rtStopIn;

    if (!bFlush) {
      avpkt.data = pDataBuffer;
      avpkt.size = buflen;
      avpkt.pts = rtStartIn;
      if (rtStartIn != AV_NOPTS_VALUE && rtStopIn != AV_NOPTS_VALUE)
        avpkt.duration = (int)(rtStopIn - rtStartIn);
      else
        avpkt.duration = 0;
      avpkt.flags = AV_PKT_FLAG_KEY;

      if (m_bHasPalette) {
        m_bHasPalette = FALSE;
        uint32_t *pal = (uint32_t *)av_packet_new_side_data(&avpkt, AV_PKT_DATA_PALETTE, AVPALETTE_SIZE);
        int pal_size = FFMIN((1 << m_pAVCtx->bits_per_coded_sample) << 2, m_pAVCtx->extradata_size);
        uint8_t *pal_src = m_pAVCtx->extradata + m_pAVCtx->extradata_size - pal_size;

        for (int i = 0; i < pal_size/4; i++)
          pal[i] = 0xFF<<24 | AV_RL32(pal_src+4*i);
      }
    } else {
      avpkt.data = NULL;
      avpkt.size = 0;
    }

    // Parse the data if a parser is present
    // This is mandatory for MPEG-1/2
    if (m_pParser) {
      BYTE *pOut = NULL;
      int pOut_size = 0;

      used_bytes = av_parser_parse2(m_pParser, m_pAVCtx, &pOut, &pOut_size, avpkt.data, avpkt.size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);

      if (used_bytes == 0 && pOut_size == 0 && !bFlush) {
        DbgLog((LOG_TRACE, 50, L"::Decode() - could not process buffer, starving?"));
        break;
      }

      // Update start time cache
      // If more data was read then output, update the cache (incomplete frame)
      // If output is bigger, a frame was completed, update the actual rtStart with the cached value, and then overwrite the cache
      if (used_bytes > pOut_size) {
        if (rtStartIn != AV_NOPTS_VALUE)
          m_rtStartCache = rtStartIn;
      } else if (used_bytes == pOut_size || ((used_bytes + 9) == pOut_size)) {
        // Why +9 above?
        // Well, apparently there are some broken MKV muxers that like to mux the MPEG-2 PICTURE_START_CODE block (which is 9 bytes) in the package with the previous frame
        // This would cause the frame timestamps to be delayed by one frame exactly, and cause timestamp reordering to go wrong.
        // So instead of failing on those samples, lets just assume that 9 bytes are that case exactly.
        m_rtStartCache = rtStartIn = AV_NOPTS_VALUE;
      } else if (pOut_size > used_bytes) {
        rtStart = m_rtStartCache;
        m_rtStartCache = rtStartIn;
        // The value was used once, don't use it for multiple frames, that ends up in weird timings
        rtStartIn = AV_NOPTS_VALUE;
      }

       bParserFrame = (pOut_size > 0);

      if (pOut_size > 0 || bFlush) {

        if (pOut && pOut_size > 0) {
          if (pOut_size > m_nFFBufferSize2) {
            m_nFFBufferSize2	= pOut_size;
            m_pFFBuffer2 = (BYTE *)av_realloc_f(m_pFFBuffer2, m_nFFBufferSize2 + FF_INPUT_BUFFER_PADDING_SIZE, 1);
            if (!m_pFFBuffer2) {
              m_nFFBufferSize2 = 0;
              return E_OUTOFMEMORY;
            }
          }
          memcpy(m_pFFBuffer2, pOut, pOut_size);
          memset(m_pFFBuffer2+pOut_size, 0, FF_INPUT_BUFFER_PADDING_SIZE);

          avpkt.data = m_pFFBuffer2;
          avpkt.size = pOut_size;
          avpkt.pts = rtStart;
          avpkt.duration = 0;
        } else {
          avpkt.data = NULL;
          avpkt.size = 0;
        }

        // Check for MPEG2 End-Of-Sequence code, and immediately flush all frames out of the decoder
        // This is mostly required to get still-frames in DVDs (and possibly Blu-rays) to output as soon as they are ready,
        // otherwise they might be delayed until the next track switch, and all you see is black (or the previous image)
        if (m_nCodecId == AV_CODEC_ID_MPEG2VIDEO && avpkt.data && avpkt.size >= 4) {
          uint32_t state;
          const uint8_t *p = avpkt.data, *end = avpkt.data + avpkt.size;
          while (p < end) {
            p = avpriv_mpv_find_start_code(p, end, &state);
            if (state == 0x000001b7) {
              DbgLog((LOG_TRACE, 50, L"Found SEQ_END_CODE at %p (end: %p)", p, end));
              bEndOfSequenceFlush = TRUE;
              break;
            }
          }
        }

        int ret2 = avcodec_decode_video2 (m_pAVCtx, m_pFrame, &got_picture, &avpkt);
        if (ret2 < 0) {
          DbgLog((LOG_TRACE, 50, L"::Decode() - decoding failed despite successfull parsing"));
          got_picture = 0;
        }
      } else {
        got_picture = 0;
      }
    } else {
      used_bytes = avcodec_decode_video2 (m_pAVCtx, m_pFrame, &got_picture, &avpkt);
    }

    if (FAILED(PostDecode())) {
      return E_FAIL;
    }

    // Decoding of this frame failed ... oh well!
    if (used_bytes < 0) {
      return S_OK;
    }

    // When Frame Threading, we won't know how much data has been consumed, so it by default eats everything.
    // In addition, if no data got consumed, and no picture was extracted, the frame probably isn't all that useufl.
    // The MJPEB decoder is somewhat buggy and doesn't let us know how much data was consumed really...
    if ((!m_pParser && (m_pAVCtx->active_thread_type & FF_THREAD_FRAME || (!got_picture && used_bytes == 0))) || m_bNoBufferConsumption || bFlush) {
      buflen = 0;
    } else {
      buflen -= used_bytes;
      pDataBuffer += used_bytes;
    }

    // Judge frame usability
    // This determines if a frame is artifact free and can be delivered
    // For H264 this does some wicked magic hidden away in the H264RandomAccess class
    // MPEG-2 and VC-1 just wait for a keyframe..
    if (m_nCodecId == AV_CODEC_ID_H264 && (bParserFrame || !m_pParser || got_picture)) {
      m_h264RandomAccess.judgeFrameUsability(m_pFrame, &got_picture);
    } else if (m_bResumeAtKeyFrame) {
      if (m_bWaitingForKeyFrame && got_picture) {
        if (m_pFrame->key_frame) {
          m_bWaitingForKeyFrame = FALSE;
        } else {
          got_picture = 0;
        }
      }
    }

    // Handle B-frame delay for frame threading codecs
    if ((m_pAVCtx->active_thread_type & FF_THREAD_FRAME) && m_bBFrameDelay) {
      m_tcBFrameDelay[m_nBFramePos] = m_tcThreadBuffer[m_CurrentThread];
      m_nBFramePos = !m_nBFramePos;
    }

    if (!got_picture || !m_pFrame->data[0]) {
      bFlush = FALSE; // End flushing, no more frames
      continue;
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////
    // Determine the proper timestamps for the frame, based on different possible flags.
    ///////////////////////////////////////////////////////////////////////////////////////////////
    if (m_bFFReordering) {
      rtStart = m_pFrame->pkt_pts;
      if (m_pFrame->pkt_duration)
        rtStop = m_pFrame->pkt_pts + m_pFrame->pkt_duration;
      else
        rtStop = AV_NOPTS_VALUE;
    } else if (m_bBFrameDelay && m_pAVCtx->has_b_frames) {
      rtStart = m_tcBFrameDelay[m_nBFramePos].rtStart;
      rtStop  = m_tcBFrameDelay[m_nBFramePos].rtStop;
    } else if (m_pAVCtx->active_thread_type & FF_THREAD_FRAME) {
      unsigned index = m_CurrentThread;
      rtStart = m_tcThreadBuffer[index].rtStart;
      rtStop  = m_tcThreadBuffer[index].rtStop;
    }

    if (m_bRVDropBFrameTimings && m_pFrame->pict_type == AV_PICTURE_TYPE_B) {
      rtStart = AV_NOPTS_VALUE;
    }

    if (m_bCalculateStopTime)
      rtStop = AV_NOPTS_VALUE;

    ///////////////////////////////////////////////////////////////////////////////////////////////
    // All required values collected, deliver the frame
    ///////////////////////////////////////////////////////////////////////////////////////////////
    LAVFrame *pOutFrame = NULL;
    AllocateFrame(&pOutFrame);

    AVRational display_aspect_ratio;
    int64_t num = (int64_t)m_pAVCtx->sample_aspect_ratio.num * m_pAVCtx->width;
    int64_t den = (int64_t)m_pAVCtx->sample_aspect_ratio.den * m_pAVCtx->height;
    av_reduce(&display_aspect_ratio.num, &display_aspect_ratio.den, num, den, 1 << 30);

    pOutFrame->width        = m_pAVCtx->width;
    pOutFrame->height       = m_pAVCtx->height;
    pOutFrame->aspect_ratio = display_aspect_ratio;
    pOutFrame->repeat       = m_pFrame->repeat_pict;
    pOutFrame->key_frame    = m_pFrame->key_frame;
    pOutFrame->frame_type   = av_get_picture_type_char(m_pFrame->pict_type);
    pOutFrame->ext_format   = GetDXVA2ExtendedFlags(m_pAVCtx, m_pFrame);

    if (m_pFrame->interlaced_frame && m_iInterlaced != 1)
      m_iInterlaced = 1;

    pOutFrame->interlaced   = (m_pFrame->interlaced_frame || (m_iInterlaced == 1 && m_pSettings->GetDeintAggressive()) || m_pSettings->GetDeintForce()) && !m_pSettings->GetDeintTreatAsProgressive();

    LAVDeintFieldOrder fo   = m_pSettings->GetDeintFieldOrder();
    pOutFrame->tff          = (fo == DeintFieldOrder_Auto) ? m_pFrame->top_field_first : (fo == DeintFieldOrder_TopFieldFirst);

    pOutFrame->rtStart      = rtStart;
    pOutFrame->rtStop       = rtStop;

    PixelFormatMapping map  = getPixFmtMapping(m_pAVCtx->pix_fmt);
    pOutFrame->format       = map.lavpixfmt;
    pOutFrame->bpp          = map.bpp;

    if (m_nCodecId == AV_CODEC_ID_MPEG2VIDEO)
      pOutFrame->avgFrameDuration = GetFrameDuration();

    if (m_bEndOfSequence)
      pOutFrame->flags |= LAV_FRAME_FLAG_END_OF_SEQUENCE;

    if (map.conversion) {
      ConvertPixFmt(m_pFrame, pOutFrame);
    } else {
      for (int i = 0; i < 4; i++) {
        pOutFrame->data[i]   = m_pFrame->data[i];
        pOutFrame->stride[i] = m_pFrame->linesize[i];
      }
#if AVCODEC_USE_DR1
      if (m_bDR1) {
        {
          CAutoLock lock(&m_BufferCritSec);
          m_Buffers[m_pFrame->data[0]] = 1;
        }
        pOutFrame->destruct = lav_frame_destruct;
        pOutFrame->priv_data = this;
      }
#endif
    }

    if (pOutFrame->format  == LAVPixFmt_DXVA2) {
      HandleDXVA2Frame(pOutFrame);
    } else {
      Deliver(pOutFrame);
    }

    if (bFlush) {
      m_CurrentThread = (m_CurrentThread + 1) % m_pAVCtx->thread_count;
    }
  }

  if (bEndOfSequenceFlush) {
    m_bEndOfSequence = TRUE;
    EndOfStream();
    m_bEndOfSequence = FALSE;
    Deliver(m_pCallback->GetFlushFrame());
  }

  return S_OK;
}

STDMETHODIMP CDecAvcodec::Flush()
{
  if (m_pAVCtx) {
    avcodec_flush_buffers(m_pAVCtx);
#if AVCODEC_DR1_DEBUG
    if (m_bDR1) {
      DbgLog((LOG_TRACE, 10, L"Buffers after flush: "));
      for (auto it = m_Buffers.begin() ; it != m_Buffers.end(); it++) {
        DbgLog((LOG_TRACE, 10, L" -> %p: %d", (*it).first, (*it).second));
      }
    }
#endif
  }

  if (m_pParser) {
    av_parser_close(m_pParser);
    m_pParser = av_parser_init(m_nCodecId);
  }

  m_CurrentThread = 0;
  m_rtStartCache = AV_NOPTS_VALUE;
  m_bWaitingForKeyFrame = TRUE;
  m_h264RandomAccess.flush(m_pAVCtx->thread_count);

  m_nBFramePos = 0;
  m_tcBFrameDelay[0].rtStart = m_tcBFrameDelay[0].rtStop = AV_NOPTS_VALUE;
  m_tcBFrameDelay[1].rtStart = m_tcBFrameDelay[1].rtStop = AV_NOPTS_VALUE;

  if (!m_bDXVA && (m_nCodecId == AV_CODEC_ID_H264 || m_nCodecId == AV_CODEC_ID_MPEG2VIDEO)) {
    InitDecoder(m_nCodecId, &m_pCallback->GetInputMediaType());
  }

  return S_OK;
}

STDMETHODIMP CDecAvcodec::EndOfStream()
{
  Decode(NULL, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, FALSE, FALSE);
  return S_OK;
}

STDMETHODIMP CDecAvcodec::GetPixelFormat(LAVPixelFormat *pPix, int *pBpp)
{
  PixelFormat pixfmt = m_pAVCtx ? m_pAVCtx->pix_fmt : PIX_FMT_NONE;
  PixelFormatMapping mapping = getPixFmtMapping(pixfmt);
  if (pPix)
    *pPix = mapping.lavpixfmt;
  if (pBpp)
    *pBpp = mapping.bpp;
  return S_OK;
}

STDMETHODIMP CDecAvcodec::ConvertPixFmt(AVFrame *pFrame, LAVFrame *pOutFrame)
{
  // Allocate the buffers to write into
  AllocLAVFrameBuffers(pOutFrame);

  // Map to swscale compatible format
  PixelFormat dstFormat = getFFPixelFormatFromLAV(pOutFrame->format, pOutFrame->bpp);

  // Get a context
  m_pSwsContext = sws_getCachedContext(m_pSwsContext, m_pAVCtx->width, m_pAVCtx->height, m_pAVCtx->pix_fmt, m_pAVCtx->width, m_pAVCtx->height, dstFormat, SWS_BILINEAR | SWS_PRINT_INFO, NULL, NULL, NULL);

  // Perform conversion
  sws_scale(m_pSwsContext, pFrame->data, pFrame->linesize, 0, m_pAVCtx->height, pOutFrame->data, pOutFrame->stride);

  return S_OK;
}

STDMETHODIMP_(REFERENCE_TIME) CDecAvcodec::GetFrameDuration()
{
  if (m_pAVCtx->time_base.den && m_pAVCtx->time_base.num)
    return (REF_SECOND_MULT * m_pAVCtx->time_base.num / m_pAVCtx->time_base.den) * m_pAVCtx->ticks_per_frame;
  return 0;
}

STDMETHODIMP_(BOOL) CDecAvcodec::IsInterlaced()
{
  return !m_pSettings->GetDeintTreatAsProgressive() && ((m_iInterlaced != 0) || m_pSettings->GetDeintForce());
}
