/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil ; -*- */
/*
 *
 *   Copyright (C) 1997 University of Chicago.
 *   See COPYRIGHT notice in top-level directory.
 *
 *   Copyright (C) 2018 Intel Corporation
 */

#include "ad_daos.h"

#include "hint_fns.h"

void ADIOI_DAOS_SetInfo(ADIO_File fd, MPI_Info users_info, int *error_code)
{
    char *value;
    int flag, tmp_value;
    static char myname[] = "ADIOI_DAOS_SETINFO";

    if ((fd->info) == MPI_INFO_NULL) {
	/* part of the open call */
	MPI_Info_create(&(fd->info));

        ADIOI_Info_set(fd->info, "romio_daos_chunk_size", "1048576");
        fd->hints->fs_hints.daos.chunk_size = 1048576;

        ADIOI_Info_set(fd->info, "romio_daos_obj_class", "DAOS_OC_LARGE_RW");
        fd->hints->fs_hints.daos.obj_class = DAOS_OC_LARGE_RW;

	if (users_info != MPI_INFO_NULL) {
	    /* Chunk size in each dkey */
	    ADIOI_Info_check_and_install_int(fd, users_info, "romio_daos_chunk_size",
		    &(fd->hints->fs_hints.daos.chunk_size), myname, error_code);

	    /* object class for each file */
	    ADIOI_Info_check_and_install_int(fd, users_info, "romio_daos_obj_class",
		    &(fd->hints->fs_hints.daos.obj_class), myname, error_code);
	}
    }

    /* set the values for collective I/O and data sieving parameters */
    ADIOI_GEN_SetInfo(fd, users_info, error_code);

    *error_code = MPI_SUCCESS;
}
