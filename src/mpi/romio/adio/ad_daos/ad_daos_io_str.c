/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil ; -*- */

/** Copy list unrolling algorithm from PVFS2 implementation */

#include "ad_daos.h"
#include "adio_extern.h"
#include <assert.h>

//#define DEBUG_LIST
//#define DEBUG_LIST2

enum {
    DAOS_WRITE,
    DAOS_READ
};

static MPIX_Grequest_class ADIOI_DAOS_greq_class = 0;
int ADIOI_DAOS_aio_free_fn(void *extra_state);
int ADIOI_DAOS_aio_poll_fn(void *extra_state, MPI_Status *status);
int ADIOI_DAOS_aio_wait_fn(int count, void ** array_of_states,
                           double timeout, MPI_Status *status);

static void print_buf_file_ol_pairs(int64_t buf_off_arr[],
                                    int32_t buf_len_arr[],
                                    int32_t buf_ol_count,
                                    int64_t file_off_arr[],
                                    int32_t file_len_arr[],
                                    int32_t file_ol_count,
                                    void *buf,
                                    int rw_type);

#define COALESCE_REGIONS  /* TODO: would we ever want to *not* coalesce? */
#define MAX_OL_COUNT 64
int ADIOI_DAOS_StridedListIO(ADIO_File fd, void *buf, int count,
			      MPI_Datatype datatype, int file_ptr_type,
			      ADIO_Offset offset, ADIO_Status *status,
			      int *error_code, int rw_type)
{
    /* list I/O parameters */
    int i = -1, ret = -1;
    int tmp_filetype_size = -1;
    int64_t cur_io_size = 0, io_size = 0;
    int etype_size = -1;
    int num_etypes_in_filetype = -1, num_filetypes = -1;
    int etypes_in_filetype = -1, size_in_filetype = -1;
    int bytes_into_filetype = 0;
    
    /* parameters for offset-length pairs arrays */
    int64_t buf_off_arr[MAX_OL_COUNT];
    int32_t buf_len_arr[MAX_OL_COUNT];
    int64_t file_off_arr[MAX_OL_COUNT];
    int32_t file_len_arr[MAX_OL_COUNT];
    int32_t buf_ol_count = 0;
    int32_t file_ol_count = 0;
    
    /* parameters for flattened memory and file datatypes*/
    int flat_buf_index = 0;
    int flat_file_index = 0;
    int64_t cur_flat_buf_reg_off = 0;
    int64_t cur_flat_file_reg_off = 0;
    ADIOI_Flatlist_node *flat_buf_p, *flat_file_p;
    MPI_Count buftype_size = -1, filetype_size = -1;
    MPI_Aint filetype_extent = -1, buftype_extent = -1;;
    int buftype_is_contig = -1, filetype_is_contig = -1;

    struct ADIO_DAOS_cont *cont = fd->fs_ptr;
    static char myname[] = "ADIOI_DAOS_STRIDED_LISTIO";

    MPI_Type_size_x(fd->filetype, &filetype_size);
    if (filetype_size == 0) {
        *error_code = MPI_SUCCESS;
        return -1;
    }
    MPI_Type_extent(fd->filetype, &filetype_extent);
    MPI_Type_size_x(datatype, &buftype_size);
    MPI_Type_extent(datatype, &buftype_extent);
    io_size = buftype_size*count;

    /* Flatten the memory datatype
     * (file datatype has already been flattened in ADIO open
     * unless it is contibuous, then we need to flatten it manually)
     * and set the correct buffers for flat_buf and flat_file */
    ADIOI_Datatype_iscontig(datatype, &buftype_is_contig);
    ADIOI_Datatype_iscontig(fd->filetype, &filetype_is_contig);
    if (buftype_is_contig == 0)
    {
	flat_buf_p = ADIOI_Flatten_and_find(datatype);
    }
    else 
    {
	/* flatten and add to the list */
	flat_buf_p = (ADIOI_Flatlist_node *) ADIOI_Malloc
	    (sizeof(ADIOI_Flatlist_node));
	flat_buf_p->blocklens = (ADIO_Offset*)ADIOI_Malloc(sizeof(ADIO_Offset));
	flat_buf_p->indices = 
		(ADIO_Offset *) ADIOI_Malloc(sizeof(ADIO_Offset));
	/* For the buffer, we can optimize the buftype, this is not 
	 * possible with the filetype since it is tiled */
	buftype_size = buftype_size*count;
	buftype_extent = buftype_size*count;
	flat_buf_p->blocklens[0] = buftype_size;
	flat_buf_p->indices[0] = 0;
	flat_buf_p->count = 1;
    }
    if (filetype_is_contig == 0)
    {
	    /* TODO: why does avery say this should already have been
	     * flattened in Open, but also says contig types don't get
	     * flattened */
	flat_file_p = ADIOI_Flatten_and_find(fd->filetype);
    }
    else
    {
        /* flatten and add to the list */
        flat_file_p = (ADIOI_Flatlist_node *) ADIOI_Malloc
            (sizeof(ADIOI_Flatlist_node));
        flat_file_p->blocklens =(ADIO_Offset*)ADIOI_Malloc(sizeof(ADIO_Offset));
        flat_file_p->indices = 
		(ADIO_Offset *) ADIOI_Malloc(sizeof(ADIO_Offset));
        flat_file_p->blocklens[0] = filetype_size;
        flat_file_p->indices[0] = 0;
        flat_file_p->count = 1;
    }
        
    /* Find out where we are in the flattened filetype (the block index, 
     * how far into the block, and how many bytes_into_filetype)
     * If the file_ptr_type == ADIO_INDIVIDUAL we will use disp, fp_ind 
     * to figure this out (offset should always be zero) 
     * If file_ptr_type == ADIO_EXPLICIT, we will use disp and offset 
     * to figure this out. */

    etype_size = fd->etype_size;
    num_etypes_in_filetype = filetype_size / etype_size;
    if (file_ptr_type == ADIO_INDIVIDUAL)
    {
	int flag = 0;

	/* Should have already been flattened in ADIO_Open*/
	num_filetypes = -1;
	while (!flag)
	{
	    num_filetypes++;
	    for (i = 0; i < flat_file_p->count; i++)
	    {
		/* Start on a non zero-length region */
		if (flat_file_p->blocklens[i])
		{
		    if (fd->disp + flat_file_p->indices[i] +
			(num_filetypes * filetype_extent) +
			flat_file_p->blocklens[i] > fd->fp_ind &&
			fd->disp + flat_file_p->indices[i] <=
			fd->fp_ind)
		    {
			flat_file_index = i;
			cur_flat_file_reg_off = fd->fp_ind -
			    (fd->disp + flat_file_p->indices[i] +
			     (num_filetypes * filetype_extent));
			flag = 1;
			break;
		    }
		    else
			bytes_into_filetype += flat_file_p->blocklens[i];
		}
	    }
	}
	/* Impossible that we don't find it in this datatype */
	assert(i != flat_file_p->count);
    }
    else
    {
	num_filetypes = (int) (offset / num_etypes_in_filetype);
	etypes_in_filetype = (int) (offset % num_etypes_in_filetype);
	size_in_filetype = etypes_in_filetype * etype_size;

	tmp_filetype_size = 0;
	for (i=0; i<flat_file_p->count; i++) {
	    tmp_filetype_size += flat_file_p->blocklens[i];
	    if (tmp_filetype_size > size_in_filetype) 
	    {
		flat_file_index = i;
		cur_flat_file_reg_off = flat_file_p->blocklens[i] - 
		    (tmp_filetype_size - size_in_filetype);
                break;
            }
            else
                bytes_into_filetype += flat_file_p->blocklens[i];
#if 0
                bytes_into_filetype = flat_file_p->indices[i] +
                    size_in_filetype - (tmp_filetype_size - flat_file_p->blocklens[i]);
                fprintf(stderr, "%d: indices %d + size_in_filetype %d - (tmp_filetype_size %d - blocklens %d)\n",
                        i, flat_file_p->indices[i], size_in_filetype, tmp_filetype_size, 
                        flat_file_p->blocklens[i]);
		//bytes_into_filetype = offset * filetype_size -
                //flat_file_p->blocklens[i];
		break;
#endif
	}
    }

#ifdef DEBUG_LIST
    fprintf(stderr, "ADIOI_DAOS_StridedListIO: (fd->fp_ind=%Ld,fd->disp=%Ld,"
            " offset=%Ld)\n(flat_file_index=%d,cur_flat_file_reg_off=%Ld,"
	    "bytes_into_filetype=%d)\n",
            fd->fp_ind, fd->disp, offset, flat_file_index, 
	    cur_flat_file_reg_off, bytes_into_filetype);
#endif
#ifdef DEBUG_LIST2
    fprintf(stderr, "flat_buf:\n");
    for (i = 0; i < flat_buf_p->count; i++)
	fprintf(stderr, "(offset, length) = (%Ld, %d)\n",
	       flat_buf_p->indices[i],
	       flat_buf_p->blocklens[i]);
    fprintf(stderr, "flat_file:\n");
    for (i = 0; i < flat_file_p->count; i++)
	fprintf(stderr, "(offset, length) = (%Ld, %d)\n",
	       flat_file_p->indices[i],
	       flat_file_p->blocklens[i]);
#endif    

    /* total data written */
    cur_io_size = 0;
    while (cur_io_size != io_size)
    {
	/* Initialize the temporarily unrolling lists and 
	 * and associated variables */
	buf_ol_count = 0;
	file_ol_count = 0;
	for (i = 0; i < MAX_OL_COUNT; i++)
	{
	    buf_off_arr[i] = 0;
	    buf_len_arr[i] = 0;
	    file_off_arr[i] = 0;
	    file_len_arr[i] = 0;
	}

        /* Generate the offset-length pairs for a
         * list I/O operation */
	gen_listio_arr(flat_buf_p,
		       &flat_buf_index,
		       &cur_flat_buf_reg_off,
		       buftype_size,
		       buftype_extent,
		       flat_file_p,
		       &flat_file_index,
		       &cur_flat_file_reg_off,
		       filetype_size,
		       filetype_extent,
		       MAX_OL_COUNT,
		       fd->disp,
		       bytes_into_filetype,
		       &cur_io_size,
		       io_size,
		       buf_off_arr,
		       buf_len_arr,
		       &buf_ol_count,
		       file_off_arr,
		       file_len_arr,
		       &file_ol_count);

	assert(buf_ol_count <= MAX_OL_COUNT);
	assert(file_ol_count <= MAX_OL_COUNT);
#ifdef DEBUG_LIST2
	print_buf_file_ol_pairs(buf_off_arr,
				buf_len_arr,
				buf_ol_count,
				file_off_arr,
				file_len_arr,
				file_ol_count,
				buf,
				rw_type);
#endif

        /* Create DAOS SGL */
        daos_sg_list_t sgl;
        daos_iov_t iovs[MAX_OL_COUNT];
        sgl.sg_nr.num = buf_ol_count;
        sgl.sg_nr.num_out = 0;
        for (i = 0; i < buf_ol_count; i++)
            daos_iov_set(&iovs[i], (char *) buf + buf_off_arr[i], buf_len_arr[i]);
        sgl.sg_iovs = iovs;

        /* Create DAOS Array ranges */
        daos_array_ranges_t ranges;
        daos_range_t rg[MAX_OL_COUNT];
        ranges.arr_nr = file_ol_count;
        for (i = 0; i < file_ol_count; i++) {
            rg[i].rg_len = file_len_arr[i];
            rg[i].rg_idx = file_off_arr[i];
            fprintf(stderr, "%d: off %lld len %zu\n", i, file_off_arr[i], file_len_arr[i]);
        }
        ranges.arr_rgs = rg;

	/* Run list I/O operation */
	if (rw_type == DAOS_WRITE)
	{
            ret = daos_array_write(cont->oh, cont->epoch, &ranges, &sgl,
                                   NULL, NULL);
            if (ret != 0) {
                printf("daos_array_write() failed with %d\n", ret);
                *error_code = MPIO_Err_create_code(MPI_SUCCESS,
                                                   MPIR_ERR_RECOVERABLE,
                                                   myname, __LINE__,
                                                   ADIOI_DAOS_error_convert(ret),
                                                   "Error in daos_array_write",
                                                   0);
                return;
            }
	} else {
            ret = daos_array_read(cont->oh, cont->epoch, &ranges, &sgl,
                                  NULL, NULL);
            if (ret != 0) {
                printf("daos_array_read() failed with %d\n", ret);
                *error_code = MPIO_Err_create_code(MPI_SUCCESS,
                                                   MPIR_ERR_RECOVERABLE,
                                                   myname, __LINE__,
                                                   ADIOI_DAOS_error_convert(ret),
                                                   "Error in daos_array_read",
                                                   0);
                return;
            }
	}
    }

    if (file_ptr_type == ADIO_INDIVIDUAL) 
	fd->fp_ind += io_size;
    *error_code = MPI_SUCCESS;

error_state:
#ifdef HAVE_STATUS_SET_BYTES
    /* TODO: why the cast? */
    MPIR_Status_set_bytes(status, datatype, io_size);
/* This is a temporary way of filling in status. The right way is to
   keep track of how much data was actually written by ADIOI_BUFFERED_WRITE. */
#endif
    if (buftype_is_contig != 0)
    {
	ADIOI_Free(flat_buf_p->blocklens);
	ADIOI_Free(flat_buf_p->indices);
	ADIOI_Free(flat_buf_p);
    }

    if (filetype_is_contig != 0)
    {
	ADIOI_Free(flat_file_p->blocklens);
	ADIOI_Free(flat_file_p->indices);
	ADIOI_Free(flat_file_p);
    }

    return 0;
}

