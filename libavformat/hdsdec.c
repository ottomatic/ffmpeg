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
static unsigned int hds_read_string(char *str, ContentBlock *block);

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

static const int DISCONTINUITY_INDICATOR_END_OF_PRESENTATION = 0;
static const int DISCONTINUITY_INDICATOR_FRAGMENT_NUMBERING = 1;
static const int DISCONTINUITY_INDICATOR_TIMESTAMPS = 2;
static const int DISCONTINUITY_INDICATOR_FRAGMENT_NUMBERING_AND_TIMESTAMPS = 3;

typedef struct FragmentRunEntry{
    u_int32_t first_fragment; ///< The identifying number of the first fragment in this
                              ///< run of fragments with the same duration. The
                              ///< fragment corresponding to the FirstFragment in the
                              ///< next FRAGMENTRUNENTRY will terminate this run.

    uint64_t first_fragment_timestamp; ///< The timestamp of the first_fragment, in TimeScale
                                       ///< units. This field ensures that the fragment
                                       ///< timestamps can be accurately represented at the
                                       ///< beginning. It also ensures that the timestamps are
                                       ///< synchronized when drifts occur due to duration
                                       ///< accuracy or timestamp discontinuities.

    u_int32_t fragment_duration; ///< The duration, in TimeScale units, of each fragment in this run



    u_int8_t discontinuity_indicator; ///< ONLY USED IF FragmentDuration == 0. Indicates discontinuities in timestamps, fragment
                                      ///< numbers, or both. This field is also used to identify
                                      ///< the end of a (live) presentation.
                                      ///< The following values are defined:
                                      ///< 0 = end of presentation.
                                      ///< 1 = a discontinuity in fragment numbering.
                                      ///< 2 = a discontinuity in timestamps.
                                      ///< 3 = a discontinuity in both timestamps and fragment numbering.
                                      ///< All other values are reserved.
                                      ///< Signaling the end of the presentation in-band is
                                      ///< useful in live scenarios. Gaps in the presentation are
                                      ///< signaled as a run of zero duration fragments with
                                      ///< both fragment number and timestamp
                                      ///< discontinuities. Fragment number discontinuities
                                      ///< are useful to signal jumps in fragment numbering
                                      ///< schemes with no discontinuities in the presentation.
} FragmentRunEntry;
/*
 * Fragment Run Table box
Box type: 'afrt'
Container: Bootstrap Info box ('abst')
Mandatory: Yes
Quantity: One or more
The Fragment Run Table (afrt) box can be used to find the fragment that contains a sample corresponding to a given
time.
Fragments are individually identifiable by the URL scheme. Fragments may vary both in duration and in number of
samples. The Durations of the Fragments are stored in the afrt box (the fragment run table).
The afrt box uses a compact encoding:
- A Fragment Run Table may represent fragments for more than one quality level.
- The Fragment Run Table is compactly coded, as each entry gives the first fragment number for a run of
fragments with the same duration. The count of fragments having this same duration can be calculated by
subtracting the first fragment number in this entry from the first fragment number in the next entry.
There may be several Fragment Run Table boxes in one Bootstrap Info box, each for different quality levels.

 * */
