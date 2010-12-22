/*
 * - CrystalHD decoder module -
 *
 * Copyright(C) 2010 Philip Langdale <mplayer.philipl@overt.org>
 *
 * Credits:
 * extract_sps_pps_from_avcc: from xbmc
 *
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

/*****************************************************************************
 * Includes
 ****************************************************************************/

#include <emmintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <libcrystalhd/bc_dts_types.h>
#include <libcrystalhd/bc_dts_defs.h>
#include <libcrystalhd/libcrystalhd_if.h>

#include "config.h"
#include "mp_msg.h"
#include "vd_internal.h"
#include "libvo/fastmemcpy.h"

#define OUTPUT_PROC_TIMEOUT 2000


/*****************************************************************************
 * Module private data
 ****************************************************************************/

typedef struct {
   HANDLE dev;
   mp_image_t* mpi;

   uint8_t *sps_pps_buf;
   uint32_t sps_pps_size;
   uint8_t nal_length_size;
   uint8_t is_avc;

   uint8_t need_second_field;
   uint32_t unseen;

} Priv;

static mp_image_t mpi_no_picture =
{
	.type = MP_IMGTYPE_INCOMPLETE
};


/*****************************************************************************
 * Helper functions
 ****************************************************************************/

static int extract_sps_pps_from_avcc(Priv *priv, uint8_t *extradata, uint32_t extradata_size)
{
  uint8_t *data = extradata;
  uint32_t data_size = extradata_size;
  int profile;
  unsigned int nal_size;
  unsigned int num_sps, num_pps;

  if(*(char *)extradata == 1) {
     priv->is_avc = 1;
     priv->nal_length_size = ((*(((char*)(data))+4))&0x03)+1;
  } else {
     priv->is_avc = 0;
     priv->nal_length_size = 4;
     return 0;
  }

  priv->sps_pps_buf = calloc(1, extradata_size);
  priv->sps_pps_size = 0;

  profile = (data[1] << 16) | (data[2] << 8) | data[3];
  mp_msg(MSGT_DECVIDEO, MSGL_V, "profile %06x", profile);

  num_sps = data[5] & 0x1f;
  mp_msg(MSGT_DECVIDEO, MSGL_V, "num sps %d", num_sps);

  data += 6;
  data_size -= 6;

  for (unsigned int i = 0; i < num_sps; i++)
  {
    if (data_size < 2)
      return -1;

    nal_size = (data[0] << 8) | data[1];
    data += 2;
    data_size -= 2;

    if (data_size < nal_size)
			return -1;

    priv->sps_pps_buf[0] = 0;
    priv->sps_pps_buf[1] = 0;
    priv->sps_pps_buf[2] = 0;
    priv->sps_pps_buf[3] = 1;

    priv->sps_pps_size += 4;

    memcpy(priv->sps_pps_buf + priv->sps_pps_size, data, nal_size);
    priv->sps_pps_size += nal_size;

    data += nal_size;
    data_size -= nal_size;
  }

  if (data_size < 1)
    return -1;

  num_pps = data[0];
  data += 1;
  data_size -= 1;

  for (unsigned int i = 0; i < num_pps; i++)
  {
    if (data_size < 2)
      return -1;

    nal_size = (data[0] << 8) | data[1];
    data += 2;
    data_size -= 2;

    if (data_size < nal_size)
      return -1;

    priv->sps_pps_buf[priv->sps_pps_size+0] = 0;
    priv->sps_pps_buf[priv->sps_pps_size+1] = 0;
    priv->sps_pps_buf[priv->sps_pps_size+2] = 0;
    priv->sps_pps_buf[priv->sps_pps_size+3] = 1;

    priv->sps_pps_size += 4;

    memcpy(priv->sps_pps_buf + priv->sps_pps_size, data, nal_size);
    priv->sps_pps_size += nal_size;

    data += nal_size;
    data_size -= nal_size;
  }

  mp_msg(MSGT_DECVIDEO, MSGL_V, "data size at end = %d\n", data_size);

  return 0;
}


