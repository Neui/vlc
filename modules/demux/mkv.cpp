/*****************************************************************************
 * mkv.cpp : matroska demuxer
 *****************************************************************************
 * Copyright (C) 2003-2004 VideoLAN
 * $Id$
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Steve Lhomme <steve.lhomme@free.fr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <stdlib.h>                                      /* malloc(), free() */

#include <vlc/vlc.h>

#ifdef HAVE_TIME_H
#   include <time.h>                                               /* time() */
#endif

#include <vlc/input.h>

#include <codecs.h>                        /* BITMAPINFOHEADER, WAVEFORMATEX */
#include "iso_lang.h"
#include "vlc_meta.h"

#include <iostream>
#include <cassert>
#include <typeinfo>
#include <string>
#include <vector>
#include <algorithm>

#ifdef HAVE_DIRENT_H
#   include <dirent.h>
#endif

/* libebml and matroska */
#include "ebml/EbmlHead.h"
#include "ebml/EbmlSubHead.h"
#include "ebml/EbmlStream.h"
#include "ebml/EbmlContexts.h"
#include "ebml/EbmlVoid.h"
#include "ebml/StdIOCallback.h"

#include "matroska/KaxAttachments.h"
#include "matroska/KaxBlock.h"
#include "matroska/KaxBlockData.h"
#include "matroska/KaxChapters.h"
#include "matroska/KaxCluster.h"
#include "matroska/KaxClusterData.h"
#include "matroska/KaxContexts.h"
#include "matroska/KaxCues.h"
#include "matroska/KaxCuesData.h"
#include "matroska/KaxInfo.h"
#include "matroska/KaxInfoData.h"
#include "matroska/KaxSeekHead.h"
#include "matroska/KaxSegment.h"
#include "matroska/KaxTag.h"
#include "matroska/KaxTags.h"
#include "matroska/KaxTagMulti.h"
#include "matroska/KaxTracks.h"
#include "matroska/KaxTrackAudio.h"
#include "matroska/KaxTrackVideo.h"
#include "matroska/KaxTrackEntryData.h"
#include "matroska/KaxContentEncoding.h"

#include "ebml/StdIOCallback.h"

extern "C" {
   #include "mp4/libmp4.h"
}
#ifdef HAVE_ZLIB_H
#   include <zlib.h>
#endif

#define MATROSKA_COMPRESSION_NONE 0
#define MATROSKA_COMPRESSION_ZLIB 1

/**
 * What's between a directory and a filename?
 */
#if defined( WIN32 )
    #define DIRECTORY_SEPARATOR '\\'
#else
    #define DIRECTORY_SEPARATOR '/'
#endif

using namespace LIBMATROSKA_NAMESPACE;
using namespace std;

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int  Open ( vlc_object_t * );
static void Close( vlc_object_t * );

vlc_module_begin();
    set_shortname( _("Matroska") );
    set_description( _("Matroska stream demuxer" ) );
    set_capability( "demux2", 50 );
    set_callbacks( Open, Close );
    set_category( CAT_INPUT );
    set_subcategory( SUBCAT_INPUT_DEMUX );

    add_bool( "mkv-seek-percent", 1, NULL,
            N_("Seek based on percent not time"),
            N_("Seek based on percent not time"), VLC_TRUE );

    add_shortcut( "mka" );
    add_shortcut( "mkv" );
vlc_module_end();

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
#ifdef HAVE_ZLIB_H
block_t *block_zlib_decompress( vlc_object_t *p_this, block_t *p_in_block ) {
    int result, dstsize, n;
    unsigned char *dst;
    block_t *p_block;
    z_stream d_stream;

    d_stream.zalloc = (alloc_func)0;
    d_stream.zfree = (free_func)0;
    d_stream.opaque = (voidpf)0;
    result = inflateInit(&d_stream);
    if( result != Z_OK )
    {
        msg_Dbg( p_this, "inflateInit() failed. Result: %d", result );
        return NULL;
    }

    d_stream.next_in = (Bytef *)p_in_block->p_buffer;
    d_stream.avail_in = p_in_block->i_buffer;
    n = 0;
    p_block = block_New( p_this, 0 );
    dst = NULL;
    do
    {
        n++;
        p_block = block_Realloc( p_block, 0, n * 1000 );
        dst = (unsigned char *)p_block->p_buffer;
        d_stream.next_out = (Bytef *)&dst[(n - 1) * 1000];
        d_stream.avail_out = 1000;
        result = inflate(&d_stream, Z_NO_FLUSH);
        if( ( result != Z_OK ) && ( result != Z_STREAM_END ) )
        {
            msg_Dbg( p_this, "Zlib decompression failed. Result: %d", result );
            return NULL;
        }
    }
    while( ( d_stream.avail_out == 0 ) && ( d_stream.avail_in != 0 ) &&
           ( result != Z_STREAM_END ) );

    dstsize = d_stream.total_out;
    inflateEnd( &d_stream );

    p_block = block_Realloc( p_block, 0, dstsize );
    p_block->i_buffer = dstsize;
    block_Release( p_in_block );

    return p_block;
}
#endif

/**
 * Helper function to print the mkv parse tree
 */
static void MkvTree( demux_t *p_this, int i_level, char *psz_format, ... )
{
    va_list args;
    if( i_level > 9 )
    {
        msg_Err( p_this, "too deep tree" );
        return;
    }
    va_start( args, psz_format );
    static char *psz_foo = "|   |   |   |   |   |   |   |   |   |";
    char *psz_foo2 = (char*)malloc( ( i_level * 4 + 3 + strlen( psz_format ) ) * sizeof(char) );
    strncpy( psz_foo2, psz_foo, 4 * i_level );
    psz_foo2[ 4 * i_level ] = '+';
    psz_foo2[ 4 * i_level + 1 ] = ' ';
    strcpy( &psz_foo2[ 4 * i_level + 2 ], psz_format );
    __msg_GenericVa( VLC_OBJECT(p_this), VLC_MSG_DBG, "mkv", psz_foo2, args );
    free( psz_foo2 );
    va_end( args );
}
    
/*****************************************************************************
 * Stream managment
 *****************************************************************************/
class vlc_stream_io_callback: public IOCallback
{
  private:
    stream_t       *s;
    vlc_bool_t     mb_eof;

  public:
    vlc_stream_io_callback( stream_t * );

    virtual uint32   read            ( void *p_buffer, size_t i_size);
    virtual void     setFilePointer  ( int64_t i_offset, seek_mode mode = seek_beginning );
    virtual size_t   write           ( const void *p_buffer, size_t i_size);
    virtual uint64   getFilePointer  ( void );
    virtual void     close           ( void );
};

/*****************************************************************************
 * Ebml Stream parser
 *****************************************************************************/
class EbmlParser
{
  public:
    EbmlParser( EbmlStream *es, EbmlElement *el_start );
    ~EbmlParser( void );

    void Up( void );
    void Down( void );
    EbmlElement *Get( void );
    void        Keep( void );

    int GetLevel( void );

  private:
    EbmlStream  *m_es;
    int         mi_level;
    EbmlElement *m_el[10];

    EbmlElement *m_got;

    int         mi_user_level;
    vlc_bool_t  mb_keep;
};


/*****************************************************************************
 * Some functions to manipulate memory
 *****************************************************************************/
#define GetFOURCC( p )  __GetFOURCC( (uint8_t*)p )
static vlc_fourcc_t __GetFOURCC( uint8_t *p )
{
    return VLC_FOURCC( p[0], p[1], p[2], p[3] );
}

/*****************************************************************************
 * definitions of structures and functions used by this plugins
 *****************************************************************************/
typedef struct
{
    vlc_bool_t  b_default;
    vlc_bool_t  b_enabled;
    int         i_number;

    int         i_extra_data;
    uint8_t     *p_extra_data;

    char         *psz_codec;

    uint64_t     i_default_duration;
    float        f_timecodescale;

    /* video */
    es_format_t fmt;
    float       f_fps;
    es_out_id_t *p_es;

    vlc_bool_t      b_inited;
    /* data to be send first */
    int             i_data_init;
    uint8_t         *p_data_init;

    /* hack : it's for seek */
    vlc_bool_t      b_search_keyframe;

    /* informative */
    char         *psz_codec_name;
    char         *psz_codec_settings;
    char         *psz_codec_info_url;
    char         *psz_codec_download_url;
    
    /* encryption/compression */
    int           i_compression_type;

} mkv_track_t;

typedef struct
{
    int     i_track;
    int     i_block_number;

    int64_t i_position;
    int64_t i_time;

    vlc_bool_t b_key;
} mkv_index_t;

class chapter_item_t
{
public:
    chapter_item_t()
    :i_start_time(0)
    ,i_end_time(-1)
    ,i_user_start_time(-1)
    ,i_user_end_time(-1)
    ,i_seekpoint_num(-1)
    ,b_display_seekpoint(true)
    ,psz_parent(NULL)
    {}
    
    int64_t RefreshChapters( bool b_ordered, int64_t i_prev_user_time, input_title_t & title );
    const chapter_item_t * FindTimecode( mtime_t i_timecode ) const;
    
    int64_t                     i_start_time, i_end_time;
    int64_t                     i_user_start_time, i_user_end_time; /* the time in the stream when an edition is ordered */
    std::vector<chapter_item_t> sub_chapters;
    int                         i_seekpoint_num;
    int64_t                     i_uid;
    bool                        b_display_seekpoint;
    std::string                 psz_name;
    chapter_item_t              *psz_parent;
    
    bool operator<( const chapter_item_t & item ) const
    {
        return ( i_user_start_time < item.i_user_start_time || (i_user_start_time == item.i_user_start_time && i_user_end_time < item.i_user_end_time) );
    }

protected:
    bool Enter();
    bool Leave();
};

class chapter_edition_t 
{
public:
    chapter_edition_t()
    :i_uid(-1)
    ,b_ordered(false)
    {}
    
    void RefreshChapters( input_title_t & title );
    double Duration() const;
    const chapter_item_t * FindTimecode( mtime_t i_timecode ) const;
    
    std::vector<chapter_item_t> chapters;
    int64_t                     i_uid;
    bool                        b_ordered;
};

class demux_sys_t;

class matroska_segment_t
{
public:
    matroska_segment_t( demux_sys_t *p_demuxer )
        :segment(NULL)
        ,i_timescale(0)
        ,f_duration(0.0)
        ,i_cues_position(0)
        ,i_chapters_position(0)
        ,i_tags_position(0)
        ,cluster(NULL)
        ,b_cues(false)
        ,i_index(0)
        ,i_index_max(0)
        ,index(NULL)
        ,psz_muxing_application(NULL)
        ,psz_writing_application(NULL)
        ,psz_segment_filename(NULL)
        ,psz_title(NULL)
        ,psz_date_utc(NULL)
        ,i_current_edition(-1)
        ,psz_current_chapter(NULL)
        ,p_sys(p_demuxer)
        ,ep(NULL)
        ,b_preloaded(false)
    {}

    ~matroska_segment_t()
    {
        for( size_t i_track = 0; i_track < tracks.size(); i_track++ )
        {
#define tk  tracks[i_track]
            if( tk->fmt.psz_description )
            {
                free( tk->fmt.psz_description );
            }
            if( tk->psz_codec )
            {
                free( tk->psz_codec );
            }
            if( tk->fmt.psz_language )
            {
                free( tk->fmt.psz_language );
            }
            delete tk;
#undef tk
        }
        
        if( psz_writing_application )
        {
            free( psz_writing_application );
        }
        if( psz_muxing_application )
        {
            free( psz_muxing_application );
        }
        if( psz_segment_filename )
        {
            free( psz_segment_filename );
        }
        if( psz_title )
        {
            free( psz_title );
        }
        if( psz_date_utc )
        {
            free( psz_date_utc );
        }
    
        delete ep;
    }

    KaxSegment              *segment;

    /* time scale */
    uint64_t                i_timescale;

    /* duration of the segment */
    float                   f_duration;

    /* all tracks */
    std::vector<mkv_track_t*> tracks;

    /* from seekhead */
    int64_t                 i_cues_position;
    int64_t                 i_chapters_position;
    int64_t                 i_tags_position;

    KaxCluster              *cluster;
    KaxSegmentUID           segment_uid;
    KaxPrevUID              prev_segment_uid;
    KaxNextUID              next_segment_uid;

    vlc_bool_t              b_cues;
    int                     i_index;
    int                     i_index_max;
    mkv_index_t             *index;

    /* info */
    char                    *psz_muxing_application;
    char                    *psz_writing_application;
    char                    *psz_segment_filename;
    char                    *psz_title;
    char                    *psz_date_utc;

    std::vector<chapter_edition_t> editions;
    int                            i_current_edition;
    const chapter_item_t           *psz_current_chapter;

    std::vector<KaxSegmentFamily>   families;
    
    demux_sys_t                      *p_sys;
    EbmlParser                       *ep;
    bool                             b_preloaded;

    inline chapter_edition_t *Edition()
    {
        if ( i_current_edition >= 0 && size_t(i_current_edition) < editions.size() )
            return &editions[i_current_edition];
        return NULL;
    }

    bool Preload( demux_t *p_demux );
    bool PreloadFamily( demux_t *p_demux, const matroska_segment_t & segment );
};

class matroska_stream_t
{
public:
    matroska_stream_t( demux_sys_t *p_demuxer )
        :in(NULL)
        ,es(NULL)
        ,i_current_segment(-1)
        ,p_sys(p_demuxer)
    {}

    ~matroska_stream_t()
    {
        for ( size_t i=0; i<segments.size(); i++ )
            delete segments[i];
        delete in;
        delete es;
    }

    vlc_stream_io_callback  *in;
    EbmlStream              *es;

    std::vector<matroska_segment_t*> segments;
    int                              i_current_segment;

    demux_sys_t                      *p_sys;
    
    inline matroska_segment_t *Segment()
    {
        if ( i_current_segment >= 0 && size_t(i_current_segment) < segments.size() )
            return segments[i_current_segment];
        return NULL;
    }
    
    matroska_segment_t *FindSegment( KaxSegmentUID & i_uid ) const;

    void PreloadFamily( demux_t *p_demux, const matroska_segment_t & segment );
};

class demux_sys_t
{
public:
    demux_sys_t()
        :i_pts(0)
        ,i_start_pts(0)
        ,i_chapter_time(0)
        ,meta(NULL)
        ,title(NULL)
        ,i_current_stream(-1)
    {}

