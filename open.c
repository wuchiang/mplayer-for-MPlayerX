
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "config.h"
#include "mp_msg.h"
#include "help_mp.h"

#ifdef __FreeBSD__
#include <sys/cdrio.h>
#endif

#include "stream.h"
#include "demuxer.h"

#ifdef STREAMING
#include "url.h"
#include "network.h"
static URL_t* url;
#endif

int dvd_title=0;
int dvd_chapter=1;
int dvd_angle=1;

#ifdef USE_DVDREAD

#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_types.h>
#include <dvdread/ifo_read.h>
#include <dvdread/nav_read.h>

#define	DVDREAD_VERSION(maj,min,micro)	((maj)*10000 + (min)*100 + (micro))

/*
 * Try to autodetect the libdvd-0.9.0 library
 * (0.9.0 removed the <dvdread/dvd_udf.h> header, and moved the two defines
 * DVD_VIDEO_LB_LEN and MAX_UDF_FILE_NAME_LEN from it to
 * <dvdread/dvd_reader.h>)
 */
#if defined(DVD_VIDEO_LB_LEN) && defined(MAX_UDF_FILE_NAME_LEN)
#define	LIBDVDREAD_VERSION	DVDREAD_VERSION(0,9,0)
#else
#define	LIBDVDREAD_VERSION	DVDREAD_VERSION(0,8,0)
#endif


typedef struct {
    dvd_reader_t *dvd;
    dvd_file_t *title;
    ifo_handle_t *vmg_file;
    tt_srpt_t *tt_srpt;
    ifo_handle_t *vts_file;
    vts_ptt_srpt_t *vts_ptt_srpt;
    pgc_t *cur_pgc;
    //
    int cur_cell;
    int cur_pack;
    int cell_last_pack;
    // Navi:
    int packs_left;
    dsi_t dsi_pack;
    int angle_seek;
} dvd_priv_t;

#endif

extern int vcd_get_track_end(int fd,int track);

// Open a new stream  (stdin/file/vcd/url)