/* To do: Fix the code to coalesce the offset-length pairs for memory
 * and file. */

/* gen_listio_arr - fills in offset-length pairs for memory and file
 * for list I/O */
int gen_listio_arr(ADIOI_Flatlist_node *flat_buf_p,
		   int *flat_buf_index_p,
		   int64_t *cur_flat_buf_reg_off_p,
		   int flat_buf_size,
		   int flat_buf_extent,
		   ADIOI_Flatlist_node *flat_file_p,
		   int *flat_file_index_p,
		   int64_t *cur_flat_file_reg_off_p,
		   int flat_file_size,
		   int flat_file_extent,
		   int max_ol_count,
		   ADIO_Offset disp,
		   int bytes_into_filetype,
		   int64_t *bytes_completed,
		   int64_t total_io_size,
		   int64_t buf_off_arr[],
		   int32_t buf_len_arr[],
		   int32_t *buf_ol_count_p,
		   int64_t file_off_arr[],
		   int32_t file_len_arr[],
		   int32_t *file_ol_count_p)
{
    int region_size = -1;
    
    /* parameters for flattened memory and file datatypes*/
    int64_t cur_flat_buf_reg_left = 0;
    int64_t cur_flat_file_reg_left = 0;
    
#ifdef DEBUG_LIST2
    fprintf(stderr, "gen_list_arr:\n");
#endif

    if ((*buf_ol_count_p) != 0 ||(*file_ol_count_p) != 0)
    {
	fprintf(stderr, "buf_ol_count != 0 || file_ol_count != 0\n");
	return -1;
    }
    
    /* Start on a non-zero memory and file region 
     * Note this does not affect the bytes_completed 
     * since no data is in these regions.  Initialize the 
     * first memory and file offsets. */
    while (flat_buf_p->blocklens[(*flat_buf_index_p)] == 0)
    {
	(*flat_buf_index_p) = ((*flat_buf_index_p) + 1) % 
	    flat_buf_p->count;
    }
    buf_off_arr[*buf_ol_count_p] =
	(*bytes_completed / flat_buf_size) * 
	flat_buf_extent + 
	flat_buf_p->indices[*flat_buf_index_p] +
	*cur_flat_buf_reg_off_p;
    buf_len_arr[*buf_ol_count_p] = 0;

    while (flat_file_p->blocklens[(*flat_file_index_p)] == 0)
    {
	(*flat_file_index_p) = ((*flat_file_index_p) + 1) % 
	    flat_file_p->count;
    }
    file_off_arr[*file_ol_count_p] = disp + 
	(((bytes_into_filetype + *bytes_completed) / flat_file_size) * 
	 flat_file_extent) + 
	flat_file_p->indices[*flat_file_index_p] +
	*cur_flat_file_reg_off_p;
    file_len_arr[*file_ol_count_p] = 0;

#ifdef DEBUG_LIST2
    fprintf(stderr, "%d + (((%d + %d) / %d) * %d) + %d + %d\n",
            (int)disp, (int)bytes_into_filetype, (int)(*bytes_completed),
            (int)flat_file_size, (int)flat_file_extent,
            (int)flat_file_p->indices[*flat_file_index_p], (int)(*cur_flat_file_reg_off_p));
    fprintf(stderr, "initial buf_off_arr[%d] = %Ld\n", *buf_ol_count_p,
	    buf_off_arr[*buf_ol_count_p]);
    fprintf(stderr, "initial file_off_arr[%d] = %Ld\n", *file_ol_count_p,
	    file_off_arr[*file_ol_count_p]);
#endif

    while (*bytes_completed != total_io_size
	   && (*buf_ol_count_p) < max_ol_count
	   && (*file_ol_count_p) < max_ol_count)
    {
	/* How much data is left in the current piece in
	 * the flattened datatypes */
	cur_flat_buf_reg_left = flat_buf_p->blocklens[*flat_buf_index_p]
	    - *cur_flat_buf_reg_off_p;
	cur_flat_file_reg_left = flat_file_p->blocklens[*flat_file_index_p]
	    - *cur_flat_file_reg_off_p;

#ifdef DEBUG_LIST2
	fprintf(stderr, 
		"flat_buf_index=%d flat_buf->blocklens[%d]=%d\n"
		"cur_flat_buf_reg_left=%Ld "
		"*cur_flat_buf_reg_off_p=%Ld\n" 
		"flat_file_index=%d flat_file->blocklens[%d]=%d\n"
		"cur_flat_file_reg_left=%Ld "
		"*cur_flat_file_reg_off_p=%Ld\n" 
		"bytes_completed=%Ld\n"
		"buf_ol_count=%d file_ol_count=%d\n"
		"buf_len_arr[%d]=%d file_len_arr[%d]=%d\n\n",
		*flat_buf_index_p, *flat_buf_index_p, 
		flat_buf_p->blocklens[*flat_buf_index_p],
		cur_flat_buf_reg_left,
		*cur_flat_buf_reg_off_p,
		*flat_file_index_p, *flat_file_index_p, 
		flat_file_p->blocklens[*flat_file_index_p],
		cur_flat_file_reg_left,
		*cur_flat_file_reg_off_p,
		*bytes_completed,
		*buf_ol_count_p, *file_ol_count_p,
		*buf_ol_count_p,
		buf_len_arr[*buf_ol_count_p],
		*file_ol_count_p,
		file_len_arr[*file_ol_count_p]);
#endif

	/* What is the size of the next contiguous region agreed
	 * upon by both memory and file regions that does not
	 * surpass the file size */
	if (cur_flat_buf_reg_left > cur_flat_file_reg_left)
	    region_size = cur_flat_file_reg_left;
	else
	    region_size = cur_flat_buf_reg_left;
	
	if (region_size > total_io_size - *bytes_completed)
	    region_size = total_io_size - *bytes_completed;
	
	/* Add this piece to both the mem and file arrays
	 * coalescing offset-length pairs if possible and advance 
	 * the pointers through the flatten mem and file datatypes
	 * as well Note: no more than a single piece can be done 
	 * since we take the smallest one possible */
	
	if (cur_flat_buf_reg_left == region_size)
	{
#ifdef DEBUG_LIST2
	    fprintf(stderr, "reached end of memory block...\n");
#endif
	    (*flat_buf_index_p) = ((*flat_buf_index_p) + 1) % 
		flat_buf_p->count;
	    while (flat_buf_p->blocklens[(*flat_buf_index_p)] == 0)
	    {
		(*flat_buf_index_p) = ((*flat_buf_index_p) + 1) % 
		    flat_buf_p->count;
	    }
	    *cur_flat_buf_reg_off_p = 0;

#ifdef COALESCE_REGIONS
	    if (*buf_ol_count_p != 0)
	    {
		if (buf_off_arr[(*buf_ol_count_p) - 1] +
		    buf_len_arr[(*buf_ol_count_p) - 1] ==
		    buf_off_arr[*buf_ol_count_p])
		{
		    buf_len_arr[(*buf_ol_count_p) - 1] +=
			region_size;
		}
		else
		{
		    buf_len_arr[*buf_ol_count_p] += region_size;
		    (*buf_ol_count_p)++;
		}
	    }
	    else
	    {
#endif
		buf_len_arr[*buf_ol_count_p] += region_size;
		(*buf_ol_count_p)++;
#ifdef COALESCE_REGIONS
	    }
#endif

	    /* Don't prepare for the next piece if we have reached 
	     * the limit or else it will segment fault. */
	    if ((*buf_ol_count_p) != max_ol_count)
	    {
		buf_off_arr[*buf_ol_count_p] = 
		    ((*bytes_completed + region_size) / flat_buf_size) * 
		    flat_buf_extent + 
		    flat_buf_p->indices[*flat_buf_index_p] +
		    (*cur_flat_buf_reg_off_p);
		buf_len_arr[*buf_ol_count_p] = 0;
	    }
	}
	else if (cur_flat_buf_reg_left > region_size)
	{
#ifdef DEBUG_LIST2
	    fprintf(stderr, "advanced %d in memory block...\n",
		    region_size);
#endif
	    (*cur_flat_buf_reg_off_p) += region_size;
	    buf_len_arr[*buf_ol_count_p] += region_size;
	}
	else
	{
	    fprintf(stderr, "gen_listio_arr: Error\n");
	}
	
	/* To calculate the absolute file offset we need to 
	 * add the disp, how many filetypes we have gone through,
	 * the relative block offset in the filetype and how far
	 * into the block we have gone. */
	if (cur_flat_file_reg_left == region_size)
	{
#ifdef DEBUG_LIST2
	    fprintf(stderr, "reached end of file block...\n");
#endif
	    (*flat_file_index_p) = ((*flat_file_index_p) + 1) % 
		flat_file_p->count;
	    while (flat_file_p->blocklens[(*flat_file_index_p)] == 0)
	    {
		(*flat_file_index_p) = ((*flat_file_index_p) + 1) % 
		    flat_file_p->count;
	    }
	    (*cur_flat_file_reg_off_p) = 0;

#ifdef COALESCE_REGIONS
            if (*file_ol_count_p != 0)
            {
                if (file_off_arr[(*file_ol_count_p) - 1] +
                    file_len_arr[(*file_ol_count_p) - 1] ==
                    file_off_arr[*file_ol_count_p])
                {
                    file_len_arr[(*file_ol_count_p) - 1] +=
                        region_size;
                }
                else
                {
                    file_len_arr[*file_ol_count_p] += region_size;
                    (*file_ol_count_p)++;
                }
            }
            else
            {
#endif
                file_len_arr[*file_ol_count_p] += region_size;
                (*file_ol_count_p)++;
#ifdef COALESCE_REGIONS
            }
#endif

	    /* Don't prepare for the next piece if we have reached
             * the limit or else it will segment fault. */
            if ((*file_ol_count_p) != max_ol_count)
	    {
		file_off_arr[*file_ol_count_p] = disp + 
		    (((bytes_into_filetype + *bytes_completed + region_size) 
		      / flat_file_size) * 
		     flat_file_extent) + 
		    flat_file_p->indices[*flat_file_index_p] +
		    (*cur_flat_file_reg_off_p);
		file_len_arr[*file_ol_count_p] = 0;
	    }
	}
	else if (cur_flat_file_reg_left > region_size)
	{
#ifdef DEBUG_LIST2
	    fprintf(stderr, "advanced %d in file block...\n",
		    region_size);
#endif
	    (*cur_flat_file_reg_off_p) += region_size;
	    file_len_arr[*file_ol_count_p] += region_size;
	}
	else
	{
	    fprintf(stderr, "gen_listio_arr: Error\n");
	}
#ifdef DEBUG_LIST2
	fprintf(stderr, 
		"------------------------------\n\n");
#endif
	*bytes_completed += region_size;
    }
    /* Increment the count if we stopped in the middle of a 
     * memory or file region */
    if (*cur_flat_buf_reg_off_p != 0)
	(*buf_ol_count_p)++;
    if (*cur_flat_file_reg_off_p != 0)
	(*file_ol_count_p)++;

    return 0;
}