    ~demux_sys_t()
    {
        for (size_t i=0; i<streams.size(); i++)
            delete streams[i];
    }

    /* current data */
    mtime_t                 i_pts;
    mtime_t                 i_start_pts;
    mtime_t                 i_chapter_time;

    vlc_meta_t              *meta;

    input_title_t           *title;

    std::vector<matroska_stream_t*> streams;
    int                             i_current_stream;

    inline matroska_stream_t *Stream()
    {
        if ( i_current_stream >= 0 && size_t(i_current_stream) < streams.size() )
            return streams[i_current_stream];
        return NULL;
    }

    matroska_segment_t *FindSegment( KaxSegmentUID & i_uid ) const;
    void PreloadFamily( demux_t *p_demux );
    void PreloadLinked( demux_t *p_demux );
};

static int  Demux  ( demux_t * );
static int  Control( demux_t *, int, va_list );
static void Seek   ( demux_t *, mtime_t i_date, double f_percent, const chapter_item_t *psz_chapter );

#define MKVD_TIMECODESCALE 1000000

#define MKV_IS_ID( el, C ) ( EbmlId( (*el) ) == C::ClassInfos.GlobalId )

static void IndexAppendCluster  ( demux_t *p_demux, KaxCluster *cluster );
static char *UTF8ToStr          ( const UTFstring &u );
static void LoadCues            ( demux_t * );
static void InformationCreate  ( demux_t * );

static void ParseInfo( demux_t *, EbmlElement *info );
static void ParseTracks( demux_t *, EbmlElement *tracks );
static void ParseSeekHead( demux_t *, EbmlElement *seekhead );
static void ParseChapters( demux_t *, EbmlElement *chapters );

/*****************************************************************************
 * Open: initializes matroska demux structures
 *****************************************************************************/
static int Open( vlc_object_t * p_this )
{
    demux_t            *p_demux = (demux_t*)p_this;
    demux_sys_t        *p_sys;
    matroska_stream_t  *p_stream;
    matroska_segment_t *p_segment;
    mkv_track_t        *p_track;
    uint8_t            *p_peek;
    std::string        s_path, s_filename;
    int                i_upper_lvl;
    size_t             i;
    size_t             i_track;

    EbmlElement *el = NULL;

    /* peek the begining */
    if( stream_Peek( p_demux->s, &p_peek, 4 ) < 4 ) return VLC_EGENERIC;

    /* is a valid file */
    if( p_peek[0] != 0x1a || p_peek[1] != 0x45 ||
        p_peek[2] != 0xdf || p_peek[3] != 0xa3 ) return VLC_EGENERIC;

    /* Set the demux function */
    p_demux->pf_demux   = Demux;
    p_demux->pf_control = Control;
    p_demux->p_sys      = p_sys = new demux_sys_t();

    p_stream = new matroska_stream_t( p_sys );
    p_segment = new matroska_segment_t( p_sys );

    p_sys->streams.push_back( p_stream );
    p_sys->i_current_stream = 0;

    p_stream->segments.push_back( p_segment );
    p_stream->i_current_segment = 0;

    p_stream->in = new vlc_stream_io_callback( p_demux->s );
    p_stream->es = new EbmlStream( *p_stream->in );
    p_segment->f_duration   = -1;
    p_segment->i_timescale     = MKVD_TIMECODESCALE;
    p_track = new mkv_track_t(); 
    p_segment->tracks.push_back( p_track );
    p_sys->i_pts   = 0;
    p_segment->i_cues_position = -1;
    p_segment->i_chapters_position = -1;
    p_segment->i_tags_position = -1;

    p_segment->b_cues       = VLC_FALSE;
    p_segment->i_index      = 0;
    p_segment->i_index_max  = 1024;
    p_segment->index        = (mkv_index_t*)malloc( sizeof( mkv_index_t ) *
                                                p_segment->i_index_max );

    p_sys->meta = NULL;
    p_sys->title = NULL;

    if( p_stream->es == NULL )
    {
        msg_Err( p_demux, "failed to create EbmlStream" );
        delete p_sys;
        return VLC_EGENERIC;
    }
    /* Find the EbmlHead element */
    el = p_stream->es->FindNextID(EbmlHead::ClassInfos, 0xFFFFFFFFL);
    if( el == NULL )
    {
        msg_Err( p_demux, "cannot find EbmlHead" );
        goto error;
    }
    msg_Dbg( p_demux, "EbmlHead" );
    /* skip it */
    el->SkipData( *p_stream->es, el->Generic().Context );
    delete el;

    /* Find a segment */
    el = p_stream->es->FindNextID( KaxSegment::ClassInfos, 0xFFFFFFFFL);
    if( el == NULL )
    {
        msg_Err( p_demux, "cannot find KaxSegment" );
        goto error;
    }
    MkvTree( p_demux, 0, "Segment" );
    p_segment->segment = (KaxSegment*)el;
    p_segment->cluster = NULL;

    p_segment->ep = new EbmlParser( p_stream->es, el );

    p_segment->Preload( p_demux );

    /* get the files from the same dir from the same family (based on p_demux->psz_path) */
    /* _todo_ handle multi-segment files */
    if (p_demux->psz_path[0] != '\0' && !strcmp(p_demux->psz_access, ""))
    {
        // assume it's a regular file
        // get the directory path
        s_path = p_demux->psz_path;
        if (s_path.at(s_path.length() - 1) == DIRECTORY_SEPARATOR)
        {
            s_path = s_path.substr(0,s_path.length()-1);
        }
        else
        {
            if (s_path.find_last_of(DIRECTORY_SEPARATOR) > 0) 
            {
                s_path = s_path.substr(0,s_path.find_last_of(DIRECTORY_SEPARATOR));
            }
        }

        struct dirent *p_file_item;
        DIR *p_src_dir = opendir(s_path.c_str());

        if (p_src_dir != NULL)
        {
            while ((p_file_item = (dirent *) readdir(p_src_dir)))
            {
                if (strlen(p_file_item->d_name) > 4)
                {
                    s_filename = s_path + DIRECTORY_SEPARATOR + p_file_item->d_name;

                    if (!s_filename.compare(p_demux->psz_path))
                        continue;

#if defined(__GNUC__) && (__GNUC__ < 3)
                    if (!s_filename.compare("mkv", s_filename.length() - 3, 3) || 
                        !s_filename.compare("mka", s_filename.length() - 3, 3))
#else
                    if (!s_filename.compare(s_filename.length() - 3, 3, "mkv") || 
                        !s_filename.compare(s_filename.length() - 3, 3, "mka"))
#endif
                    {
                        // test wether this file belongs to the our family
                        bool b_keep_file_opened = false;
                        StdIOCallback *p_file_io = new StdIOCallback(s_filename.c_str(), MODE_READ);
                        EbmlStream *p_estream = new EbmlStream(*p_file_io);
                        EbmlElement *p_l0, *p_l1, *p_l2;

                        // verify the EBML Header
                        p_l0 = p_estream->FindNextID(EbmlHead::ClassInfos, 0xFFFFFFFFL);
                        if (p_l0 == NULL)
                        {
                            delete p_estream;
                            delete p_file_io;
                            continue;
                        }

                        matroska_stream_t  *p_stream1 = new matroska_stream_t( p_sys );
                        p_sys->streams.push_back( p_stream1 );

                        p_l0->SkipData(*p_estream, EbmlHead_Context);
                        delete p_l0;

                        // find all segments in this file
                        p_l0 = p_estream->FindNextID(KaxSegment::ClassInfos, 0xFFFFFFFFL);
                        if (p_l0 == NULL)
                        {
                            delete p_estream;
                            delete p_file_io;
                            continue;
                        }

                        i_upper_lvl = 0;

                        while (p_l0 != 0)
                        {
                            if (EbmlId(*p_l0) == KaxSegment::ClassInfos.GlobalId)
                            {
                                EbmlParser  *ep;
                                matroska_segment_t *p_segment1 = new matroska_segment_t( p_sys );

                                p_stream1->segments.push_back( p_segment1 );

                                ep = new EbmlParser(p_estream, p_l0);
                                p_segment1->ep = ep;

                                bool b_this_segment_matches = false;
                                while ((p_l1 = ep->Get()))
                                {
                                    if (MKV_IS_ID(p_l1, KaxInfo))
                                    {
                                        // find the families of this segment
                                        KaxInfo *p_info = static_cast<KaxInfo*>(p_l1);

                                        p_info->Read(*p_estream, KaxInfo::ClassInfos.Context, i_upper_lvl, p_l2, true);
                                        for( i = 0; i < p_info->ListSize() && !b_this_segment_matches; i++ )
                                        {
                                            EbmlElement *l = (*p_info)[i];

                                            if( MKV_IS_ID( l, KaxSegmentUID ) )
                                            {
                                                KaxSegmentUID *p_uid = static_cast<KaxSegmentUID*>(l);
                                                if (p_segment->segment_uid == *p_uid)
                                                    break;
                                                p_segment1->segment_uid = *( new KaxSegmentUID(*p_uid) );
                                            }
                                            else if( MKV_IS_ID( l, KaxPrevUID ) )
                                            {
                                                p_segment1->prev_segment_uid = *( new KaxPrevUID( *static_cast<KaxPrevUID*>(l) ) );
                                            }
                                            else if( MKV_IS_ID( l, KaxNextUID ) )
                                            {
                                                p_segment1->next_segment_uid = *( new KaxNextUID( *static_cast<KaxNextUID*>(l) ) );
                                            }
                                            else if( MKV_IS_ID( l, KaxSegmentFamily ) )
                                            {
                                                KaxSegmentFamily *p_fam = new KaxSegmentFamily( *static_cast<KaxSegmentFamily*>(l) );
                                                std::vector<KaxSegmentFamily>::iterator iter;
                                                p_segment1->families.push_back( *p_fam );
/*                                                for( iter = p_segment->families.begin();
                                                     iter != p_segment->families.end();
                                                     iter++ )
                                                {
                                                    if( *iter == *p_fam )
                                                    {
                                                        b_this_segment_matches = true;
                                                        p_segment1->families.push_back( *p_fam );
                                                        break;
                                                    }
                                                }*/
                                            }
                                        }
                                        break;
                                    }
                                }

                                if (b_this_segment_matches)
                                {
                                    b_keep_file_opened = true;
                                }
                            }

                            p_l0->SkipData(*p_estream, EbmlHead_Context);
                            delete p_l0;
                            p_l0 = p_estream->FindNextID(KaxSegment::ClassInfos, 0xFFFFFFFFL);
                        }

                        if (!b_keep_file_opened)
                        {
                            delete p_estream;
                            delete p_file_io;
                        }
                    }
                }
            }
            closedir( p_src_dir );
        }
    }

    if( p_segment->cluster == NULL )
    {
        msg_Err( p_demux, "cannot find any cluster, damaged file ?" );
        goto error;
    }

    p_sys->PreloadFamily( p_demux );
    p_sys->PreloadLinked( p_demux );

    /* *** Load the cue if found *** */
    if( p_segment->i_cues_position >= 0 )
    {
        vlc_bool_t b_seekable;

        stream_Control( p_demux->s, STREAM_CAN_FASTSEEK, &b_seekable );
        if( b_seekable )
        {
            LoadCues( p_demux );
        }
    }

    if( !p_segment->b_cues || p_segment->i_index <= 0 )
    {
        msg_Warn( p_demux, "no cues/empty cues found->seek won't be precise" );

        IndexAppendCluster( p_demux, p_segment->cluster );

        p_segment->b_cues = VLC_FALSE;
    }

    /* add all es */
    msg_Dbg( p_demux, "found %d es", p_segment->tracks.size() );
    for( i_track = 0; i_track < p_segment->tracks.size(); i_track++ )
    {
#define tk  p_segment->tracks[i_track]
        if( tk->fmt.i_cat == UNKNOWN_ES )
        {
            msg_Warn( p_demux, "invalid track[%d, n=%d]", i_track, tk->i_number );
            tk->p_es = NULL;
            continue;
        }

        if( !strcmp( tk->psz_codec, "V_MS/VFW/FOURCC" ) )
        {
            if( tk->i_extra_data < (int)sizeof( BITMAPINFOHEADER ) )
            {
                msg_Err( p_demux, "missing/invalid BITMAPINFOHEADER" );
                tk->fmt.i_codec = VLC_FOURCC( 'u', 'n', 'd', 'f' );
            }
            else
            {
                BITMAPINFOHEADER *p_bih = (BITMAPINFOHEADER*)tk->p_extra_data;

                tk->fmt.video.i_width = GetDWLE( &p_bih->biWidth );
                tk->fmt.video.i_height= GetDWLE( &p_bih->biHeight );
                tk->fmt.i_codec       = GetFOURCC( &p_bih->biCompression );

                tk->fmt.i_extra       = GetDWLE( &p_bih->biSize ) - sizeof( BITMAPINFOHEADER );
                if( tk->fmt.i_extra > 0 )
                {
                    tk->fmt.p_extra = malloc( tk->fmt.i_extra );
                    memcpy( tk->fmt.p_extra, &p_bih[1], tk->fmt.i_extra );
                }
            }
        }
        else if( !strcmp( tk->psz_codec, "V_MPEG1" ) ||
                 !strcmp( tk->psz_codec, "V_MPEG2" ) )
        {
            tk->fmt.i_codec = VLC_FOURCC( 'm', 'p', 'g', 'v' );
        }
        else if( !strncmp( tk->psz_codec, "V_MPEG4", 7 ) )
        {
            if( !strcmp( tk->psz_codec, "V_MPEG4/MS/V3" ) )
            {
                tk->fmt.i_codec = VLC_FOURCC( 'D', 'I', 'V', '3' );
            }
            else if( !strcmp( tk->psz_codec, "V_MPEG4/ISO/AVC" ) )
            {
                tk->fmt.i_codec = VLC_FOURCC( 'a', 'v', 'c', '1' );
                tk->fmt.b_packetized = VLC_FALSE;
                tk->fmt.i_extra = tk->i_extra_data;
                tk->fmt.p_extra = malloc( tk->i_extra_data );
                memcpy( tk->fmt.p_extra,tk->p_extra_data, tk->i_extra_data );
            }
            else
            {
                tk->fmt.i_codec = VLC_FOURCC( 'm', 'p', '4', 'v' );
            }
        }
        else if( !strcmp( tk->psz_codec, "V_QUICKTIME" ) )
        {
            MP4_Box_t *p_box = (MP4_Box_t*)malloc( sizeof( MP4_Box_t ) );
            stream_t *p_mp4_stream = stream_MemoryNew( VLC_OBJECT(p_demux),
                                                       tk->p_extra_data,
                                                       tk->i_extra_data );
            MP4_ReadBoxCommon( p_mp4_stream, p_box );
            MP4_ReadBox_sample_vide( p_mp4_stream, p_box );
            tk->fmt.i_codec = p_box->i_type;
            tk->fmt.video.i_width = p_box->data.p_sample_vide->i_width;
            tk->fmt.video.i_height = p_box->data.p_sample_vide->i_height;
            tk->fmt.i_extra = p_box->data.p_sample_vide->i_qt_image_description;
            tk->fmt.p_extra = malloc( tk->fmt.i_extra );
            memcpy( tk->fmt.p_extra, p_box->data.p_sample_vide->p_qt_image_description, tk->fmt.i_extra );
            MP4_FreeBox_sample_vide( p_box );
            stream_MemoryDelete( p_mp4_stream, VLC_TRUE );
        }
        else if( !strcmp( tk->psz_codec, "A_MS/ACM" ) )
        {
            if( tk->i_extra_data < (int)sizeof( WAVEFORMATEX ) )
            {
                msg_Err( p_demux, "missing/invalid WAVEFORMATEX" );
                tk->fmt.i_codec = VLC_FOURCC( 'u', 'n', 'd', 'f' );
            }
            else
            {
                WAVEFORMATEX *p_wf = (WAVEFORMATEX*)tk->p_extra_data;

                wf_tag_to_fourcc( GetWLE( &p_wf->wFormatTag ), &tk->fmt.i_codec, NULL );

                tk->fmt.audio.i_channels   = GetWLE( &p_wf->nChannels );
                tk->fmt.audio.i_rate = GetDWLE( &p_wf->nSamplesPerSec );
                tk->fmt.i_bitrate    = GetDWLE( &p_wf->nAvgBytesPerSec ) * 8;
                tk->fmt.audio.i_blockalign = GetWLE( &p_wf->nBlockAlign );;
                tk->fmt.audio.i_bitspersample = GetWLE( &p_wf->wBitsPerSample );

                tk->fmt.i_extra            = GetWLE( &p_wf->cbSize );
                if( tk->fmt.i_extra > 0 )
                {
                    tk->fmt.p_extra = malloc( tk->fmt.i_extra );
                    memcpy( tk->fmt.p_extra, &p_wf[1], tk->fmt.i_extra );
                }
            }
        }
        else if( !strcmp( tk->psz_codec, "A_MPEG/L3" ) ||
                 !strcmp( tk->psz_codec, "A_MPEG/L2" ) ||
                 !strcmp( tk->psz_codec, "A_MPEG/L1" ) )
        {
            tk->fmt.i_codec = VLC_FOURCC( 'm', 'p', 'g', 'a' );
        }
        else if( !strcmp( tk->psz_codec, "A_AC3" ) )
        {
            tk->fmt.i_codec = VLC_FOURCC( 'a', '5', '2', ' ' );
        }
        else if( !strcmp( tk->psz_codec, "A_DTS" ) )
        {
            tk->fmt.i_codec = VLC_FOURCC( 'd', 't', 's', ' ' );
        }
        else if( !strcmp( tk->psz_codec, "A_FLAC" ) )
        {
            tk->fmt.i_codec = VLC_FOURCC( 'f', 'l', 'a', 'c' );
            tk->fmt.i_extra = tk->i_extra_data;
            tk->fmt.p_extra = malloc( tk->i_extra_data );
            memcpy( tk->fmt.p_extra,tk->p_extra_data, tk->i_extra_data );
        }
        else if( !strcmp( tk->psz_codec, "A_VORBIS" ) )
        {
            int i, i_offset = 1, i_size[3], i_extra;
            uint8_t *p_extra;

            tk->fmt.i_codec = VLC_FOURCC( 'v', 'o', 'r', 'b' );

            /* Split the 3 headers */
            if( tk->p_extra_data[0] != 0x02 )
                msg_Err( p_demux, "invalid vorbis header" );

            for( i = 0; i < 2; i++ )
            {
                i_size[i] = 0;
                while( i_offset < tk->i_extra_data )
                {
                    i_size[i] += tk->p_extra_data[i_offset];
                    if( tk->p_extra_data[i_offset++] != 0xff ) break;
                }
            }

            i_size[0] = __MIN(i_size[0], tk->i_extra_data - i_offset);
            i_size[1] = __MIN(i_size[1], tk->i_extra_data -i_offset -i_size[0]);
            i_size[2] = tk->i_extra_data - i_offset - i_size[0] - i_size[1];

            tk->fmt.i_extra = 3 * 2 + i_size[0] + i_size[1] + i_size[2];
            tk->fmt.p_extra = malloc( tk->fmt.i_extra );
            p_extra = (uint8_t *)tk->fmt.p_extra; i_extra = 0;
            for( i = 0; i < 3; i++ )
            {
                *(p_extra++) = i_size[i] >> 8;
                *(p_extra++) = i_size[i] & 0xFF;
                memcpy( p_extra, tk->p_extra_data + i_offset + i_extra,
                        i_size[i] );
                p_extra += i_size[i];
                i_extra += i_size[i];
            }
        }
        else if( !strncmp( tk->psz_codec, "A_AAC/MPEG2/", strlen( "A_AAC/MPEG2/" ) ) ||
                 !strncmp( tk->psz_codec, "A_AAC/MPEG4/", strlen( "A_AAC/MPEG4/" ) ) )
        {
            int i_profile, i_srate;
            static unsigned int i_sample_rates[] =
            {
                    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
                        16000, 12000, 11025, 8000,  7350,  0,     0,     0
            };

            tk->fmt.i_codec = VLC_FOURCC( 'm', 'p', '4', 'a' );
            /* create data for faad (MP4DecSpecificDescrTag)*/

            if( !strcmp( &tk->psz_codec[12], "MAIN" ) )
            {
                i_profile = 0;
            }
            else if( !strcmp( &tk->psz_codec[12], "LC" ) )
            {
                i_profile = 1;
            }
            else if( !strcmp( &tk->psz_codec[12], "SSR" ) )
            {
                i_profile = 2;
            }
            else
            {
                i_profile = 3;
            }

            for( i_srate = 0; i_srate < 13; i_srate++ )
            {
                if( i_sample_rates[i_srate] == tk->fmt.audio.i_rate )
                {
                    break;
                }
            }
            msg_Dbg( p_demux, "profile=%d srate=%d", i_profile, i_srate );

            tk->fmt.i_extra = 2;
            tk->fmt.p_extra = malloc( tk->fmt.i_extra );
            ((uint8_t*)tk->fmt.p_extra)[0] = ((i_profile + 1) << 3) | ((i_srate&0xe) >> 1);
            ((uint8_t*)tk->fmt.p_extra)[1] = ((i_srate & 0x1) << 7) | (tk->fmt.audio.i_channels << 3);
        }
        else if( !strcmp( tk->psz_codec, "A_PCM/INT/BIG" ) ||
                 !strcmp( tk->psz_codec, "A_PCM/INT/LIT" ) ||
                 !strcmp( tk->psz_codec, "A_PCM/FLOAT/IEEE" ) )
        {
            if( !strcmp( tk->psz_codec, "A_PCM/INT/BIG" ) )
            {
                tk->fmt.i_codec = VLC_FOURCC( 't', 'w', 'o', 's' );
            }
            else
            {
                tk->fmt.i_codec = VLC_FOURCC( 'a', 'r', 'a', 'w' );
            }
            tk->fmt.audio.i_blockalign = ( tk->fmt.audio.i_bitspersample + 7 ) / 8 * tk->fmt.audio.i_channels;
        }
        else if( !strcmp( tk->psz_codec, "A_TTA1" ) )
        {
            /* FIXME: support this codec */
            msg_Err( p_demux, "TTA not supported yet[%d, n=%d]", i_track, tk->i_number );
            tk->fmt.i_codec = VLC_FOURCC( 'u', 'n', 'd', 'f' );
        }
        else if( !strcmp( tk->psz_codec, "A_WAVPACK4" ) )
        {
            /* FIXME: support this codec */
            msg_Err( p_demux, "Wavpack not supported yet[%d, n=%d]", i_track, tk->i_number );
            tk->fmt.i_codec = VLC_FOURCC( 'u', 'n', 'd', 'f' );
        }
        else if( !strcmp( tk->psz_codec, "S_TEXT/UTF8" ) )
        {
            tk->fmt.i_codec = VLC_FOURCC( 's', 'u', 'b', 't' );
            tk->fmt.subs.psz_encoding = strdup( "UTF-8" );
        }
        else if( !strcmp( tk->psz_codec, "S_TEXT/SSA" ) ||
                 !strcmp( tk->psz_codec, "S_TEXT/ASS" ) ||
                 !strcmp( tk->psz_codec, "S_SSA" ) ||
                 !strcmp( tk->psz_codec, "S_ASS" ))
        {
            tk->fmt.i_codec = VLC_FOURCC( 's', 's', 'a', ' ' );
            tk->fmt.subs.psz_encoding = strdup( "UTF-8" );
        }
        else if( !strcmp( tk->psz_codec, "S_VOBSUB" ) )
        {
            tk->fmt.i_codec = VLC_FOURCC( 's','p','u',' ' );
            if( tk->i_extra_data )
            {
                char *p_start;
                char *p_buf = (char *)malloc( tk->i_extra_data + 1);
                memcpy( p_buf, tk->p_extra_data , tk->i_extra_data );
                p_buf[tk->i_extra_data] = '\0';
                
                p_start = strstr( p_buf, "size:" );
                if( sscanf( p_start, "size: %dx%d",
                        &tk->fmt.subs.spu.i_original_frame_width, &tk->fmt.subs.spu.i_original_frame_height ) == 2 )
                {
                    msg_Dbg( p_demux, "original frame size vobsubs: %dx%d", tk->fmt.subs.spu.i_original_frame_width, tk->fmt.subs.spu.i_original_frame_height );
                }
                else
                {
                    msg_Warn( p_demux, "reading original frame size for vobsub failed" );
                }
                free( p_buf );
            }
        }
        else if( !strcmp( tk->psz_codec, "B_VOBBTN" ) )
        {
            /* FIXME: support this codec */
            msg_Err( p_demux, "Vob Buttons not supported yet[%d, n=%d]", i_track, tk->i_number );
            tk->fmt.i_codec = VLC_FOURCC( 'u', 'n', 'd', 'f' );
        }
        else
        {
            msg_Err( p_demux, "unknow codec id=`%s'", tk->psz_codec );
            tk->fmt.i_codec = VLC_FOURCC( 'u', 'n', 'd', 'f' );
        }
        if( tk->b_default )
        {
            tk->fmt.i_priority = 1000;
        }

        tk->p_es = es_out_Add( p_demux->out, &tk->fmt );
#undef tk
    }

    /* add information */
    InformationCreate( p_demux );

    return VLC_SUCCESS;

error:
    delete p_sys;
    return VLC_EGENERIC;
}

