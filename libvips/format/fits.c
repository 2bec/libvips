/* Read FITS files with cfitsio
 *
 * 26/10/10
 *	- from matlab.c
 * 27/10/10
 * 	- oops, forgot to init status in close
 * 30/11/10
 * 	- set RGB16/GREY16 if appropriate
 * 	- allow up to 10 dimensions as long as they are empty
 * 27/1/11
 * 	- lazy read
 */

/*

    This file is part of VIPS.
    
    VIPS is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

 */

/*

    These files are distributed with VIPS - http://www.vips.ecs.soton.ac.uk

 */

/*
 */
#define DEBUG
#define VIPS_DEBUG

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /*HAVE_CONFIG_H*/
#include <vips/intl.h>

#ifdef HAVE_CFITSIO

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <vips/vips.h>
#include <vips/internal.h>
#include <vips/debug.h>

#include <fitsio.h>

#ifdef WITH_DMALLOC
#include <dmalloc.h>
#endif /*WITH_DMALLOC*/

/*

   new plan: 
   
   	* read single-band only, with band specified in a param 
	* read with y inverted
	* no tile cache (will make large zoom-out very slow)

	... so we can now read whole chunks at once, if the alignment is 
	correct

	on top of this, make a many-band reader by bandjoining lots of 
	one-band readers

	on top of this, flip y





   	TODO

	- test colour read with valgrind

	- ask Doug for a test colour image

		found WFPC2u5780205r_c0fx.fits on the fits samples page, 
		though we don't read it correctly, argh

		when we read a line of a tile out, y and z seem to be swapped

	- read whole tiles, if the alignment is right

		  actually, this is hard, we'd need to flip y somehow

	- test performance

	- remove the old scanline reader?

 */

/* vips only supports 3 dimensions, but we allow up to MAX_DIMENSIONS as long
 * as the higher dimensions are all empty. If you change this value, change
 * fits2vips_get_header() as well.
 */
#define MAX_DIMENSIONS (10)

/* What we track during a cfitsio-file read.
 */
typedef struct {
	char *filename;
	IMAGE *out;

	fitsfile *fptr;
	int datatype;
	int naxis;
	long long int naxes[MAX_DIMENSIONS];

	GMutex *lock;		/* Lock fits_*() calls with this */
} Read;

static void
read_error( int status )
{
	char buf[80];

	fits_get_errstatus( status, buf );
	im_error( "fits", "%s", buf );
}

static void
read_destroy( Read *read )
{
	IM_FREE( read->filename );
	IM_FREEF( g_mutex_free, read->lock );
	if( read->fptr ) {
		int status;

		status = 0;

		if( fits_close_file( read->fptr, &status ) ) 
			read_error( status );

		read->fptr = NULL;
	}

	im_free( read );
}

static Read *
read_new( const char *filename, IMAGE *out )
{
	Read *read;
	int status;

	if( !(read = IM_NEW( NULL, Read )) )
		return( NULL );

	read->filename = im_strdup( NULL, filename );
	read->out = out;
	read->fptr = NULL;
	read->lock = NULL;

	if( im_add_close_callback( out, 
		(im_callback_fn) read_destroy, read, NULL ) ) {
		read_destroy( read );
		return( NULL );
	}

	status = 0;
	if( fits_open_file( &read->fptr, filename, READONLY, &status ) ) {
		im_error( "fits", _( "unable to open \"%s\"" ), filename );
		read_error( status );
		return( NULL );
	}

	read->lock = g_mutex_new();

	return( read );
}

/* fits image types -> VIPS band formats. VIPS doesn't have 64-bit int, so no
 * entry for LONGLONG_IMG (64).
 */
