/*
 * opngcore/codec.c
 * PNG encoding and decoding.
 *
 * Copyright (C) 2001-2012 Cosmin Truta.
 *
 * This software is distributed under the zlib license.
 * Please see the accompanying LICENSE file.
 */

/*Modified by Felix Hanau.*/

#include <stdio.h>
#include <string.h>

#include "../zlib/zlib.h"

#include "cexcept/cexcept.h"
#include "codec.h"
#include "image.h"
#include "trans.h"
#include "opngcore.h"


/*TODO: */
typedef FILE *(*opng_get_FILE_ptr)(png_voidp io_ptr);

static int opng_sig_is_png(png_structp png_ptr, png_bytep sig, size_t sig_size)
{
    static const png_byte png_file_sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    static const png_byte png_ihdr_sig[8] = {0, 0, 0, 13, 73, 72, 68, 82};

    /* Since png_read_png() fails rather abruptly with png_error(),
     * spend a little more effort to ensure that the format is indeed PNG.
     * Among other things, look for the presence of IHDR.
     */
    if (sig_size <= 43)  /* size of (IHDR + IDAT) > (12+13) + (12+6) */
        return -1;
    int has_png_sig = (memcmp(sig, png_file_sig, 8) == 0);
    if (memcmp(sig + (has_png_sig ? 8 : 0), png_ihdr_sig, 8) != 0)
    {
        png_error(png_ptr, "Not a valid PNG");
    }
    return 1;  /*valid PNG*/
}

static int PNGAPI opng_read_image(png_structp png_ptr, png_infop info_ptr, opng_get_FILE_ptr get_FILE_fn)
{
    /* Precondition. */
#ifdef PNG_FLAG_MALLOC_NULL_MEM_OK
    if (png_ptr->flags & PNG_FLAG_MALLOC_NULL_MEM_OK)
        png_error(png_ptr, "ECT requires a safe allocator");
#endif

    FILE *stream;
    fpos_t fpos;

    /* Read the signature bytes. */
    if (get_FILE_fn != NULL)
        stream = get_FILE_fn(png_get_io_ptr(png_ptr));
    else
        stream = (FILE *)png_get_io_ptr(png_ptr);
    if (fgetpos(stream, &fpos) != 0)
        png_error(png_ptr, "Can't ftell in input file stream");
    png_byte sig[128];
    size_t num = fread(sig, 1, sizeof(sig), stream);
    if (fsetpos(stream, &fpos) != 0)
        png_error(png_ptr, "Can't fseek in input file stream");
    if (opng_sig_is_png(png_ptr, sig, num) > 0)
    {
        png_read_png(png_ptr, info_ptr, 0, NULL);
        if (getc(stream) != EOF)
        {
            png_warning(png_ptr, "Extraneous data found after IEND");
            fseek(stream, 0, SEEK_END);
        }
        return 1;
    }
    return 0;
}

/*
 * User exception setup.
 * See cexcept.h for more info
 */
define_exception_type(const char *);
struct exception_context the_exception_context[1];

/*
 * Encoder tables
 */
static const int filter_table[OPNG_FILTER_MAX + 1] =
{
    PNG_FILTER_NONE  /* 0 */,
    PNG_FILTER_SUB   /* 1 */,
    PNG_FILTER_UP    /* 2 */,
    PNG_FILTER_AVG   /* 3 */,
    PNG_FILTER_PAETH /* 4 */,
    PNG_ALL_FILTERS  /* 5 */
};

/*
 * The chunk signatures recognized and handled by this codec.
 */
const png_byte opng_sig_PLTE[4] = { 0x50, 0x4c, 0x54, 0x45 };
const png_byte opng_sig_tRNS[4] = { 0x74, 0x52, 0x4e, 0x53 };
const png_byte opng_sig_IDAT[4] = { 0x49, 0x44, 0x41, 0x54 };
const png_byte opng_sig_IEND[4] = { 0x49, 0x45, 0x4e, 0x44 };
const png_byte opng_sig_bKGD[4] = { 0x62, 0x4b, 0x47, 0x44 };
const png_byte opng_sig_hIST[4] = { 0x68, 0x49, 0x53, 0x54 };
const png_byte opng_sig_sBIT[4] = { 0x73, 0x42, 0x49, 0x54 };
const png_byte opng_sig_dSIG[4] = { 0x64, 0x53, 0x49, 0x47 };
const png_byte opng_sig_acTL[4] = { 0x61, 0x63, 0x54, 0x4c };
const png_byte opng_sig_fcTL[4] = { 0x66, 0x63, 0x54, 0x4c };
const png_byte opng_sig_fdAT[4] = { 0x66, 0x64, 0x41, 0x54 };