/*****************************************************************************
 * Close: frees unused data
 *****************************************************************************/
static void Close( vlc_object_t *p_this )
{
    demux_t     *p_demux = (demux_t*)p_this;
    demux_sys_t *p_sys   = p_demux->p_sys;
    matroska_stream_t  *p_stream = p_sys->Stream();
    matroska_segment_t *p_segment = p_stream->Segment();

    /* TODO close everything ? */
    
    delete p_segment->segment;

    delete p_sys;
}

/*****************************************************************************
 * Control:
 *****************************************************************************/
static int Control( demux_t *p_demux, int i_query, va_list args )
{
    demux_sys_t        *p_sys = p_demux->p_sys;
    matroska_stream_t  *p_stream = p_sys->Stream();
    matroska_segment_t *p_segment = p_stream->Segment();
    int64_t     *pi64;
    double      *pf, f;
    int         i_skp;

    vlc_meta_t **pp_meta;

    switch( i_query )
    {
        case DEMUX_GET_META:
            pp_meta = (vlc_meta_t**)va_arg( args, vlc_meta_t** );
            *pp_meta = vlc_meta_Duplicate( p_sys->meta );
            return VLC_SUCCESS;

        case DEMUX_GET_LENGTH:
            pi64 = (int64_t*)va_arg( args, int64_t * );
            if( p_segment->f_duration > 0.0 )
            {
                *pi64 = (int64_t)(p_segment->f_duration * 1000);
                return VLC_SUCCESS;
            }
            return VLC_EGENERIC;

        case DEMUX_GET_POSITION:
            pf = (double*)va_arg( args, double * );
            *pf = (double)p_sys->i_pts / (1000.0 * p_segment->f_duration);
            return VLC_SUCCESS;

        case DEMUX_SET_POSITION:
            f = (double)va_arg( args, double );
            Seek( p_demux, -1, f, NULL );
            return VLC_SUCCESS;

        case DEMUX_GET_TIME:
            pi64 = (int64_t*)va_arg( args, int64_t * );
            *pi64 = p_sys->i_pts;
            return VLC_SUCCESS;

        case DEMUX_GET_TITLE_INFO:
            if( p_sys->title && p_sys->title->i_seekpoint > 0 )
            {
                input_title_t ***ppp_title = (input_title_t***)va_arg( args, input_title_t*** );
                int *pi_int    = (int*)va_arg( args, int* );

                *pi_int = 1;
                *ppp_title = (input_title_t**)malloc( sizeof( input_title_t**) );

                (*ppp_title)[0] = vlc_input_title_Duplicate( p_sys->title );

                return VLC_SUCCESS;
            }
            return VLC_EGENERIC;

        case DEMUX_SET_TITLE:
            /* TODO handle editions as titles & DVD titles as well */
            if( p_sys->title && p_sys->title->i_seekpoint > 0 )
            {
                return VLC_SUCCESS;
            }
            return VLC_EGENERIC;

        case DEMUX_SET_SEEKPOINT:
            /* FIXME do a better implementation */
            i_skp = (int)va_arg( args, int );

            if( p_sys->title && i_skp < p_sys->title->i_seekpoint)
            {
                Seek( p_demux, (int64_t)p_sys->title->seekpoint[i_skp]->i_time_offset, -1, NULL);
                p_demux->info.i_seekpoint |= INPUT_UPDATE_SEEKPOINT;
                p_demux->info.i_seekpoint = i_skp;
                return VLC_SUCCESS;
            }
            return VLC_EGENERIC;

        case DEMUX_SET_TIME:
        case DEMUX_GET_FPS:
        default:
            return VLC_EGENERIC;
    }
}