void print_buf_file_ol_pairs(int64_t buf_off_arr[],
			     int32_t buf_len_arr[],
			     int32_t buf_ol_count,
			     int64_t file_off_arr[],
			     int32_t file_len_arr[],
			     int32_t file_ol_count,
			     void *buf,
			     int rw_type)
{
    int i = -1;
    
    fprintf(stderr, "buf_ol_pairs(offset,length) count = %d\n",
	    buf_ol_count);
    for (i = 0; i < buf_ol_count; i++)
    {
	fprintf(stderr, "(%lld, %d) ", (long long)buf_off_arr[i], buf_len_arr[i]);
    }
    fprintf(stderr, "\n");

    fprintf(stderr, "file_ol_pairs(offset,length) count = %d\n",
	    file_ol_count);
    for (i = 0; i < file_ol_count; i++)
    {
	fprintf(stderr, "(%lld, %d) ", (long long)file_off_arr[i], file_len_arr[i]);
    }
    fprintf(stderr, "\n\n");

}

void ADIOI_DAOS_ReadStrided(ADIO_File fd, void *buf, int count,
                            MPI_Datatype datatype, int file_ptr_type,
                            ADIO_Offset offset, ADIO_Status *status,
                            int *error_code)
{
    ADIOI_DAOS_StridedListIO(fd, buf, count, datatype, file_ptr_type,
                             offset, status, error_code, DAOS_READ);
    return;
}