typedef struct FragmentRunTable {
    u_int8_t version; ///< Either 0 or 1.

    unsigned int flags; ///< The following values are defined:
                        ///< 0 = A full table.
                        ///< 1 = The records in this table are updates (or new
                        ///< entries to be appended) to the previously
                        ///< defined Segment Run Table. The Update flag in
                        ///< the containing Bootstrap Info box shall be 1
                        ///< when this flag is 1.
    
    u_int32_t time_scale; ///< The number of time units per second, used in
                          ///< the FirstFragmentTimestamp and
                          ///< FragmentDuration fields. Typically, the value is 1.

    
    unsigned int nb_quality_segment_url_modifiers; ///< The number of QualitySegmentUrlModifiers
                                                   ///  (quality level references) that follow. If 0, this
                                                   ///  Fragment Run Table applies to all quality levels,
                                                   ///  and there shall be only one Fragment Run Table
                                                   ///  box in the Bootstrap Info box.
                                               
    char **quality_segment_url_modifiers; ///< An array of names of the quality levels that this
                                          ///  table applies to. The names are null-terminated
                                          ///  UTF-8 strings. The array shall be a subset of the
                                          ///  QualityEntryTable in the Bootstrap Info (abst) box.
                                          ///  The names shall not be present in any other
                                          ///  Fragment Run Table in the Bootstrap Info box.

    u_int32_t nb_fragment_run_entries; ///< The number of items in this
                                       ///< FragmentRunEntryTable. The minimum value is 1.

    FragmentRunEntry **fragment_run_entries; ///< Array of pointers to fragment run entries

} FragmentRunTable;

typedef struct SegmentRunEntry{
    u_int32_t first_segment; ///< The identifying number of the first segment in the run of
                             ///  segments containing the same number of fragments. The
                             ///  segment corresponding to the first_segment in the next
                             ///  SegmentRunEntry will terminate this run.

    u_int32_t fragments_per_segment; ///< The number of fragments in each segment in this run.

} SegmentRunEntry;

/*
 * Box type: 'asrt'
Container: Bootstrap Info box ('abst')
Mandatory: Yes
Quantity: One or more
The Segment Run Table (asrt) box can be used to locate a segment that contains a particular fragment.
There may be several asrt boxes, each for different quality levels.
41
ADOBE FLASH VIDEO FILE FORMAT SPECIFICATION VERSION 10.1
F4V Box Definitions
The asrt box uses a compact encoding:
- A Segment Run Table may represent fragment runs for several quality levels.
- The Segment Run Table is compactly coded. Each entry gives the first segment number for a run of segments
with the same count of fragments. The count of segments having this same count of fragments can be
calculated by subtracting the first segment number in this entry from the first segment number in the next
entry.

 * */
typedef struct SegmentRunTable {
    u_int8_t version; ///< Either 0 or 1.

    unsigned int flags; ///< The following values are defined:
                        ///< 0 = A full table.
                        ///< 1 = The records in this table are updates (or new
                        ///< entries to be appended) to the previously
                        ///< defined Segment Run Table. The Update flag in
                        ///< the containing Bootstrap Info box shall be 1
                        ///< when this flag is 1.

    
    unsigned int nb_quality_segment_url_modifiers; ///< The number of QualitySegmentUrlModifiers
                                               ///  (quality level references) that follow. If 0, this
                                               ///  Segment Run Table applies to all quality levels,
                                               ///  and there shall be only one Segment Run Table
                                               ///  box in the Bootstrap Info box.
                                               
    char **quality_segment_url_modifiers; ///< An array of names of the quality levels that this
                                          ///  table applies to. The names are null-terminated
                                          ///  UTF-8 strings. The array shall be a subset of the
                                          ///  QualityEntryTable in the Bootstrap Info (abst) box.
                                          ///  The names shall not be present in any other
                                          ///  Segment Run Table in the Bootstrap Info box.

    unsigned int nb_segment_run_entries; ///< The number of items in this
                                         ///  SegmentRunEntryTable. The minimum value is 1.

    SegmentRunEntry **segment_run_entries; ///< Array of segment run entries

} SegmentRunTable;

