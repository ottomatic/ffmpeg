/*
 * Copyright (c) 2012 Clément Bœsch <clement.boesch@smartjog.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file Adobe HTTP Dynamic Streaming for Flash fragmenter
 */

//#include <stdio.h>
#include <ctype.h>
#include <expat.h>

#include "avformat.h"
#include "internal.h"
#include "libavutil/base64.h"
#include "libavutil/eval.h"

/* the following ifdef-ing is directly imported from the libexpat examples */
#if defined(__amigaos__) && defined(__USE_INLINE__)
#include <proto/expat.h>
#endif

#ifdef XML_LARGE_SIZE
#if defined(XML_USE_MSC_EXTENSIONS) && _MSC_VER < 1400
#define XML_FMT_INT_MOD "I64"
#else
#define XML_FMT_INT_MOD "ll"
#endif
#else
#define XML_FMT_INT_MOD "l"
#endif

typedef struct ContentBlock {
    char *str;      ///< zero terminated string of read data in the current chunk
    int len;        ///< length of str, not accounting the '\0' end character
    uint8_t *dec;   ///< the decoded version of str (base64 or just stripped string), zero terminated
    int dec_len;    ///< length of dec, not accounting the '\0' end character
    int dec_ptr;    ///< pointer to where in the buffer the parsing of the content block is happening.
} ContentBlock;
/*
 * @name functions for reading data from base64 decoded blocks
 * in medias or bootstraps
 * */
static unsigned int hds_read_unsigned_byte(ContentBlock *block);
static unsigned int hds_read_int16(ContentBlock *block);
static unsigned int hds_read_int24(ContentBlock *block);
static unsigned int hds_read_int32(ContentBlock *block);
static uint64_t hds_read_int64(ContentBlock *block);

struct media {
    int bootstrap_id; // 0-based index of the bootstrap associated with the media
    //char url[MAX_URL_SIZE];
    AVFormatContext *ctx;
    AVPacket pkt;
    AVIOContext pb;
    AVFormatContext *parent;

    /* from xml */
    char *stream_id;
    char *url;
    int bitrate;
    char *bootstrap_info_id;
    ContentBlock metadata;
};

typedef struct Bootstrap {
    u_int8_t version; ///< Either 0 or 1.

    u_int8_t flags; ///< Reserved. Set to 0.

    unsigned int bootstrap_version; ///< The version number of the boostrap information.
                                         /// When the update field is set, bootstrap_info_version
                                         /// indicates the version number that is being updated.

    u_int8_t profile; ///< Indicates if it is the Named Access (0) or the Range
                      ///  Access (1) profile. One bit reserved for future profiles.

    u_int8_t live;    ///< Indicates if the media presentation is live (1) or not.

    u_int8_t update;  ///< Indicates if this table is a full version (0) or an update (1) to a 
                      ///  previously defined (sent) full version of the bootstrap box or file.

    unsigned int time_scale; ///< The number of units per second. The field current_media_time
                             ///  uses thisvalue to represent accurate time. Typically the value
                             ///  is 1000, for a unit of milliseconds.

    uint64_t smtpe_time_code_offset; ///< The offset of the current media time from the SMTPE time code,
                                     ///  converted to milliseconds. This ioffset is NOT in time_scale units.
                                     /// The field is zero when not used.

    char movie_identifier[MAX_URL_SIZE]; ///< The identifier of this presentation. It can be a filename or a url.
    
    int nb_server_entries; ///< The number of server entries. The minimum value is 0.
    
    char **server_entries; ///< Server URLs in descending order of preference, without trailing /.
    
    int nb_quality_entries; ///< The number of quality entries. 
    
    char **quality_entries; ///< Array with names of quality segment files, optionally with a trailing /.
    
    char *drm_data; ///< Null or string which holds digital rights management meta-data.
    
    char *meta_data; ///< Null or string which holds meta data.
    
    int nb_segments_runs; ///< The number of entries in the segment_run_table. The minimum value is 1.
                          ///  Typically, one table contains all segment runs. However, there may be a
                          ///  segment run for each quality.
    
 //   struct segment_run_table *segment_runs; ///< Array of segment run table elements.
    
    int nb_fragment_runs; ///< The number of entries in the fragment_run_table. The minimum value is 1.
    
 //   struct fragment_run_table *fragment_runs; ///< Array of fragment run table elements.
    
} Bootstrap;