/*
 * Tests whether the given chunk is an image chunk.
 */
int opng_is_image_chunk(const png_byte *chunk_type)
{
    if ((chunk_type[0] & 0x20) == 0)
        return 1;
    /* Although tRNS is listed as ancillary in the PNG specification, it stores
     * alpha samples, which is critical information. For example, tRNS cannot
     * be generally ignored when rendering overlayed images or animations.
     * Lossless operations must treat tRNS as a critical chunk.
     */
    if (memcmp(chunk_type, opng_sig_tRNS, 4) == 0)
        return 1;
    return 0;
}

/*
 * Tests whether the given chunk is an APNG chunk.
 */
int opng_is_apng_chunk(const png_byte *chunk_type)
{
    if (memcmp(chunk_type, opng_sig_acTL, 4) == 0 ||
        memcmp(chunk_type, opng_sig_fcTL, 4) == 0 ||
        memcmp(chunk_type, opng_sig_fdAT, 4) == 0)
        return 1;
    return 0;
}

/*
 * Initializes a stats object.
 */
static void opng_init_stats(struct opng_encoding_stats *stats)
{
    memset(stats, 0, sizeof(*stats));
}

/*
 * Initializes a codec context object.
 */
void opng_init_codec_context(struct opng_codec_context *context, struct opng_image *image, struct opng_encoding_stats *stats, const opng_transformer_t *transformer)
{
    memset(context, 0, sizeof(*context));
    context->image = image;
    context->stats = stats;
    context->transformer = transformer;
    context->expected_idat_size = OPNG_IDAT_SIZE_MAX + 1;
}

/*
 * Read error callback.
 */
static void opng_read_error(png_structp png_ptr, png_const_charp message)
{
    struct opng_codec_context * context = (struct opng_codec_context *)png_get_io_ptr(png_ptr);
    context->stats->flags |= OPNG_HAS_ERRORS;
    Throw message;
}

/*
 * Write error callback.
 */
static void opng_write_error(png_structp png_ptr, png_const_charp message)
{
    (void)png_ptr;
    Throw message;
}

/*
 * Read warning callback.
 */
static void opng_read_warning(png_structp png_ptr, png_const_charp message)
{
    struct opng_codec_context * context = (struct opng_codec_context *)png_get_io_ptr(png_ptr);
    context->stats->flags |= OPNG_HAS_ERRORS;
    opng_warning(context->fname, message);
}

/*
 * Write warning callback.
 */
static void opng_write_warning(png_structp png_ptr, png_const_charp message)
{
    struct opng_codec_context * context = (struct opng_codec_context *)png_get_io_ptr(png_ptr);
    opng_warning(context->fname, message);
}

/*
 * Extension to libpng's unknown chunk handler.
 */
static void opng_set_keep_unknown_chunk(png_structp png_ptr, int keep, png_bytep chunk_type)
{
    png_byte chunk_name[5];

    /* Call png_set_keep_unknown_chunks() once per each chunk type only. */
    memcpy(chunk_name, chunk_type, 4);
    chunk_name[4] = 0;
    if (!png_handle_as_unknown(png_ptr, chunk_name))
        png_set_keep_unknown_chunks(png_ptr, keep, chunk_name, 1);
}

/*
 * Retrieves the file stream from libpng's io_ptr field.
 */
static FILE * opng_user_get_FILE(png_voidp io_ptr)
{
    return ((struct opng_codec_context *)io_ptr)->stream;
}

/*
 * Chunk filter
 */
static int opng_allow_chunk(struct opng_codec_context *context, png_bytep chunk_type)
{
    if (memcmp(chunk_type, opng_sig_dSIG, 4) == 0)
    {
        /* Always block the digital signature chunks. */
        return 0;
    }
    return !opng_transform_query_strip_chunk(context->transformer, chunk_type);
}

/*
 * Chunk handler
 */