typedef struct Bootstrap {
    u_int8_t version; ///< Either 0 or 1.

    unsigned int flags; ///< Reserved. Set to 0.

    unsigned int bootstrap_version; ///< The version number of the boostrap information.
                                         /// When the update field is set, bootstrap_info_version
                                         /// indicates the version number that is being updated.

    u_int8_t profile; ///< Indicates if it is the Named Access (0) or the Range
                      ///  Access (1) profile. One bit reserved for future profiles.

    u_int8_t live;    ///< Indicates if the media presentation is live (1) or not.

    u_int8_t update;  ///< Indicates if this table is a full version (0) or an update (1) to a 
                      ///  previously defined (sent) full version of the bootstrap box or file.

    u_int32_t time_scale; ///< The number of units per second. The field current_media_time
                             ///  uses thisvalue to represent accurate time. Typically the value
                             ///  is 1000, for a unit of milliseconds.

    uint64_t current_media_time; ///< The timestamp in TimeScale units of the latest 
                                 ///  available Fragment in the media presentation. This
                                 ///  timestamp is used to request the right fragment
                                 ///  number. The CurrentMediaTime can be the total
                                 ///  duration. For media presentations that are not live,
                                 ///  CurrentMediaTime can be 0.


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
    
    int nb_segment_runs; ///< The number of entries in the segment_run_table. The minimum value is 1.
                          ///  Typically, one table contains all segment runs. However, there may be a
                          ///  segment run for each quality.
    
    SegmentRunTable **segment_runs; ///< Array of pointers to segment run table elements.
    
    int nb_fragment_runs; ///< The number of entries in the fragment_run_table. The minimum value is 1.
    
    FragmentRunTable **fragment_runs; ///< Array of pointers to fragment run table elements.
    
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
        struct media *m;
        m = av_mallocz(sizeof(*m));
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

    av_log(NULL,AV_LOG_DEBUG,"  data->dec_ptr: [%d] \n", data->dec_ptr);
        
    
    av_log(NULL,AV_LOG_DEBUG,"  Calling hds_read_int32 \n");
    *box_size = (int64_t)hds_read_int32(data);
    av_log(NULL,AV_LOG_DEBUG,"  box_size=[%"PRIu64"] \n", *box_size);    
    
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

static int parse_fragment_runtable_box(ContentBlock *data, FragmentRunTable *afrt)
{  
    int nb_quality_segment_url_modifiers;
    int nb_fragment_run_entries;
    
    av_log(NULL,AV_LOG_DEBUG,"  In parse_fragment_runtable_box \n");

    afrt->version = hds_read_unsigned_byte(data);
    av_log(NULL,AV_LOG_DEBUG,"  In parse_fragment_runtable_box, afrt->version: [%d] \n", afrt->version);

    afrt->flags = hds_read_int24(data);
    av_log(NULL,AV_LOG_DEBUG,"  In parse_fragment_runtable_box, afrt->flags: [%d] \n", afrt->flags);

    afrt->time_scale = hds_read_int32(data);
    av_log(NULL,AV_LOG_DEBUG,"  In parse_fragment_runtable_box, afrt->time_scale: [%d] \n", afrt->time_scale);
    
    nb_quality_segment_url_modifiers = hds_read_unsigned_byte(data);
    av_log(NULL,AV_LOG_DEBUG,"  In parse_fragment_runtable_box, nb_quality_segment_url_modifiers: [%d] \n", nb_quality_segment_url_modifiers);
    
    for (int index = 0; index < nb_quality_segment_url_modifiers; index++)
    {
        char *quality_segment_url_modifyer = av_mallocz(sizeof(*quality_segment_url_modifyer));
        if (!quality_segment_url_modifyer) {
            return AVERROR(ENOMEM);
        }
        hds_read_string(quality_segment_url_modifyer, data);
        av_log(NULL,AV_LOG_DEBUG,"  In parse_fragment_runtable_box, quality_segment_url_modifyer[%d]: [%s] \n", index, quality_segment_url_modifyer);
        
        dynarray_add(&afrt->quality_segment_url_modifiers, &afrt->nb_quality_segment_url_modifiers, quality_segment_url_modifyer);
    }
    av_log(NULL,AV_LOG_DEBUG,"  In parse_fragment_runtable_box, afrt->nb_quality_segment_url_modifiers: [%d] \n", afrt->nb_quality_segment_url_modifiers);

    nb_fragment_run_entries = hds_read_int32(data);
    av_log(NULL,AV_LOG_DEBUG,"  In parse_fragment_runtable_box, nb_fragment_run_entries: [%d] \n", nb_fragment_run_entries);
    for (int index = 0; index < nb_fragment_run_entries; index++)
    {
        // Parse fragment run entries
        FragmentRunEntry *fragment_run_entry = av_mallocz(sizeof(*fragment_run_entry));
        if (!fragment_run_entry) {
            return AVERROR(ENOMEM);
        }
        fragment_run_entry->first_fragment = hds_read_int32(data);
        fragment_run_entry->first_fragment_timestamp = hds_read_int64(data);
        fragment_run_entry->fragment_duration = hds_read_int32(data);
        av_log(NULL,AV_LOG_DEBUG,"  In parse_fragment_runtable_box, fragment_run_entry[%d]->first_fragment: [%d] \n", index, fragment_run_entry->first_fragment);
        av_log(NULL,AV_LOG_DEBUG,"  In parse_fragment_runtable_box, fragment_run_entry[%d]->first_fragment_timestamp: [%"PRIu64"] \n", index, fragment_run_entry->first_fragment_timestamp);
        av_log(NULL,AV_LOG_DEBUG,"  In parse_fragment_runtable_box, fragment_run_entry[%d]->fragment_duration: [%d] \n", index, fragment_run_entry->fragment_duration);
        
        if (fragment_run_entry->fragment_duration == 0)
        {
            fragment_run_entry->discontinuity_indicator = hds_read_unsigned_byte(data);
            av_log(NULL,AV_LOG_DEBUG,"  In parse_fragment_runtable_box, fragment_run_entry[%d]->discontinuity_indicator: [%d] \n", index, fragment_run_entry->discontinuity_indicator);
        }
        av_log(NULL,AV_LOG_DEBUG,"  adding FragmentRunTable[%d] to dynarray.\n", index);
        dynarray_add(&afrt->fragment_run_entries, &afrt->nb_fragment_run_entries, fragment_run_entry);
    }
    av_log(NULL,AV_LOG_DEBUG,"  In parse_fragment_runtable_box, afrt->nb_fragment_run_entries: [%d] \n", afrt->nb_fragment_run_entries);

    return 0;
}

static int parse_segment_runtable_box(ContentBlock *data, SegmentRunTable *asrt)
{  
    int nb_quality_segment_url_modifiers;
    int nb_segment_run_entries;
    
    av_log(NULL,AV_LOG_DEBUG,"  In parse_segment_runtable_box \n");
    
    asrt->version = hds_read_unsigned_byte(data);
    av_log(NULL,AV_LOG_DEBUG,"  In parse_segment_runtable_box, asrt->version: [%d] \n", asrt->version);
    
    asrt->flags = hds_read_int24(data);
    av_log(NULL,AV_LOG_DEBUG,"  In parse_segment_runtable_box, asrt->flags: [%d] \n", asrt->flags);
    
    nb_quality_segment_url_modifiers = hds_read_unsigned_byte(data);
    av_log(NULL,AV_LOG_DEBUG,"  In parse_segment_runtable_box, nb_quality_segment_url_modifiers: [%d] \n", nb_quality_segment_url_modifiers);
    
    for (int index = 0; index < nb_quality_segment_url_modifiers; index++)
    {
        char *quality_segment_url_modifyer = av_mallocz(sizeof(*quality_segment_url_modifyer));
        if (!quality_segment_url_modifyer) {
            return AVERROR(ENOMEM);
        }
        hds_read_string(quality_segment_url_modifyer, data);
        av_log(NULL,AV_LOG_DEBUG,"  In parse_segment_runtable_box, quality_segment_url_modifyer[%d]: [%s] \n", index, quality_segment_url_modifyer);
        
        dynarray_add(&asrt->quality_segment_url_modifiers, &asrt->nb_quality_segment_url_modifiers, quality_segment_url_modifyer);
    }
    av_log(NULL,AV_LOG_DEBUG,"  In parse_segment_runtable_box, asrt->nb_quality_segment_url_modifiers: [%d] \n", asrt->nb_quality_segment_url_modifiers);

    nb_segment_run_entries = hds_read_int32(data);
    av_log(NULL,AV_LOG_DEBUG,"  In parse_segment_runtable_box, nb_segment_run_entries: [%d] \n", nb_segment_run_entries);
    for (int index = 0; index < nb_segment_run_entries; index++)
    {
        // Parse segment run entries
        SegmentRunEntry *segment_run_entry = av_mallocz(sizeof(*segment_run_entry));
        if (!segment_run_entry) {
            return AVERROR(ENOMEM);
        }
        segment_run_entry->first_segment = hds_read_int32(data);
        segment_run_entry->fragments_per_segment = hds_read_int32(data);
        av_log(NULL,AV_LOG_DEBUG,"  In parse_segment_runtable_box, segment_run_entry[%d]->first_segment: [%d] \n", index, segment_run_entry->first_segment);
        av_log(NULL,AV_LOG_DEBUG,"  In parse_segment_runtable_box, segment_run_entry[%d]->fragments_per_segment: [%d] \n", index, segment_run_entry->fragments_per_segment);
                
        av_log(NULL,AV_LOG_DEBUG,"  adding SegmentRunTable[%d] to dynarray.\n", index);
        dynarray_add(&asrt->segment_run_entries, &asrt->nb_segment_run_entries, segment_run_entry);
    }
    av_log(NULL,AV_LOG_DEBUG,"  In parse_segment_runtable_box, asrt->nb_segment_run_entries: [%d] \n", asrt->nb_segment_run_entries);

    return 0;
}

static int parse_bootstrap_box(BootstrapInfo *binfo)
{
    int ret = 0;
    uint64_t box_size = 0;
    char box_type[5];
    const char expected_boxtype_segment_runs[5] = "asrt";
    const char expected_boxtype_fragment_runs[5] = "afrt";
    
    ContentBlock *data = &binfo->data;
    
    Bootstrap *bootstrap = &binfo->bootstrap; 
    uint8_t temp;
    int nb_server_entries, nb_quality_entries;
    int nb_segment_runs;
    int nb_fragment_runs;
    
    av_log(NULL,AV_LOG_DEBUG,"  In parse_bootstrap_box \n");

    bootstrap->version = hds_read_unsigned_byte(data);
    av_log(NULL,AV_LOG_DEBUG,"  In parse_bootstrap_box, bootstrap->version: [%d] \n", bootstrap->version);
    bootstrap->flags = hds_read_int24(data);
    
    bootstrap->bootstrap_version = hds_read_int32(data);
    
    temp = hds_read_unsigned_byte(data);
    bootstrap->profile = (temp & 0xC0) >> 6;
    av_log(NULL,AV_LOG_DEBUG,"  In parse_bootstrap_box, bootstrap->profile: [%d] \n", bootstrap->profile);
    bootstrap->live = ((temp & 0x20) == 0x20);
    av_log(NULL,AV_LOG_DEBUG,"  In parse_bootstrap_box, bootstrap->live: [%d] \n", bootstrap->live);
    bootstrap->update = ((temp & 0x1) == 0x1);
    
    bootstrap->time_scale = hds_read_int32(data);
    av_log(NULL,AV_LOG_DEBUG,"  In parse_bootstrap_box, bootstrap->time_scale: [%d] \n", bootstrap->time_scale);

    bootstrap->current_media_time = hds_read_int64(data);
    av_log(NULL,AV_LOG_DEBUG,"  In parse_bootstrap_box, bootstrap->current_media_time: [%"PRIu64"]\n", bootstrap->current_media_time);
    bootstrap->smtpe_time_code_offset = hds_read_int64(data);
    av_log(NULL,AV_LOG_DEBUG,"  In parse_bootstrap_box, bootstrap->smtpe_time_code_offset: [%"PRIu64"]\n", bootstrap->smtpe_time_code_offset);
    
    hds_read_string(bootstrap->movie_identifier, data);    
    av_log(NULL,AV_LOG_DEBUG,"  In parse_bootstrap_box, bootstrap->movie_identifier: [%s] \n", bootstrap->movie_identifier);
    
    nb_server_entries = hds_read_unsigned_byte(data);
    
    for (u_int8_t index = 0; index < nb_server_entries; index++)
    {
        char *server_entry;
        server_entry = av_mallocz(sizeof(server_entry));
        if (!server_entry) {
            return AVERROR(ENOMEM);
        }
        hds_read_string(server_entry, data);
        
        dynarray_add(&bootstrap->server_entries, &bootstrap->nb_server_entries, server_entry);
    }
    av_log(NULL,AV_LOG_DEBUG,"  In parse_bootstrap_box, bootstrap->nb_server_entries: [%d] \n", bootstrap->nb_server_entries);

    nb_quality_entries = hds_read_unsigned_byte(data);
    av_log(NULL,AV_LOG_DEBUG,"  In parse_bootstrap_box, nb_quality_entries: [%d] \n", nb_quality_entries);
    
    for (int index = 0; index < nb_quality_entries; index++)
    {
        char *quality_entry;
        quality_entry = av_mallocz(sizeof(quality_entry));
        if (!quality_entry) {
            return AVERROR(ENOMEM);
        }
        hds_read_string(quality_entry, data);
        av_log(NULL,AV_LOG_DEBUG,"  In parse_bootstrap_box, quality_entry: [%s] \n", quality_entry);
        
        dynarray_add(&bootstrap->quality_entries, &bootstrap->nb_quality_entries, quality_entry);
    }
    av_log(NULL,AV_LOG_DEBUG,"  In parse_bootstrap_box, bootstrap->nb_quality_entries: [%d] \n", bootstrap->nb_quality_entries);

    if (!bootstrap->drm_data)
        bootstrap->drm_data = av_mallocz(sizeof(bootstrap->drm_data));
    hds_read_string(bootstrap->drm_data, data);    
    av_log(NULL,AV_LOG_DEBUG,"  In parse_bootstrap_box, bootstrap->drm_data: [%s] \n", bootstrap->drm_data);
    
    if (!bootstrap->meta_data)
        bootstrap->meta_data = av_mallocz(sizeof(bootstrap->meta_data));
    hds_read_string(bootstrap->meta_data, data);    
    av_log(NULL,AV_LOG_DEBUG,"  In parse_bootstrap_box, bootstrap->meta_data: [%s] \n", bootstrap->meta_data);

    nb_segment_runs = hds_read_unsigned_byte(data);
    av_log(NULL,AV_LOG_DEBUG,"  In parse_bootstrap_box, nb_segment_runs: [%d] \n", nb_segment_runs);
    
    for (int index = 0; index < nb_segment_runs; index++)
    {
        // Parse segment run table, dynamically add to bootstrap segment run tables list    
        av_log(NULL,AV_LOG_DEBUG,"  Calling read_box_header, expecting to find segment run table (asrt)\n");
        
        ret = read_box_header(data, &box_type, &box_size);
        if (ret < 0)
            return ret;

        av_log(NULL,AV_LOG_DEBUG,"  box_type: [%s], box_size: [%"PRIu64"]\n", &box_type, box_size);
        
        if (strncmp(box_type, expected_boxtype_segment_runs, 4) != 0)
        {
            av_log(NULL,AV_LOG_ERROR,"  bootstrap meta data does not have segment run table box (asrt) at expected position. data->dec_ptr: [%d]\n", data->dec_ptr);        
            return AVERROR_INVALIDDATA;
        }
        
        SegmentRunTable *asrt = av_mallocz(sizeof(*asrt));
        if (!asrt) {
            return AVERROR(ENOMEM);
        }     
        av_log(NULL,AV_LOG_DEBUG,"  adding SegmentRunTable to dynarray. \n");
        dynarray_add(&bootstrap->segment_runs, &bootstrap->nb_segment_runs, asrt);

        av_log(NULL,AV_LOG_DEBUG,"  ok. calling parse_segment_runtable_box\n");        
        ret = parse_segment_runtable_box(data, asrt);
        if (ret < 0)
            return ret;
    }
    av_log(NULL,AV_LOG_DEBUG,"  In parse_bootstrap_box, bootstrap->nb_segment_runs: [%d] \n", bootstrap->nb_segment_runs);

    nb_fragment_runs = hds_read_unsigned_byte(data);
    av_log(NULL,AV_LOG_DEBUG,"  In parse_bootstrap_box, nb_fragment_runs: [%d] \n", nb_fragment_runs);
    
    for (int index = 0; index < nb_fragment_runs; index++)
    {
        // Parse fragment run table, dynamically add to bootstrap fragment run tables list    
        av_log(NULL,AV_LOG_DEBUG,"  Calling read_box_header, expecting to find fragment run table (afrt)\n");
        
        ret = read_box_header(data, &box_type, &box_size);
        if (ret < 0)
            return ret;

        av_log(NULL,AV_LOG_DEBUG,"  box_type: [%s], box_size: [%"PRIu64"]\n", &box_type, box_size);
        
        if (strncmp(box_type, expected_boxtype_fragment_runs, 4) != 0)
        {
            av_log(NULL,AV_LOG_ERROR,"  bootstrap meta data does not have fragment run table box (afrt) at expected position. data->dec_ptr: [%d]\n", data->dec_ptr);        
            return AVERROR_INVALIDDATA;
        }

        FragmentRunTable *afrt = av_mallocz(sizeof(*afrt));
        if (!afrt) {
            return AVERROR(ENOMEM);
        }     
        av_log(NULL,AV_LOG_DEBUG,"  adding FragmentRunTable to dynarray. \n");
        dynarray_add(&bootstrap->fragment_runs, &bootstrap->nb_fragment_runs, afrt);

        av_log(NULL,AV_LOG_DEBUG,"  ok. calling parse_fragment_runtable_box\n");        
        
        ret = parse_fragment_runtable_box(data, afrt);
        if (ret < 0)
            return ret;
    }
    av_log(NULL,AV_LOG_DEBUG,"  In parse_bootstrap_box, bootstrap->nb_fragment_runs: [%d] \n", bootstrap->nb_fragment_runs);

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

    av_log(NULL,AV_LOG_DEBUG,"  box_type: [%s], expected_boxtype: [%s], box_size: [%"PRIu64"]\n", &box_type, &expected_boxtype, box_size);
    av_log(NULL,AV_LOG_DEBUG,"  strncmp(box_type, expected_boxtype, 4): [%d]\n", strncmp(box_type, expected_boxtype, 4));
    
    if (strncmp(box_type, expected_boxtype, 4) != 0)
    {
        av_log(NULL,AV_LOG_ERROR,"  bootstrap meta data does not start with bootstrap. box_type: [%s]\n", &box_type);        
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

static unsigned int hds_read_string(char *str, ContentBlock *block)
{
    u_int8_t strlen = 0;
    while (block->dec[block->dec_ptr + strlen] != '\0')
        strlen++;
    av_log(NULL,AV_LOG_DEBUG,"  hds_read_string, strlen: [%d] \n", strlen);
    
    if (strlen > 0)
        strncpy(str, block->dec + block->dec_ptr, strlen);

    av_log(NULL,AV_LOG_DEBUG,"  hds_read_string, inserting null character \n");

    str[strlen] = '\0';
    av_log(NULL,AV_LOG_DEBUG,"  hds_read_string, moving dec_ptr\n");
    block->dec_ptr += strlen + 1;
    av_log(NULL,AV_LOG_DEBUG,"  hds_read_string, returning \n");
    return strlen;
}