static int fits2vips_formats[][3] = {
	{ BYTE_IMG, IM_BANDFMT_UCHAR, TBYTE },
	{ SHORT_IMG,  IM_BANDFMT_USHORT, TUSHORT },
	{ LONG_IMG,  IM_BANDFMT_UINT, TUINT },
	{ FLOAT_IMG,  IM_BANDFMT_FLOAT, TFLOAT },
	{ DOUBLE_IMG, IM_BANDFMT_DOUBLE, TDOUBLE }
};

static int
fits2vips_get_header( Read *read, IMAGE *out )
{
	int status;
	int bitpix;

	int width, height, bands, format, type;
	int keysexist;
	int morekeys;
	int i;

	status = 0;

	if( fits_get_img_paramll( read->fptr, 
		10, &bitpix, &read->naxis, read->naxes, &status ) ) {
		read_error( status );
		return( -1 );
	}

#ifdef DEBUG
	VIPS_DEBUG_MSG( "naxis = %d\n", read->naxis );
	for( i = 0; i < read->naxis; i++ )
		VIPS_DEBUG_MSG( "%d) %lld\n", i, read->naxes[i] );
#endif /*DEBUG*/

	width = 1;
	height = 1;
	bands = 1;
	switch( read->naxis ) {
	/* If you add more dimensions here, adjust data read below. See also
	 * the definition of MAX_DIMENSIONS above.
	 */
	case 10:
	case 9:
	case 8:
	case 7:
	case 6:
	case 5:
	case 4:
		for( i = read->naxis; i > 3; i-- )
			if( read->naxes[i - 1] != 1 ) {
				im_error( "fits", "%s", _( "dimensions above 3 "
					"must be size 1" ) );
				return( -1 );
			}

	case 3:
		bands = read->naxes[2];

	case 2:
		height = read->naxes[1];

	case 1:
		width = read->naxes[0];
		break;

	default:
		im_error( "fits", _( "bad number of axis %d" ), read->naxis );
		return( -1 );
	}

	/* Get image format. We want the 'raw' format of the image, our caller
	 * can convert using the meta info if they want.
	 */
	for( i = 0; i < IM_NUMBER( fits2vips_formats ); i++ )
		if( fits2vips_formats[i][0] == bitpix )
			break;
	if( i == IM_NUMBER( fits2vips_formats ) ) {
		im_error( "im_fits2vips", _( "unsupported bitpix %d\n" ),
			bitpix );
		return( -1 );
	}
	format = fits2vips_formats[i][1];
	read->datatype = fits2vips_formats[i][2];

	if( bands == 1 ) {
		if( format == IM_BANDFMT_USHORT )
			type = IM_TYPE_GREY16;
		else
			type = IM_TYPE_B_W;
	}
	else if( bands == 3 ) {
		if( format == IM_BANDFMT_USHORT )
			type = IM_TYPE_RGB16;
		else
			type = IM_TYPE_RGB;
	}
	else
		type = IM_TYPE_MULTIBAND;

	im_initdesc( out,
		 width, height, bands,
		 im_bits_of_fmt( format ), format,
		 IM_CODING_NONE, type, 1.0, 1.0, 0, 0 );

	/* Read all keys into meta.
	 */
	if( fits_get_hdrspace( read->fptr, &keysexist, &morekeys, &status ) ) {
		read_error( status );
		return( -1 );
	}

	for( i = 0; i < keysexist; i++ ) {
		char key[81];
		char value[81];
		char comment[81];
		char vipsname[100];

		if( fits_read_keyn( read->fptr, i + 1, 
			key, value, comment, &status ) ) {
			read_error( status );
			return( -1 );
		}

		VIPS_DEBUG_MSG( "fits: seen:\n" );
		VIPS_DEBUG_MSG( " key == %s\n", key );
		VIPS_DEBUG_MSG( " value == %s\n", value );
		VIPS_DEBUG_MSG( " comment == %s\n", comment );

		im_snprintf( vipsname, 100, "fits-%s", key );
		if( im_meta_set_string( out, vipsname, value ) ) 
			return( -1 );
		im_snprintf( vipsname, 100, "fits-%s-comment", key );
		if( im_meta_set_string( out, vipsname, comment ) ) 
			return( -1 );
	}

	return( 0 );
}