typedef struct BootstrapInfo {
    int media_id; // 0-based index of the media associated with the bootstrap

    /* from xml */
    char *id;
    char *profile;
    ContentBlock data;
    
    Bootstrap bootstrap;
} BootstrapInfo;

enum {
    DATA_TYPE_NONE,
    DATA_TYPE_ID,
    DATA_TYPE_STREAM_TYPE,
    DATA_TYPE_DURATION,
    DATA_TYPE_MEDIA_METADATA,
    DATA_TYPE_BOOTSTRAP_INFO,
};

typedef struct HDSContext {
    AVIOInterruptCB *interrupt_callback;

    /* xml info */
    ContentBlock id;            ///< "id" root field
    ContentBlock stream_type;   ///< "streamType" root field
    ContentBlock duration;      ///< "duration" float root field

    struct media **medias;              ///< array of media
    int nb_medias;                      ///< number of element in the medias array

    BootstrapInfo **bs_info;         ///< array of bootstrap info
    int nb_bs_info;                     ///< number of element in the bootstrap info array

    /* xml parsing */
    int parse_ret;                      ///< an error occurred while parsing XML
    int data_type;                      ///< next data chunk will be of this style
} HDSContext;

static int hds_probe(AVProbeData *p)
{
    char *ptr = p->buf;

    while (isspace(*ptr))
        ptr++;
    if (!strncmp(ptr, "<?xml", 5) &&
        strstr(ptr, "<manifest") && strstr(ptr, "ns.adobe.com/f4m"))
    {
        av_log(NULL, AV_LOG_DEBUG, "Matched hds manifest, returning max score for hds format.\n");			
        return AVPROBE_SCORE_MAX;
    }
    return 0;
}

static void XMLCALL xml_start(void *data, const char *el, const char **attr)
{
    int i;
    HDSContext *hds = data;

    av_log(NULL,AV_LOG_DEBUG,"  In xml_start \n");

    hds->data_type = DATA_TYPE_NONE;
    if      (!strcmp(el, "id"))         hds->data_type = DATA_TYPE_ID;
    else if (!strcmp(el, "streamType")) hds->data_type = DATA_TYPE_STREAM_TYPE;
    else if (!strcmp(el, "duration"))   hds->data_type = DATA_TYPE_DURATION;
    else if (!strcmp(el, "media")) {
        av_log(NULL,AV_LOG_DEBUG,"  data_type is media. Allocating media struct. \n");
        struct media *m = av_mallocz(sizeof(*m));
        if (!m) {
            hds->parse_ret = AVERROR(ENOMEM);
            return;
        }
        av_log(NULL,AV_LOG_DEBUG,"  adding media to dynarray. \n");
        dynarray_add(&hds->medias, &hds->nb_medias, m);
 
        for (i = 0; attr[i]; i += 2) {
            if      (!strcmp(attr[i], "streamId"))        m->stream_id         = av_strdup(attr[i + 1]);
            else if (!strcmp(attr[i], "url"))             m->url               = av_strdup(attr[i + 1]);
            else if (!strcmp(attr[i], "bitrate"))         m->bitrate           = strtol(attr[i + 1], NULL, 10);
            else if (!strcmp(attr[i], "bootstrapInfoId")) m->bootstrap_info_id = av_strdup(attr[i + 1]);
        }
    } else if (!strcmp(el, "metadata")) {
        hds->data_type = DATA_TYPE_MEDIA_METADATA;
    } else if (!strcmp(el, "bootstrapInfo")) {
        av_log(NULL,AV_LOG_DEBUG,"  data_type is bootstrapInfo. Allocating BootstrapInfo type. \n");
        
        BootstrapInfo *bi = av_mallocz(sizeof(*bi));
        if (!bi) {
            hds->parse_ret = AVERROR(ENOMEM);
            return;
        }     
        av_log(NULL,AV_LOG_DEBUG,"  adding BootstrapInfo to dynarray. \n");
        dynarray_add(&hds->bs_info, &hds->nb_bs_info, bi);

        for (i = 0; attr[i]; i += 2) {
            if      (!strcmp(attr[i], "profile")) bi->profile = av_strdup(attr[i + 1]);
            else if (!strcmp(attr[i], "id"))      bi->id      = av_strdup(attr[i + 1]);
        }
        hds->data_type = DATA_TYPE_BOOTSTRAP_INFO;
    }
}