stream_t* open_stream(char* filename,int vcd_track,int* file_format){
stream_t* stream=NULL;
int f=-1;
off_t len;
#ifdef VCD_CACHE
int vcd_cache_size=128;
#endif
#ifdef __FreeBSD__
int bsize = VCD_SECTOR_SIZE;
#endif

//============ Open VideoCD track ==============
if(vcd_track){
  int ret,ret2;
  if(!filename) filename=DEFAULT_CDROM_DEVICE;
  f=open(filename,O_RDONLY);
  if(f<0){ mp_msg(MSGT_OPEN,MSGL_ERR,MSGTR_CdDevNotfound,filename);return NULL; }
  vcd_read_toc(f);
  ret2=vcd_get_track_end(f,vcd_track);
  if(ret2<0){ mp_msg(MSGT_OPEN,MSGL_ERR,MSGTR_ErrTrackSelect " (get)\n");return NULL;}
  ret=vcd_seek_to_track(f,vcd_track);
  if(ret<0){ mp_msg(MSGT_OPEN,MSGL_ERR,MSGTR_ErrTrackSelect " (seek)\n");return NULL;}
//  seek_to_byte+=ret;
  mp_msg(MSGT_OPEN,MSGL_V,"VCD start byte position: 0x%X  end: 0x%X\n",ret,ret2);
#ifdef VCD_CACHE
  vcd_cache_init(vcd_cache_size);
#endif
#ifdef __FreeBSD__
  if (ioctl (f, CDRIOCSETBLOCKSIZE, &bsize) == -1) {
        perror ( "Error in CDRIOCSETBLOCKSIZE");
  }
#endif
  stream=new_stream(f,STREAMTYPE_VCD);
  stream->start_pos=ret;
  stream->end_pos=ret2;
  return stream;
}

//============ Open DVD title ==============
#ifdef USE_DVDREAD
if(dvd_title){
  int ret,ret2;
  dvd_priv_t *d;
    int ttn,pgc_id,pgn;
    dvd_reader_t *dvd;
    dvd_file_t *title;
    ifo_handle_t *vmg_file;
    tt_srpt_t *tt_srpt;
    ifo_handle_t *vts_file;
    /**
     * Open the disc.
     */
    if(!filename) filename=DEFAULT_DVD_DEVICE;
    dvd = DVDOpen(filename);
    if( !dvd ) {
        mp_msg(MSGT_OPEN,MSGL_ERR, "Couldn't open DVD: %s\n",filename);
        return NULL;
    }

    mp_msg(MSGT_OPEN,MSGL_INFO, "Reading disc structure, please wait...\n");

    /**
     * Load the video manager to find out the information about the titles on
     * this disc.
     */
    vmg_file = ifoOpen( dvd, 0 );
    if( !vmg_file ) {
        mp_msg(MSGT_OPEN,MSGL_ERR, "Can't open VMG info!\n");
        DVDClose( dvd );
        return NULL;
    }
    tt_srpt = vmg_file->tt_srpt;
    /**
     * Make sure our title number is valid.
     */
    mp_msg(MSGT_OPEN,MSGL_INFO, "There are %d titles on this DVD.\n",
             tt_srpt->nr_of_srpts );
    if( dvd_title < 1 || dvd_title > tt_srpt->nr_of_srpts ) {
	mp_msg(MSGT_OPEN,MSGL_ERR, "Invalid DVD title number: %d\n", dvd_title);
        ifoClose( vmg_file );
        DVDClose( dvd );
        return NULL;
    }
    --dvd_title; // remap 1.. -> 0..
    /**
     * Make sure the chapter number is valid for this title.
     */
    mp_msg(MSGT_OPEN,MSGL_INFO, "There are %d chapters in this DVD title.\n",
             tt_srpt->title[dvd_title].nr_of_ptts );
    if( dvd_chapter<1 || dvd_chapter>tt_srpt->title[dvd_title].nr_of_ptts ) {
	mp_msg(MSGT_OPEN,MSGL_ERR, "Invalid DVD chapter number: %d\n", dvd_chapter);
        ifoClose( vmg_file );
        DVDClose( dvd );
        return NULL;
    }
    --dvd_chapter; // remap 1.. -> 0..
    /**
     * Make sure the angle number is valid for this title.
     */
    mp_msg(MSGT_OPEN,MSGL_INFO, "There are %d angles in this DVD title.\n",
             tt_srpt->title[dvd_title].nr_of_angles );
    if( dvd_angle<1 || dvd_angle>tt_srpt->title[dvd_title].nr_of_angles ) {
	mp_msg(MSGT_OPEN,MSGL_ERR, "Invalid DVD angle number: %d\n", dvd_angle);
        ifoClose( vmg_file );
        DVDClose( dvd );
        return NULL;
    }
    --dvd_angle; // remap 1.. -> 0..
    /**
     * Load the VTS information for the title set our title is in.
     */
    vts_file = ifoOpen( dvd, tt_srpt->title[dvd_title].title_set_nr );
    if( !vts_file ) {
	mp_msg(MSGT_OPEN,MSGL_ERR, "Can't open the IFO file for DVD title %d.\n",
                 tt_srpt->title[dvd_title].title_set_nr );
        ifoClose( vmg_file );
        DVDClose( dvd );
        return NULL;
    }
    /**
     * We've got enough info, time to open the title set data.
     */
    title = DVDOpenFile( dvd, tt_srpt->title[dvd_title].title_set_nr,
                         DVD_READ_TITLE_VOBS );
    if( !title ) {
	mp_msg(MSGT_OPEN,MSGL_ERR, "Can't open title VOBS (VTS_%02d_1.VOB).\n",
                 tt_srpt->title[dvd_title].title_set_nr );
        ifoClose( vts_file );
        ifoClose( vmg_file );
        DVDClose( dvd );
        return NULL;
    }

    mp_msg(MSGT_OPEN,MSGL_INFO, "DVD successfully opened!\n");
    // store data
    d=malloc(sizeof(dvd_priv_t)); memset(d,0,sizeof(dvd_priv_t));
    d->dvd=dvd;
    d->title=title;
    d->vmg_file=vmg_file;
    d->tt_srpt=tt_srpt;
    d->vts_file=vts_file;

    /**
     * Determine which program chain we want to watch.  This is based on the
     * chapter number.
     */
    ttn = tt_srpt->title[ dvd_title ].vts_ttn; // local
    pgc_id = vts_file->vts_ptt_srpt->title[ttn-1].ptt[dvd_chapter].pgcn; // local
    pgn    = vts_file->vts_ptt_srpt->title[ttn-1].ptt[dvd_chapter].pgn;  // local
    d->cur_pgc = vts_file->vts_pgcit->pgci_srp[pgc_id-1].pgc;
    d->cur_cell = d->cur_pgc->program_map[pgn-1] - 1; // start playback here
    d->packs_left=-1;      // for Navi stuff
    d->angle_seek=0;
    
    if( d->cur_pgc->cell_playback[d->cur_cell].block_type 
	== BLOCK_TYPE_ANGLE_BLOCK ) d->cur_cell+=dvd_angle;
    d->cur_pack = d->cur_pgc->cell_playback[ d->cur_cell ].first_sector;
    d->cell_last_pack=d->cur_pgc->cell_playback[ d->cur_cell ].last_sector;
    mp_msg(MSGT_DVD,MSGL_V, "DVD start cell: %d  pack: 0x%X-0x%X  \n",d->cur_cell,d->cur_pack,d->cell_last_pack);

    // ... (unimplemented)
//    return NULL;
  stream=new_stream(-1,STREAMTYPE_DVD);
  stream->start_pos=(off_t)d->cur_pack*2048;
  //stream->end_pos=0;
  stream->priv=(void*)d;
  return stream;
}
#endif

//============ Open STDIN ============
  if(!strcmp(filename,"-")){
      // read from stdin
      mp_msg(MSGT_OPEN,MSGL_INFO,MSGTR_ReadSTDIN);
      f=0; // 0=stdin
      stream=new_stream(f,STREAMTYPE_STREAM);
      return stream;
  }
  
#ifdef STREAMING
  url = url_new(filename);
  if(url) {
        (*file_format)=autodetectProtocol( url, &f );
        if( (*file_format)==DEMUXER_TYPE_UNKNOWN ) { 
          mp_msg(MSGT_OPEN,MSGL_ERR,MSGTR_UnableOpenURL, filename);
          url_free(url);
          return NULL;
        }
        f=streaming_start( &url, f, *file_format );
        if(f<0){ mp_msg(MSGT_OPEN,MSGL_ERR,MSGTR_UnableOpenURL, url->url); return NULL; }
        mp_msg(MSGT_OPEN,MSGL_INFO,MSGTR_ConnToServer, url->hostname );
        stream=new_stream(f,STREAMTYPE_STREAM);
	return NULL;
  }
#endif

//============ Open plain FILE ============
       f=open(filename,O_RDONLY);
       if(f<0){ mp_msg(MSGT_OPEN,MSGL_ERR,MSGTR_FileNotFound,filename);return NULL; }
       len=lseek(f,0,SEEK_END); lseek(f,0,SEEK_SET);
       if (len == -1)
	 perror("Error: lseek failed to obtain video file size");
       else
        if(verbose)
#ifdef _LARGEFILE_SOURCE
	 mp_msg(MSGT_OPEN,MSGL_V,"File size is %lld bytes\n", (long long)len);
#else
	 mp_msg(MSGT_OPEN,MSGL_V,"File size is %u bytes\n", (unsigned int)len);
#endif
       stream=new_stream(f,STREAMTYPE_FILE);
       stream->end_pos=len;
       return stream;

}