static uint8_t name2subtype(Priv *priv, const char *name)
{
   if (strcmp(name, "chddivx") == 0) {
      return BC_MSUBTYPE_DIVX;
   } else if (strcmp(name, "chddivx3") == 0) {
      return BC_MSUBTYPE_DIVX311;
   } else if (strcmp(name, "chdmpeg1") == 0) {
      return BC_MSUBTYPE_MPEG1VIDEO;
   } else if (strcmp(name, "chdmpeg2") == 0) {
      return BC_MSUBTYPE_MPEG2VIDEO;
   } else if (strcmp(name, "chdvc1") == 0) {
      return BC_MSUBTYPE_VC1;
   } else if (strcmp(name, "chdwvc1") == 0) {
      return BC_MSUBTYPE_WVC1;
   } else if (strcmp(name, "chdwmv3") == 0) {
      return BC_MSUBTYPE_WMV3;
   } else if (strcmp(name, "chdwmva") == 0) {
      return BC_MSUBTYPE_WMVA;
   } else if (strcmp(name, "chdh264") == 0) {
      return priv->is_avc ? BC_MSUBTYPE_AVC1 : BC_MSUBTYPE_H264;
   } else {
      return BC_MSUBTYPE_INVALID;
   }
}


/*****************************************************************************
 * Video decoder API function definitions
 ****************************************************************************/

/*============================================================================
 * control - to set/get/query special features/parameters
 *==========================================================================*/

static int control(sh_video_t *sh, int cmd, void* arg, ...)
{
   Priv *priv = sh->context;
   switch(cmd){
   case VDCTRL_QUERY_FORMAT:
      {
         int format =(*((int *)arg));
         switch(format){
         case IMGFMT_YUY2:
            return CONTROL_TRUE;
         default:
            return CONTROL_FALSE;
         }
      }
   case VDCTRL_RESYNC_STREAM:
      priv->need_second_field = 0;
      priv->unseen = 0;
      DtsFlushInput(priv->dev, 2);
      return CONTROL_TRUE;
   case VDCTRL_QUERY_UNSEEN_FRAMES:
      return 10 + priv->unseen;
   default:
      return CONTROL_UNKNOWN;
   }
}

/*============================================================================
 * init - initialize the codec
 *==========================================================================*/