static ContentBlock *get_content_block(HDSContext *hds, int data_type)
{
    switch (hds->data_type) {
    case DATA_TYPE_ID:              return &hds->id;
    case DATA_TYPE_STREAM_TYPE:     return &hds->stream_type;
    case DATA_TYPE_DURATION:        return &hds->duration;
    case DATA_TYPE_MEDIA_METADATA:  return hds->nb_medias  ? &hds->medias [hds->nb_medias  - 1]->metadata : NULL;
    case DATA_TYPE_BOOTSTRAP_INFO:  return hds->nb_bs_info ? &hds->bs_info[hds->nb_bs_info - 1]->data     : NULL;
    }
    return NULL;
}

static void XMLCALL xml_data(void *data, const char *s, int len)
{
    HDSContext *hds = data;
    ContentBlock *block = get_content_block(hds, hds->data_type);
    av_log(NULL,AV_LOG_DEBUG,"  In xml_data \n");

    if (!block || !len || block->len >= INT_MAX - len - 1)
        return;
    block->str = av_realloc(block->str, block->len + len + 1);
    if (!block->str) {
        hds->parse_ret = AVERROR(ENOMEM);
        return;
    }
    memcpy(block->str + block->len, s, len);
    block->len += len;
    block->str[block->len] = 0;
}

static void XMLCALL xml_end(void *data, const char *el)
{
    HDSContext *hds = data;
    ContentBlock *block = get_content_block(hds, hds->data_type);

    av_log(NULL,AV_LOG_DEBUG,"  In xml_end \n");
    /* block can be null if we have a node which we don't track, such as akamai:version */
    if (!block)
        return;
 
         
    if (block->len && !block->dec) {
        int i;

        av_log(NULL,AV_LOG_DEBUG,"  setting zero lengths. \n");
        block->dec_len = 0;
        block->dec_ptr = 0;
        av_log(NULL,AV_LOG_DEBUG,"  zero lengths were set. \n");

        av_log(NULL,AV_LOG_DEBUG,"  rl-stripping string. \n");
        /* [rl]strip the string */
        char *s = block->str;
        for (i = block->len - 1; i >= 0 && isspace(s[i]); i--)
            s[i] = 0;
        while (isspace(*s))
            s++;

        av_log(NULL,AV_LOG_DEBUG,"  checking data type. \n");
        if (hds->data_type == DATA_TYPE_MEDIA_METADATA ||
            hds->data_type == DATA_TYPE_BOOTSTRAP_INFO) {
            av_log(NULL,AV_LOG_DEBUG,"  This is media or bootstrap. Allocating decoding buffer to block->len size (%d). \n", block->len);
            /* decode base64 fields */
            block->dec = av_mallocz(block->len);
            if (!block->dec) {
                av_log(NULL,AV_LOG_DEBUG,"  decoding buffer was NOT allocated. Setting hds->parse_ret\n");
                hds->parse_ret = AVERROR(ENOMEM);
                return;
            }
            av_log(NULL,AV_LOG_DEBUG,"  decoding buffer was allocated. \n");
            av_log(NULL,AV_LOG_DEBUG,"  decoding, [%d] characters. \n", block->len);
            block->dec_len = av_base64_decode(block->dec, s, block->len);
            av_log(NULL,AV_LOG_DEBUG,"  block->dec_len: [%d]. \n", block->dec_len);
        } else if (hds->data_type == DATA_TYPE_ID ||
                   hds->data_type == DATA_TYPE_STREAM_TYPE ||
                   hds->data_type == DATA_TYPE_DURATION) {
            /* duplicated cleaned string */
            block->dec = av_strdup(s);
        }
    }
    av_freep(&block->str);
    block->len = 0;
}