static void opng_handle_chunk(png_structp png_ptr, png_bytep chunk_type)
{
    if (opng_is_image_chunk(chunk_type) || (memcmp(chunk_type, opng_sig_PLTE, 4) == 0 || memcmp(chunk_type, opng_sig_tRNS, 4) == 0))
        return;
    struct opng_codec_context * context = (struct opng_codec_context *)png_get_io_ptr(png_ptr);
    struct opng_encoding_stats * stats = context->stats;

    /* Bypass the chunks that are intended to be stripped. */
    if (opng_transform_query_strip_chunk(context->transformer, chunk_type))
    {
        char debug_chunk_name[5];
        memcpy(debug_chunk_name, chunk_type, 4);
        debug_chunk_name[4] = (char)0;
        if (opng_is_apng_chunk(chunk_type))
        {
            opng_printf("Snipping: %s\n", debug_chunk_name);
            stats->flags |= OPNG_HAS_SNIPPED_IMAGES;
        }
        else
        {
            opng_printf("Stripping: %s\n", debug_chunk_name);
            stats->flags |= OPNG_HAS_STRIPPED_METADATA;
        }
        opng_set_keep_unknown_chunk(png_ptr, PNG_HANDLE_CHUNK_NEVER, chunk_type);
        return;
    }

    /* Let libpng handle bKGD, hIST and sBIT. */
    if (memcmp(chunk_type, opng_sig_bKGD, 4) == 0 ||
        memcmp(chunk_type, opng_sig_hIST, 4) == 0 ||
        memcmp(chunk_type, opng_sig_sBIT, 4) == 0)
        return;

    /* Everything else is handled as unknown by libpng. */
    if (memcmp(chunk_type, opng_sig_dSIG, 4) == 0)
        stats->flags |= OPNG_HAS_DIGITAL_SIGNATURE;
    else if (memcmp(chunk_type, opng_sig_fdAT, 4) == 0)
        stats->flags |= OPNG_HAS_MULTIPLE_IMAGES;
    opng_set_keep_unknown_chunk(png_ptr, PNG_HANDLE_CHUNK_ALWAYS, chunk_type);
}

/*
 * Input handler
 */
static void opng_read_data(png_structp png_ptr, png_bytep data, size_t length)
{
    struct opng_codec_context * context = (struct opng_codec_context *)png_get_io_ptr(png_ptr);
    struct opng_encoding_stats * stats = context->stats;
    FILE * stream = context->stream;

    /* Read the data. */
    if (fread(data, 1, length, stream) != length)
        png_error(png_ptr, "Can't read file or unexpected end of file");

    if (stats->file_size == 0)  /* first piece of PNG data */
    {
        OPNG_ASSERT(length == 8, "PNG I/O must start with the first 8 bytes");
        stats->datastream_offset = ftell(stream) - 8;
        if (stats->datastream_offset < 0)
            png_error(png_ptr,"Can't get the file-position indicator in file");
        stats->file_size = (unsigned long)stats->datastream_offset;
    }
    stats->file_size += length;

    /* Handle the optipng-specific events. */
    OPNG_ASSERT((io_state & PNG_IO_READING) && (io_state_loc != 0), "Incorrect info in png_ptr->io_state");
    if ((png_get_io_state(png_ptr) & PNG_IO_MASK_LOC) == PNG_IO_CHUNK_HDR)
    {
        /* In libpng 1.4.x and later, the chunk length and the chunk name
         * are serialized in a single operation. This is also ensured by
         * the opngio add-on for libpng 1.2.x and earlier.
         */
        OPNG_ASSERT(length == 8, "Reading chunk header, expecting 8 bytes");
        png_bytep chunk_sig = data + 4;

        if (memcmp(chunk_sig, opng_sig_IDAT, 4) == 0)
        {
            OPNG_ASSERT(png_ptr == context->libpng_ptr, "Incorrect I/O handler setup");
            if (png_get_rows(context->libpng_ptr, context->info_ptr) == NULL)
            {
                /* This is the first IDAT. Allocate the rows here, bypassing
                 * libpng. This allows to initialize the contents and perform
                 * recovery in case of a premature EOF.
                 */
                OPNG_ASSERT(stats->idat_size == 0, "Found IDAT with no rows");
                if (png_get_image_height(context->libpng_ptr, context->info_ptr) == 0)
                    return;  /* premature IDAT; an error will occur later */
                png_data_freer(context->libpng_ptr, context->info_ptr, PNG_USER_WILL_FREE_DATA, PNG_FREE_ROWS);
            }
            /* Else there is split IDAT overhead. Join IDATs. */
            stats->idat_size += png_get_uint_32(data);
        }
        else
            opng_handle_chunk(png_ptr, chunk_sig);
    }
}

/*
 * Output handler
 */