static int BlockGet( demux_t *p_demux, KaxBlock **pp_block, int64_t *pi_ref1, int64_t *pi_ref2, int64_t *pi_duration )
{
    demux_sys_t        *p_sys = p_demux->p_sys;
    matroska_stream_t  *p_stream = p_sys->Stream();
    matroska_segment_t *p_segment = p_stream->Segment();

    *pp_block = NULL;
    *pi_ref1  = -1;
    *pi_ref2  = -1;

    for( ;; )
    {
        EbmlElement *el;
        int         i_level;

        if( p_demux->b_die )
        {
            return VLC_EGENERIC;
        }

        el = p_segment->ep->Get();
        i_level = p_segment->ep->GetLevel();

        if( el == NULL && *pp_block != NULL )
        {
            /* update the index */
#define idx p_segment->index[p_segment->i_index - 1]
            if( p_segment->i_index > 0 && idx.i_time == -1 )
            {
                idx.i_time        = (*pp_block)->GlobalTimecode() / (mtime_t)1000;
                idx.b_key         = *pi_ref1 == -1 ? VLC_TRUE : VLC_FALSE;
            }
#undef idx
            return VLC_SUCCESS;
        }

        if( el == NULL )
        {
            if( p_segment->ep->GetLevel() > 1 )
            {
                p_segment->ep->Up();
                continue;
            }
            msg_Warn( p_demux, "EOF" );
            return VLC_EGENERIC;
        }

        /* do parsing */
        if( i_level == 1 )
        {
            if( MKV_IS_ID( el, KaxCluster ) )
            {
                p_segment->cluster = (KaxCluster*)el;

                /* add it to the index */
                if( p_segment->i_index == 0 ||
                    ( p_segment->i_index > 0 && p_segment->index[p_segment->i_index - 1].i_position < (int64_t)p_segment->cluster->GetElementPosition() ) )
                {
                    IndexAppendCluster( p_demux, p_segment->cluster );
                }

                p_segment->ep->Down();
            }
            else if( MKV_IS_ID( el, KaxCues ) )
            {
                msg_Warn( p_demux, "find KaxCues FIXME" );
                return VLC_EGENERIC;
            }
            else
            {
                msg_Dbg( p_demux, "unknown (%s)", typeid( el ).name() );
            }
        }
        else if( i_level == 2 )
        {
            if( MKV_IS_ID( el, KaxClusterTimecode ) )
            {
                KaxClusterTimecode &ctc = *(KaxClusterTimecode*)el;

                ctc.ReadData( p_stream->es->I_O(), SCOPE_ALL_DATA );
                p_segment->cluster->InitTimecode( uint64( ctc ), p_segment->i_timescale );
            }
            else if( MKV_IS_ID( el, KaxBlockGroup ) )
            {
                p_segment->ep->Down();
            }
        }
        else if( i_level == 3 )
        {
            if( MKV_IS_ID( el, KaxBlock ) )
            {
                *pp_block = (KaxBlock*)el;

                (*pp_block)->ReadData( p_stream->es->I_O() );
                (*pp_block)->SetParent( *p_segment->cluster );

                p_segment->ep->Keep();
            }
            else if( MKV_IS_ID( el, KaxBlockDuration ) )
            {
                KaxBlockDuration &dur = *(KaxBlockDuration*)el;

                dur.ReadData( p_stream->es->I_O() );
                *pi_duration = uint64( dur );
            }
            else if( MKV_IS_ID( el, KaxReferenceBlock ) )
            {
                KaxReferenceBlock &ref = *(KaxReferenceBlock*)el;

                ref.ReadData( p_stream->es->I_O() );
                if( *pi_ref1 == -1 )
                {
                    *pi_ref1 = int64( ref );
                }
                else
                {
                    *pi_ref2 = int64( ref );
                }
            }
        }
        else
        {
            msg_Err( p_demux, "invalid level = %d", i_level );
            return VLC_EGENERIC;
        }
    }
}

static block_t *MemToBlock( demux_t *p_demux, uint8_t *p_mem, int i_mem)
{
    block_t *p_block;
    if( !(p_block = block_New( p_demux, i_mem ) ) ) return NULL;
    memcpy( p_block->p_buffer, p_mem, i_mem );
    //p_block->i_rate = p_input->stream.control.i_rate;
    return p_block;
}

static void BlockDecode( demux_t *p_demux, KaxBlock *block, mtime_t i_pts,
                         mtime_t i_duration )
{
    demux_sys_t        *p_sys = p_demux->p_sys;
    matroska_stream_t  *p_stream = p_sys->Stream();
    matroska_segment_t *p_segment = p_stream->Segment();

    size_t          i_track;
    unsigned int    i;
    vlc_bool_t      b;

#define tk  p_segment->tracks[i_track]
    for( i_track = 0; i_track < p_segment->tracks.size(); i_track++ )
    {
        if( tk->i_number == block->TrackNum() )
        {
            break;
        }
    }

    if( i_track >= p_segment->tracks.size() )
    {
        msg_Err( p_demux, "invalid track number=%d", block->TrackNum() );
        return;
    }
    if( tk->p_es == NULL )
    {
        msg_Err( p_demux, "unknown track number=%d", block->TrackNum() );
        return;
    }
    if( i_pts < p_sys->i_start_pts && tk->fmt.i_cat == AUDIO_ES )
    {
        return; /* discard audio packets that shouldn't be rendered */
    }

    es_out_Control( p_demux->out, ES_OUT_GET_ES_STATE, tk->p_es, &b );
    if( !b )
    {
        tk->b_inited = VLC_FALSE;
        return;
    }

    /* First send init data */
    if( !tk->b_inited && tk->i_data_init > 0 )
    {
        block_t *p_init;

        msg_Dbg( p_demux, "sending header (%d bytes)", tk->i_data_init );
        p_init = MemToBlock( p_demux, tk->p_data_init, tk->i_data_init );
        if( p_init ) es_out_Send( p_demux->out, tk->p_es, p_init );
    }
    tk->b_inited = VLC_TRUE;


    for( i = 0; i < block->NumberFrames(); i++ )
    {
        block_t *p_block;
        DataBuffer &data = block->GetBuffer(i);

        p_block = MemToBlock( p_demux, data.Buffer(), data.Size() );

        if( p_block == NULL )
        {
            break;
        }

#if defined(HAVE_ZLIB_H)
        if( tk->i_compression_type )
        {
            p_block = block_zlib_decompress( VLC_OBJECT(p_demux), p_block );
        }
#endif

        // TODO implement correct timestamping when B frames are used
        if( tk->fmt.i_cat != VIDEO_ES )
        {
            p_block->i_dts = p_block->i_pts = i_pts;
        }
        else
        {
            p_block->i_dts = i_pts;
            p_block->i_pts = 0;
        }

        if( tk->fmt.i_cat == SPU_ES && strcmp( tk->psz_codec, "S_VOBSUB" ) )
        {
            p_block->i_length = i_duration * 1000;
        }
        es_out_Send( p_demux->out, tk->p_es, p_block );

        /* use time stamp only for first block */
        i_pts = 0;
    }

#undef tk
}

static void UpdateCurrentToChapter( demux_t & demux )
{
    demux_sys_t & sys = *demux.p_sys;
    matroska_stream_t  *p_stream = sys.Stream();
    matroska_segment_t *p_segment = p_stream->Segment();
    const chapter_item_t *psz_curr_chapter;

    /* update current chapter/seekpoint */
    if ( p_segment->editions.size())
    {
        /* 1st, we need to know in which chapter we are */
        psz_curr_chapter = p_segment->editions[p_segment->i_current_edition].FindTimecode( sys.i_pts );

        /* we have moved to a new chapter */
        if (p_segment->psz_current_chapter != NULL && psz_curr_chapter != NULL && p_segment->psz_current_chapter != psz_curr_chapter)
        {
            if (p_segment->psz_current_chapter->i_seekpoint_num != psz_curr_chapter->i_seekpoint_num && psz_curr_chapter->i_seekpoint_num > 0)
            {
                demux.info.i_update |= INPUT_UPDATE_SEEKPOINT;
                demux.info.i_seekpoint = psz_curr_chapter->i_seekpoint_num - 1;
            }

            if (p_segment->editions[p_segment->i_current_edition].b_ordered )
            {
                /* TODO check if we need to silently seek to a new location in the stream (switch to another chapter) */
                if (p_segment->psz_current_chapter->i_end_time != psz_curr_chapter->i_start_time)
                    Seek(&demux, sys.i_pts, -1, psz_curr_chapter);
                /* count the last duration time found for each track in a table (-1 not found, -2 silent) */
                /* only seek after each duration >= end timecode of the current chapter */
            }

//            p_segment->i_user_time = psz_curr_chapter->i_user_start_time - psz_curr_chapter->i_start_time;
//            p_segment->i_start_pts = psz_curr_chapter->i_user_start_time;
        }
        p_segment->psz_current_chapter = psz_curr_chapter;
    }
}

static void Seek( demux_t *p_demux, mtime_t i_date, double f_percent, const chapter_item_t *psz_chapter)
{
    demux_sys_t        *p_sys = p_demux->p_sys;
    matroska_stream_t  *p_stream = p_sys->Stream();
    matroska_segment_t *p_segment = p_stream->Segment();
    mtime_t            i_time_offset = 0;

    KaxBlock    *block;
    int64_t     i_block_duration;
    int64_t     i_block_ref1;
    int64_t     i_block_ref2;

    int         i_index = 0;
    int         i_track_skipping;
    size_t      i_track;

    msg_Dbg( p_demux, "seek request to "I64Fd" (%f%%)", i_date, f_percent );
    if( i_date < 0 && f_percent < 0 )
    {
        msg_Warn( p_demux, "cannot seek nowhere !" );
        return;
    }
    if( f_percent > 1.0 )
    {
        msg_Warn( p_demux, "cannot seek so far !" );
        return;
    }

    delete p_segment->ep;
    p_segment->ep = new EbmlParser( p_stream->es, p_segment->segment );
    p_segment->cluster = NULL;

    /* seek without index or without date */
    if( f_percent >= 0 && (config_GetInt( p_demux, "mkv-seek-percent" ) || !p_segment->b_cues || i_date < 0 ))
    {
        if (p_segment->f_duration >= 0)
        {
            i_date = int64_t( f_percent * p_segment->f_duration * 1000.0 );
        }
        else
        {
            int64_t i_pos = int64_t( f_percent * stream_Size( p_demux->s ) );

            msg_Dbg( p_demux, "inacurate way of seeking" );
            for( i_index = 0; i_index < p_segment->i_index; i_index++ )
            {
                if( p_segment->index[i_index].i_position >= i_pos)
                {
                    break;
                }
            }
            if( i_index == p_segment->i_index )
            {
                i_index--;
            }

            i_date = p_segment->index[i_index].i_time;

#if 0
            if( p_segment->index[i_index].i_position < i_pos )
            {
                EbmlElement *el;

                msg_Warn( p_demux, "searching for cluster, could take some time" );

                /* search a cluster */
                while( ( el = p_sys->ep->Get() ) != NULL )
                {
                    if( MKV_IS_ID( el, KaxCluster ) )
                    {
                        KaxCluster *cluster = (KaxCluster*)el;

                        /* add it to the index */
                        IndexAppendCluster( p_demux, cluster );

                        if( (int64_t)cluster->GetElementPosition() >= i_pos )
                        {
                            p_sys->cluster = cluster;
                            p_sys->ep->Down();
                            break;
                        }
                    }
                }
            }
#endif
        }
    }

    // find the actual time for an ordered edition
    if ( psz_chapter == NULL )
    {
        if ( p_segment->editions.size() && p_segment->editions[p_segment->i_current_edition].b_ordered )
        {
            /* 1st, we need to know in which chapter we are */
            psz_chapter = p_segment->editions[p_segment->i_current_edition].FindTimecode( i_date );
        }
    }

    if ( psz_chapter != NULL )
    {
        p_segment->psz_current_chapter = psz_chapter;
        p_sys->i_chapter_time = i_time_offset = psz_chapter->i_user_start_time - psz_chapter->i_start_time;
        p_demux->info.i_update |= INPUT_UPDATE_SEEKPOINT;
        p_demux->info.i_seekpoint = psz_chapter->i_seekpoint_num - 1;
    }

    for( ; i_index < p_segment->i_index; i_index++ )
    {
        if( p_segment->index[i_index].i_time + i_time_offset > i_date )
        {
            break;
        }
    }

    if( i_index > 0 )
    {
        i_index--;
    }

    msg_Dbg( p_demux, "seek got "I64Fd" (%d%%)",
                p_segment->index[i_index].i_time,
                (int)( 100 * p_segment->index[i_index].i_position /
                    stream_Size( p_demux->s ) ) );

    p_stream->in->setFilePointer( p_segment->index[i_index].i_position,
                                seek_beginning );

    p_sys->i_start_pts = i_date;

    es_out_Control( p_demux->out, ES_OUT_RESET_PCR );

    /* now parse until key frame */
#define tk  p_segment->tracks[i_track]
    i_track_skipping = 0;
    for( i_track = 0; i_track < p_segment->tracks.size(); i_track++ )
    {
        if( tk->fmt.i_cat == VIDEO_ES )
        {
            tk->b_search_keyframe = VLC_TRUE;
            i_track_skipping++;
        }
        es_out_Control( p_demux->out, ES_OUT_SET_NEXT_DISPLAY_TIME, tk->p_es, i_date );
    }


    while( i_track_skipping > 0 )
    {
        if( BlockGet( p_demux, &block, &i_block_ref1, &i_block_ref2, &i_block_duration ) )
        {
            msg_Warn( p_demux, "cannot get block EOF?" );

            return;
        }

        for( i_track = 0; i_track < p_segment->tracks.size(); i_track++ )
        {
            if( tk->i_number == block->TrackNum() )
            {
                break;
            }
        }

        p_sys->i_pts = p_sys->i_chapter_time + block->GlobalTimecode() / (mtime_t) 1000;

        if( i_track < p_segment->tracks.size() )
        {
            if( tk->fmt.i_cat == VIDEO_ES )
            {
                if( i_block_ref1 == -1 && tk->b_search_keyframe )
                {
                    tk->b_search_keyframe = VLC_FALSE;
                    i_track_skipping--;
                }
                if( !tk->b_search_keyframe )
                {
                    BlockDecode( p_demux, block, p_sys->i_pts, 0 );
                }
            }
        }

        delete block;
    }
#undef tk
}