static int parse_manifest(AVFormatContext *s)
{
    int done = 0;
    char buf[4096];
    XML_Parser xmlp;
    HDSContext *hds = s->priv_data;

    av_log(s,AV_LOG_DEBUG,"  In parse_manifest \n");

    xmlp = XML_ParserCreate(NULL);
    if (!xmlp) {
        av_log(s, AV_LOG_ERROR, "Unable to allocate memory for the libexpat XML parser\n");
        return AVERROR(ENOMEM);
    }
    XML_SetUserData(xmlp, hds);
    XML_SetElementHandler(xmlp, xml_start, xml_end);
    XML_SetCharacterDataHandler(xmlp, xml_data);

    while (!done && !hds->parse_ret) {
        int len;

        len = avio_read(s->pb, buf, sizeof(buf));
        if (len < 0) {
            done = len == AVERROR_EOF;
            len = 0;
        }

        if (XML_Parse(xmlp, buf, len, done) == XML_STATUS_ERROR) {
            av_log(s, AV_LOG_ERROR, "Parse error at line %" XML_FMT_INT_MOD "u:\n%s\n",
                   XML_GetCurrentLineNumber(xmlp),
                   XML_ErrorString(XML_GetErrorCode(xmlp)));
            return AVERROR_INVALIDDATA;
        }
    }

    // Make links between bootstrap infos and medias
    for (int i = 0; i < hds->nb_bs_info; i++) {
        for (int j = 0; j < hds->nb_medias; j++) {
            if (strcmp(hds->medias[j]->bootstrap_info_id, hds->bs_info[i]->id) == 0)
            {
                av_log(s, AV_LOG_DEBUG, "Linking media at index [%d] to bootstrap info at index [%d] (bootstrap_id: [%s])\n", j, i, hds->bs_info[i]->id);
                hds->bs_info[i]->media_id = j;
                hds->medias[j]->bootstrap_id = i;
            }
        }
    }

    XML_ParserFree(xmlp);
    return hds->parse_ret;
}



static int read_box_header(ContentBlock *data, char (*box_type)[5], uint64_t *box_size)
{
    av_log(NULL,AV_LOG_DEBUG,"  In read_box_header \n");

//    if (!data || !*data->dec)
//        return -1;
    if (!data)
    {
        av_log(NULL,AV_LOG_DEBUG,"  In read_box_header, NO DATA!\n");
        return -1;
    }
    av_log(NULL,AV_LOG_DEBUG,"  data->dec_len: [%d] \n", data->dec_len);
    
    if (!data->dec)
    {
        av_log(NULL,AV_LOG_DEBUG,"  In read_box_header, NO data->dec!\n");
        return -1;
    }

    av_log(NULL,AV_LOG_DEBUG,"  data->dec_len: [%d] \n", data->dec_len);
    av_log(NULL,AV_LOG_DEBUG,"  data->dec_ptr: [%d] \n", data->dec_ptr);
        
    /* If there is no starting position we assume we shall start from the beginning */
//    if (data->dec_ptr == NULL)
//        data->dec_ptr = 0;
    
    av_log(NULL,AV_LOG_DEBUG,"  Calling hds_read_int32 \n");
    *box_size = (int64_t)hds_read_int32(data);
    av_log(NULL,AV_LOG_DEBUG,"  box_size=[%"PRId64"] \n", *box_size);    
    
    /* The box type is in the next four bytes after the initial size */
    strncpy(*box_type, data->dec + data->dec_ptr, 4);
    *box_type[4] = '\0';
    av_log(NULL,AV_LOG_DEBUG,"  determined box_type=[%s] \n", *box_type);        

    data->dec_ptr += 4;
    
    /* if the size is 1, it points to an extended size in the next 8 bytes */
    if (*box_size == 1)
    {
        *box_size = hds_read_int64(data) - (uint64_t)16;
    }
    else
    {
        *box_size -= 8;
    }
    
    return 0;    
}
static int parse_bootstrap_box(BootstrapInfo *binfo)
{
    ContentBlock *data = &binfo->data;
    
    Bootstrap *bootstrap = &binfo->bootstrap; 
    uint8_t temp;
    
    av_log(NULL,AV_LOG_DEBUG,"  In parse_bootstrap_box \n");

    bootstrap->version = hds_read_unsigned_byte(data);
    bootstrap->flags = hds_read_int24(data);
    
    bootstrap->bootstrap_version = hds_read_int32(data);
    
    temp = hds_read_unsigned_byte(data);
    bootstrap->profile = (temp & 0xC0) >> 6;
    bootstrap->live = ((temp & 0x20) == 0x20);
    bootstrap->update = ((temp & 0x1) == 0x1);
        
    return 0;
}