void ADIOI_DAOS_WriteStrided(ADIO_File fd, const void *buf, int count,
                             MPI_Datatype datatype, int file_ptr_type,
                             ADIO_Offset offset, ADIO_Status *status,
                             int *error_code)
{
    ADIOI_DAOS_StridedListIO(fd, (void *)buf, count, datatype, file_ptr_type,
                             offset, status, error_code, DAOS_WRITE);
    return;
}






#if 0
void ADIOI_DAOS_Strided_naive(ADIO_File fd, void *buf, int count,
                              MPI_Datatype buftype, int file_ptr_type,
                              ADIO_Offset offset, ADIO_Status *status,
                              int *error_code, int rw_type)
{
    /* offset is in units of etype relative to the filetype. */

    ADIOI_Flatlist_node *flat_buf, *flat_file;
    /* bwr == buffer write; fwr == file write */
    ADIO_Offset bwr_size, fwr_size=0, sum, size_in_filetype; 
    int b_index;
    MPI_Count bufsize;
    ADIO_Offset n_etypes_in_filetype;
    ADIO_Offset size, n_filetypes, etype_in_filetype;
    ADIO_Offset abs_off_in_filetype=0, req_len;
    MPI_Count filetype_size, etype_size, buftype_size;
    MPI_Aint filetype_extent, buftype_extent; 
    int buf_count, buftype_is_contig, filetype_is_contig;
    ADIO_Offset userbuf_off;
    ADIO_Offset off, req_off, disp, end_offset=0, start_off;
    ADIO_Status status1;

    *error_code = MPI_SUCCESS;  /* changed below if error */

    ADIOI_Datatype_iscontig(buftype, &buftype_is_contig);
    ADIOI_Datatype_iscontig(fd->filetype, &filetype_is_contig);

    MPI_Type_size_x(fd->filetype, &filetype_size);
    if ( ! filetype_size ) {
	MPIR_Status_set_bytes(status, buftype, 0);
	*error_code = MPI_SUCCESS; 
	return;
    }

    MPI_Type_extent(fd->filetype, &filetype_extent);
    MPI_Type_size_x(buftype, &buftype_size);
    MPI_Type_extent(buftype, &buftype_extent);
    etype_size = fd->etype_size;

    ADIOI_Assert((buftype_size * count) == ((ADIO_Offset)(unsigned)buftype_size * (ADIO_Offset)count));
    bufsize = buftype_size * count;

    /* contiguous in buftype and filetype is handled elsewhere */

    if (!buftype_is_contig && filetype_is_contig) {
    	int b_count;
	/* noncontiguous in memory, contiguous in file. */

	flat_buf = ADIOI_Flatten_and_find(buftype);

        off = (file_ptr_type == ADIO_INDIVIDUAL) ? fd->fp_ind : 
              fd->disp + (ADIO_Offset)etype_size * offset;

	start_off = off;
	end_offset = off + bufsize - 1;

	/* for each region in the buffer, grab the data and put it in
	 * place
	 */
        for (b_count=0; b_count < count; b_count++) {
            for (b_index=0; b_index < flat_buf->count; b_index++) {
                userbuf_off = (ADIO_Offset)b_count*(ADIO_Offset)buftype_extent + 
		              flat_buf->indices[b_index];
		req_off = off;
		req_len = flat_buf->blocklens[b_index];

                ADIOI_Assert(req_len == (int) req_len);
                ADIOI_Assert((((ADIO_Offset)(uintptr_t)buf) + userbuf_off) ==
                             (ADIO_Offset)(uintptr_t)((uintptr_t)buf + userbuf_off));

                if (rw_type == DAOS_WRITE)
                    ADIO_WriteContig(fd, 
                                     (char *) buf + userbuf_off,
                                     (int)req_len, 
                                     MPI_BYTE, 
                                     ADIO_EXPLICIT_OFFSET,
                                     req_off,
                                     &status1,
                                     error_code);
                else
                    ADIO_ReadContig(fd, 
                                    (char *) buf + userbuf_off,
                                    (int)req_len, 
                                    MPI_BYTE, 
                                    ADIO_EXPLICIT_OFFSET,
                                    req_off,
                                    &status1,
                                    error_code);

		if (*error_code != MPI_SUCCESS) return;

		/* off is (potentially) used to save the final offset later */
                off += flat_buf->blocklens[b_index];
            }
	}

        if (file_ptr_type == ADIO_INDIVIDUAL) fd->fp_ind = off;
    }

    else {  /* noncontiguous in file */
    	int f_index, st_index = 0;
        ADIO_Offset st_fwr_size, st_n_filetypes;
	int flag;

        /* First we're going to calculate a set of values for use in all
	 * the noncontiguous in file cases:
	 * start_off - starting byte position of data in file
	 * end_offset - last byte offset to be acessed in the file
	 * st_n_filetypes - how far into the file we start in terms of
	 *                  whole filetypes
	 * st_index - index of block in first filetype that we will be
	 *            starting in (?)
	 * st_fwr_size - size of the data in the first filetype block
	 *               that we will write (accounts for being part-way
	 *               into writing this block of the filetype
	 *
	 */

	flat_file = ADIOI_Flatten_and_find(fd->filetype);
	disp = fd->disp;

	if (file_ptr_type == ADIO_INDIVIDUAL) {
	    start_off = fd->fp_ind; /* in bytes */
	    n_filetypes = -1;
	    flag = 0;
	    while (!flag) {
                n_filetypes++;
		for (f_index=0; f_index < flat_file->count; f_index++) {
		    if (disp + flat_file->indices[f_index] + 
                       n_filetypes*(ADIO_Offset)filetype_extent + 
		       flat_file->blocklens[f_index] >= start_off) 
		    {
		    	/* this block contains our starting position */

			st_index = f_index;
			fwr_size = disp + flat_file->indices[f_index] + 
		 	           n_filetypes*(ADIO_Offset)filetype_extent + 
				   flat_file->blocklens[f_index] - start_off;
			flag = 1;
			break;
		    }
		}
	    }
	}
	else {
	    n_etypes_in_filetype = filetype_size/etype_size;
	    n_filetypes = offset / n_etypes_in_filetype;
	    etype_in_filetype = offset % n_etypes_in_filetype;
	    size_in_filetype = etype_in_filetype * etype_size;
 
	    sum = 0;
	    for (f_index=0; f_index < flat_file->count; f_index++) {
		sum += flat_file->blocklens[f_index];
		if (sum > size_in_filetype) {
		    st_index = f_index;
		    fwr_size = sum - size_in_filetype;
		    abs_off_in_filetype = flat_file->indices[f_index] +
			                  size_in_filetype - 
			                  (sum - flat_file->blocklens[f_index]);
		    break;
		}
	    }

	    /* abs. offset in bytes in the file */
	    start_off = disp + n_filetypes*(ADIO_Offset)filetype_extent + 
	    	        abs_off_in_filetype;
	}

	st_fwr_size = fwr_size;
	st_n_filetypes = n_filetypes;

	/* start_off, st_n_filetypes, st_index, and st_fwr_size are 
	 * all calculated at this point
	 */

        /* Calculate end_offset, the last byte-offset that will be accessed.
         * e.g., if start_off=0 and 100 bytes to be written, end_offset=99
	 */
	userbuf_off = 0;
	f_index = st_index;
	off = start_off;
	fwr_size = MPL_MIN(st_fwr_size, bufsize);
	while (userbuf_off < bufsize) {
	    userbuf_off += fwr_size;
	    end_offset = off + fwr_size - 1;

	    if (f_index < (flat_file->count - 1)) f_index++;
	    else {
		f_index = 0;
		n_filetypes++;
	    }

	    off = disp + flat_file->indices[f_index] + 
	          n_filetypes*(ADIO_Offset)filetype_extent;
	    fwr_size = MPL_MIN(flat_file->blocklens[f_index], 
	                         bufsize-(unsigned)userbuf_off);
	}

	/* End of calculations.  At this point the following values have
	 * been calculated and are ready for use:
	 * - start_off
	 * - end_offset
	 * - st_n_filetypes
	 * - st_index
	 * - st_fwr_size
	 */

	if (buftype_is_contig && !filetype_is_contig) {
	    /* contiguous in memory, noncontiguous in file. should be the
	     * most common case.
	     */

	    userbuf_off = 0;
	    f_index = st_index;
	    off = start_off;
	    n_filetypes = st_n_filetypes;
	    fwr_size = MPL_MIN(st_fwr_size, bufsize);

	    /* while there is still space in the buffer, write more data */
	    while (userbuf_off < bufsize) {
                if (fwr_size) { 
                    /* TYPE_UB and TYPE_LB can result in 
                       fwr_size = 0. save system call in such cases */ 
		    req_off = off;
		    req_len = fwr_size;

                    ADIOI_Assert(req_len == (int) req_len);
                    ADIOI_Assert((((ADIO_Offset)(uintptr_t)buf) + userbuf_off) ==
                                 (ADIO_Offset)(uintptr_t)((uintptr_t)buf + userbuf_off));

                    if (rw_type == DAOS_WRITE)
                        ADIO_WriteContig(fd, 
                                         (char *) buf + userbuf_off,
                                         (int)req_len, 
                                         MPI_BYTE, 
                                         ADIO_EXPLICIT_OFFSET,
                                         req_off,
                                         &status1,
                                         error_code);
                    else
                        ADIO_ReadContig(fd, 
                                        (char *) buf + userbuf_off,
                                        (int)req_len, 
                                        MPI_BYTE, 
                                        ADIO_EXPLICIT_OFFSET,
                                        req_off,
                                        &status1,
                                        error_code);
		    if (*error_code != MPI_SUCCESS) return;
		}
		userbuf_off += fwr_size;

                if (off + fwr_size < disp + flat_file->indices[f_index] +
                   flat_file->blocklens[f_index] + 
		   n_filetypes*(ADIO_Offset)filetype_extent)
		{
		    /* important that this value be correct, as it is
		     * used to set the offset in the fd near the end of
		     * this function.
		     */
                    off += fwr_size;
		}
                /* did not reach end of contiguous block in filetype.
                 * no more I/O needed. off is incremented by fwr_size.
		 */
                else {
		    if (f_index < (flat_file->count - 1)) f_index++;
		    else {
			f_index = 0;
			n_filetypes++;
		    }
		    off = disp + flat_file->indices[f_index] + 
                          n_filetypes*(ADIO_Offset)filetype_extent;
		    fwr_size = MPL_MIN(flat_file->blocklens[f_index], 
		                         bufsize-(unsigned)userbuf_off);
		}
	    }
	}
	else {
	    ADIO_Offset i_offset, tmp_bufsize = 0;
	    /* noncontiguous in memory as well as in file */

	    flat_buf = ADIOI_Flatten_and_find(buftype);

	    b_index = buf_count = 0;
	    i_offset = flat_buf->indices[0];
	    f_index = st_index;
	    off = start_off;
	    n_filetypes = st_n_filetypes;
	    fwr_size = st_fwr_size;
	    bwr_size = flat_buf->blocklens[0];

	    /* while we haven't read size * count bytes, keep going */
	    while (tmp_bufsize < bufsize) {
    		ADIO_Offset new_bwr_size = bwr_size, new_fwr_size = fwr_size;

		size = MPL_MIN(fwr_size, bwr_size);
		if (size) {
		    req_off = off;
		    req_len = size;
		    userbuf_off = i_offset;

                    ADIOI_Assert(req_len == (int) req_len);
                    ADIOI_Assert((((ADIO_Offset)(uintptr_t)buf) + userbuf_off) ==
                                 (ADIO_Offset)(uintptr_t)((uintptr_t)buf + userbuf_off));

                    if (rw_type == DAOS_WRITE)
                        ADIO_WriteContig(fd, 
                                         (char *) buf + userbuf_off,
                                         (int)req_len, 
                                         MPI_BYTE, 
                                         ADIO_EXPLICIT_OFFSET,
                                         req_off,
                                         &status1,
                                         error_code);
                    else
                        ADIO_ReadContig(fd, 
                                         (char *) buf + userbuf_off,
                                         (int)req_len, 
                                         MPI_BYTE, 
                                         ADIO_EXPLICIT_OFFSET,
                                         req_off,
                                         &status1,
                                         error_code);
		    if (*error_code != MPI_SUCCESS) return;
		}

		if (size == fwr_size) {
		    /* reached end of contiguous block in file */
		    if (f_index < (flat_file->count - 1)) f_index++;
		    else {
			f_index = 0;
			n_filetypes++;
		    }

		    off = disp + flat_file->indices[f_index] + 
                          n_filetypes*(ADIO_Offset)filetype_extent;

		    new_fwr_size = flat_file->blocklens[f_index];
		    if (size != bwr_size) {
			i_offset += size;
			new_bwr_size -= size;
		    }
		}

		if (size == bwr_size) {
		    /* reached end of contiguous block in memory */

		    b_index = (b_index + 1)%flat_buf->count;
		    buf_count++;
		    i_offset = (ADIO_Offset)buftype_extent*(ADIO_Offset)(buf_count/flat_buf->count) +
			flat_buf->indices[b_index];
		    new_bwr_size = flat_buf->blocklens[b_index];
		    if (size != fwr_size) {
			off += size;
			new_fwr_size -= size;
		    }
		}
		tmp_bufsize += size;
		fwr_size = new_fwr_size;
                bwr_size = new_bwr_size;
	    }
	}

	if (file_ptr_type == ADIO_INDIVIDUAL) fd->fp_ind = off;
    } /* end of (else noncontiguous in file) */

    fd->fp_sys_posn = -1;   /* mark it as invalid. */

    MPIR_Status_set_bytes(status, buftype, bufsize);

}
#endif
