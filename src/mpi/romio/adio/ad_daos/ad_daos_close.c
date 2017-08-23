/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil ; -*- */

#include "ad_daos.h"

void ADIOI_DAOS_Close(ADIO_File fd, int *error_code)
{
    int rank;
    struct ADIO_DAOS_cont *cont = (struct ADIO_DAOS_cont *)fd->fs_ptr;
    static char myname[] = "ADIOI_DAOS_CLOSE";
    int rc;

    if (cont->amode == DAOS_COO_RW) {
        MPI_Barrier(fd->comm);

        MPI_Comm_rank(fd->comm, &rank);
        if (rank == 0)
            rc = daos_epoch_commit(cont->coh, cont->epoch, NULL, NULL);

        MPI_Bcast(&rc, 1, MPI_INT, 0, fd->comm);

        if (rc != 0) {
            *error_code = MPIO_Err_create_code(MPI_SUCCESS,
                                               MPIR_ERR_RECOVERABLE,
                                               myname, __LINE__,
                                               ADIOI_DAOS_error_convert(rc),
                                               "Epoch Commit failed", 0);
            return;
        }
    }

    rc = daos_array_close(cont->oh, NULL);
    if (rc != 0) {
        printf("daos_array_close() failed (%d)\n", rc);
        *error_code = MPIO_Err_create_code(MPI_SUCCESS,
                                           MPIR_ERR_RECOVERABLE,
                                           myname, __LINE__,
                                           ADIOI_DAOS_error_convert(rc),
                                           "Container Close failed", 0);
        return;
    }

    rc = daos_cont_close(cont->coh, NULL);
    if (rc != 0) {
        printf("daos_cont_close() failed (%d)\n", rc);
        *error_code = MPIO_Err_create_code(MPI_SUCCESS,
                                           MPIR_ERR_RECOVERABLE,
                                           myname, __LINE__,
                                           ADIOI_DAOS_error_convert(rc),
                                           "Container Close failed", 0);
        return;
    }

    ADIOI_Free(fd->fs_ptr);
    fd->fs_ptr = NULL;

    *error_code = MPI_SUCCESS;
}
/*
 * vim: ts=8 sts=4 sw=4 noexpandtab
 */