static void opng_write_data(png_structp png_ptr, png_bytep data, size_t length)
{
    struct opng_codec_context * context = (struct opng_codec_context *)png_get_io_ptr(png_ptr);
    struct opng_encoding_stats * stats = context->stats;
    FILE * stream = context->stream;

    int io_state = png_get_io_state(png_ptr);
    int io_state_loc = io_state & PNG_IO_MASK_LOC;
    OPNG_ASSERT((io_state & PNG_IO_WRITING) && (io_state_loc != 0), "Incorrect info in png_ptr->io_state");

    /* Handle the optipng-specific events. */
    if (io_state_loc == PNG_IO_CHUNK_HDR)
    {
        OPNG_ASSERT(length == 8, "Writing chunk header, expecting 8 bytes");
        png_bytep chunk_sig = data + 4;
        context->crt_chunk_is_allowed = opng_allow_chunk(context, chunk_sig);
        if (memcmp(chunk_sig, opng_sig_IDAT, 4) == 0)
        {
            context->crt_chunk_is_idat = 1;
            stats->idat_size += png_get_uint_32(data);
            /* Abandon the trial if IDAT is bigger than the maximum allowed. */
        }
        else  /* not IDAT */
        {
            context->crt_chunk_is_idat = 0;
        }
    }

    /* Continue only if the current chunk type is allowed. */
    if (io_state_loc != PNG_IO_SIGNATURE && !context->crt_chunk_is_allowed)
        return;

    /* Here comes an elaborate way of writing the data, in which all IDATs
     * are joined into a single chunk.
     * Normally, the user-supplied I/O routines are not so complicated.
     */
    switch (io_state_loc)
    {
    case PNG_IO_CHUNK_HDR:
        if (context->crt_chunk_is_idat)
        {
            if (context->crt_idat_offset == 0)
            {
                /* This is the header of the first IDAT. */
                context->crt_idat_offset = ftell(stream);
                /* Try guessing the size of the final (joined) IDAT. */
                if (context->expected_idat_size <= OPNG_IDAT_SIZE_MAX)
                {
                    /* The guess is expected to be right. */
                    context->crt_idat_size = context->expected_idat_size;
                    /* TODO:
                     * This algorithm can't handle IDAT sizes larger than
                     * the maximum chunk size (2**31 - 1) correctly.
                     */
                    if (context->expected_idat_size > 2147483647)
                        png_error(png_ptr,  "Can't write IDAT if size >= 2GB");
                }
                else
                {
                    /* The guess could be wrong.
                     * The size of the final IDAT will be revised.
                     */
                    context->crt_idat_size = length;
                }
                png_save_uint_32(data, (png_uint_32)context->crt_idat_size);
                /* Start computing the CRC of the final IDAT. */
                context->crt_idat_crc = crc32(0, opng_sig_IDAT, 4);
            }
            else
            {
                /* This is not the first IDAT. Do not write its header. */
                return;
            }
        }
        else
        {
            if (context->crt_idat_offset != 0)
            {
                png_byte buf[4];
                /* This is the header of the first chunk after IDAT.
                 * Finalize IDAT before resuming the normal operation.
                 */
                png_save_uint_32(buf, context->crt_idat_crc);
                if (fwrite(buf, 1, 4, stream) != 4)
                    io_state = 0;  /* error */
                stats->file_size += 4;
                if (stats->idat_size != context->crt_idat_size)
                {
                    /* The IDAT size, unknown at the start of encoding,
                     * has not been guessed correctly.
                     * It must be updated in a non-streamable way.
                     */
                    png_save_uint_32(buf, (png_uint_32)stats->idat_size);
                    fpos_t pos;
                    if (fgetpos(stream, &pos) != 0 || fflush(stream) != 0 || (fseek(stream, context->crt_idat_offset, SEEK_SET) != 0) ||
                        (fwrite(buf, 1, 4, stream)!=4) || (fflush(stream) != 0) || (fsetpos(stream, &pos) != 0)){
                        io_state = 0;}
                    /* Ensure that the IDAT size was indeed unknown. */
                }
                if (io_state == 0)
                    png_error(png_ptr, "Can't finalize IDAT");
                context->crt_idat_offset = 0;
            }
        }
        break;
    case PNG_IO_CHUNK_DATA:
        if (context->crt_chunk_is_idat)
            context->crt_idat_crc = crc32(context->crt_idat_crc, data, length);
        break;
    case PNG_IO_CHUNK_CRC:
        if (context->crt_chunk_is_idat)
            return;  /* defer writing until the first non-IDAT occurs */
        break;
    }

    /* Write the data. */
    if (fwrite(data, 1, length, stream) != length)
        png_error(png_ptr, "Can't write file");
    stats->file_size += length;
}