/*****************************************************************************
 * Demux: reads and demuxes data packets
 *****************************************************************************
 * Returns -1 in case of error, 0 in case of EOF, 1 otherwise
 *****************************************************************************/
static int Demux( demux_t *p_demux)
{
    demux_sys_t        *p_sys = p_demux->p_sys;
    matroska_stream_t  *p_stream = p_sys->Stream();
    matroska_segment_t *p_segment = p_stream->Segment();
    int                i_block_count = 0;

    KaxBlock *block;
    int64_t i_block_duration;
    int64_t i_block_ref1;
    int64_t i_block_ref2;

    for( ;; )
    {
        if( p_sys->i_pts >= p_sys->i_start_pts  )
            UpdateCurrentToChapter( *p_demux );
        
        if ( p_segment->editions.size() && p_segment->editions[p_segment->i_current_edition].b_ordered && p_segment->psz_current_chapter == NULL )
        {
            /* nothing left to read in this ordered edition */
            return 0;
        }

        if( BlockGet( p_demux, &block, &i_block_ref1, &i_block_ref2, &i_block_duration ) )
        {
            if ( p_segment->editions.size() && p_segment->editions[p_segment->i_current_edition].b_ordered )
            {
                // check if there are more chapters to read
                if ( p_segment->psz_current_chapter != NULL )
                {
                    p_sys->i_pts = p_segment->psz_current_chapter->i_user_end_time;
                    return 1;
                }

                return 0;
            }
            msg_Warn( p_demux, "cannot get block EOF?" );

            return 0;
        }

        p_sys->i_pts = p_sys->i_chapter_time + block->GlobalTimecode() / (mtime_t) 1000;

        if( p_sys->i_pts >= p_sys->i_start_pts  )
        {
            es_out_Control( p_demux->out, ES_OUT_SET_PCR, p_sys->i_pts );
        }

        BlockDecode( p_demux, block, p_sys->i_pts, i_block_duration );

        delete block;
        i_block_count++;

        // TODO optimize when there is need to leave or when seeking has been called
        if( i_block_count > 5 )
        {
            return 1;
        }
    }
}



/*****************************************************************************
 * Stream managment
 *****************************************************************************/
vlc_stream_io_callback::vlc_stream_io_callback( stream_t *s_ )
{
    s = s_;
    mb_eof = VLC_FALSE;
}

uint32 vlc_stream_io_callback::read( void *p_buffer, size_t i_size )
{
    if( i_size <= 0 || mb_eof )
    {
        return 0;
    }

    return stream_Read( s, p_buffer, i_size );
}
void vlc_stream_io_callback::setFilePointer(int64_t i_offset, seek_mode mode )
{
    int64_t i_pos;

    switch( mode )
    {
        case seek_beginning:
            i_pos = i_offset;
            break;
        case seek_end:
            i_pos = stream_Size( s ) - i_offset;
            break;
        default:
            i_pos= stream_Tell( s ) + i_offset;
            break;
    }

    if( i_pos < 0 || i_pos >= stream_Size( s ) )
    {
        mb_eof = VLC_TRUE;
        return;
    }

    mb_eof = VLC_FALSE;
    if( stream_Seek( s, i_pos ) )
    {
        mb_eof = VLC_TRUE;
    }
    return;
}
size_t vlc_stream_io_callback::write( const void *p_buffer, size_t i_size )
{
    return 0;
}
uint64 vlc_stream_io_callback::getFilePointer( void )
{
    return stream_Tell( s );
}
void vlc_stream_io_callback::close( void )
{
    return;
}


/*****************************************************************************
 * Ebml Stream parser
 *****************************************************************************/
EbmlParser::EbmlParser( EbmlStream *es, EbmlElement *el_start )
{
    int i;

    m_es = es;
    m_got = NULL;
    m_el[0] = el_start;

    for( i = 1; i < 6; i++ )
    {
        m_el[i] = NULL;
    }
    mi_level = 1;
    mi_user_level = 1;
    mb_keep = VLC_FALSE;
}

EbmlParser::~EbmlParser( void )
{
    int i;

    for( i = 1; i < mi_level; i++ )
    {
        if( !mb_keep )
        {
            delete m_el[i];
        }
        mb_keep = VLC_FALSE;
    }
}

void EbmlParser::Up( void )
{
    if( mi_user_level == mi_level )
    {
        fprintf( stderr," arrrrrrrrrrrrrg Up cannot escape itself\n" );
    }

    mi_user_level--;
}

void EbmlParser::Down( void )
{
    mi_user_level++;
    mi_level++;
}

void EbmlParser::Keep( void )
{
    mb_keep = VLC_TRUE;
}

int EbmlParser::GetLevel( void )
{
    return mi_user_level;
}

EbmlElement *EbmlParser::Get( void )
{
    int i_ulev = 0;

    if( mi_user_level != mi_level )
    {
        return NULL;
    }
    if( m_got )
    {
        EbmlElement *ret = m_got;
        m_got = NULL;

        return ret;
    }

    if( m_el[mi_level] )
    {
        m_el[mi_level]->SkipData( *m_es, m_el[mi_level]->Generic().Context );
        if( !mb_keep )
        {
            delete m_el[mi_level];
        }
        mb_keep = VLC_FALSE;
    }

    m_el[mi_level] = m_es->FindNextElement( m_el[mi_level - 1]->Generic().Context, i_ulev, 0xFFFFFFFFL, true, 1 );
    if( i_ulev > 0 )
    {
        while( i_ulev > 0 )
        {
            if( mi_level == 1 )
            {
                mi_level = 0;
                return NULL;
            }

            delete m_el[mi_level - 1];
            m_got = m_el[mi_level -1] = m_el[mi_level];
            m_el[mi_level] = NULL;

            mi_level--;
            i_ulev--;
        }
        return NULL;
    }
    else if( m_el[mi_level] == NULL )
    {
        fprintf( stderr," m_el[mi_level] == NULL\n" );
    }

    return m_el[mi_level];
}


/*****************************************************************************
 * Tools
 *  * LoadCues : load the cues element and update index
 *
 *  * LoadTags : load ... the tags element
 *
 *  * InformationCreate : create all information, load tags if present
 *
 *****************************************************************************/
static void LoadCues( demux_t *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    matroska_stream_t  *p_stream = p_sys->Stream();
    matroska_segment_t *p_segment = p_stream->Segment();
    int64_t     i_sav_position = p_stream->in->getFilePointer();
    EbmlParser  *ep;
    EbmlElement *el, *cues;

    msg_Dbg( p_demux, "loading cues" );
    p_stream->in->setFilePointer( p_segment->i_cues_position, seek_beginning );
    cues = p_stream->es->FindNextID( KaxCues::ClassInfos, 0xFFFFFFFFL);

    if( cues == NULL )
    {
        msg_Err( p_demux, "cannot load cues (broken seekhead or file)" );
        p_stream->in->setFilePointer( i_sav_position, seek_beginning );
        return;
    }

    ep = new EbmlParser( p_stream->es, cues );
    while( ( el = ep->Get() ) != NULL )
    {
        if( MKV_IS_ID( el, KaxCuePoint ) )
        {
#define idx p_segment->index[p_segment->i_index]

            idx.i_track       = -1;
            idx.i_block_number= -1;
            idx.i_position    = -1;
            idx.i_time        = 0;
            idx.b_key         = VLC_TRUE;

            ep->Down();
            while( ( el = ep->Get() ) != NULL )
            {
                if( MKV_IS_ID( el, KaxCueTime ) )
                {
                    KaxCueTime &ctime = *(KaxCueTime*)el;

                    ctime.ReadData( p_stream->es->I_O() );

                    idx.i_time = uint64( ctime ) * p_segment->i_timescale / (mtime_t)1000;
                }
                else if( MKV_IS_ID( el, KaxCueTrackPositions ) )
                {
                    ep->Down();
                    while( ( el = ep->Get() ) != NULL )
                    {
                        if( MKV_IS_ID( el, KaxCueTrack ) )
                        {
                            KaxCueTrack &ctrack = *(KaxCueTrack*)el;

                            ctrack.ReadData( p_stream->es->I_O() );
                            idx.i_track = uint16( ctrack );
                        }
                        else if( MKV_IS_ID( el, KaxCueClusterPosition ) )
                        {
                            KaxCueClusterPosition &ccpos = *(KaxCueClusterPosition*)el;

                            ccpos.ReadData( p_stream->es->I_O() );
                            idx.i_position = p_segment->segment->GetGlobalPosition( uint64( ccpos ) );
                        }
                        else if( MKV_IS_ID( el, KaxCueBlockNumber ) )
                        {
                            KaxCueBlockNumber &cbnum = *(KaxCueBlockNumber*)el;

                            cbnum.ReadData( p_stream->es->I_O() );
                            idx.i_block_number = uint32( cbnum );
                        }
                        else
                        {
                            msg_Dbg( p_demux, "         * Unknown (%s)", typeid(*el).name() );
                        }
                    }
                    ep->Up();
                }
                else
                {
                    msg_Dbg( p_demux, "     * Unknown (%s)", typeid(*el).name() );
                }
            }
            ep->Up();

#if 0
            msg_Dbg( p_demux, " * added time="I64Fd" pos="I64Fd
                     " track=%d bnum=%d", idx.i_time, idx.i_position,
                     idx.i_track, idx.i_block_number );
#endif

            p_segment->i_index++;
            if( p_segment->i_index >= p_segment->i_index_max )
            {
                p_segment->i_index_max += 1024;
                p_segment->index = (mkv_index_t*)realloc( p_segment->index, sizeof( mkv_index_t ) * p_segment->i_index_max );
            }
#undef idx
        }
        else
        {
            msg_Dbg( p_demux, " * Unknown (%s)", typeid(*el).name() );
        }
    }
    delete ep;
    delete cues;

    p_segment->b_cues = VLC_TRUE;

    msg_Dbg( p_demux, "loading cues done." );
    p_stream->in->setFilePointer( i_sav_position, seek_beginning );
}

static void LoadTags( demux_t *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    matroska_stream_t  *p_stream = p_sys->Stream();
    matroska_segment_t *p_segment = p_stream->Segment();
    int64_t     i_sav_position = p_stream->in->getFilePointer();
    EbmlParser  *ep;
    EbmlElement *el, *tags;

    msg_Dbg( p_demux, "loading tags" );
    p_stream->in->setFilePointer( p_segment->i_tags_position, seek_beginning );
    tags = p_stream->es->FindNextID( KaxTags::ClassInfos, 0xFFFFFFFFL);

    if( tags == NULL )
    {
        msg_Err( p_demux, "cannot load tags (broken seekhead or file)" );
        p_stream->in->setFilePointer( i_sav_position, seek_beginning );
        return;
    }

    msg_Dbg( p_demux, "Tags" );
    ep = new EbmlParser( p_stream->es, tags );
    while( ( el = ep->Get() ) != NULL )
    {
        if( MKV_IS_ID( el, KaxTag ) )
        {
            msg_Dbg( p_demux, "+ Tag" );
            ep->Down();
            while( ( el = ep->Get() ) != NULL )
            {
                if( MKV_IS_ID( el, KaxTagTargets ) )
                {
                    msg_Dbg( p_demux, "|   + Targets" );
                    ep->Down();
                    while( ( el = ep->Get() ) != NULL )
                    {
                        msg_Dbg( p_demux, "|   |   + Unknown (%s)", typeid( *el ).name() );
                    }
                    ep->Up();
                }
                else if( MKV_IS_ID( el, KaxTagGeneral ) )
                {
                    msg_Dbg( p_demux, "|   + General" );
                    ep->Down();
                    while( ( el = ep->Get() ) != NULL )
                    {
                        msg_Dbg( p_demux, "|   |   + Unknown (%s)", typeid( *el ).name() );
                    }
                    ep->Up();
                }
                else if( MKV_IS_ID( el, KaxTagGenres ) )
                {
                    msg_Dbg( p_demux, "|   + Genres" );
                    ep->Down();
                    while( ( el = ep->Get() ) != NULL )
                    {
                        msg_Dbg( p_demux, "|   |   + Unknown (%s)", typeid( *el ).name() );
                    }
                    ep->Up();
                }
                else if( MKV_IS_ID( el, KaxTagAudioSpecific ) )
                {
                    msg_Dbg( p_demux, "|   + Audio Specific" );
                    ep->Down();
                    while( ( el = ep->Get() ) != NULL )
                    {
                        msg_Dbg( p_demux, "|   |   + Unknown (%s)", typeid( *el ).name() );
                    }
                    ep->Up();
                }
                else if( MKV_IS_ID( el, KaxTagImageSpecific ) )
                {
                    msg_Dbg( p_demux, "|   + Images Specific" );
                    ep->Down();
                    while( ( el = ep->Get() ) != NULL )
                    {
                        msg_Dbg( p_demux, "|   |   + Unknown (%s)", typeid( *el ).name() );
                    }
                    ep->Up();
                }
                else if( MKV_IS_ID( el, KaxTagMultiComment ) )
                {
                    msg_Dbg( p_demux, "|   + Multi Comment" );
                }
                else if( MKV_IS_ID( el, KaxTagMultiCommercial ) )
                {
                    msg_Dbg( p_demux, "|   + Multi Commercial" );
                }
                else if( MKV_IS_ID( el, KaxTagMultiDate ) )
                {
                    msg_Dbg( p_demux, "|   + Multi Date" );
                }
                else if( MKV_IS_ID( el, KaxTagMultiEntity ) )
                {
                    msg_Dbg( p_demux, "|   + Multi Entity" );
                }
                else if( MKV_IS_ID( el, KaxTagMultiIdentifier ) )
                {
                    msg_Dbg( p_demux, "|   + Multi Identifier" );
                }
                else if( MKV_IS_ID( el, KaxTagMultiLegal ) )
                {
                    msg_Dbg( p_demux, "|   + Multi Legal" );
                }
                else if( MKV_IS_ID( el, KaxTagMultiTitle ) )
                {
                    msg_Dbg( p_demux, "|   + Multi Title" );
                }
                else
                {
                    msg_Dbg( p_demux, "|   + Unknown (%s)", typeid( *el ).name() );
                }
            }
            ep->Up();
        }
        else
        {
            msg_Dbg( p_demux, "+ Unknown (%s)", typeid( *el ).name() );
        }
    }
    delete ep;
    delete tags;

    msg_Dbg( p_demux, "loading tags done." );
    p_stream->in->setFilePointer( i_sav_position, seek_beginning );
}