static int parse_bootstrap_data(BootstrapInfo *binfo)
{
    int ret = 0;
    uint64_t box_size = 0;
    
    char box_type[5];
    char expected_boxtype[5] = "abst";
    
    ContentBlock *data = &binfo->data;
  
    av_log(NULL,AV_LOG_DEBUG,"  In parse_bootstrap_data \n");
    
    if (!data)
        return -1;

    av_log(NULL,AV_LOG_DEBUG,"  Calling read_box_header \n");
    
    ret = read_box_header(data, &box_type, &box_size);
    if (ret < 0)
        return ret;

    av_log(NULL,AV_LOG_DEBUG,"  box_type: [%s], box_size: [%"PRId64"]\n", &box_type, box_size);
    
    if (!strcmp(box_type, expected_boxtype))
    {
        av_log(NULL,AV_LOG_ERROR,"  bootstrap meta data does not start whith bootstrap. box_type: [%s]\n", &box_type);        
        return AVERROR_INVALIDDATA;
    }

    av_log(NULL,AV_LOG_DEBUG,"  ok. calling parse_bootstrap_box\n");        
    ret = parse_bootstrap_box(binfo);
    if (ret < 0)
        return ret;
    
    return 0;
}

static int hds_read_header(AVFormatContext *s)
{
    int ret;
    HDSContext *hds = s->priv_data;

    hds->interrupt_callback = &s->interrupt_callback;

    if ((ret = parse_manifest(s)) < 0)
        return ret;

    /* parse the contents of the bootstrap_infos */
    for (int i = 0; i < hds->nb_bs_info; i++)
    {
        if ((ret = parse_bootstrap_data(hds->bs_info[i])) < 0)
            return ret;
    }

    return 0;
}

static void destroy_block(ContentBlock *block)
{
    av_freep(&block->str);
    av_freep(&block->dec);
    block->len = block->dec_len = block->dec_ptr = 0;
}