static int init(sh_video_t *sh)
{
	Priv* priv;
        BC_INPUT_FORMAT format;
        BC_STATUS ret;

        uint8_t subtype;
        uint8_t *extradata = (uint8_t *)(sh->bih + 1);
        int extradata_size;

        if (!sh->bih || sh->bih->biSize <= sizeof(*sh->bih)) {
           extradata_size = 0;
        } else {
           extradata_size = sh->bih->biSize - sizeof(*sh->bih);
        }

        uint32_t mode = DTS_PLAYBACK_MODE |
                        DTS_LOAD_FILE_PLAY_FW |
                        DTS_SKIP_TX_CHK_CPB |
                        DTS_PLAYBACK_DROP_RPT_MODE |
                        DTS_DFLT_RESOLUTION(vdecRESOLUTION_1080p23_976);

        mp_msg(MSGT_DECVIDEO, MSGL_V, "CrystalHD Init for %s\n",
               sh->codec->dll);

	switch(sh->codec->outfmt[sh->outfmtidx]){
	case IMGFMT_YUY2:
		break;
	default:
		mp_msg(MSGT_DECVIDEO, MSGL_V, "Unsupported out_fmt: 0x%X\n",
		       sh->codec->outfmt[sh->outfmtidx]);
		return 0;
	}

	/* Gather some information about the host library */

	/* Initialize the library */
        priv = calloc(1, sizeof (Priv));
        priv->is_avc = extradata_size > 0 ? (*(char *)extradata == 1) : 0;
        sh->context = priv;

        memset(&format, 0, sizeof(BC_INPUT_FORMAT));
        format.FGTEnable = FALSE;
        format.Progressive = TRUE;
        format.OptFlags = 0x80000000 | vdecFrameRate59_94 | 0x40;
        format.width = sh->disp_w;
        format.height = sh->disp_h;

        subtype = name2subtype(priv, sh->codec->dll);
        switch (subtype) {
        case BC_MSUBTYPE_AVC1:
           format.startCodeSz = priv->nal_length_size;
           if (extract_sps_pps_from_avcc(priv, extradata, extradata_size) < 0) {
              mp_msg(MSGT_DECVIDEO, MSGL_V, "extract_sps_pps failed\n");
              return 0;
           }
           format.pMetaData = priv->sps_pps_buf;
           format.metaDataSz = priv->sps_pps_size;
           mp_msg(MSGT_DECVIDEO, MSGL_V, "AVC1: nal_length_size: %u\n", priv->nal_length_size);
           mp_msg(MSGT_DECVIDEO, MSGL_V, "AVC1: sps_pps_size: %u\n",  priv->sps_pps_size);
           mp_msg(MSGT_DECVIDEO, MSGL_V, "AVC1: extradata size: %u\n",  extradata_size);
           break;
        case BC_MSUBTYPE_H264:
           format.startCodeSz = priv->nal_length_size;
           // Fall-through
        case BC_MSUBTYPE_VC1:
        case BC_MSUBTYPE_WVC1:
        case BC_MSUBTYPE_WMV3:
        case BC_MSUBTYPE_WMVA:
        case BC_MSUBTYPE_MPEG1VIDEO:
        case BC_MSUBTYPE_MPEG2VIDEO:
        case BC_MSUBTYPE_DIVX:
        case BC_MSUBTYPE_DIVX311:
           format.pMetaData = extradata;
           format.metaDataSz = extradata_size;
           break;
        default:
           mp_msg(MSGT_DECVIDEO, MSGL_ERR, "CrystalHD: Unknown codec name\n");
           return 0;
        }
        format.mSubtype = subtype;

   /* Get a decoder instance */
   mp_msg(MSGT_DECVIDEO, MSGL_V, "starting up\n");
   // Initialize the Link and Decoder devices
   ret = DtsDeviceOpen(&priv->dev, mode);
   if (ret != BC_STS_SUCCESS) {
      mp_msg(MSGT_DECVIDEO, MSGL_V, "crap, DtsDeviceOpen failed\n");
      return 0;
   }

   ret = DtsSetInputFormat(priv->dev, &format);
   if (ret != BC_STS_SUCCESS) {
      mp_msg(MSGT_DECVIDEO, MSGL_V, "CrystalHD: SetInputFormat failed\n");
      return 0;
   }

    ret = DtsOpenDecoder(priv->dev, BC_STREAM_TYPE_ES);
    if (ret != BC_STS_SUCCESS) {
      mp_msg(MSGT_DECVIDEO, MSGL_V, "crap, DtsOpenDecoder failed\n");
      return 0;
    }

    ret = DtsSetColorSpace(priv->dev, OUTPUT_MODE422_YUY2);
    if (ret != BC_STS_SUCCESS) {
      mp_msg(MSGT_DECVIDEO, MSGL_V, "crap, DtsSetColorSpace failed\n");
      return 0;
    }
    ret = DtsStartDecoder(priv->dev);
    if (ret != BC_STS_SUCCESS) {
      mp_msg(MSGT_DECVIDEO, MSGL_V, "crap, DtsStartDecoder failed\n");
      return 0;
    }
    ret = DtsStartCapture(priv->dev);
    if (ret != BC_STS_SUCCESS) {
      mp_msg(MSGT_DECVIDEO, MSGL_V, "crap, DtsStartCapture failed\n");
      return 0;
    }

    mp_msg(MSGT_DECVIDEO, MSGL_V, "try calls done\n");

   return 1;
}

/*============================================================================
 * uninit - close the codec
 *==========================================================================*/

static void uninit(sh_video_t *sh){
	Priv *priv = sh->context;
	if(!priv)
		return;

        HANDLE device = priv->dev;
        DtsStopDecoder(device);
        DtsCloseDecoder(device);
        DtsDeviceClose(device);

        free(priv->sps_pps_buf);
        free(priv);
}


/*============================================================================
 * decode - decode a frame from stream
 *==========================================================================*/