/*****************************************************************************
 * ParseInfo:
 *****************************************************************************/
static void ParseSeekHead( demux_t *p_demux, EbmlElement *seekhead )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    matroska_stream_t  *p_stream = p_sys->Stream();
    matroska_segment_t *p_segment = p_stream->Segment();
    EbmlElement *el;
    EbmlMaster  *m;
    unsigned int i;
    int i_upper_level = 0;

    msg_Dbg( p_demux, "|   + Seek head" );

    /* Master elements */
    m = static_cast<EbmlMaster *>(seekhead);
    m->Read( *p_stream->es, seekhead->Generic().Context, i_upper_level, el, true );

    for( i = 0; i < m->ListSize(); i++ )
    {
        EbmlElement *l = (*m)[i];

        if( MKV_IS_ID( l, KaxSeek ) )
        {
            EbmlMaster *sk = static_cast<EbmlMaster *>(l);
            EbmlId id = EbmlVoid::ClassInfos.GlobalId;
            int64_t i_pos = -1;

            unsigned int j;

            for( j = 0; j < sk->ListSize(); j++ )
            {
                EbmlElement *l = (*sk)[j];

                if( MKV_IS_ID( l, KaxSeekID ) )
                {
                    KaxSeekID &sid = *(KaxSeekID*)l;
                    id = EbmlId( sid.GetBuffer(), sid.GetSize() );
                }
                else if( MKV_IS_ID( l, KaxSeekPosition ) )
                {
                    KaxSeekPosition &spos = *(KaxSeekPosition*)l;
                    i_pos = uint64( spos );
                }
                else
                {
                    msg_Dbg( p_demux, "|   |   |   + Unknown (%s)", typeid(*l).name() );
                }
            }

            if( i_pos >= 0 )
            {
                if( id == KaxCues::ClassInfos.GlobalId )
                {
                    msg_Dbg( p_demux, "|   |   |   = cues at "I64Fd, i_pos );
                    p_segment->i_cues_position = p_segment->segment->GetGlobalPosition( i_pos );
                }
                else if( id == KaxChapters::ClassInfos.GlobalId )
                {
                    msg_Dbg( p_demux, "|   |   |   = chapters at "I64Fd, i_pos );
                    p_segment->i_chapters_position = p_segment->segment->GetGlobalPosition( i_pos );
                }
                else if( id == KaxTags::ClassInfos.GlobalId )
                {
                    msg_Dbg( p_demux, "|   |   |   = tags at "I64Fd, i_pos );
                    p_segment->i_tags_position = p_segment->segment->GetGlobalPosition( i_pos );
                }
            }
        }
        else
        {
            msg_Dbg( p_demux, "|   |   + Unknown (%s)", typeid(*l).name() );
        }
    }
}

/*****************************************************************************
 * ParseTracks:
 *****************************************************************************/
static void ParseTrackEntry( demux_t *p_demux, EbmlMaster *m )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    matroska_stream_t  *p_stream = p_sys->Stream();
    matroska_segment_t *p_segment = p_stream->Segment();
    unsigned int i;

    mkv_track_t *tk;

    msg_Dbg( p_demux, "|   |   + Track Entry" );

    tk = new mkv_track_t();
    p_segment->tracks.push_back( tk );

    /* Init the track */
    memset( tk, 0, sizeof( mkv_track_t ) );

    es_format_Init( &tk->fmt, UNKNOWN_ES, 0 );
    tk->fmt.psz_language = strdup("English");
    tk->fmt.psz_description = NULL;

    tk->b_default = VLC_TRUE;
    tk->b_enabled = VLC_TRUE;
    tk->i_number = p_segment->tracks.size() - 1;
    tk->i_extra_data = 0;
    tk->p_extra_data = NULL;
    tk->psz_codec = NULL;
    tk->i_default_duration = 0;
    tk->f_timecodescale = 1.0;

    tk->b_inited = VLC_FALSE;
    tk->i_data_init = 0;
    tk->p_data_init = NULL;

    tk->psz_codec_name = NULL;
    tk->psz_codec_settings = NULL;
    tk->psz_codec_info_url = NULL;
    tk->psz_codec_download_url = NULL;
    
    tk->i_compression_type = MATROSKA_COMPRESSION_NONE;

    for( i = 0; i < m->ListSize(); i++ )
    {
        EbmlElement *l = (*m)[i];

        if( MKV_IS_ID( l, KaxTrackNumber ) )
        {
            KaxTrackNumber &tnum = *(KaxTrackNumber*)l;

            tk->i_number = uint32( tnum );
            msg_Dbg( p_demux, "|   |   |   + Track Number=%u", uint32( tnum ) );
        }
        else  if( MKV_IS_ID( l, KaxTrackUID ) )
        {
            KaxTrackUID &tuid = *(KaxTrackUID*)l;

            msg_Dbg( p_demux, "|   |   |   + Track UID=%u",  uint32( tuid ) );
        }
        else  if( MKV_IS_ID( l, KaxTrackType ) )
        {
            char *psz_type;
            KaxTrackType &ttype = *(KaxTrackType*)l;

            switch( uint8(ttype) )
            {
                case track_audio:
                    psz_type = "audio";
                    tk->fmt.i_cat = AUDIO_ES;
                    break;
                case track_video:
                    psz_type = "video";
                    tk->fmt.i_cat = VIDEO_ES;
                    break;
                case track_subtitle:
                    psz_type = "subtitle";
                    tk->fmt.i_cat = SPU_ES;
                    break;
                default:
                    psz_type = "unknown";
                    tk->fmt.i_cat = UNKNOWN_ES;
                    break;
            }

            msg_Dbg( p_demux, "|   |   |   + Track Type=%s", psz_type );
        }
//        else  if( EbmlId( *l ) == KaxTrackFlagEnabled::ClassInfos.GlobalId )
//        {
//            KaxTrackFlagEnabled &fenb = *(KaxTrackFlagEnabled*)l;

//            tk->b_enabled = uint32( fenb );
//            msg_Dbg( p_demux, "|   |   |   + Track Enabled=%u",
//                     uint32( fenb )  );
//        }
        else  if( MKV_IS_ID( l, KaxTrackFlagDefault ) )
        {
            KaxTrackFlagDefault &fdef = *(KaxTrackFlagDefault*)l;

            tk->b_default = uint32( fdef );
            msg_Dbg( p_demux, "|   |   |   + Track Default=%u", uint32( fdef )  );
        }
        else  if( MKV_IS_ID( l, KaxTrackFlagLacing ) )
        {
            KaxTrackFlagLacing &lac = *(KaxTrackFlagLacing*)l;

            msg_Dbg( p_demux, "|   |   |   + Track Lacing=%d", uint32( lac ) );
        }
        else  if( MKV_IS_ID( l, KaxTrackMinCache ) )
        {
            KaxTrackMinCache &cmin = *(KaxTrackMinCache*)l;

            msg_Dbg( p_demux, "|   |   |   + Track MinCache=%d", uint32( cmin ) );
        }
        else  if( MKV_IS_ID( l, KaxTrackMaxCache ) )
        {
            KaxTrackMaxCache &cmax = *(KaxTrackMaxCache*)l;

            msg_Dbg( p_demux, "|   |   |   + Track MaxCache=%d", uint32( cmax ) );
        }
        else  if( MKV_IS_ID( l, KaxTrackDefaultDuration ) )
        {
            KaxTrackDefaultDuration &defd = *(KaxTrackDefaultDuration*)l;

            tk->i_default_duration = uint64(defd);
            msg_Dbg( p_demux, "|   |   |   + Track Default Duration="I64Fd, uint64(defd) );
        }
        else  if( MKV_IS_ID( l, KaxTrackTimecodeScale ) )
        {
            KaxTrackTimecodeScale &ttcs = *(KaxTrackTimecodeScale*)l;

            tk->f_timecodescale = float( ttcs );
            msg_Dbg( p_demux, "|   |   |   + Track TimeCodeScale=%f", tk->f_timecodescale );
        }
        else if( MKV_IS_ID( l, KaxTrackName ) )
        {
            KaxTrackName &tname = *(KaxTrackName*)l;

            tk->fmt.psz_description = UTF8ToStr( UTFstring( tname ) );
            msg_Dbg( p_demux, "|   |   |   + Track Name=%s", tk->fmt.psz_description );
        }
        else  if( MKV_IS_ID( l, KaxTrackLanguage ) )
        {
            KaxTrackLanguage &lang = *(KaxTrackLanguage*)l;

            tk->fmt.psz_language = strdup( string( lang ).c_str() );
            msg_Dbg( p_demux,
                     "|   |   |   + Track Language=`%s'", tk->fmt.psz_language );
        }
        else  if( MKV_IS_ID( l, KaxCodecID ) )
        {
            KaxCodecID &codecid = *(KaxCodecID*)l;

            tk->psz_codec = strdup( string( codecid ).c_str() );
            msg_Dbg( p_demux, "|   |   |   + Track CodecId=%s", string( codecid ).c_str() );
        }
        else  if( MKV_IS_ID( l, KaxCodecPrivate ) )
        {
            KaxCodecPrivate &cpriv = *(KaxCodecPrivate*)l;

            tk->i_extra_data = cpriv.GetSize();
            if( tk->i_extra_data > 0 )
            {
                tk->p_extra_data = (uint8_t*)malloc( tk->i_extra_data );
                memcpy( tk->p_extra_data, cpriv.GetBuffer(), tk->i_extra_data );
            }
            msg_Dbg( p_demux, "|   |   |   + Track CodecPrivate size="I64Fd, cpriv.GetSize() );
        }
        else if( MKV_IS_ID( l, KaxCodecName ) )
        {
            KaxCodecName &cname = *(KaxCodecName*)l;

            tk->psz_codec_name = UTF8ToStr( UTFstring( cname ) );
            msg_Dbg( p_demux, "|   |   |   + Track Codec Name=%s", tk->psz_codec_name );
        }
        else if( MKV_IS_ID( l, KaxContentEncodings ) )
        {
            EbmlMaster *cencs = static_cast<EbmlMaster*>(l);
            MkvTree( p_demux, 3, "Content Encodings" );
            for( unsigned int i = 0; i < cencs->ListSize(); i++ )
            {
                EbmlElement *l2 = (*cencs)[i];
                if( MKV_IS_ID( l2, KaxContentEncoding ) )
                {
                    MkvTree( p_demux, 4, "Content Encoding" );
                    EbmlMaster *cenc = static_cast<EbmlMaster*>(l2);
                    for( unsigned int i = 0; i < cenc->ListSize(); i++ )
                    {
                        EbmlElement *l3 = (*cenc)[i];
                        if( MKV_IS_ID( l3, KaxContentEncodingOrder ) )
                        {
                            KaxContentEncodingOrder &encord = *(KaxContentEncodingOrder*)l3;
                            MkvTree( p_demux, 5, "Order: %i", uint32( encord ) );
                        }
                        else if( MKV_IS_ID( l3, KaxContentEncodingScope ) )
                        {
                            KaxContentEncodingScope &encscope = *(KaxContentEncodingScope*)l3;
                            MkvTree( p_demux, 5, "Scope: %i", uint32( encscope ) );
                        }
                        else if( MKV_IS_ID( l3, KaxContentEncodingType ) )
                        {
                            KaxContentEncodingType &enctype = *(KaxContentEncodingType*)l3;
                            MkvTree( p_demux, 5, "Type: %i", uint32( enctype ) );
                        }
                        else if( MKV_IS_ID( l3, KaxContentCompression ) )
                        {
                            EbmlMaster *compr = static_cast<EbmlMaster*>(l3);
                            MkvTree( p_demux, 5, "Content Compression" );
                            for( unsigned int i = 0; i < compr->ListSize(); i++ )
                            {
                                EbmlElement *l4 = (*compr)[i];
                                if( MKV_IS_ID( l4, KaxContentCompAlgo ) )
                                {
                                    KaxContentCompAlgo &compalg = *(KaxContentCompAlgo*)l4;
                                    MkvTree( p_demux, 6, "Compression Algorithm: %i", uint32(compalg) );
                                    if( uint32( compalg ) == 0 )
                                    {
                                        tk->i_compression_type = MATROSKA_COMPRESSION_ZLIB;
                                    }
                                }
                                else
                                {
                                    MkvTree( p_demux, 6, "Unknown (%s)", typeid(*l4).name() );
                                }
                            }
                        }

                        else
                        {
                            MkvTree( p_demux, 5, "Unknown (%s)", typeid(*l3).name() );
                        }
                    }
                    
                }
                else
                {
                    MkvTree( p_demux, 4, "Unknown (%s)", typeid(*l2).name() );
                }
            }
                
        }
//        else if( EbmlId( *l ) == KaxCodecSettings::ClassInfos.GlobalId )
//        {
//            KaxCodecSettings &cset = *(KaxCodecSettings*)l;

//            tk->psz_codec_settings = UTF8ToStr( UTFstring( cset ) );
//            msg_Dbg( p_demux, "|   |   |   + Track Codec Settings=%s", tk->psz_codec_settings );
//        }
//        else if( EbmlId( *l ) == KaxCodecInfoURL::ClassInfos.GlobalId )
//        {
//            KaxCodecInfoURL &ciurl = *(KaxCodecInfoURL*)l;

//            tk->psz_codec_info_url = strdup( string( ciurl ).c_str() );
//            msg_Dbg( p_demux, "|   |   |   + Track Codec Info URL=%s", tk->psz_codec_info_url );
//        }
//        else if( EbmlId( *l ) == KaxCodecDownloadURL::ClassInfos.GlobalId )
//        {
//            KaxCodecDownloadURL &cdurl = *(KaxCodecDownloadURL*)l;

//            tk->psz_codec_download_url = strdup( string( cdurl ).c_str() );
//            msg_Dbg( p_demux, "|   |   |   + Track Codec Info URL=%s", tk->psz_codec_download_url );
//        }
//        else if( EbmlId( *l ) == KaxCodecDecodeAll::ClassInfos.GlobalId )
//        {
//            KaxCodecDecodeAll &cdall = *(KaxCodecDecodeAll*)l;

//            msg_Dbg( p_demux, "|   |   |   + Track Codec Decode All=%u <== UNUSED", uint8( cdall ) );
//        }
//        else if( EbmlId( *l ) == KaxTrackOverlay::ClassInfos.GlobalId )
//        {
//            KaxTrackOverlay &tovr = *(KaxTrackOverlay*)l;

//            msg_Dbg( p_demux, "|   |   |   + Track Overlay=%u <== UNUSED", uint32( tovr ) );
//        }
        else  if( MKV_IS_ID( l, KaxTrackVideo ) )
        {
            EbmlMaster *tkv = static_cast<EbmlMaster*>(l);
            unsigned int j;

            msg_Dbg( p_demux, "|   |   |   + Track Video" );
            tk->f_fps = 0.0;

            for( j = 0; j < tkv->ListSize(); j++ )
            {
                EbmlElement *l = (*tkv)[j];
//                if( EbmlId( *el4 ) == KaxVideoFlagInterlaced::ClassInfos.GlobalId )
//                {
//                    KaxVideoFlagInterlaced &fint = *(KaxVideoFlagInterlaced*)el4;

//                    msg_Dbg( p_demux, "|   |   |   |   + Track Video Interlaced=%u", uint8( fint ) );
//                }
//                else if( EbmlId( *el4 ) == KaxVideoStereoMode::ClassInfos.GlobalId )
//                {
//                    KaxVideoStereoMode &stereo = *(KaxVideoStereoMode*)el4;

//                    msg_Dbg( p_demux, "|   |   |   |   + Track Video Stereo Mode=%u", uint8( stereo ) );
//                }
//                else
                if( MKV_IS_ID( l, KaxVideoPixelWidth ) )
                {
                    KaxVideoPixelWidth &vwidth = *(KaxVideoPixelWidth*)l;

                    tk->fmt.video.i_width = uint16( vwidth );
                    msg_Dbg( p_demux, "|   |   |   |   + width=%d", uint16( vwidth ) );
                }
                else if( MKV_IS_ID( l, KaxVideoPixelHeight ) )
                {
                    KaxVideoPixelWidth &vheight = *(KaxVideoPixelWidth*)l;

                    tk->fmt.video.i_height = uint16( vheight );
                    msg_Dbg( p_demux, "|   |   |   |   + height=%d", uint16( vheight ) );
                }
                else if( MKV_IS_ID( l, KaxVideoDisplayWidth ) )
                {
                    KaxVideoDisplayWidth &vwidth = *(KaxVideoDisplayWidth*)l;

                    tk->fmt.video.i_visible_width = uint16( vwidth );
                    msg_Dbg( p_demux, "|   |   |   |   + display width=%d", uint16( vwidth ) );
                }
                else if( MKV_IS_ID( l, KaxVideoDisplayHeight ) )
                {
                    KaxVideoDisplayWidth &vheight = *(KaxVideoDisplayWidth*)l;

                    tk->fmt.video.i_visible_height = uint16( vheight );
                    msg_Dbg( p_demux, "|   |   |   |   + display height=%d", uint16( vheight ) );
                }
                else if( MKV_IS_ID( l, KaxVideoFrameRate ) )
                {
                    KaxVideoFrameRate &vfps = *(KaxVideoFrameRate*)l;

                    tk->f_fps = float( vfps );
                    msg_Dbg( p_demux, "   |   |   |   + fps=%f", float( vfps ) );
                }
//                else if( EbmlId( *l ) == KaxVideoDisplayUnit::ClassInfos.GlobalId )
//                {
//                     KaxVideoDisplayUnit &vdmode = *(KaxVideoDisplayUnit*)l;

//                    msg_Dbg( p_demux, "|   |   |   |   + Track Video Display Unit=%s",
//                             uint8( vdmode ) == 0 ? "pixels" : ( uint8( vdmode ) == 1 ? "centimeters": "inches" ) );
//                }
//                else if( EbmlId( *l ) == KaxVideoAspectRatio::ClassInfos.GlobalId )
//                {
//                    KaxVideoAspectRatio &ratio = *(KaxVideoAspectRatio*)l;

//                    msg_Dbg( p_demux, "   |   |   |   + Track Video Aspect Ratio Type=%u", uint8( ratio ) );
//                }
//                else if( EbmlId( *l ) == KaxVideoGamma::ClassInfos.GlobalId )
//                {
//                    KaxVideoGamma &gamma = *(KaxVideoGamma*)l;

//                    msg_Dbg( p_demux, "   |   |   |   + fps=%f", float( gamma ) );
//                }
                else
                {
                    msg_Dbg( p_demux, "|   |   |   |   + Unknown (%s)", typeid(*l).name() );
                }
            }
            if ( tk->fmt.video.i_visible_height && tk->fmt.video.i_visible_width )
                tk->fmt.video.i_aspect = VOUT_ASPECT_FACTOR * tk->fmt.video.i_visible_width / tk->fmt.video.i_visible_height;
        }
        else  if( MKV_IS_ID( l, KaxTrackAudio ) )
        {
            EbmlMaster *tka = static_cast<EbmlMaster*>(l);
            unsigned int j;

            msg_Dbg( p_demux, "|   |   |   + Track Audio" );

            for( j = 0; j < tka->ListSize(); j++ )
            {
                EbmlElement *l = (*tka)[j];

                if( MKV_IS_ID( l, KaxAudioSamplingFreq ) )
                {
                    KaxAudioSamplingFreq &afreq = *(KaxAudioSamplingFreq*)l;

                    tk->fmt.audio.i_rate = (int)float( afreq );
                    msg_Dbg( p_demux, "|   |   |   |   + afreq=%d", tk->fmt.audio.i_rate );
                }
                else if( MKV_IS_ID( l, KaxAudioChannels ) )
                {
                    KaxAudioChannels &achan = *(KaxAudioChannels*)l;

                    tk->fmt.audio.i_channels = uint8( achan );
                    msg_Dbg( p_demux, "|   |   |   |   + achan=%u", uint8( achan ) );
                }
                else if( MKV_IS_ID( l, KaxAudioBitDepth ) )
                {
                    KaxAudioBitDepth &abits = *(KaxAudioBitDepth*)l;

                    tk->fmt.audio.i_bitspersample = uint8( abits );
                    msg_Dbg( p_demux, "|   |   |   |   + abits=%u", uint8( abits ) );
                }
                else
                {
                    msg_Dbg( p_demux, "|   |   |   |   + Unknown (%s)", typeid(*l).name() );
                }
            }
        }
        else
        {
            msg_Dbg( p_demux, "|   |   |   + Unknown (%s)",
                     typeid(*l).name() );
        }
    }
}

