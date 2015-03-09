/* save to png
 *
 * 2/12/11
 * 	- wrap a class around the png writer
 * 16/7/12
 * 	- compression should be 0-9, not 1-10
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
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
    02110-1301  USA

 */

/*

    These files are distributed with VIPS - http://www.vips.ecs.soton.ac.uk

 */

/*
#define DEBUG_VERBOSE
#define DEBUG
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /*HAVE_CONFIG_H*/
#include <vips/intl.h>

#ifdef HAVE_PNG

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vips/vips.h>

#include "vipspng.h"

typedef struct _VipsForeignSavePng {
	VipsForeignSave parent_object;

	int compression;
	gboolean interlace;
	char *profile;
	VipsForeignPngFilter filter;
} VipsForeignSavePng;

typedef VipsForeignSaveClass VipsForeignSavePngClass;

G_DEFINE_ABSTRACT_TYPE( VipsForeignSavePng, vips_foreign_save_png, 
	VIPS_TYPE_FOREIGN_SAVE );

/* Save a bit of typing.
 */
#define UC VIPS_FORMAT_UCHAR
#define C VIPS_FORMAT_CHAR
#define US VIPS_FORMAT_USHORT
#define S VIPS_FORMAT_SHORT
#define UI VIPS_FORMAT_UINT
#define I VIPS_FORMAT_INT
#define F VIPS_FORMAT_FLOAT
#define X VIPS_FORMAT_COMPLEX
#define D VIPS_FORMAT_DOUBLE
#define DX VIPS_FORMAT_DPCOMPLEX

static int bandfmt_png[10] = {
/* UC  C   US  S   UI  I   F   X   D   DX */
   UC, UC, US, US, US, US, UC, UC, UC, UC
};

static void
vips_foreign_save_png_class_init( VipsForeignSavePngClass *class )
{
	GObjectClass *gobject_class = G_OBJECT_CLASS( class );
	VipsObjectClass *object_class = (VipsObjectClass *) class;
	VipsForeignClass *foreign_class = (VipsForeignClass *) class;
	VipsForeignSaveClass *save_class = (VipsForeignSaveClass *) class;

	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	object_class->nickname = "pngsave_base";
	object_class->description = _( "save png" );

	foreign_class->suffs = vips__png_suffs;

	save_class->saveable = VIPS_SAVEABLE_RGBA;
	save_class->format_table = bandfmt_png;

	VIPS_ARG_INT( class, "compression", 6, 
		_( "Compression" ), 
		_( "Compression factor" ),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET( VipsForeignSavePng, compression ),
		0, 9, 6 );

	VIPS_ARG_BOOL( class, "interlace", 7, 
		_( "Interlace" ), 
		_( "Interlace image" ),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET( VipsForeignSavePng, interlace ),
		FALSE );

	VIPS_ARG_STRING( class, "profile", 11, 
		_( "Profile" ), 
		_( "ICC profile to embed" ),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET( VipsForeignSavePng, profile ),
		NULL );

	VIPS_ARG_FLAGS( class, "filter", 12,
		_( "Filter" ),
		_( "libpng row filter flag(s)" ),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET( VipsForeignSavePng, filter ),
		VIPS_TYPE_FOREIGN_PNG_FILTER,
		VIPS_FOREIGN_PNG_FILTER_ALL );

}

static void
vips_foreign_save_png_init( VipsForeignSavePng *png )
{
	png->compression = 6;
	png->filter = VIPS_FOREIGN_PNG_FILTER_ALL;
}

typedef struct _VipsForeignSavePngFile {
	VipsForeignSavePng parent_object;

	char *filename; 
} VipsForeignSavePngFile;

typedef VipsForeignSavePngClass VipsForeignSavePngFileClass;

G_DEFINE_TYPE( VipsForeignSavePngFile, vips_foreign_save_png_file, 
	vips_foreign_save_png_get_type() );

static int
vips_foreign_save_png_file_build( VipsObject *object )
{
	VipsForeignSave *save = (VipsForeignSave *) object;
	VipsForeignSavePng *png = (VipsForeignSavePng *) object;
	VipsForeignSavePngFile *png_file = (VipsForeignSavePngFile *) object;

	if( VIPS_OBJECT_CLASS( vips_foreign_save_png_file_parent_class )->
		build( object ) )
		return( -1 );

	if( vips__png_write( save->ready, png_file->filename,
		png->compression, png->interlace, png->profile, png->filter ) )
		return( -1 );

	return( 0 );
}