#ifdef USE_DVDREAD

static int dvd_next_cell(dvd_priv_t *d){
    int next_cell=d->cur_cell;

    mp_msg(MSGT_DVD,MSGL_V, "dvd_next_cell: next1=0x%X  \n",next_cell);
    
    if( d->cur_pgc->cell_playback[ next_cell ].block_type
                                        == BLOCK_TYPE_ANGLE_BLOCK ) {
	    int i;
	    while(next_cell<d->cur_pgc->nr_of_cells){
                if( d->cur_pgc->cell_playback[next_cell].block_mode
                                          == BLOCK_MODE_LAST_CELL ) break;
		++next_cell;
            }
    }
    mp_msg(MSGT_DVD,MSGL_V, "dvd_next_cell: next2=0x%X  \n",next_cell);
    
    ++next_cell;
    if(next_cell>=d->cur_pgc->nr_of_cells) return -1; // EOF
    if( d->cur_pgc->cell_playback[next_cell].block_type == BLOCK_TYPE_ANGLE_BLOCK ){
	next_cell+=dvd_angle;
	if(next_cell>=d->cur_pgc->nr_of_cells) return -1; // EOF
    }
    mp_msg(MSGT_DVD,MSGL_V, "dvd_next_cell: next3=0x%X  \n",next_cell);
    return next_cell;
}