static mp_image_t* decode(sh_video_t *sh, void* data, int len, int flags)
{
   BC_STATUS ret;
   BC_DTS_PROC_OUT output;
   BC_DTS_STATUS decoder_status;
   Priv *priv = sh->context;
   HANDLE dev = priv->dev;

   uint8_t input_full = 0;
   uint8_t eos;

   mp_msg(MSGT_DECVIDEO, MSGL_V, "CrystalHD: decode_frame\n");

   if (flags != 0) {
      mp_msg(MSGT_DECVIDEO, MSGL_STATUS, "CrystalHD: Frame drop requested: %u\n", flags);
   }

   do {
      if (len != 0) {
         int32_t tx_free = (int32_t)DtsTxFreeSize(dev);
         if (len < tx_free - 1024) {
            uint64_t pts = (uint64_t)(sh->pts * 1000) * 1000 * 10;
            ret = DtsProcInput(dev, data, len, pts, 0);
            if (ret == BC_STS_BUSY) {
               usleep(1000);
            } else if (ret != BC_STS_SUCCESS) {
               mp_msg(MSGT_DECVIDEO, MSGL_STATUS, "CrystalHD: ProcInput failed: %u\n", ret);
               return NULL;
            }
            len = 0; // We don't want to try and resubmit the input...
            input_full = 0;
            priv->unseen++;
         } else {
            mp_msg(MSGT_DECVIDEO, MSGL_STATUS, "CrystalHD: Input buffer full\n");
            input_full = 1;
         }
      } else {
         mp_msg(MSGT_DECVIDEO, MSGL_STATUS, "CrystalHD: No more input data\n");
      }

      ret = DtsGetDriverStatus(dev, &decoder_status);
      if (ret != BC_STS_SUCCESS) {
         mp_msg(MSGT_DECVIDEO, MSGL_STATUS, "CrystalHD: GetDriverStatus failed\n");
         return NULL;
      }
      /*
       * No frames ready. Don't try to extract.
       */
      if (decoder_status.ReadyListCount == 0) {
         usleep(1000);
      } else {
         break;
      }
   } while (input_full == 1);
   if (decoder_status.ReadyListCount == 0) {
      mp_msg(MSGT_DECVIDEO, MSGL_STATUS, "CrystalHD: No frames ready. Returning\n");
      return NULL;
   }

   memset(&output, 0, sizeof(BC_DTS_PROC_OUT));
   output.PicInfo.width = sh->disp_w;
   output.PicInfo.height =  sh->disp_h;

   // Request decoded data from the driver
   ret = DtsProcOutputNoCopy(dev, OUTPUT_PROC_TIMEOUT, &output);
   if (ret == BC_STS_FMT_CHANGE) {
      mp_msg(MSGT_DECVIDEO, MSGL_V, "CrystalHD: Initial format change\n");
      sh->disp_w = output.PicInfo.width;
      sh->disp_h = output.PicInfo.height;
      if (output.PicInfo.height == 1088) {
         sh->disp_h = 1080;
      }
      mpcodecs_config_vo(sh, sh->disp_w, sh->disp_h, IMGFMT_YUY2);
      return NULL;
   } else if (ret == BC_STS_SUCCESS) {
      //mp_msg(MSGT_DECVIDEO, MSGL_V, "Proceeding to evaluate output\n");
      mp_image_t* mpi = NULL;
      if (!priv->need_second_field) {
         mp_msg(MSGT_DECVIDEO, MSGL_V, "CrystalHD: Allocating MPI\n");
         mpi = mpcodecs_get_image(sh, MP_IMGTYPE_TEMP,
	                          MP_IMGFLAG_ACCEPT_STRIDE,
			          sh->disp_w, sh->disp_h);
         priv->mpi = mpi;
      }

      if (output.PoutFlags & BC_POUT_FLAGS_PIB_VALID) {
         uint8_t interlaced = 0, top_field = 0, bottom_field = 0;
         mp_msg(MSGT_DECVIDEO, MSGL_V, "YBuffSz: %u\n", output.YbuffSz);
         mp_msg(MSGT_DECVIDEO, MSGL_V, "YBuffDoneSz: %u\n", output.YBuffDoneSz);
         mp_msg(MSGT_DECVIDEO, MSGL_V, "UVBuffDoneSz: %u\n", output.UVBuffDoneSz);
         mp_msg(MSGT_DECVIDEO, MSGL_V, "Timestamp: %lu\n", output.PicInfo.timeStamp);
         mp_msg(MSGT_DECVIDEO, MSGL_V, "Picture Number: %u\n", output.PicInfo.picture_number);

          mp_msg(MSGT_DECVIDEO, MSGL_V, "\tWidth: %u\n", output.PicInfo.width);
          mp_msg(MSGT_DECVIDEO, MSGL_V, "\tHeight: %u\n", output.PicInfo.height);
          mp_msg(MSGT_DECVIDEO, MSGL_V, "\tChroma: 0x%03x\n", output.PicInfo.chroma_format);
          mp_msg(MSGT_DECVIDEO, MSGL_V, "\tPulldown: %u\n", output.PicInfo.pulldown);
          mp_msg(MSGT_DECVIDEO, MSGL_V, "\tFlags: 0x%08x\n", output.PicInfo.flags);
          mp_msg(MSGT_DECVIDEO, MSGL_V, "\tFrame Rate/Res: %u\n", output.PicInfo.frame_rate);
          mp_msg(MSGT_DECVIDEO, MSGL_V, "\tAspect Ratio: %u\n", output.PicInfo.aspect_ratio);
          mp_msg(MSGT_DECVIDEO, MSGL_V, "\tColor Primaries: %u\n", output.PicInfo.colour_primaries);
          mp_msg(MSGT_DECVIDEO, MSGL_V, "\tMetaData: %u\n", output.PicInfo.picture_meta_payload);
          mp_msg(MSGT_DECVIDEO, MSGL_V, "\tSession Number: %u\n", output.PicInfo.sess_num);
          mp_msg(MSGT_DECVIDEO, MSGL_V, "\tycom: %u\n", output.PicInfo.ycom);
          mp_msg(MSGT_DECVIDEO, MSGL_V, "\tCustom Aspect: %u\n", output.PicInfo.custom_aspect_ratio_width_height);
          mp_msg(MSGT_DECVIDEO, MSGL_V, "\tFrames to Drop: %u\n", output.PicInfo.n_drop);
          mp_msg(MSGT_DECVIDEO, MSGL_V, "\tH264 Valid Fields: 0x%08x\n", output.PicInfo.other.h264.valid);

          interlaced =  (output.PicInfo.flags & VDEC_FLAG_INTERLACED_SRC);// &&
                       //!(output.PicInfo.flags & VDEC_FLAG_UNKNOWN_SRC);
          top_field = (output.PicInfo.flags & VDEC_FLAG_TOPFIELD) == VDEC_FLAG_TOPFIELD;
          bottom_field = (output.PicInfo.flags & VDEC_FLAG_BOTTOMFIELD) == VDEC_FLAG_BOTTOMFIELD;

         {
            int width = output.PicInfo.width * 2; // 16bits per pixel
            int height = output.PicInfo.height;
            int dStride = priv->mpi->stride[0];
            uint8_t *src = output.Ybuff;
            uint8_t *dst = priv->mpi->planes[0];

            if (interlaced) {
               int dY = 0;
               int sY = 0;

               height /= 2;
               if (bottom_field) {
                  mp_msg(MSGT_DECVIDEO, MSGL_V, "Interlaced: bottom field\n");
                  dY = 1;
               } else if (top_field) {
                  mp_msg(MSGT_DECVIDEO, MSGL_V, "Interlaced: top field\n");
                  dY = 0;
               }

               for (sY = 0; sY < height; dY++, sY++) {
                  fast_memcpy(&(dst[dY * dStride]), &(src[sY * width]), width);
                  if (interlaced) {
                     dY++;
                  }
               }
            } else {
               memcpy_pic(dst, src, width, height, dStride, width);
            }
         }
         priv->need_second_field = interlaced && !priv->need_second_field;

         if (interlaced) {
            priv->mpi->fields |= MP_IMGFIELD_INTERLACED;
            if (!(output.PicInfo.flags & VDEC_FLAG_BOTTOM_FIRST)) {
               priv->mpi->fields |= MP_IMGFIELD_TOP_FIRST;
            }
         }
      }
      DtsReleaseOutputBuffs(dev, NULL, FALSE);

      DtsIsEndOfStream(dev, &eos);
      if (eos) {
         mp_msg(MSGT_DECVIDEO, MSGL_STATUS, "CrystalHD: Is EOS\n");
      }

      if (priv->need_second_field) {
         return &mpi_no_picture;
      } else {
         mp_msg(MSGT_DECVIDEO, MSGL_V, "CrystalHD: Returning MPI: %p\n", priv->mpi);
         priv->unseen--;
         return priv->mpi;
      }
   } else if (ret == BC_STS_BUSY) {
      usleep(1000);
      return NULL;
   } else {
      mp_msg(MSGT_DECVIDEO, MSGL_ERR, "CrystalHD: ProcOutput failed %d\n", ret);
      return NULL;
   }
}

/*****************************************************************************
 * Module structure definition
 ****************************************************************************/

static const vd_info_t info =
{
	"CrystalHD 1.0 decoder",
	"crystalhd",
	"Philip Langdale <philipl@overt.org>",
	"Philip Langdale <philipl@overt.org>",
	"No Comment"
};

LIBVD_EXTERN(crystalhd)