static void
vips_foreign_save_png_file_class_init( VipsForeignSavePngFileClass *class )
{
	GObjectClass *gobject_class = G_OBJECT_CLASS( class );
	VipsObjectClass *object_class = (VipsObjectClass *) class;

	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	object_class->nickname = "pngsave";
	object_class->description = _( "save image to png file" );
	object_class->build = vips_foreign_save_png_file_build;

	VIPS_ARG_STRING( class, "filename", 1, 
		_( "Filename" ),
		_( "Filename to save to" ),
		VIPS_ARGUMENT_REQUIRED_INPUT, 
		G_STRUCT_OFFSET( VipsForeignSavePngFile, filename ),
		NULL );
}

static void
vips_foreign_save_png_file_init( VipsForeignSavePngFile *file )
{
}

typedef struct _VipsForeignSavePngBuffer {
	VipsForeignSavePng parent_object;

	VipsArea *buf;
} VipsForeignSavePngBuffer;

typedef VipsForeignSavePngClass VipsForeignSavePngBufferClass;

G_DEFINE_TYPE( VipsForeignSavePngBuffer, vips_foreign_save_png_buffer, 
	vips_foreign_save_png_get_type() );

static int
vips_foreign_save_png_buffer_build( VipsObject *object )
{
	VipsForeignSave *save = (VipsForeignSave *) object;
	VipsForeignSavePng *png = (VipsForeignSavePng *) object;

	void *obuf;
	size_t olen;
	VipsBlob *blob;

	if( VIPS_OBJECT_CLASS( vips_foreign_save_png_buffer_parent_class )->
		build( object ) )
		return( -1 );

	if( vips__png_write_buf( save->ready, &obuf, &olen,
		png->compression, png->interlace, png->profile, png->filter ) )
		return( -1 );

	blob = vips_blob_new( (VipsCallbackFn) vips_free, obuf, olen );
	g_object_set( object, "buffer", blob, NULL );
	vips_area_unref( VIPS_AREA( blob ) );

	return( 0 );
}

static void
vips_foreign_save_png_buffer_class_init( VipsForeignSavePngBufferClass *class )
{
	GObjectClass *gobject_class = G_OBJECT_CLASS( class );
	VipsObjectClass *object_class = (VipsObjectClass *) class;

	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	object_class->nickname = "pngsave_buffer";
	object_class->description = _( "save image to png buffer" );
	object_class->build = vips_foreign_save_png_buffer_build;

	VIPS_ARG_BOXED( class, "buffer", 1, 
		_( "Buffer" ),
		_( "Buffer to save to" ),
		VIPS_ARGUMENT_REQUIRED_OUTPUT, 
		G_STRUCT_OFFSET( VipsForeignSavePngBuffer, buf ),
		VIPS_TYPE_BLOB );
}

static void
vips_foreign_save_png_buffer_init( VipsForeignSavePngBuffer *buffer )
{
}

typedef struct _VipsForeignSavePngStream {
	VipsForeignSavePng parent_object;

	VipsStreamOutput *stream;
} VipsForeignSavePngStream;

typedef VipsForeignSavePngClass VipsForeignSavePngStreamClass;

G_DEFINE_TYPE( VipsForeignSavePngStream, vips_foreign_save_png_stream, 
	vips_foreign_save_png_get_type() );

static int
vips_foreign_save_png_stream_build( VipsObject *object )
{
	VipsForeignSave *save = (VipsForeignSave *) object;
	VipsForeignSavePng *png = (VipsForeignSavePng *) object;
	VipsForeignSavePngStream *stream = 
		(VipsForeignSavePngStream *) object;

	if( VIPS_OBJECT_CLASS( vips_foreign_save_png_stream_parent_class )->
		build( object ) )
		return( -1 );

	if( vips__png_write_stream( save->ready, stream->stream,
		png->compression, png->interlace, png->profile, png->filter ) )
		return( -1 );

	return( 0 );
}

static void
vips_foreign_save_png_stream_class_init( VipsForeignSavePngStreamClass *class )
{
	GObjectClass *gobject_class = G_OBJECT_CLASS( class );
	VipsObjectClass *object_class = (VipsObjectClass *) class;

	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	object_class->nickname = "pngsave_stream";
	object_class->description = _( "save image to stream as png" );
	object_class->build = vips_foreign_save_png_stream_build;

	VIPS_ARG_STREAM_OUTPUT( class, "stream", 1, 
		_( "Stream" ),
		_( "Stream to write to" ),
		VIPS_ARGUMENT_REQUIRED_INPUT, 
		G_STRUCT_OFFSET( VipsForeignSavePngStream, stream ) ); 
}

static void
vips_foreign_save_png_stream_init( VipsForeignSavePngStream *stream )
{
}

#endif /*HAVE_PNG*/