static void ParseTracks( demux_t *p_demux, EbmlElement *tracks )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    matroska_stream_t  *p_stream = p_sys->Stream();
    matroska_segment_t *p_segment = p_stream->Segment();
    EbmlElement *el;
    EbmlMaster  *m;
    unsigned int i;
    int i_upper_level = 0;

    msg_Dbg( p_demux, "|   + Tracks" );

    /* Master elements */
    m = static_cast<EbmlMaster *>(tracks);
    m->Read( *p_stream->es, tracks->Generic().Context, i_upper_level, el, true );

    for( i = 0; i < m->ListSize(); i++ )
    {
        EbmlElement *l = (*m)[i];

        if( MKV_IS_ID( l, KaxTrackEntry ) )
        {
            ParseTrackEntry( p_demux, static_cast<EbmlMaster *>(l) );
        }
        else
        {
            msg_Dbg( p_demux, "|   |   + Unknown (%s)", typeid(*l).name() );
        }
    }
}

/*****************************************************************************
 * ParseInfo:
 *****************************************************************************/
static void ParseInfo( demux_t *p_demux, EbmlElement *info )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    matroska_stream_t  *p_stream = p_sys->Stream();
    matroska_segment_t *p_segment = p_stream->Segment();
    EbmlElement *el;
    EbmlMaster  *m;
    unsigned int i;
    int i_upper_level = 0;

    msg_Dbg( p_demux, "|   + Information" );

    /* Master elements */
    m = static_cast<EbmlMaster *>(info);
    m->Read( *p_stream->es, info->Generic().Context, i_upper_level, el, true );

    for( i = 0; i < m->ListSize(); i++ )
    {
        EbmlElement *l = (*m)[i];

        if( MKV_IS_ID( l, KaxSegmentUID ) )
        {
            p_segment->segment_uid = *(new KaxSegmentUID(*static_cast<KaxSegmentUID*>(l)));

            msg_Dbg( p_demux, "|   |   + UID=%d", *(uint32*)p_segment->segment_uid.GetBuffer() );
        }
        else if( MKV_IS_ID( l, KaxPrevUID ) )
        {
            p_segment->prev_segment_uid = *(new KaxPrevUID(*static_cast<KaxPrevUID*>(l)));

            msg_Dbg( p_demux, "|   |   + PrevUID=%d", *(uint32*)p_segment->prev_segment_uid.GetBuffer() );
        }
        else if( MKV_IS_ID( l, KaxNextUID ) )
        {
            p_segment->next_segment_uid = *(new KaxNextUID(*static_cast<KaxNextUID*>(l)));

            msg_Dbg( p_demux, "|   |   + NextUID=%d", *(uint32*)p_segment->next_segment_uid.GetBuffer() );
        }
        else if( MKV_IS_ID( l, KaxTimecodeScale ) )
        {
            KaxTimecodeScale &tcs = *(KaxTimecodeScale*)l;

            p_segment->i_timescale = uint64(tcs);

            msg_Dbg( p_demux, "|   |   + TimecodeScale="I64Fd,
                     p_segment->i_timescale );
        }
        else if( MKV_IS_ID( l, KaxDuration ) )
        {
            KaxDuration &dur = *(KaxDuration*)l;

            p_segment->f_duration = float(dur);

            msg_Dbg( p_demux, "|   |   + Duration=%f",
                     p_segment->f_duration );
        }
        else if( MKV_IS_ID( l, KaxMuxingApp ) )
        {
            KaxMuxingApp &mapp = *(KaxMuxingApp*)l;

            p_segment->psz_muxing_application = UTF8ToStr( UTFstring( mapp ) );

            msg_Dbg( p_demux, "|   |   + Muxing Application=%s",
                     p_segment->psz_muxing_application );
        }
        else if( MKV_IS_ID( l, KaxWritingApp ) )
        {
            KaxWritingApp &wapp = *(KaxWritingApp*)l;

            p_segment->psz_writing_application = UTF8ToStr( UTFstring( wapp ) );

            msg_Dbg( p_demux, "|   |   + Writing Application=%s",
                     p_segment->psz_writing_application );
        }
        else if( MKV_IS_ID( l, KaxSegmentFilename ) )
        {
            KaxSegmentFilename &sfn = *(KaxSegmentFilename*)l;

            p_segment->psz_segment_filename = UTF8ToStr( UTFstring( sfn ) );

            msg_Dbg( p_demux, "|   |   + Segment Filename=%s",
                     p_segment->psz_segment_filename );
        }
        else if( MKV_IS_ID( l, KaxTitle ) )
        {
            KaxTitle &title = *(KaxTitle*)l;

            p_segment->psz_title = UTF8ToStr( UTFstring( title ) );

            msg_Dbg( p_demux, "|   |   + Title=%s", p_segment->psz_title );
        }
        else if( MKV_IS_ID( l, KaxSegmentFamily ) )
        {
            KaxSegmentFamily *uid = static_cast<KaxSegmentFamily*>(l);

            p_segment->families.push_back(*uid);

            msg_Dbg( p_demux, "|   |   + family=%d", *(uint32*)uid->GetBuffer() );
        }
#if defined( HAVE_GMTIME_R ) && !defined( SYS_DARWIN )
        else if( MKV_IS_ID( l, KaxDateUTC ) )
        {
            KaxDateUTC &date = *(KaxDateUTC*)l;
            time_t i_date;
            struct tm tmres;
            char   buffer[256];

            i_date = date.GetEpochDate();
            memset( buffer, 0, 256 );
            if( gmtime_r( &i_date, &tmres ) &&
                asctime_r( &tmres, buffer ) )
            {
                buffer[strlen( buffer)-1]= '\0';
                p_segment->psz_date_utc = strdup( buffer );
                msg_Dbg( p_demux, "|   |   + Date=%s", p_segment->psz_date_utc );
            }
        }
#endif
        else
        {
            msg_Dbg( p_demux, "|   |   + Unknown (%s)", typeid(*l).name() );
        }
    }

    p_segment->f_duration *= p_segment->i_timescale / 1000000.0;
}


/*****************************************************************************
 * ParseChapterAtom
 *****************************************************************************/
static void ParseChapterAtom( demux_t *p_demux, int i_level, EbmlMaster *ca, chapter_item_t & chapters )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    unsigned int i;

    if( p_sys->title == NULL )
    {
        p_sys->title = vlc_input_title_New();
    }

    msg_Dbg( p_demux, "|   |   |   + ChapterAtom (level=%d)", i_level );
    for( i = 0; i < ca->ListSize(); i++ )
    {
        EbmlElement *l = (*ca)[i];

        if( MKV_IS_ID( l, KaxChapterUID ) )
        {
            chapters.i_uid = uint64_t(*(KaxChapterUID*)l);
            msg_Dbg( p_demux, "|   |   |   |   + ChapterUID: %lld", chapters.i_uid );
        }
        else if( MKV_IS_ID( l, KaxChapterFlagHidden ) )
        {
            KaxChapterFlagHidden &flag =*(KaxChapterFlagHidden*)l;
            chapters.b_display_seekpoint = uint8( flag ) == 0;

            msg_Dbg( p_demux, "|   |   |   |   + ChapterFlagHidden: %s", chapters.b_display_seekpoint ? "no":"yes" );
        }
        else if( MKV_IS_ID( l, KaxChapterTimeStart ) )
        {
            KaxChapterTimeStart &start =*(KaxChapterTimeStart*)l;
            chapters.i_start_time = uint64( start ) / I64C(1000);

            msg_Dbg( p_demux, "|   |   |   |   + ChapterTimeStart: %lld", chapters.i_start_time );
        }
        else if( MKV_IS_ID( l, KaxChapterTimeEnd ) )
        {
            KaxChapterTimeEnd &end =*(KaxChapterTimeEnd*)l;
            chapters.i_end_time = uint64( end ) / I64C(1000);

            msg_Dbg( p_demux, "|   |   |   |   + ChapterTimeEnd: %lld", chapters.i_end_time );
        }
        else if( MKV_IS_ID( l, KaxChapterDisplay ) )
        {
            EbmlMaster *cd = static_cast<EbmlMaster *>(l);
            unsigned int j;

            msg_Dbg( p_demux, "|   |   |   |   + ChapterDisplay" );
            for( j = 0; j < cd->ListSize(); j++ )
            {
                EbmlElement *l= (*cd)[j];

                if( MKV_IS_ID( l, KaxChapterString ) )
                {
                    int k;

                    KaxChapterString &name =*(KaxChapterString*)l;
                    for (k = 0; k < i_level; k++)
                        chapters.psz_name += '+';
                    chapters.psz_name += ' ';
                    chapters.psz_name += UTF8ToStr( UTFstring( name ) );

                    msg_Dbg( p_demux, "|   |   |   |   |    + ChapterString '%s'", UTF8ToStr(UTFstring(name)) );
                }
                else if( MKV_IS_ID( l, KaxChapterLanguage ) )
                {
                    KaxChapterLanguage &lang =*(KaxChapterLanguage*)l;
                    const char *psz = string( lang ).c_str();

                    msg_Dbg( p_demux, "|   |   |   |   |    + ChapterLanguage '%s'", psz );
                }
                else if( MKV_IS_ID( l, KaxChapterCountry ) )
                {
                    KaxChapterCountry &ct =*(KaxChapterCountry*)l;
                    const char *psz = string( ct ).c_str();

                    msg_Dbg( p_demux, "|   |   |   |   |    + ChapterCountry '%s'", psz );
                }
            }
        }
        else if( MKV_IS_ID( l, KaxChapterAtom ) )
        {
            chapter_item_t new_sub_chapter;
            ParseChapterAtom( p_demux, i_level+1, static_cast<EbmlMaster *>(l), new_sub_chapter );
            new_sub_chapter.psz_parent = &chapters;
            chapters.sub_chapters.push_back( new_sub_chapter );
        }
    }
}