static int
fits2vips_header( const char *filename, IMAGE *out )
{
	Read *read;

	VIPS_DEBUG_MSG( "fits2vips_header: reading \"%s\"\n", filename );

	if( !(read = read_new( filename, out )) || 
		fits2vips_get_header( read, out ) ) 
		return( -1 );

	return( 0 );
}

/* Read the whole image in scanlines.

   	kept for reference ... this works for colour fits images

 */
static int
fits2vips_get_data_scanlinewise( Read *read )
{
	IMAGE *im = read->out;
	const int es = IM_IMAGE_SIZEOF_ELEMENT( im );

	PEL *line_buffer;
	PEL *band_buffer;
	PEL *p, *q;
	int x, y, b, z;
	int status;

	long fpixel[MAX_DIMENSIONS];

	status = 0;

	if( !(line_buffer = IM_ARRAY( im, IM_IMAGE_SIZEOF_LINE( im ), PEL )) ||
		!(band_buffer = IM_ARRAY( im, es * im->Xsize, PEL )) ||
		im_outcheck( im ) ||
		im_setupout( im ) )
		return( -1 );

	for( y = 0; y < im->Ysize; y++ ) {
		/* Start of scanline. We have to read top-to-bottom.
		 */
		for( b = 0; b < MAX_DIMENSIONS; b++ )
			fpixel[b] = 1;
		fpixel[1] = im->Ysize - y;

		for( b = 0; b < im->Bands; b++ ) {
			fpixel[2] = b + 1;

			/* Read one band of one scanline, then scatter-write
			 * into the line buffer.
			 */
			if( fits_read_pix( read->fptr, 
				read->datatype, fpixel, im->Xsize,
				NULL, band_buffer, NULL, &status ) ) {
				read_error( status );
				return( -1 );
			}

			p = band_buffer;
			q = line_buffer + b * es;
			for( x = 0; x < im->Xsize; x++ ) {
				for( z = 0; z < es; z++ )
					q[z] = p[z];

				p += es;
				q += im->Bands * es;
			}
		}

		if( im_writeline( y, im, line_buffer ) )
			return( -1 );
	}

	return( 0 );
}

/* Allocate a line buffer. Have one of these for each thread so we can unpack
 * to vips in parallel.
 */
static void *
fits2vips_start( IMAGE *out, void *a, void *b )
{
	Read *read = (Read *) a;
	IMAGE *out = read->out;
	const int es = IM_IMAGE_SIZEOF_ELEMENT( out );

	PEL *band_buffer;

	if( !(band_buffer = IM_ARRAY( out, es * out->Xsize, PEL )) )
		return( NULL );

	return( (void *) band_buffer );
}

static int
fits2vips_stop( void *seq, void *a, void *b )
{
	im_free( seq );

	return( 0 );
}