/*
 * Imports an image from an image file stream.
 * The image may be either in PNG format or in an external file format.
 * The function returns 0 on success or -1 on error.
 */
int opng_decode_image(struct opng_codec_context *context, FILE *stream, const char *fname)
{
    context->libpng_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, opng_read_error, opng_read_warning);
    context->info_ptr = png_create_info_struct(context->libpng_ptr);
    if (context->libpng_ptr == NULL || context->info_ptr == NULL)
    {
        opng_error(NULL, "Out of memory");
        png_destroy_read_struct(&context->libpng_ptr, &context->info_ptr, NULL);
        return -1;
    }

    opng_init_image(context->image);
    const char * volatile err_msg;  /* volatile is required by cexcept */
    struct opng_encoding_stats * stats = context->stats;
    opng_init_stats(stats);
    context->stream = stream;
    context->fname = fname;

    Try
    {
        png_set_keep_unknown_chunks(context->libpng_ptr, PNG_HANDLE_CHUNK_ALWAYS, NULL, 0);
        png_set_read_fn(context->libpng_ptr, context, opng_read_data);
        if (opng_read_image(context->libpng_ptr, context->info_ptr, opng_user_get_FILE) <= 0)
        {
            opng_error(context->fname, "Unrecognized image file format");
            return -1;
        }
        if (stats->file_size == 0)
        {
            if (fseek(context->stream, 0, SEEK_END) == 0)
            {
                stats->file_size = (unsigned long)ftell(context->stream);
                if (stats->file_size > LONG_MAX)
                    stats->file_size = 0;
            }
            if (stats->file_size == 0)
                opng_warning(context->fname, "Can't get the file size");
        }
    }
    Catch (err_msg)
    {
        OPNG_ASSERT(err_msg != NULL, "No error message");
        OPNG_ASSERT(stats->flags & OPNG_HAS_ERRORS, "No error flag");
        if (opng_validate_image(context->libpng_ptr, context->info_ptr))
        {
            /* The critical image info has already been loaded.
             * Treat this error as a warning in order to allow data recovery.
             */
            opng_warning(fname, err_msg);
        }
        else
        {
            opng_error(fname, err_msg);
            return -1;
        }
    }

    opng_load_image(context->image, context->libpng_ptr, context->info_ptr);
    return 0;
}

/*
 * Attempts to reduce the imported image.
 */
int opng_decode_reduce_image(struct opng_codec_context *context, int reductions, bool force_palette_if_possible, bool force_no_palette)
{
    const char * volatile err_msg;  /* volatile is required by cexcept */

    Try
    {
        png_uint_32 result = opng_reduce_image(context->libpng_ptr, context->info_ptr, (png_uint_32)reductions, force_palette_if_possible, force_no_palette);
        if (result != OPNG_REDUCE_NONE)
        {
            /* Write over the old image object. */
            opng_load_image(context->image, context->libpng_ptr, context->info_ptr);
        }
        return (int)result;
    }
    Catch (err_msg)
    {
        opng_error(context->fname, err_msg);
        return -1;
    }

    /* This is not reached, but the compiler can't see that through cexcept. */
    return -1;
}

/*
 * Stops the decoder.
 */
void opng_decode_finish(struct opng_codec_context *context, int free_data)
{
    if (context->libpng_ptr == NULL)
        return;

    int freer;
    if (free_data)
    {
        freer = PNG_DESTROY_WILL_FREE_DATA;
        /* Wipe out the image object, but let libpng deallocate it. */
        opng_init_image(context->image);
    }
    else
        freer = PNG_USER_WILL_FREE_DATA;

    png_data_freer(context->libpng_ptr, context->info_ptr, freer, PNG_FREE_ALL);
    png_destroy_read_struct(&context->libpng_ptr, &context->info_ptr, NULL);
}

/*
 * Encodes an image to a PNG file stream.
 */