/*****************************************************************************
 * ParseChapters:
 *****************************************************************************/
static void ParseChapters( demux_t *p_demux, EbmlElement *chapters )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    matroska_stream_t  *p_stream = p_sys->Stream();
    matroska_segment_t *p_segment = p_stream->Segment();
    EbmlElement *el;
    EbmlMaster  *m;
    unsigned int i;
    int i_upper_level = 0;
    int i_default_edition = 0;
    float f_duration;

    /* Master elements */
    m = static_cast<EbmlMaster *>(chapters);
    m->Read( *p_stream->es, chapters->Generic().Context, i_upper_level, el, true );

    for( i = 0; i < m->ListSize(); i++ )
    {
        EbmlElement *l = (*m)[i];

        if( MKV_IS_ID( l, KaxEditionEntry ) )
        {
            chapter_edition_t edition;
            
            EbmlMaster *E = static_cast<EbmlMaster *>(l );
            unsigned int j;
            msg_Dbg( p_demux, "|   |   + EditionEntry" );
            for( j = 0; j < E->ListSize(); j++ )
            {
                EbmlElement *l = (*E)[j];

                if( MKV_IS_ID( l, KaxChapterAtom ) )
                {
                    chapter_item_t new_sub_chapter;
                    ParseChapterAtom( p_demux, 0, static_cast<EbmlMaster *>(l), new_sub_chapter );
                    edition.chapters.push_back( new_sub_chapter );
                }
                else if( MKV_IS_ID( l, KaxEditionUID ) )
                {
                    edition.i_uid = uint64(*static_cast<KaxEditionUID *>( l ));
                }
                else if( MKV_IS_ID( l, KaxEditionFlagOrdered ) )
                {
                    edition.b_ordered = uint8(*static_cast<KaxEditionFlagOrdered *>( l )) != 0;
                }
                else if( MKV_IS_ID( l, KaxEditionFlagDefault ) )
                {
                    if (uint8(*static_cast<KaxEditionFlagDefault *>( l )) != 0)
                        i_default_edition = p_segment->editions.size();
                }
                else
                {
                    msg_Dbg( p_demux, "|   |   |   + Unknown (%s)", typeid(*l).name() );
                }
            }
            p_segment->editions.push_back( edition );
        }
        else
        {
            msg_Dbg( p_demux, "|   |   + Unknown (%s)", typeid(*l).name() );
        }
    }

    for( i = 0; i < p_segment->editions.size(); i++ )
    {
        p_segment->editions[i].RefreshChapters( *p_sys->title );
    }
    
    p_segment->i_current_edition = i_default_edition;
    
    if ( p_segment->editions[i_default_edition].b_ordered )
    {
        /* update the duration of the segment according to the sum of all sub chapters */
        f_duration = p_segment->editions[i_default_edition].Duration() / I64C(1000);
        if (f_duration > 0.0)
            p_segment->f_duration = f_duration;
    }
}

/*****************************************************************************
 * InformationCreate:
 *****************************************************************************/
static void InformationCreate( demux_t *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    matroska_stream_t  *p_stream = p_sys->Stream();
    matroska_segment_t *p_segment = p_stream->Segment();
    size_t      i_track;

    p_sys->meta = vlc_meta_New();

    if( p_segment->psz_title )
    {
        vlc_meta_Add( p_sys->meta, VLC_META_TITLE, p_segment->psz_title );
    }
    if( p_segment->psz_date_utc )
    {
        vlc_meta_Add( p_sys->meta, VLC_META_DATE, p_segment->psz_date_utc );
    }
    if( p_segment->psz_segment_filename )
    {
        vlc_meta_Add( p_sys->meta, _("Segment filename"), p_segment->psz_segment_filename );
    }
    if( p_segment->psz_muxing_application )
    {
        vlc_meta_Add( p_sys->meta, _("Muxing application"), p_segment->psz_muxing_application );
    }
    if( p_segment->psz_writing_application )
    {
        vlc_meta_Add( p_sys->meta, _("Writing application"), p_segment->psz_writing_application );
    }

    for( i_track = 0; i_track < p_segment->tracks.size(); i_track++ )
    {
        mkv_track_t *tk = p_segment->tracks[i_track];
        vlc_meta_t *mtk = vlc_meta_New();

        p_sys->meta->track = (vlc_meta_t**)realloc( p_sys->meta->track,
                                                    sizeof( vlc_meta_t * ) * ( p_sys->meta->i_track + 1 ) );
        p_sys->meta->track[p_sys->meta->i_track++] = mtk;

        if( tk->fmt.psz_description )
        {
            vlc_meta_Add( p_sys->meta, VLC_META_DESCRIPTION, tk->fmt.psz_description );
        }
        if( tk->psz_codec_name )
        {
            vlc_meta_Add( p_sys->meta, VLC_META_CODEC_NAME, tk->psz_codec_name );
        }
        if( tk->psz_codec_settings )
        {
            vlc_meta_Add( p_sys->meta, VLC_META_SETTING, tk->psz_codec_settings );
        }
        if( tk->psz_codec_info_url )
        {
            vlc_meta_Add( p_sys->meta, VLC_META_CODEC_DESCRIPTION, tk->psz_codec_info_url );
        }
        if( tk->psz_codec_download_url )
        {
            vlc_meta_Add( p_sys->meta, VLC_META_URL, tk->psz_codec_download_url );
        }
    }

    if( p_segment->i_tags_position >= 0 )
    {
        vlc_bool_t b_seekable;

        stream_Control( p_demux->s, STREAM_CAN_FASTSEEK, &b_seekable );
        if( b_seekable )
        {
            LoadTags( p_demux );
        }
    }
}


/*****************************************************************************
 * Divers
 *****************************************************************************/

static void IndexAppendCluster( demux_t *p_demux, KaxCluster *cluster )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    matroska_stream_t  *p_stream = p_sys->Stream();
    matroska_segment_t *p_segment = p_stream->Segment();

#define idx p_segment->index[p_segment->i_index]
    idx.i_track       = -1;
    idx.i_block_number= -1;
    idx.i_position    = cluster->GetElementPosition();
    idx.i_time        = -1;
    idx.b_key         = VLC_TRUE;

    p_segment->i_index++;
    if( p_segment->i_index >= p_segment->i_index_max )
    {
        p_segment->i_index_max += 1024;
        p_segment->index = (mkv_index_t*)realloc( p_segment->index, sizeof( mkv_index_t ) * p_segment->i_index_max );
    }
#undef idx
}

static char * UTF8ToStr( const UTFstring &u )
{
    int     i_src;
    const wchar_t *src;
    char *dst, *p;

    i_src = u.length();
    src   = u.c_str();

    p = dst = (char*)malloc( i_src + 1);
    while( i_src > 0 )
    {
        if( *src < 255 )
        {
            *p++ = (char)*src;
        }
        else
        {
            *p++ = '?';
        }
        src++;
        i_src--;
    }
    *p++= '\0';

    return dst;
}

void chapter_edition_t::RefreshChapters( input_title_t & title )
{
    int64_t i_prev_user_time = 0;
    std::vector<chapter_item_t>::iterator index = chapters.begin();

    while ( index != chapters.end() )
    {
        i_prev_user_time = (*index).RefreshChapters( b_ordered, i_prev_user_time, title );
        index++;
    }
}

int64_t chapter_item_t::RefreshChapters( bool b_ordered, int64_t i_prev_user_time, input_title_t & title )
{
    int64_t i_user_time = i_prev_user_time;
    
    // first the sub-chapters, and then ourself
    std::vector<chapter_item_t>::iterator index = sub_chapters.begin();
    while ( index != sub_chapters.end() )
    {
        i_user_time = (*index).RefreshChapters( b_ordered, i_user_time, title );
        index++;
    }

    if ( b_ordered )
    {
        i_user_start_time = i_prev_user_time;
        if ( i_end_time != -1 && i_user_time == i_prev_user_time )
        {
            i_user_end_time = i_user_start_time - i_start_time + i_end_time;
        }
        else
        {
            i_user_end_time = i_user_time;
        }
    }
    else
    {
        std::sort( sub_chapters.begin(), sub_chapters.end() );
        i_user_start_time = i_start_time;
        i_user_end_time = i_end_time;
    }

    if (b_display_seekpoint)
    {
        seekpoint_t *sk = vlc_seekpoint_New();

//        sk->i_level = i_level;
        sk->i_time_offset = i_start_time;
        sk->psz_name = strdup( psz_name.c_str() );

        // A start time of '0' is ok. A missing ChapterTime element is ok, too, because '0' is its default value.
        title.i_seekpoint++;
        title.seekpoint = (seekpoint_t**)realloc( title.seekpoint, title.i_seekpoint * sizeof( seekpoint_t* ) );
        title.seekpoint[title.i_seekpoint-1] = sk;
    }

    i_seekpoint_num = title.i_seekpoint;

    return i_user_end_time;
}

double chapter_edition_t::Duration() const
{
    double f_result = 0.0;
    
    if ( chapters.size() )
    {
        std::vector<chapter_item_t>::const_iterator index = chapters.end();
        index--;
        f_result = (*index).i_user_end_time;
    }
    
    return f_result;
}

const chapter_item_t *chapter_item_t::FindTimecode( mtime_t i_user_timecode ) const
{
    const chapter_item_t *psz_result = NULL;

    if (i_user_timecode >= i_user_start_time && i_user_timecode < i_user_end_time)
    {
        std::vector<chapter_item_t>::const_iterator index = sub_chapters.begin();
        while ( index != sub_chapters.end() && psz_result == NULL )
        {
            psz_result = (*index).FindTimecode( i_user_timecode );
            index++;
        }
        
        if ( psz_result == NULL )
            psz_result = this;
    }

    return psz_result;
}

const chapter_item_t *chapter_edition_t::FindTimecode( mtime_t i_user_timecode ) const
{
    const chapter_item_t *psz_result = NULL;

    std::vector<chapter_item_t>::const_iterator index = chapters.begin();
    while ( index != chapters.end() && psz_result == NULL )
    {
        psz_result = (*index).FindTimecode( i_user_timecode );
        index++;
    }

    return psz_result;
}

void demux_sys_t::PreloadFamily( demux_t *p_demux )
{
    matroska_stream_t *p_stream = Stream();
    if ( p_stream )
    {
        matroska_segment_t *p_segment = p_stream->Segment();
        if ( p_segment )
        {
            for (size_t i=0; i<streams.size(); i++)
            {
                streams[i]->PreloadFamily( p_demux, *p_segment );
            }
        }
    }
}

void matroska_stream_t::PreloadFamily( demux_t *p_demux, const matroska_segment_t & of_segment )
{
    for (size_t i=0; i<segments.size(); i++)
    {
        segments[i]->PreloadFamily( p_demux, of_segment );
    }
}

void demux_sys_t::PreloadLinked( demux_t *p_demux )
{
}

bool matroska_segment_t::PreloadFamily( demux_t *p_demux, const matroska_segment_t & of_segment )
{
    if ( b_preloaded )
        return false;

    for (size_t i=0; i<families.size(); i++)
    {
        for (size_t j=0; j<of_segment.families.size(); j++)
        {
            if ( families[i] == of_segment.families[j] )
                return Preload( p_demux );
        }
    }

    return false;
}

bool matroska_segment_t::Preload( demux_t *p_demux )
{
    if ( b_preloaded )
        return false;

    EbmlElement *el = NULL;

    while( ( el = ep->Get() ) != NULL )
    {
        if( MKV_IS_ID( el, KaxInfo ) )
        {
            ParseInfo( p_demux, el );
        }
        else if( MKV_IS_ID( el, KaxTracks ) )
        {
            ParseTracks( p_demux, el );
        }
        else if( MKV_IS_ID( el, KaxSeekHead ) )
        {
            ParseSeekHead( p_demux, el );
        }
        else if( MKV_IS_ID( el, KaxCues ) )
        {
            msg_Dbg( p_demux, "|   + Cues" );
        }
        else if( MKV_IS_ID( el, KaxCluster ) )
        {
            msg_Dbg( p_demux, "|   + Cluster" );

            cluster = (KaxCluster*)el;

            ep->Down();
            /* stop parsing the stream */
            break;
        }
        else if( MKV_IS_ID( el, KaxAttachments ) )
        {
            msg_Dbg( p_demux, "|   + Attachments FIXME TODO (but probably never supported)" );
        }
        else if( MKV_IS_ID( el, KaxChapters ) )
        {
            msg_Dbg( p_demux, "|   + Chapters" );
            ParseChapters( p_demux, el );
        }
        else if( MKV_IS_ID( el, KaxTag ) )
        {
            msg_Dbg( p_demux, "|   + Tags FIXME TODO" );
        }
        else
        {
            msg_Dbg( p_demux, "|   + Unknown (%s)", typeid(*el).name() );
        }
    }

    b_preloaded = true;

    return true;
}