static int
fits2vips_generate( REGION *out, void *seq, void *a, void *b )
{
	PEL *band_buffer = (PEL *) seq;
	Read *read = (Read *) a;
	Rect *r = &out->valid;

	IMAGE *im = read->out;

	PEL *q;
	int y, z;
	int status;

	status = 0;

	long fpixel[MAX_DIMENSIONS];
	long lpixel[MAX_DIMENSIONS];
	long inc[MAX_DIMENSIONS];

	VIPS_DEBUG_MSG( "fits2vips_generate: "
		"generating left = %d, top = %d, width = %d, height = %d\n", 
		r->left, r->top, r->width, r->height );

	for( y = r->top; y < IM_RECT_BOTTOM( r ); y ++ ) {
		for( z = 0; z < MAX_DIMENSIONS; z++ )
			fpixel[z] = 1;
		fpixel[0] = r->left + 1;
		fpixel[1] = im->Ysize - y;

		for( z = 0; z < MAX_DIMENSIONS; z++ )
			lpixel[z] = 1;
		lpixel[0] = IM_RECT_RIGHT( r );
		lpixel[1] = im->Ysize - y;

		for( z = 0; z < MAX_DIMENSIONS; z++ )
			inc[z] = 1;

		for( z = 0; z < im->Bands; z++ ) {
			/* Read one band of one scanline, then scatter-write
			 * into the line buffer.
			 */
			fpixel[2] = z + 1;
			lpixel[2] = z + 1;

			/* Break on ffgsv() for this call.
			 */
			g_mutex_lock( read->lock );
			if( fits_read_subset( read->fptr, read->datatype, 
				fpixel, lpixel, inc, 
				NULL, band_buffer, NULL, &status ) ) {
				read_error( status );
				g_mutex_unlock( read->lock );
				return( -1 );
			}
			g_mutex_unlock( read->lock );


			p = band_buffer;
			q = line_buffer + b * es;
			for( x = 0; x < im->Xsize; x++ ) {
				for( z = 0; z < es; z++ )
					q[z] = p[z];

				p += es;
				q += im->Bands * es;
			}
		}

		q = (PEL *) IM_REGION_ADDR( out, r->left, y );

		for( z = 0; z < im->Bands; z++ ) {


	}

	return( 0 );
}

/**
 * im_fits2vips:
 * @filename: file to load
 * @out: image to write to
 *
 * Read a FITS image file into a VIPS image. 
 *
 * See also: #VipsFormat.
 *
 * Returns: 0 on success, -1 on error.
 */
int
im_fits2vips( const char *filename, IMAGE *out )
{
	const int tile_size = 128;

	Read *read;
	IMAGE *cache;

	VIPS_DEBUG_MSG( "im_fits2vips: reading \"%s\"\n", filename );

	if( !(cache = im_open_local( out, "cache", "p" )) ||
		!(read = read_new( filename, out )) || 
		fits2vips_get_header( read, cache ) ||
		im_demand_hint( cache, IM_SMALLTILE, NULL ) ||
		im_generate( cache, 
			fits2vips_start, fits2vips_generate, fits2vips_stop, 
			read, NULL ) ||
		im_tile_cache( cache, out, 
			tile_size, tile_size, 
			2 * (1 + cache->Xsize / tile_size) ) ) 
		return( -1 );

	return( 0 );
}

static int
isfits( const char *filename )
{
	fitsfile *fptr;
	int status;

	VIPS_DEBUG_MSG( "isfits: testing \"%s\"\n", filename );

	status = 0;

	if( fits_open_image( &fptr, filename, READONLY, &status ) ) {
		VIPS_DEBUG_MSG( "isfits: error reading \"%s\"\n", filename );
#ifdef DEBUG
		read_error( status );
#endif /*DEBUG*/

		return( 0 );
	}
	fits_close_file( fptr, &status );

	return( 1 );
}

static const char *fits_suffs[] = { ".fits", NULL };

/* fits format adds no new members.
 */
typedef VipsFormat VipsFormatFits;
typedef VipsFormatClass VipsFormatFitsClass;

static void
vips_format_fits_class_init( VipsFormatFitsClass *class )
{
	VipsObjectClass *object_class = (VipsObjectClass *) class;
	VipsFormatClass *format_class = (VipsFormatClass *) class;

	object_class->nickname = "fits";
	object_class->description = _( "FITS" );

	format_class->is_a = isfits;
	format_class->header = fits2vips_header;
	format_class->load = im_fits2vips;
	format_class->save = NULL;
	format_class->suffs = fits_suffs;
}

static void
vips_format_fits_init( VipsFormatFits *object )
{
}

G_DEFINE_TYPE( VipsFormatFits, vips_format_fits, VIPS_TYPE_FORMAT );

#endif /*HAVE_CFITSIO*/