int opng_encode_image(struct opng_codec_context *context, int filter, FILE *stream, const char *fname, int mode)
{
    const char * volatile err_msg;  /* volatile is required by cexcept */

    context->libpng_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, opng_write_error, opng_write_warning);
    context->info_ptr = png_create_info_struct(context->libpng_ptr);
    if (context->libpng_ptr == NULL || context->info_ptr == NULL)
    {
        opng_error(NULL, "Out of memory");
        png_destroy_write_struct(&context->libpng_ptr, &context->info_ptr);
        return -1;
    }

    struct opng_encoding_stats * stats = context->stats;
    opng_init_stats(stats);
    context->stream = stream;
    context->fname = fname;

    Try
    {
        png_set_filter(context->libpng_ptr, PNG_FILTER_TYPE_BASE, filter_table[filter]);
        if (mode!=2){
            png_set_compression_level(context->libpng_ptr, mode == 3 ? 7 : 9);
        }
        png_set_compression_mem_level(context->libpng_ptr, 8);
        png_set_compression_window_bits(context->libpng_ptr, 15);
        png_set_compression_strategy(context->libpng_ptr, 0);
        png_set_keep_unknown_chunks(context->libpng_ptr, PNG_HANDLE_CHUNK_ALWAYS, NULL, 0);
        opng_store_image(context->image, context->libpng_ptr, context->info_ptr);

        /* Write the PNG stream. */
        png_set_write_fn(context->libpng_ptr, context, opng_write_data, NULL);
        png_write_png(context->libpng_ptr, context->info_ptr, 0, NULL);
    }
    Catch (err_msg)
    {
        /* err_msg can be NULL if this was an interrupted trial.
         * Set IDAT size to anything beyond OPNG_IDAT_SIZE_MAX.
         */
        stats->idat_size = OPNG_IDAT_SIZE_MAX + 1;
        if (err_msg != NULL)
        {
            opng_error(fname, err_msg);
            return -1;
        }
    }
    return 0;
}

/*
 * Stops the encoder.
 */
void opng_encode_finish(struct opng_codec_context *context)
{
    png_data_freer(context->libpng_ptr, context->info_ptr, PNG_USER_WILL_FREE_DATA, PNG_FREE_ALL);
    png_destroy_write_struct(&context->libpng_ptr, &context->info_ptr);
}

/*
 * Copies a PNG file stream to another PNG file stream.
 */
int opng_copy_png(struct opng_codec_context *context,FILE *in_stream, const char *in_fname,FILE *out_stream, const char *out_fname)
{
    volatile png_bytep buf;  /* volatile is required by cexcept */
    const png_uint_32 buf_size_incr = 0x1000;
    png_uint_32 length;
    png_byte chunk_hdr[8];
    const char * volatile err_msg;
    int volatile result = 0;

    context->libpng_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, opng_write_error, opng_write_warning);
    if (context->libpng_ptr == NULL)
    {
        opng_error(NULL, "Out of memory");
        return -1;
    }

    struct opng_encoding_stats * stats = context->stats;
    opng_init_stats(stats);
    context->stream = out_stream;
    context->fname = out_fname;
    png_set_write_fn(context->libpng_ptr, context, opng_write_data, NULL);

    Try
    {
        buf = NULL;
        png_uint_32 buf_size = 0;

        /* Write the signature in the output file. */
        png_write_sig(context->libpng_ptr);

        /* Copy all chunks until IEND. */
        /* Error checking is done only at a very basic level. */
        do
        {
            if (fread(chunk_hdr, 8, 1, in_stream) != 1)  /* length + name */
            {
                opng_error(in_fname, "Read error");
                result = -1;
                break;
            }
            length = png_get_uint_32(chunk_hdr);
            if (length > PNG_UINT_31_MAX)
            {
                if (buf == NULL && length == 0x89504e47UL)  /* "\x89PNG" */
                    continue;  /* skip the signature */
                opng_error(in_fname, "Data error");
                result = -1;
                break;
            }
            if (length + 4 > buf_size)
            {
                png_free(context->libpng_ptr, buf);
                buf_size = (((length + 4) + (buf_size_incr - 1))
                            / buf_size_incr) * buf_size_incr;
                buf = (png_bytep)png_malloc(context->libpng_ptr, buf_size);
                /* Do not use realloc() here, it's unnecessarily slow. */
            }
            if (fread(buf, length + 4, 1, in_stream) != 1)  /* data + crc */
            {
                opng_error(in_fname, "Read error");
                result = -1;
                break;
            }
            png_write_chunk(context->libpng_ptr, chunk_hdr + 4, buf, length);
        } while (memcmp(chunk_hdr + 4, opng_sig_IEND, 4) != 0);
    }
    Catch (err_msg)
    {
        opng_error(out_fname, err_msg);
        result = -1;
    }

    png_free(context->libpng_ptr, buf);
    png_destroy_write_struct(&context->libpng_ptr, NULL);
    return result;
}