int dvd_read_sector(dvd_priv_t *d,unsigned char* data){
    int len;
    
    if(d->packs_left==0){
            /**
             * If we're not at the end of this cell, we can determine the next
             * VOBU to display using the VOBU_SRI information section of the
             * DSI.  Using this value correctly follows the current angle,
             * avoiding the doubled scenes in The Matrix, and makes our life
             * really happy.
             *
             * Otherwise, we set our next address past the end of this cell to
             * force the code above to go to the next cell in the program.
             */
            if( d->dsi_pack.vobu_sri.next_vobu != SRI_END_OF_CELL ) {
                d->cur_pack= d->dsi_pack.dsi_gi.nv_pck_lbn +
		( d->dsi_pack.vobu_sri.next_vobu & 0x7fffffff );
		mp_msg(MSGT_DVD,MSGL_DBG2, "Navi  new pos=0x%X  \n",d->cur_pack);
            } else {
		// end of cell! find next cell!
		mp_msg(MSGT_DVD,MSGL_V, "--- END OF CELL !!! ---\n");
		d->cur_pack=d->cell_last_pack+1;
            }
    }

read_next:

    if(d->cur_pack>d->cell_last_pack){
	// end of cell!
	int next=dvd_next_cell(d);
	if(next>=0){
	    d->cur_cell=next;

//    if( d->cur_pgc->cell_playback[d->cur_cell].block_type 
//	== BLOCK_TYPE_ANGLE_BLOCK ) d->cur_cell+=dvd_angle;
    d->cur_pack = d->cur_pgc->cell_playback[ d->cur_cell ].first_sector;
    d->cell_last_pack=d->cur_pgc->cell_playback[ d->cur_cell ].last_sector;
    mp_msg(MSGT_DVD,MSGL_V, "DVD next cell: %d  pack: 0x%X-0x%X  \n",d->cur_cell,d->cur_pack,d->cell_last_pack);
	    
	} else return -1; // EOF
    }

    len = DVDReadBlocks( d->title, d->cur_pack, 1, data );
    if(!len) return -1; //error
    
    if(data[38]==0 && data[39]==0 && data[40]==1 && data[41]==0xBF &&
       data[1024]==0 && data[1025]==0 && data[1026]==1 && data[1027]==0xBF){
	// found a Navi packet!!!
#if LIBDVDREAD_VERSION >= DVDREAD_VERSION(0,9,0)
        navRead_DSI( &d->dsi_pack, &(data[ DSI_START_BYTE ]) );
#else
        navRead_DSI( &d->dsi_pack, &(data[ DSI_START_BYTE ]), sizeof(dsi_t) );
#endif
	if(d->cur_pack != d->dsi_pack.dsi_gi.nv_pck_lbn ){
	    mp_msg(MSGT_DVD,MSGL_V, "Invalid NAVI packet! lba=0x%X  navi=0x%X  \n",
		d->cur_pack,d->dsi_pack.dsi_gi.nv_pck_lbn);
	} else {
	    // process!
    	    d->packs_left = d->dsi_pack.dsi_gi.vobu_ea;
	    mp_msg(MSGT_DVD,MSGL_DBG2, "Found NAVI packet! lba=0x%X  len=%d  \n",d->cur_pack,d->packs_left);
	    if(d->angle_seek){
		int skip=d->dsi_pack.sml_agli.data[dvd_angle].address;
		if(skip) d->cur_pack=d->dsi_pack.dsi_gi.nv_pck_lbn+skip;
		d->angle_seek=0;
		mp_msg(MSGT_DVD,MSGL_V, "Angle-seek synced! skip=%d  new_lba=0x%X  \n",skip,d->cur_pack);
	    }
	}
	++d->cur_pack;
	goto read_next;
    }

    ++d->cur_pack;
    if(d->packs_left>=0) --d->packs_left;
    
    if(d->angle_seek) goto read_next; // searching for Navi packet

    return d->cur_pack-1;
}

void dvd_seek(dvd_priv_t *d,int pos){
    d->packs_left=-1;
    d->cur_pack=pos;
    
// check if we stay in current cell (speedup things, and avoid angle skip)
if(d->cur_pack>d->cell_last_pack ||
   d->cur_pack<d->cur_pgc->cell_playback[ d->cur_cell ].first_sector){

    // ok, cell change, find the right cell!
    d->cur_cell=0;
    if( d->cur_pgc->cell_playback[d->cur_cell].block_type 
	== BLOCK_TYPE_ANGLE_BLOCK ) d->cur_cell+=dvd_angle;

  while(1){
    int next;
    d->cell_last_pack=d->cur_pgc->cell_playback[ d->cur_cell ].last_sector;
    if(d->cur_pack<d->cur_pgc->cell_playback[ d->cur_cell ].first_sector){
	d->cur_pack=d->cur_pgc->cell_playback[ d->cur_cell ].first_sector;
	break;
    }
    if(d->cur_pack<=d->cell_last_pack) break; // ok, we find it! :)
    next=dvd_next_cell(d);
    if(next<0){
//	d->cur_pack=d->cell_last_pack+1;
	break; // we're after the last cell
    }
    d->cur_cell=next;
  }

}

mp_msg(MSGT_DVD,MSGL_V, "DVD Seek! lba=0x%X  cell=%d  packs: 0x%X-0x%X  \n",
    d->cur_pack,d->cur_cell,d->cur_pgc->cell_playback[ d->cur_cell ].first_sector,d->cell_last_pack);

// if we're in interleaved multi-angle cell, find the right angle chain!
// (read Navi block, and use the seamless angle jump table)
d->angle_seek=1;

}

#endif