static int hds_read_close(AVFormatContext *s)
{
    int i;
    HDSContext *hds = s->priv_data;

#if 1
    av_log(0,AV_LOG_DEBUG,"ID=[%s]\n", hds->id.dec);
    av_log(NULL,AV_LOG_DEBUG,"StreamType=[%s]\n", hds->stream_type.dec);
    av_log(NULL,AV_LOG_DEBUG,"Duration=[%s]\n", hds->duration.dec);

    for (i = 0; i < hds->nb_medias; i++) {
        const struct media *m = hds->medias[i];
        av_log(NULL,AV_LOG_DEBUG," media #%d\n", i);
        av_log(NULL,AV_LOG_DEBUG,"  stream_id=[%s] url=[%s] bitrate=%d bs=[%s]\n",
               m->stream_id, m->url, m->bitrate, m->bootstrap_info_id);
        av_log(NULL,AV_LOG_DEBUG,"  bootstrap_id=%d\n", m->bootstrap_id);
        av_log(NULL,AV_LOG_DEBUG,"  metadata (len=%d):\n", m->metadata.dec_len);
        av_hex_dump_log(0,0, (m->metadata).dec, m->metadata.dec_len);
    }

    for (i = 0; i < hds->nb_bs_info; i++) {
        BootstrapInfo *bi = hds->bs_info[i];
        av_log(NULL,AV_LOG_DEBUG," bs #%d\n", i);
        av_log(NULL,AV_LOG_DEBUG,"  id=[%s] profile=[%s]\n", bi->id, bi->profile);
        av_log(NULL,AV_LOG_DEBUG,"  data (len=%d):\n", bi->data.dec_len);
        av_log(NULL,AV_LOG_DEBUG,"  media_id=%d\n", bi->media_id);
        av_hex_dump_log(NULL,AV_LOG_DEBUG, bi->data.dec, bi->data.dec_len);
    }
#endif

    destroy_block(&hds->id);
    destroy_block(&hds->stream_type);
    destroy_block(&hds->duration);

    for (i = 0; i < hds->nb_medias; i++) {
        av_freep(&hds->medias[i]->stream_id);
        av_freep(&hds->medias[i]->url);
        av_freep(&hds->medias[i]->bootstrap_info_id);
        //destroy_block(&hds->medias[i]->metadata);
        av_freep(&hds->medias[i]->metadata);        
        av_freep(&hds->medias[i]);
    }
    av_freep(&hds->medias);

    for (i = 0; i < hds->nb_bs_info; i++) {
        av_freep(&hds->bs_info[i]->id);
        av_freep(&hds->bs_info[i]->profile);
        //destroy_block(&hds->bs_info[i]->data);
        av_freep(&hds->bs_info[i]->data);
        av_freep(&hds->bs_info[i]);
        // todo: free booststrap
    }
    av_freep(&hds->bs_info);

    return 0;
}

static int hds_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    //HDSContext *hds = s->priv_data;
    pkt->size = 0;
    pkt->data = NULL;
    return AVERROR_EOF;
}

AVInputFormat ff_hds_demuxer = {
    .name           = "hds",
    .long_name      = NULL_IF_CONFIG_SMALL("Adobe HTTP Dynamic Streaming"),
    .priv_data_size = sizeof(HDSContext),
    .read_probe     = hds_probe,
    .read_header    = hds_read_header,
    .read_packet    = hds_read_packet,
    .read_close     = hds_read_close,
    //.read_seek      = hds_read_seek,
};

/* Functions to help parse Boxes as defined in the F4V / F4F spec 
 * Numbers in streams / base64 blocks are big endian.
 * */
static unsigned int hds_read_unsigned_byte(ContentBlock *block)
{
//    av_log(NULL,AV_LOG_DEBUG,"  block->dec_len=%d\n", block->dec_len);
//    av_log(NULL,AV_LOG_DEBUG,"  block->len=%d\n", block->len);
//    av_log(NULL,AV_LOG_DEBUG,"  block->dec_ptr=%d\n", block->dec_ptr);

    if (block->dec_ptr < block->dec_len)
        return block->dec[block->dec_ptr++];
    return 0;
}

static unsigned int hds_read_int16(ContentBlock *block)
{
    unsigned int val;
    val = hds_read_unsigned_byte(block) << 8;
    val |= hds_read_unsigned_byte(block);
    return val;
}

static unsigned int hds_read_int24(ContentBlock *block)
{
    unsigned int val;
    val = hds_read_int16(block) << 8;
    val |= hds_read_unsigned_byte(block);
    return val;
}

static unsigned int hds_read_int32(ContentBlock *block)
{
    unsigned int val;
    val = hds_read_int16(block) << 16;
    val |= hds_read_int16(block);
    return val;
}

static uint64_t hds_read_int64(ContentBlock *block)
{
    uint64_t val;
    val = (uint64_t)hds_read_int32(block) << 32;
    val |= (uint64_t)hds_read_int32(block);
    return val;
}
